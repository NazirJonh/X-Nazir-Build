/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Paint Layers row bake tests: the cache key, the synchronous planner, the worker job, and what the
 * generator and the CPU composite read once a row is baked.
 *
 * The fixture lives in `paint_layers_generate_test_fixture.hh`.
 */

#include "paint_layers_generate_test_fixture.hh"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <utility>

#include "MEM_guardedalloc.h"

#include "BLI_math_base.h"
#include "BLI_math_color.h"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_set.hh"

#include "BKE_attribute.hh"
#include "BKE_appdir.hh"
#include "BKE_blender.hh"
#include "BKE_collection.hh"
#include "BKE_idprop.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_layers_asset.hh"
#include "BKE_paint_layers_generator.hh"
#include "BKE_paint_layers_gradient.hh"
#include "BKE_paint_layers_gradient_surface.hh"
#include "BKE_paint_material_composite_surface.hh"
#include "BKE_scene.hh"

#include "DEG_depsgraph.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "BLI_fileops.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "BLO_readfile.hh"
#include "BLO_writefile.hh"

#include "BKE_wm_runtime.hh"

#include "DNA_windowmanager_types.h"

#include "../paint_layers_bake_intern.hh"
#include "../paint_layers_intern.hh"

namespace blender::bke::tests {

class PaintLayersBakeTest : public PaintLayersGenerateTest {
 public:
  void SetUp() override
  {
    PaintLayersGenerateTest::SetUp();
    /* A dummy window manager, the way the blend-file loading tests make one: the frame update
     * notifies image editors by walking bmain->wm and dereferences it without a check, so the
     * update crashes in a bare test Main that has no window manager of its own. */
    wmWindowManager *wm = BKE_id_new<wmWindowManager>(bmain, "WMdummy");
    wm->runtime = MEM_new<bke::WindowManagerRuntime>(__func__);
  }

  void TearDown() override
  {
    /* A bake of a row or mask item that carries a CPU-computed effect leaves the computed grey in
     * the process-global effect caches; freed here, the way the effect tests free them, so the
     * leak check at the end of the run sees none of a test's entries. */
    BKE_paint_layers_effect_cache_free_all();
    BKE_paint_layers_effect_gpu_cache_free_all();
    BKE_paint_layers_mask_effect_cache_free_all();
    /* The stroke flag is runtime state a test may have left set; clear it so it cannot leak into
     * the next test. */
    BKE_paint_layers_bake_stroke_active_set(false);
    PaintLayersGenerateTest::TearDown();
  }

  /** A Paint row baked on request: Always, so the planner does not wait for it to be heavy. */
  MaterialPaintLayer *add_baked_row(const char *name)
  {
    MaterialPaintLayer *layer = add_paint_layer(name, add_image(name));
    EXPECT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *layer, MA_PAINT_LAYER_BAKE_ALWAYS));
    return layer;
  }

  /** Nothing is active, so no row is held live for editing. */
  void clear_active() const
  {
    BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  }

  /**
   * A scene and view layer with one mesh object shaded by \a ma, and an evaluated depsgraph. The
   * mesh has no UV map, which is the permanent reason an Object/Generated bake must stay live.
   */
  Scene *add_scene_with_uvless_object()
  {
    Scene *scene = BKE_scene_add(bmain, "BakeScene");
    ViewLayer *view_layer = static_cast<ViewLayer *>(scene->view_layers.first);
    Mesh *mesh = BKE_mesh_add(bmain, "BakeMesh");
    Object *object = BKE_object_add_only_object(bmain, OB_MESH, "BakeObject");
    object->data = &mesh->id;
    id_us_plus(&mesh->id);
    BKE_object_material_assign(bmain, object, ma, 1, BKE_MAT_ASSIGN_OBDATA);
    BKE_collection_object_add(bmain, scene->master_collection, object);
    Depsgraph *depsgraph = BKE_scene_ensure_depsgraph(bmain, scene, view_layer);
    EXPECT_NE(depsgraph, nullptr);
    if (depsgraph != nullptr) {
      BKE_scene_graph_update_for_newframe(depsgraph);
    }
    return scene;
  }

  /** One quad with a unit UV map, shaded by \a ma and linked to \a scene. */
  Object *add_uv_quad_object(Scene &scene, const char *name)
  {
    Object *object = BKE_object_add_only_object(bmain, OB_MESH, name);
    Mesh *mesh = BKE_mesh_add(bmain, name);
    object->data = &mesh->id;
    id_us_plus(&mesh->id);

    Mesh *source = BKE_mesh_new_nomain(4, 0, 1, 4);
    MutableSpan<float3> positions = source->vert_positions_for_write();
    positions[0] = float3(0.0f, 0.0f, 0.0f);
    positions[1] = float3(2.0f, 0.0f, 0.0f);
    positions[2] = float3(2.0f, 2.0f, 0.0f);
    positions[3] = float3(0.0f, 2.0f, 0.0f);
    MutableSpan<int> offsets = source->face_offsets_for_write();
    offsets[0] = 0;
    offsets[1] = 4;
    MutableSpan<int> corners = source->corner_verts_for_write();
    corners[0] = 0;
    corners[1] = 1;
    corners[2] = 2;
    corners[3] = 3;
    bke::SpanAttributeWriter<float2> uvs =
        source->attributes_for_write().lookup_or_add_for_write_span<float2>(
            "UVMap", bke::AttrDomain::Corner);
    uvs.span[0] = float2(0.0f, 0.0f);
    uvs.span[1] = float2(1.0f, 0.0f);
    uvs.span[2] = float2(1.0f, 1.0f);
    uvs.span[3] = float2(0.0f, 1.0f);
    uvs.finish();
    source->tag_topology_changed();
    source->tag_positions_changed();
    BKE_mesh_nomain_to_mesh(source, mesh, object);
    mesh_ensure_default_uv_map(*mesh);

    BKE_object_material_assign(bmain, object, ma, 1, BKE_MAT_ASSIGN_OBDATA);
    BKE_collection_object_add(bmain, scene.master_collection, object);
    return object;
  }

  void update_scene(Scene &scene)
  {
    ViewLayer *view_layer = static_cast<ViewLayer *>(scene.view_layers.first);
    Depsgraph *depsgraph = BKE_scene_ensure_depsgraph(bmain, &scene, view_layer);
    ASSERT_NE(depsgraph, nullptr);
    BKE_scene_graph_update_for_newframe(depsgraph);
  }

  /** The RGBA bytes of \a image's tile, as the map's own buffer keeps them. */
  static Vector<uchar> tile_pixel_bytes(Image &image, const int tile = 0)
  {
    ImageUser iuser{};
    iuser.tile = tile;
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(&image, tile > 0 ? &iuser : nullptr, &lock);
    Vector<uchar> pixels;
    if (ibuf != nullptr) {
      if (const uchar *data = ibuf->byte_data()) {
        pixels.extend(data, int64_t(ibuf->x) * ibuf->y * 4);
      }
    }
    BKE_image_release_ibuf(&image, ibuf, lock);
    return pixels;
  }

  /** The RGBA floats of \a image's tile, as the map's own buffer keeps them. */
  static Vector<float> tile_pixel_floats(Image &image, const int tile = 0)
  {
    ImageUser iuser{};
    iuser.tile = tile;
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(&image, tile > 0 ? &iuser : nullptr, &lock);
    Vector<float> pixels;
    if (ibuf != nullptr && ibuf->float_buffer.data != nullptr) {
      pixels.extend(ibuf->float_buffer.data, int64_t(ibuf->x) * ibuf->y * 4);
    }
    BKE_image_release_ibuf(&image, ibuf, lock);
    return pixels;
  }

  /** The common factor of one channel's render: what the coverage map folds out of it. */
  static void fold_channel_common(Span<float> color,
                                  Span<float> coverage,
                                  MutableSpan<float> common)
  {
    for (const int64_t i : common.index_range()) {
      const float content = color[i * 4 + 3];
      if (content > 0.002f) {
        common[i] = std::max(common[i], clamp_f(coverage[i] / content, 0.0f, 1.0f));
      }
    }
  }

  /**
   * A folder with two rows inside another folder with one more row, the inner one isolating (its
   * opacity is below one: a plain grouping folder is Pass Through and never baked). The inner
   * folder is nested at level 2, the depth AUTO bakes, and heavy enough to go to the worker.
   */
  MaterialPaintLayer *add_nested_folder(MaterialPaintLayer **r_first = nullptr)
  {
    MaterialPaintLayer *first = add_paint_layer("A", add_image("A"));
    MaterialPaintLayer *second = add_paint_layer("B", add_image("B"));
    MaterialPaintLayer *members[] = {first, second};
    MaterialPaintLayer *inner = BKE_paint_layers_group(*ma, Span<MaterialPaintLayer *>(members, 2));
    EXPECT_NE(inner, nullptr);
    EXPECT_TRUE(BKE_paint_layers_set_opacity(*ma, inner, 0.9f));
    MaterialPaintLayer *top = add_paint_layer("C", add_image("C"));
    MaterialPaintLayer *outer_members[] = {inner, top};
    EXPECT_NE(BKE_paint_layers_group(*ma, Span<MaterialPaintLayer *>(outer_members, 2)), nullptr);
    if (r_first != nullptr) {
      *r_first = first;
    }
    return inner;
  }

  /** The image node of \a tree (and the layer groups under it) that reads \a image. */
  static int image_reader_count(const bNodeTree &tree, const Image *image)
  {
    int count = 0;
    for (const bNode &node : tree.nodes) {
      if (node.type_legacy == SH_NODE_TEX_IMAGE && node.id == &image->id) {
        count++;
      }
      if (node.is_group() && node.id != nullptr && GS(node.id->name) == ID_NT &&
          !BKE_paint_material_is_normal_combine_group(node))
      {
        count += image_reader_count(*reinterpret_cast<const bNodeTree *>(node.id), image);
      }
    }
    return count;
  }
};

TEST_F(PaintLayersBakeTest, mode_and_size_round_trip)
{
  MaterialPaintLayer *layer = add_paint_layer("Row", add_image("Row"));
  EXPECT_EQ(BKE_paint_layers_bake_mode_get(*layer), MA_PAINT_LAYER_BAKE_AUTO);
  EXPECT_EQ(layer->bake, nullptr) << "reading the mode must not allocate a cache";

  EXPECT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *layer, MA_PAINT_LAYER_BAKE_NEVER));
  EXPECT_EQ(BKE_paint_layers_bake_mode_get(*layer), MA_PAINT_LAYER_BAKE_NEVER);
  EXPECT_TRUE(BKE_paint_layers_bake_size_set(*ma, *layer, 64));
  EXPECT_EQ(BKE_paint_layers_bake_size_get(*layer), 64);

  EXPECT_FALSE(BKE_paint_layers_bake_mode_set(*ma, *layer, 99)) << "an unknown mode is refused";
  EXPECT_FALSE(BKE_paint_layers_bake_size_set(*ma, *layer, 0));
}

TEST_F(PaintLayersBakeTest, a_bake_is_invalid_until_its_hash_is_stamped)
{
  MaterialPaintLayer *layer = add_paint_layer("Row", add_image("Row"));
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *layer)) << "no cache at all";

  BKE_paint_layers_bake_struct_ensure(*layer);
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *layer)) << "a zero hash proves nothing";

  BKE_paint_layers_bake_finalize(*ma, *layer);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *layer));

  /* The opacity is baked into the coverage, so it is part of what the hash vouches for. */
  EXPECT_TRUE(BKE_paint_layers_set_opacity(*ma, layer, 0.5f));
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *layer));
}

TEST_F(PaintLayersBakeTest, the_hash_ignores_the_visibility_of_the_row_itself)
{
  MaterialPaintLayer *layer = add_paint_layer("Row", add_image("Row"));
  BKE_paint_layers_bake_struct_ensure(*layer);
  BKE_paint_layers_bake_finalize(*ma, *layer);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *layer));

  /* Visibility stays a live factor, so hiding the row neither invalidates nor needs a re-bake. */
  EXPECT_TRUE(BKE_paint_layers_set_enabled(*ma, layer, false));
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *layer));
}

TEST_F(PaintLayersBakeTest, the_hash_ignores_the_lock_of_the_row_itself)
{
  MaterialPaintLayer *layer = add_paint_layer("Row", add_image("Row"));
  BKE_paint_layers_bake_struct_ensure(*layer);
  BKE_paint_layers_bake_finalize(*ma, *layer);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *layer));

  /* A lock changes no pixel, so it must neither invalidate nor schedule a re-bake. */
  ASSERT_TRUE(BKE_paint_layers_set_locked(*ma, layer, true));
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *layer));
  ASSERT_TRUE(BKE_paint_layers_set_locked(*ma, layer, false));
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *layer));
}

TEST_F(PaintLayersBakeTest, a_uv_gradient_can_be_row_baked)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *layer = BKE_paint_layers_gradient_add(
      *ma, "Gradient", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *layer, MA_PAINT_LAYER_GRADIENT_SPACE_UV));
  EXPECT_TRUE(BKE_paint_layers_generator_is_shared_bakeable(*layer));
  EXPECT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *layer, MA_PAINT_LAYER_BAKE_ALWAYS));
  clear_active();

  bool changed = false;
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_TRUE(changed);
  ASSERT_NE(layer->bake, nullptr);
  EXPECT_NE(layer->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR], nullptr);
  EXPECT_NE(layer->bake->coverage, nullptr);
}

/**
 * AUTO is the mode every row starts in, and a Gradient row has no map for the live path to read
 * instead: leaving the row must be enough for the cache to be made, with no ALWAYS asked for.
 */
TEST_F(PaintLayersBakeTest, an_auto_gradient_row_bakes_itself_and_matches_the_live_preview)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *layer = BKE_paint_layers_gradient_add(
      *ma, "Gradient", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *layer, MA_PAINT_LAYER_GRADIENT_SPACE_UV));
  const float start[3] = {0.0f, 0.5f, 0.0f};
  const float end[3] = {1.0f, 0.5f, 0.0f};
  ASSERT_TRUE(BKE_paint_layers_gradient_set_line(*ma, *layer, start, end));
  clear_active();

  /* The live row, before anything is cached. */
  ImBuf *live = IMB_allocImBuf(8, 8, ImBufFlags::ByteData);
  ASSERT_NE(live, nullptr);
  live->channels = 4;
  ASSERT_TRUE(BKE_paint_layers_composite_channel(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, *live));

  EXPECT_EQ(BKE_paint_layers_bake_mode_get(*layer), int(MA_PAINT_LAYER_BAKE_AUTO));
  bool changed = false;
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_TRUE(changed);
  ASSERT_NE(layer->bake, nullptr);
  EXPECT_EQ(layer->bake->size, 8) << "the cache follows the canvas the row is laid on";
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *layer));
  EXPECT_TRUE(paint_layer_row_substituted(*ma, *layer));
  EXPECT_NE(layer->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR], nullptr);
  EXPECT_NE(layer->bake->coverage, nullptr);

  /* The composite reads the maps, so no Gradient payload is laid any more. */
  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  ASSERT_EQ(layers.size(), 1);
  EXPECT_FALSE(layers[0].gradient.enabled) << "a substituted row costs no Gradient sample";
  EXPECT_NE(layers[0].color_image, nullptr);

  /* And it is the same picture: the bake renders the formula the live row did. */
  ImBuf *baked = IMB_allocImBuf(8, 8, ImBufFlags::ByteData);
  ASSERT_NE(baked, nullptr);
  baked->channels = 4;
  ASSERT_TRUE(BKE_paint_layers_composite_channel(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, *baked));
  const uchar *live_pixels = live->byte_data();
  const uchar *baked_pixels = baked->byte_data();
  ASSERT_NE(live_pixels, nullptr);
  ASSERT_NE(baked_pixels, nullptr);
  for (const int64_t i : IndexRange(8 * 8)) {
    for (const int k : IndexRange(4)) {
      EXPECT_NEAR(int(live_pixels[i * 4 + k]), int(baked_pixels[i * 4 + k]), 2) << i << " " << k;
    }
  }
  IMB_freeImBuf(live);
  IMB_freeImBuf(baked);
}

/**
 * The regression of a baked Gradient losing its ramp alpha: the bake folds the content alpha out
 * of the coverage it stores (#bake_common_from_channel), so the substituted row has to cover by
 * the colour map's alpha again, or the half-transparent end of the ramp shows opaque. A scalar
 * channel bakes an alpha of one (a value ramp has no transparency), so the multiply it gains is a
 * no-op and its picture keeps matching the live one.
 */
TEST_F(PaintLayersBakeTest, a_baked_gradient_keeps_the_ramp_alpha_the_live_row_covered_by)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *layer = BKE_paint_layers_gradient_add(
      *ma, "Gradient", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *layer, MA_PAINT_LAYER_GRADIENT_SPACE_UV));
  const float start[3] = {0.0f, 0.5f, 0.0f};
  const float end[3] = {1.0f, 0.5f, 0.0f};
  ASSERT_TRUE(BKE_paint_layers_gradient_set_line(*ma, *layer, start, end));
  /* A half-transparent end stop: the content alpha the live row covers by, which only the colour
   * map's alpha carries across the bake. */
  layer->gradient->ramp.data[1].a = 0.5f;
  /* A scalar channel of the same row: its bake carries an alpha of one, so covering by it again
   * must not move the picture. */
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS), nullptr);
  clear_active();

  /* The live stack, before anything is cached. */
  ImBuf *live_color = IMB_allocImBuf(8, 8, ImBufFlags::ByteData);
  ASSERT_NE(live_color, nullptr);
  live_color->channels = 4;
  ASSERT_TRUE(
      BKE_paint_layers_composite_channel(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, *live_color));
  ImBuf *live_rough = IMB_allocImBuf(8, 8, ImBufFlags::ByteData);
  ASSERT_NE(live_rough, nullptr);
  live_rough->channels = 4;
  ASSERT_TRUE(
      BKE_paint_layers_composite_channel(*ma, PAINT_MATERIAL_CHANNEL_ROUGHNESS, *live_rough));

  bool changed = false;
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  ASSERT_TRUE(paint_layer_row_substituted(*ma, *layer));
  ASSERT_NE(layer->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR], nullptr);
  ASSERT_NE(layer->bake->images[PAINT_MATERIAL_CHANNEL_ROUGHNESS], nullptr);

  /* The substituted row covers by the colour map's alpha again, as the live one did -- both in the
   * colour channel the ramp alpha belongs to and in the scalar one. */
  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  ASSERT_EQ(layers.size(), 1);
  EXPECT_TRUE(layers[0].color_alpha_coverage);
  Vector<PaintMaterialCompositeImageLayer> rough_layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_ROUGHNESS, rough_layers));
  ASSERT_EQ(rough_layers.size(), 1);
  EXPECT_TRUE(rough_layers[0].color_alpha_coverage);

  /* And both pictures are the ones the live stack laid: the alpha folded into the coverage at bake
   * time comes back out. */
  const auto expect_matches_live = [&](const int channel, const ImBuf &live) {
    ImBuf *baked = IMB_allocImBuf(8, 8, ImBufFlags::ByteData);
    ASSERT_NE(baked, nullptr);
    baked->channels = 4;
    ASSERT_TRUE(BKE_paint_layers_composite_channel(*ma, eMaterialPaintChannel(channel), *baked));
    const uchar *live_pixels = live.byte_data();
    const uchar *baked_pixels = baked->byte_data();
    ASSERT_NE(live_pixels, nullptr);
    ASSERT_NE(baked_pixels, nullptr);
    for (const int64_t i : IndexRange(8 * 8)) {
      for (const int k : IndexRange(4)) {
        EXPECT_NEAR(int(live_pixels[i * 4 + k]), int(baked_pixels[i * 4 + k]), 2)
            << channel << " texel " << i << " channel " << k;
      }
    }
    IMB_freeImBuf(baked);
  };
  expect_matches_live(PAINT_MATERIAL_CHANNEL_BASE_COLOR, *live_color);
  expect_matches_live(PAINT_MATERIAL_CHANNEL_ROUGHNESS, *live_rough);
  IMB_freeImBuf(live_color);
  IMB_freeImBuf(live_rough);
}

/**
 * What the storage of a generated row's bake costs per channel: the live stack evaluated at the
 * canvas size, the bake made, the substituted stack evaluated again over the same texels.
 *
 * One printed line per channel, so the policy the formats rest on stays reproducible: a byte map
 * keeps 256 levels of a ramp, which a shading input shows as terraces, while a float one reproduces
 * the live calculation. The tolerance asserted is the one the channel's own format promises.
 */
TEST_F(PaintLayersBakeTest, a_gradient_row_bake_keeps_the_precision_its_channel_needs)
{
  /* Long enough that one ramp step is finer than a byte: the case 8 bits cannot hold. */
  const int size = 512;
  ma->paint_layers_image_size = size;
  MaterialPaintLayer *layer = BKE_paint_layers_gradient_add(
      *ma, "Gradient", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *layer, MA_PAINT_LAYER_GRADIENT_SPACE_UV));
  const float line_start[3] = {0.0f, 0.5f, 0.0f};
  const float line_end[3] = {1.0f, 0.5f, 0.0f};
  ASSERT_TRUE(BKE_paint_layers_gradient_set_line(*ma, *layer, line_start, line_end));

  /* Every channel a generated row can lay in. A tangent normal is not a colour a ramp produces,
   * so the row refuses a Normal record outright instead of baking one. Height is left out: a
   * generated row keeps it as a value pair rather than a map, so this map-based measurement has
   * nothing to read back for it (its float map and precision are covered by the layered Height
   * tests in paint_material_channel_test.cc). */
  EXPECT_EQ(BKE_paint_layers_channel_add(*ma, layer, PAINT_MATERIAL_CHANNEL_NORMAL), nullptr);
  Vector<int> channels;
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    if (info.channel == PAINT_MATERIAL_CHANNEL_NORMAL ||
        info.channel == PAINT_MATERIAL_CHANNEL_HEIGHT)
    {
      continue;
    }
    ASSERT_NE(BKE_paint_layers_channel_add(*ma, layer, info.channel), nullptr) << info.ui_name;
    /* A record alone lays nothing for a channel outside the material's set: the set is what the
     * composite and the bake walk. An image-paint channel is enabled the way a stack that lays it
     * does, while the ones without such a target (Custom, AO) stay out, which is the report the
     * measurement below makes for them. */
    if (info.supports_image_paint) {
      ASSERT_TRUE(BKE_paint_layers_channel_set_enable(*ma, info.channel, true)) << info.ui_name;
    }
    const float value[4] = {0.05f, 0.95f, 0.0f, 1.0f};
    ASSERT_TRUE(BKE_paint_layers_channel_set_value(*ma, layer, info.channel, value));
    channels.append(int(info.channel));
  }
  clear_active();

  const auto evaluate = [&](const int channel, const MutableSpan<float> r_pixels) {
    Vector<PaintMaterialCompositeImageLayer> layers;
    if (!BKE_paint_layers_composite_image_layers(*ma, channel, layers)) {
      return false;
    }
    float bottom[4];
    BKE_paint_layers_channel_bottom_color(eMaterialPaintChannel(channel), bottom);
    rcti area;
    BLI_rcti_init(&area, 0, size, 0, size);
    return BKE_paint_material_composite_eval_images_linear(
        layers,
        PaintMaterialCompositeLinearTarget::packed(r_pixels, area),
        nullptr,
        nullptr,
        bottom);
  };

  const int64_t pixel_num = int64_t(size) * size;
  Vector<Vector<float>> live;
  Vector<int> measured;
  for (const int channel : channels) {
    const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
        eMaterialPaintChannel(channel));
    Vector<float> pixels(pixel_num * 4);
    if (!evaluate(channel, pixels.as_mutable_span())) {
      /* A channel with an image-paint target always resolves; one without (Custom) has no map to
       * bake yet, so it is reported and left out instead of failing the measurement. Height is
       * not measured here at all (see above). */
      ASSERT_FALSE(info.supports_image_paint) << info.ui_name;
      printf("[paint-layers gradient row bake] precision %s: no stack in this channel\n",
             info.ui_name);
      continue;
    }
    live.append(std::move(pixels));
    measured.append(channel);
  }
  ASSERT_FALSE(measured.is_empty());

  /* Six laid channels put the row's generated graph over the AUTO node budget, so the synchronous
   * plan leaves it to the worker: the bake goes through the job the way any heavy row is baked. */
  bool changed = false;
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_FALSE(changed);
  PaintLayersBakeJob *job = BKE_paint_layers_bake_job_create(*bmain, *ma);
  ASSERT_NE(job, nullptr);
  BKE_paint_layers_bake_job_compute(*job, {}, {});
  ASSERT_TRUE(BKE_paint_layers_bake_job_commit(*job));
  BKE_paint_layers_bake_job_free(*job);
  ASSERT_TRUE(paint_layer_row_substituted(*ma, *layer));
  ASSERT_EQ(layer->bake->size, size);

  for (const int index : measured.index_range()) {
    const int channel = measured[index];
    const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
        eMaterialPaintChannel(channel));
    Vector<float> baked(pixel_num * 4);
    if (!evaluate(channel, baked.as_mutable_span())) {
      ASSERT_FALSE(info.supports_image_paint)
          << info.ui_name << ": a channel with an image map must stay readable after its bake";
      continue;
    }
    const Span<float> live_pixels = live[index];
    double max_error = 0.0;
    double sum_error = 0.0;
    for (const int64_t i : IndexRange(pixel_num)) {
      for (const int k : IndexRange(3)) {
        const double error = std::abs(double(baked[i * 4 + k]) - double(live_pixels[i * 4 + k]));
        max_error = std::max(max_error, error);
        sum_error += error;
      }
    }
    /* One line of the canvas: how many different values the ramp keeps there, which is what a
     * 256-level map cannot do. */
    const auto distinct_along_line = [&](const Span<float> pixels) {
      const int64_t row = int64_t(size / 2) * size;
      Set<float> values;
      for (const int x : IndexRange(size)) {
        values.add(pixels[(row + x) * 4]);
      }
      return int64_t(values.size());
    };
    printf("[paint-layers gradient row bake] precision %s storage=%s max=%.5f mean=%.5f "
           "distinct=%lld/%lld\n",
           info.ui_name,
           info.bake_float ? "float" : "byte",
           max_error,
           sum_error / (double(pixel_num) * 3.0),
           static_cast<long long>(distinct_along_line(baked)),
           static_cast<long long>(distinct_along_line(live_pixels)));

    /* A float channel reproduces the live row; a byte one is a byte texture, and 3 levels of a
     * 255-step map is the most the sRGB round trip costs. */
    const double tolerance = info.bake_float ? 1e-4 : 3.0 / 255.0;
    EXPECT_LT(max_error, tolerance) << info.ui_name;
  }
}

/**
 * A file saved before the precise channels went float holds byte maps under the same kind of row.
 * Its hash was made by another version, so the planner re-bakes it, and the re-bake has to replace
 * the map's format instead of writing float values into the byte buffer it found.
 */
TEST_F(PaintLayersBakeTest, an_older_byte_gradient_bake_is_rebuilt_as_float)
{
  const int size = 8;
  ma->paint_layers_image_size = size;
  MaterialPaintLayer *layer = BKE_paint_layers_gradient_add(
      *ma, "Gradient", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *layer, MA_PAINT_LAYER_GRADIENT_SPACE_UV));
  const float value[4] = {0.05f, 0.95f, 0.0f, 1.0f};
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS), nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_value(
      *ma, layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS, value));
  clear_active();
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma));
  ASSERT_NE(layer->bake->images[PAINT_MATERIAL_CHANNEL_ROUGHNESS], nullptr);

  /* Rebuild the state an old file would restore: a byte map of the right size and a hash from a
   * version that knew nothing of the format. */
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *old_map = BKE_image_add_generated(bmain,
                                          size,
                                          size,
                                          "Old Roughness Bake",
                                          32,
                                          false,
                                          IMA_GENTYPE_BLANK,
                                          black,
                                          false,
                                          true,
                                          false);
  ASSERT_NE(old_map, nullptr);
  /* The bake's own map is left to the structure: only the slot is swapped for the old file's. */
  layer->bake->images[PAINT_MATERIAL_CHANNEL_ROUGHNESS] = old_map;
  /* The rebuild releases the byte map it replaces, so the old data-block is identified before the
   * plan runs: comparing the slot pointer afterwards would read a freed address the allocator is
   * free to hand out again for the new map. */
  const unsigned int old_uid = old_map->id.session_uid;
  layer->bake->hash[0] ^= 0x1u;
  ASSERT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *layer));

  bool changed = false;
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_TRUE(changed);
  Image *rebuilt = layer->bake->images[PAINT_MATERIAL_CHANNEL_ROUGHNESS];
  ASSERT_NE(rebuilt, nullptr);
  EXPECT_NE(rebuilt->id.session_uid, old_uid)
      << "a byte map cannot hold the float values the channel needs";
  const ImageTile *tile = BKE_image_get_tile(rebuilt, 0);
  ASSERT_NE(tile, nullptr);
  EXPECT_NE(tile->gen_flag & IMA_GEN_FLOAT, 0);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *layer));
}

/**
 * The float rule reads the channel, not the kind: every generated row's scalar bake carries the
 * channel's precision, the Noise's as much as the Gradient's. The schema-parameter row goes through
 * the same bake as the Gradient's, so one kind is enough to pin the rule for all of them.
 */
TEST_F(PaintLayersBakeTest, a_noise_row_scalar_bake_is_float)
{
  const int size = 16;
  ma->paint_layers_image_size = size;
  MaterialPaintLayer *layer = BKE_paint_layers_generator_add(
      *ma, MA_PAINT_LAYER_GENERATOR_NOISE, "Noise", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS), nullptr);
  clear_active();

  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma));
  ASSERT_TRUE(paint_layer_row_substituted(*ma, *layer));
  Image *baked = layer->bake->images[PAINT_MATERIAL_CHANNEL_ROUGHNESS];
  ASSERT_NE(baked, nullptr);
  const ImageTile *tile = BKE_image_get_tile(baked, 0);
  ASSERT_NE(tile, nullptr);
  EXPECT_NE(tile->gen_flag & IMA_GEN_FLOAT, 0)
      << "the noise's ramp of a factor reads as terraces in 8 bits";

  /* The baked map answers what the live row answers, at the bake's own tolerance. The row stands in
   * through its bake while it is baked, so its generator is read from the live description (mode
   * Never) and the baked pixels from the substituted one. */
  Vector<PaintMaterialCompositeImageLayer> baked_layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_ROUGHNESS, baked_layers));
  ASSERT_FALSE(baked_layers.is_empty());
  float bottom[4];
  BKE_paint_layers_channel_bottom_color(PAINT_MATERIAL_CHANNEL_ROUGHNESS, bottom);
  rcti area;
  BLI_rcti_init(&area, 0, size, 0, size);
  Vector<float> pixels(int64_t(size) * size * 4);
  ASSERT_TRUE(BKE_paint_material_composite_eval_images_linear(
      baked_layers,
      PaintMaterialCompositeLinearTarget::packed(pixels.as_mutable_span(), area),
      nullptr,
      nullptr,
      bottom));
  const int64_t centre = (int64_t(size / 2) * size + size / 2) * 4;

  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *layer, MA_PAINT_LAYER_BAKE_NEVER));
  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_ROUGHNESS, layers));
  ASSERT_FALSE(layers.is_empty());
  ASSERT_TRUE(layers[0].gradient.enabled);
  float live[4];
  ASSERT_TRUE(
      BKE_paint_material_generator_sample(layers[0].gradient, size / 2, size / 2, size, size, live));
  EXPECT_NEAR(pixels[centre], live[0], 1e-4f);
}

/**
 * The canvas is what a generated row is baked at, and it is not stored anywhere of its own: a
 * resize has to move the hash, or the row would keep reading a bake of the old size.
 */
TEST_F(PaintLayersBakeTest, resizing_the_canvas_rebuilds_the_gradient_bake)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *layer = BKE_paint_layers_gradient_add(
      *ma, "Gradient", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *layer, MA_PAINT_LAYER_GRADIENT_SPACE_UV));
  clear_active();
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma));
  ASSERT_EQ(layer->bake->size, 8);
  Image *small_map = layer->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(small_map, nullptr);

  ma->paint_layers_image_size = 16;
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *layer))
      << "the cache was rendered at the old canvas size";
  bool changed = false;
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_TRUE(changed);
  EXPECT_EQ(layer->bake->size, 16);
  int width = 0;
  int height = 0;
  Image *large_map = layer->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(large_map, nullptr);
  BKE_image_get_size(large_map, nullptr, &width, &height);
  EXPECT_EQ(width, 16);
  EXPECT_EQ(height, 16);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *layer));
}

TEST_F(PaintLayersBakeTest, a_gradient_parameter_edit_invalidates_the_row_bake)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *layer = BKE_paint_layers_gradient_add(
      *ma, "Gradient", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *layer, MA_PAINT_LAYER_GRADIENT_SPACE_UV));
  clear_active();
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma));
  ASSERT_NE(layer->bake, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *layer));

  /* Every parameter of the payload moves the pixels, so every one of them moves the hash. */
  ASSERT_TRUE(BKE_paint_layers_gradient_set_midpoint(*ma, *layer, 0.3f));
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *layer));

  bool changed = false;
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_TRUE(changed);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *layer));

  const float start[3] = {0.25f, 0.5f, 0.0f};
  const float end[3] = {1.0f, 0.5f, 0.0f};
  ASSERT_TRUE(BKE_paint_layers_gradient_set_line(*ma, *layer, start, end));
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *layer));
}

TEST_F(PaintLayersBakeTest, a_never_gradient_row_stays_live)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *layer = BKE_paint_layers_gradient_add(
      *ma, "Gradient", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *layer, MA_PAINT_LAYER_GRADIENT_SPACE_UV));
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *layer, MA_PAINT_LAYER_BAKE_NEVER));
  clear_active();

  bool changed = false;
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_FALSE(changed);
  ASSERT_NE(layer->bake, nullptr) << "NEVER is the user asking for the live row";
  EXPECT_EQ(BKE_paint_layers_bake_mode_get(*layer), MA_PAINT_LAYER_BAKE_NEVER);
  EXPECT_EQ(layer->bake->coverage, nullptr) << "NEVER writes no cache";
  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  ASSERT_EQ(layers.size(), 1);
  EXPECT_TRUE(layers[0].gradient.enabled);
}

/**
 * The material's Gradient answer gates the automatic bake of a Gradient row. Zero bits -- what a
 * file saved before the setting carries -- reads as Always, so such a file bakes as it always did;
 * Only Complex Graphs and Never keep the light row live and release a cache the earlier answer
 * left, and a change of the answer sets the cache again. A mode the user chose on the row itself
 * outranks the setting, as it always has.
 */
TEST_F(PaintLayersBakeTest, the_materials_gradient_answer_gates_a_gradient_row_bake)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *layer = BKE_paint_layers_gradient_add(
      *ma, "Gradient", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *layer, MA_PAINT_LAYER_GRADIENT_SPACE_UV));
  clear_active();
  EXPECT_EQ(ma->paint_layers_gradient_bake, MA_PAINT_LAYERS_GRADIENT_BAKE_ALWAYS)
      << "a file saved before the setting carries no answer, which reads as Always";

  bool changed = false;
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_TRUE(changed);
  ASSERT_NE(layer->bake, nullptr) << "Always, the zero an old file reads, bakes as before";
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *layer));

  /* Only Complex Graphs: the row's own subtree is light, so the cache is released and the row
   * stays live. */
  ma->paint_layers_gradient_bake = MA_PAINT_LAYERS_GRADIENT_BAKE_ONLY_COMPLEX;
  changed = false;
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed))
      << "releasing a cache the rules no longer want is a change";
  EXPECT_TRUE(changed);
  EXPECT_EQ(layer->bake, nullptr) << "a light graph stays live";
  changed = false;
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_FALSE(changed);
  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  ASSERT_EQ(layers.size(), 1);
  EXPECT_TRUE(layers[0].gradient.enabled) << "the Gradient is laid live again";

  /* Never answers the same way; there is nothing left to release. */
  ma->paint_layers_gradient_bake = MA_PAINT_LAYERS_GRADIENT_BAKE_NEVER;
  changed = false;
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_FALSE(changed);
  EXPECT_EQ(layer->bake, nullptr);

  /* A manual Always is the user's own and outranks the setting. */
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *layer, MA_PAINT_LAYER_BAKE_ALWAYS));
  changed = false;
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_TRUE(changed);
  ASSERT_NE(layer->bake, nullptr);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *layer));

  /* Back to AUTO under the same Never: the cache the manual mode left is released. */
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *layer, MA_PAINT_LAYER_BAKE_AUTO));
  changed = false;
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_TRUE(changed);
  EXPECT_EQ(layer->bake, nullptr);

  /* And the answer back to Always sets the cache again, with no manual step. */
  ma->paint_layers_gradient_bake = MA_PAINT_LAYERS_GRADIENT_BAKE_ALWAYS;
  changed = false;
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_TRUE(changed);
  ASSERT_NE(layer->bake, nullptr);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *layer));
}

/**
 * "Only Complex Graphs" is the plain AUTO weight rule applied to a row a Gradient folds into: a
 * light one stays live, a heavy one is cached the way any heavy AUTO row is, and Never releases
 * the cache whatever the row weighs.
 */
TEST_F(PaintLayersBakeTest, the_materials_gradient_answer_weights_a_gradient_folding_row)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *row = add_paint_layer("Row", add_image("Row"));
  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(
      *ma, row, "Grad", MA_PAINT_LAYER_SOURCE_GENERATOR);
  ASSERT_NE(effect, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(*ma, *effect, MA_PAINT_LAYER_GRADIENT_SPACE_UV));
  clear_active();
  ma->paint_layers_gradient_bake = MA_PAINT_LAYERS_GRADIENT_BAKE_ONLY_COMPLEX;

  /* One Gradient correction keeps the row light: the space change precreated its canvas, and
   * "Only Complex Graphs" wants the row live, so the plan's one change is releasing that canvas. */
  bool changed = false;
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_TRUE(changed);
  EXPECT_EQ(row->bake, nullptr);

  /* Four more push the row past the node count a plain AUTO row answers to: the row is the heavy
   * thing the setting asks a cache for, and the worker is what bakes it. */
  const char *names[4] = {"Grad 2", "Grad 3", "Grad 4", "Grad 5"};
  for (const int i : IndexRange(4)) {
    MaterialPaintLayer *more = BKE_paint_layers_correction_add(
        *ma, row, names[i], MA_PAINT_LAYER_SOURCE_GENERATOR);
    ASSERT_NE(more, nullptr);
    ASSERT_TRUE(
        BKE_paint_layers_gradient_set_space(*ma, *more, MA_PAINT_LAYER_GRADIENT_SPACE_UV));
  }
  EXPECT_TRUE(BKE_paint_layers_bake_is_heavy(*ma, *row));
  EXPECT_TRUE(BKE_paint_layers_bake_heavy_pending(*ma));

  /* The synchronous planner leaves a heavy row to the worker. The fifth correction's space change
   * made the row a legitimate bake candidate, which precreated its canvas; nothing is baked into
   * it before the job runs. */
  changed = false;
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  ASSERT_NE(row->bake, nullptr);
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *row))
      << "the synchronous planner leaves the heavy row to the worker";

  PaintLayersBakeJob *job = BKE_paint_layers_bake_job_create(*bmain, *ma);
  ASSERT_NE(job, nullptr);
  BKE_paint_layers_bake_job_compute(*job, {}, {});
  EXPECT_TRUE(BKE_paint_layers_bake_job_commit(*job));
  BKE_paint_layers_bake_job_free(*job);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *row));
  EXPECT_TRUE(paint_layer_row_substituted(*ma, *row));
  EXPECT_FALSE(BKE_paint_layers_bake_heavy_pending(*ma));

  /* Never forbids the automatic bake whatever the row weighs: the cache is released, and nothing
   * is queued again. */
  ma->paint_layers_gradient_bake = MA_PAINT_LAYERS_GRADIENT_BAKE_NEVER;
  changed = false;
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_TRUE(changed);
  EXPECT_EQ(row->bake, nullptr);
  EXPECT_FALSE(paint_layer_row_substituted(*ma, *row));
  EXPECT_FALSE(BKE_paint_layers_bake_heavy_pending(*ma));
}

TEST_F(PaintLayersBakeTest, switching_a_uv_gradient_to_object_keeps_its_maps_without_a_view)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *layer = BKE_paint_layers_gradient_add(
      *ma, "Gradient", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *layer, MA_PAINT_LAYER_GRADIENT_SPACE_UV));
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *layer, MA_PAINT_LAYER_BAKE_ALWAYS));
  clear_active();
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma));
  ASSERT_NE(layer->bake, nullptr);

  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *layer, MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT));
  bool changed = false;
  /* Without a view layer there is no object to judge the 3D row by: the old maps stay, but they
   * are not substituted. */
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_FALSE(changed);
  ASSERT_NE(layer->bake, nullptr) << "the record keeps the mode the user chose";
  EXPECT_NE(layer->bake->coverage, nullptr) << "a contextless plan drops nothing";
  EXPECT_EQ(BKE_paint_layers_bake_mode_get(*layer), MA_PAINT_LAYER_BAKE_ALWAYS);
  EXPECT_FALSE(paint_layer_row_substituted(*ma, *layer))
      << "a shared UV bake cannot stand in for a 3D row";

  changed = false;
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_FALSE(changed);
  EXPECT_NE(layer->bake->coverage, nullptr);
}

TEST_F(PaintLayersBakeTest, an_object_or_generated_gradient_is_never_shared_baked)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *object = BKE_paint_layers_gradient_add(
      *ma, "Object", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *generated = BKE_paint_layers_gradient_add(
      *ma, "Generated", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(object, nullptr);
  ASSERT_NE(generated, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *object, MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT));
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *generated, MA_PAINT_LAYER_GRADIENT_SPACE_GENERATED));

  for (MaterialPaintLayer *layer : {object, generated}) {
    EXPECT_FALSE(BKE_paint_layers_generator_is_shared_bakeable(*layer));
    EXPECT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *layer, MA_PAINT_LAYER_BAKE_ALWAYS));
  }
  clear_active();

  bool changed = false;
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_FALSE(changed);
  for (MaterialPaintLayer *layer : {object, generated}) {
    ASSERT_NE(layer->bake, nullptr) << "the row keeps its mode record";
    /* The per-object canvas is precreated as soon as the space is chosen: one tiled image whose
     * first tile the bake fills. No shared map is ever made for a 3D gradient, which is what the
     * refused render below still pins. */
    EXPECT_NE(layer->bake->coverage, nullptr);
    EXPECT_EQ(layer->bake->coverage->source, IMA_SRC_TILED);
    EXPECT_NE(BKE_image_get_tile(layer->bake->coverage, 1001), nullptr);
    EXPECT_EQ(BKE_paint_layers_bake_mode_get(*layer), MA_PAINT_LAYER_BAKE_ALWAYS);
    Vector<float> color(8 * 8 * 4);
    Vector<float> coverage(8 * 8);
    EXPECT_FALSE(BKE_paint_layers_bake_render_node(*ma,
                                                   *layer,
                                                   PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                                   8,
                                                   color.data(),
                                                   coverage.data()))
        << "no shared render is made for a 3D gradient";
  }
}

TEST_F(PaintLayersBakeTest, a_folder_with_a_3d_gradient_is_not_shared_baked)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *gradient = BKE_paint_layers_gradient_add(
      *ma, "Gradient", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(gradient, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *gradient, MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT));

  MaterialPaintLayer *members[] = {gradient};
  MaterialPaintLayer *folder = BKE_paint_layers_group(*ma, Span<MaterialPaintLayer *>(members, 1));
  ASSERT_NE(folder, nullptr);
  /* Isolating, so the folder would normally be a bake candidate. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.9f));
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *folder, MA_PAINT_LAYER_BAKE_ALWAYS));
  clear_active();

  bool changed = false;
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_FALSE(changed);
  ASSERT_NE(folder->bake, nullptr) << "an object-specific child keeps the whole folder live";
  EXPECT_EQ(folder->bake->coverage, nullptr);
  EXPECT_EQ(BKE_paint_layers_bake_mode_get(*folder), MA_PAINT_LAYER_BAKE_ALWAYS);
  /* The 3D gradient row precreates its own per-object canvas; it is the folder's record that
   * keeps no maps, so the folder itself never gets a shared bake. */
  ASSERT_NE(gradient->bake, nullptr);
  EXPECT_NE(gradient->bake->coverage, nullptr);
  EXPECT_EQ(gradient->bake->coverage->source, IMA_SRC_TILED);

  Vector<float> color(8 * 8 * 4);
  Vector<float> coverage(8 * 8);
  EXPECT_FALSE(BKE_paint_layers_bake_render_node(*ma,
                                                 *folder,
                                                 PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                                 8,
                                                 color.data(),
                                                 coverage.data()))
      << "a folder holding a 3D gradient has no shared render";

  /* Control: the same folder shape without a 3D gradient still renders into a shared map. */
  MaterialPaintLayer *plain = add_paint_layer("Plain", add_image("Plain"));
  MaterialPaintLayer *plain_members[] = {plain};
  MaterialPaintLayer *plain_folder = BKE_paint_layers_group(
      *ma, Span<MaterialPaintLayer *>(plain_members, 1));
  ASSERT_NE(plain_folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, plain_folder, 0.9f));
  EXPECT_TRUE(BKE_paint_layers_bake_render_node(*ma,
                                                *plain_folder,
                                                PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                                8,
                                                color.data(),
                                                coverage.data()));
}

TEST_F(PaintLayersBakeTest, the_planner_bakes_a_requested_row_and_it_stands_in)
{
  MaterialPaintLayer *layer = add_baked_row("Row");
  clear_active();
  EXPECT_FALSE(paint_layer_row_substituted(*ma, *layer));

  bool changed = false;
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_TRUE(changed);

  ASSERT_NE(layer->bake, nullptr);
  EXPECT_EQ(layer->bake->size, 8) << "the size follows the row's map";
  EXPECT_NE(layer->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR], nullptr);
  EXPECT_NE(layer->bake->coverage, nullptr);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *layer));
  EXPECT_TRUE(paint_layer_row_substituted(*ma, *layer));
  Image *baked = nullptr;
  EXPECT_TRUE(paint_layer_row_baked_channel(
      *ma, *layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &baked));
  EXPECT_EQ(baked, layer->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR]);
  EXPECT_FALSE(paint_layer_row_baked_channel(*ma, *layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS, nullptr))
      << "a channel the row has no map in has no baked map either";

  /* A second pass has nothing left to do. */
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_FALSE(changed);
}

TEST_F(PaintLayersBakeTest, the_row_being_edited_stays_live)
{
  MaterialPaintLayer *layer = add_baked_row("Row");
  BKE_paint_layers_active_set(*ma, layer->marker);
  EXPECT_TRUE(BKE_paint_layers_bake_row_is_deferred(*ma, *layer));
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma));
  EXPECT_EQ(layer->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR], nullptr);

  clear_active();
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma));
  EXPECT_TRUE(paint_layer_row_substituted(*ma, *layer));
}

TEST_F(PaintLayersBakeTest, a_hidden_row_and_a_never_row_are_not_baked)
{
  MaterialPaintLayer *hidden = add_baked_row("Hidden");
  MaterialPaintLayer *never = add_baked_row("Never");
  EXPECT_TRUE(BKE_paint_layers_set_enabled(*ma, hidden, false));
  EXPECT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *never, MA_PAINT_LAYER_BAKE_NEVER));
  clear_active();

  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma));
  EXPECT_FALSE(paint_layer_row_substituted(*ma, *hidden));
  EXPECT_FALSE(paint_layer_row_substituted(*ma, *never));
}

TEST_F(PaintLayersBakeTest, a_value_edit_makes_the_row_live_until_it_is_baked_again)
{
  MaterialPaintLayer *layer = add_baked_row("Row");
  clear_active();
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma));
  ASSERT_TRUE(paint_layer_row_substituted(*ma, *layer));
  Image *first_map = layer->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];

  EXPECT_TRUE(BKE_paint_layers_set_opacity(*ma, layer, 0.25f));
  EXPECT_FALSE(paint_layer_row_substituted(*ma, *layer));

  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma));
  EXPECT_TRUE(paint_layer_row_substituted(*ma, *layer));
  EXPECT_EQ(layer->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR], first_map)
      << "a re-bake fills the row's maps in place";
}

TEST_F(PaintLayersBakeTest, a_pixel_edit_of_a_source_map_makes_the_row_live)
{
  Image *source = add_image("Row");
  MaterialPaintLayer *layer = add_paint_layer("Row", source);
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *layer, MA_PAINT_LAYER_BAKE_ALWAYS));
  clear_active();
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma));
  ASSERT_TRUE(paint_layer_row_substituted(*ma, *layer));
  ma->paint_layers_flag &= ~MA_PAINT_LAYERS_REGEN;

  BKE_image_partial_update_mark_full_update(source);
  BKE_paint_layers_bake_notice_changes(*ma);

  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *layer));
  EXPECT_FALSE(paint_layer_row_substituted(*ma, *layer));
  EXPECT_TRUE(BKE_paint_layers_bake_stale_get(*ma));
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0)
      << "the generated graph was built from the bake and has to be rebuilt";
}

TEST_F(PaintLayersBakeTest, the_cpu_composite_reads_the_bake_as_one_plain_layer)
{
  MaterialPaintLayer *layer = add_baked_row("Row");
  ASSERT_NE(BKE_paint_layers_correction_add(*ma, layer, "Fix", MA_PAINT_LAYER_SOURCE_CONSTANT),
            nullptr);
  clear_active();

  Vector<PaintMaterialCompositeImageLayer> live;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, live));
  ASSERT_EQ(live.size(), 1);
  EXPECT_NE(live[0].color_image, nullptr);
  EXPECT_EQ(live[0].content_corrections.size(), 1);

  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma));
  Vector<PaintMaterialCompositeImageLayer> baked;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, baked));
  ASSERT_EQ(baked.size(), 1);
  EXPECT_EQ(baked[0].color_image, layer->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR]);
  EXPECT_EQ(baked[0].mask_image, layer->bake->coverage);
  /* The bake already holds the corrections and the mask, so nothing is laid twice. */
  EXPECT_TRUE(baked[0].content_corrections.is_empty());
  EXPECT_TRUE(baked[0].mask_items.is_empty());
}

TEST_F(PaintLayersBakeTest, a_baked_gradient_covers_by_the_alpha_of_its_colour_map)
{
  /* A generator row has no map to take its bake size from, so the canvas size has to be set. */
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *gradient = BKE_paint_layers_gradient_add(
      *ma, "Gradient", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(gradient, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *gradient, MA_PAINT_LAYER_GRADIENT_SPACE_UV));
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *gradient, MA_PAINT_LAYER_BAKE_ALWAYS));
  clear_active();

  /* The bake folds the ramp alpha out of the stored coverage, so the reader has to multiply the
   * colour map's alpha back, or the gradient lays opaque. */
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma));
  ASSERT_TRUE(paint_layer_row_substituted(*ma, *gradient));
  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(
      BKE_paint_layers_composite_image_layers(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  ASSERT_EQ(layers.size(), 1);
  EXPECT_EQ(layers[0].color_image, gradient->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR]);
  EXPECT_TRUE(layers[0].color_alpha_coverage);
}

TEST_F(PaintLayersBakeTest, the_generator_reads_the_bake_instead_of_the_row)
{
  Image *source = add_image("Row");
  MaterialPaintLayer *layer = add_paint_layer("Row", source);
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *layer, MA_PAINT_LAYER_BAKE_ALWAYS));
  clear_active();

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(ma->paint_layers_tree, nullptr);
  EXPECT_EQ(image_reader_count(*ma->paint_layers_tree, source), 1);

  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  Image *color_map = layer->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(color_map, nullptr);
  EXPECT_EQ(image_reader_count(*ma->paint_layers_tree, source), 0)
      << "a baked row no longer reads its own map";
  EXPECT_EQ(image_reader_count(*ma->paint_layers_tree, color_map), 1);
  EXPECT_EQ(image_reader_count(*ma->paint_layers_tree, layer->bake->coverage), 1);

  /* A value edit of the baked row is topology: the row is live again and reads its own map. */
  EXPECT_TRUE(BKE_paint_layers_set_opacity(*ma, layer, 0.5f));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(image_reader_count(*ma->paint_layers_tree, source), 1);
  EXPECT_EQ(image_reader_count(*ma->paint_layers_tree, color_map), 0);
}

/**
 * A row carrying a CPU-computed effect is built from its bake, and that bake holds the content alone:
 * the group samples the baked map, keeps reading the row's mask map (the mask is laid live over the
 * content) and has no input left for the row's own source or for a correction the bake folded in.
 * An opacity edit is then a value of that shape -- the row stays baked and keeps its mask nodes --
 * where the same edit of a row baked whole is a different shape.
 */
TEST_F(PaintLayersBakeTest, a_content_bake_row_keeps_its_mask_inputs_in_the_graph)
{
  Image *source = add_image("Row");
  MaterialPaintLayer *layer = add_paint_layer("Row", source);
  ASSERT_NE(BKE_paint_layers_filter_add(*ma, layer, MA_PAINT_LAYER_FILTER_NOISE), nullptr);
  MaterialPaintLayer *exposure = BKE_paint_layers_filter_add(*ma,
                                                             layer,
                                                             MA_PAINT_LAYER_FILTER_EXPOSURE);
  ASSERT_NE(exposure, nullptr);
  Image *mask_map = add_image("Mask");
  ASSERT_NE(BKE_paint_layers_mask_add_image(*ma, *layer, *mask_map), nullptr);
  clear_active();

  /* The name a correction's inputs are given, and the count their absence is read from. */
  char exposure_label[128];
  SNPRINTF(exposure_label, "Row %s ", exposure->name);
  const auto exposure_inputs = [](bNodeTree &tree, const char *prefix) {
    tree.ensure_interface_cache();
    int count = 0;
    for (const bNodeTreeInterfaceSocket *socket : tree.interface_inputs()) {
      if (socket->name != nullptr && STRPREFIX(socket->name, prefix)) {
        count++;
      }
    }
    return count;
  };

  /* Live: the row reads its own map and its mask, and the exposure it lays is a set of inputs. */
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(ma->paint_layers_tree, nullptr);
  bNodeTree *group = layer_tree_find(*bmain, "Row");
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(image_reader_count(*ma->paint_layers_tree, source), 1);
  EXPECT_EQ(image_reader_count(*ma->paint_layers_tree, mask_map), 1);
  EXPECT_GT(exposure_inputs(*group, exposure_label), 0)
      << "a live correction carries its opacity and its parameters as inputs";

  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma));
  ASSERT_TRUE(paint_layer_row_substituted(*ma, *layer));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  Image *color_map = layer->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(color_map, nullptr);
  group = layer_tree_find(*bmain, "Row");
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(image_reader_count(*ma->paint_layers_tree, source), 0)
      << "the content comes from the cache, not from the row's map";
  EXPECT_EQ(image_reader_count(*ma->paint_layers_tree, color_map), 1);
  EXPECT_EQ(image_reader_count(*ma->paint_layers_tree, mask_map), 1)
      << "the mask stays live over a bake of the content";
  /* The row's own colour values are gone with its source; the mask's are not. */
  EXPECT_EQ(exposure_inputs(*group, exposure_label), 0)
      << "a correction folded into the bake has no input to change it";
  EXPECT_NE(interface_input_find(*group, "Row Mask Opacity"), nullptr);
  EXPECT_NE(interface_input_find(*group, "Row Base Color Opacity"), nullptr);

  /* The opacity of a content bake is an input of the shape the group has now: a value, not a new
   * shape -- unlike the same edit of a row whose bake holds the mask and the opacity as well. */
  EXPECT_TRUE(BKE_paint_layers_set_opacity(*ma, layer, 0.5f));
  EXPECT_TRUE(BKE_paint_layers_values_sync(*ma)) << "the mask and the map are read as they were";
  EXPECT_TRUE(paint_layer_row_substituted(*ma, *layer));
}

TEST_F(PaintLayersBakeTest, a_value_sync_of_a_stale_baked_shape_asks_for_a_rebuild)
{
  MaterialPaintLayer *layer = add_baked_row("Row");
  clear_active();
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_TRUE(BKE_paint_layers_values_sync(*ma));

  /* The group was built from the bake; once the row is live the sync cannot make it right. */
  EXPECT_TRUE(BKE_paint_layers_set_opacity(*ma, layer, 0.5f));
  EXPECT_FALSE(BKE_paint_layers_values_sync(*ma));
}

TEST_F(PaintLayersBakeTest, a_nested_folder_is_baked_by_the_worker_job)
{
  MaterialPaintLayer *inner = add_nested_folder();
  ASSERT_NE(inner, nullptr);
  clear_active();

  MaterialPaintLayer *outer = BKE_paint_layers_parent(*ma, *inner);
  ASSERT_NE(outer, nullptr);
  EXPECT_FALSE(BKE_paint_layers_folder_auto_bake_allowed(*ma, *outer))
      << "a folder in the stack root stays live";
  ASSERT_TRUE(BKE_paint_layers_folder_auto_bake_allowed(*ma, *inner));
  EXPECT_TRUE(BKE_paint_layers_bake_is_heavy(*ma, *inner));
  EXPECT_TRUE(BKE_paint_layers_bake_heavy_pending(*ma));

  /* The synchronous planner leaves a heavy row to the worker. */
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma));
  EXPECT_FALSE(paint_layer_row_substituted(*ma, *inner));

  PaintLayersBakeJob *job = BKE_paint_layers_bake_job_create(*bmain, *ma);
  ASSERT_NE(job, nullptr);
  float last_progress = -1.0f;
  BKE_paint_layers_bake_job_compute(*job, [&](const float progress) { last_progress = progress; }, {});
  EXPECT_FLOAT_EQ(last_progress, 1.0f);
  EXPECT_TRUE(BKE_paint_layers_bake_job_commit(*job));
  BKE_paint_layers_bake_job_free(*job);

  EXPECT_TRUE(paint_layer_row_substituted(*ma, *inner));
  EXPECT_FALSE(BKE_paint_layers_bake_heavy_pending(*ma));

  /* The outer folder is Pass Through, so its rows are laid in the stack as if it were not there:
   * the baked folder is one plain layer for the CPU, its children no longer composited. */
  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  ASSERT_EQ(layers.size(), 2);
  EXPECT_FALSE(layers[0].is_folder);
  EXPECT_TRUE(layers[0].children.empty());
  EXPECT_EQ(layers[0].color_image, inner->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR]);
  EXPECT_EQ(layers[0].mask_image, inner->bake->coverage);
}

TEST_F(PaintLayersBakeTest, a_job_result_of_a_row_edited_meanwhile_is_not_taken_for_current)
{
  MaterialPaintLayer *first = nullptr;
  MaterialPaintLayer *inner = add_nested_folder(&first);
  ASSERT_NE(inner, nullptr);
  clear_active();

  PaintLayersBakeJob *job = BKE_paint_layers_bake_job_create(*bmain, *ma);
  ASSERT_NE(job, nullptr);
  BKE_paint_layers_bake_job_compute(*job, {}, {});
  /* An edit lands while the worker has run on the old state. */
  EXPECT_TRUE(BKE_paint_layers_set_opacity(*ma, first, 0.25f));
  BKE_paint_layers_bake_job_commit(*job);
  BKE_paint_layers_bake_job_free(*job);

  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *inner))
      << "the stamped hash is the one the render was made from";
  EXPECT_FALSE(paint_layer_row_substituted(*ma, *inner));
  EXPECT_TRUE(BKE_paint_layers_bake_heavy_pending(*ma)) << "the planner queues it anew";
}

TEST_F(PaintLayersBakeTest, a_copied_material_starts_without_a_cache_of_its_own)
{
  MaterialPaintLayer *layer = add_baked_row("Row");
  clear_active();
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma));
  ASSERT_TRUE(paint_layer_row_substituted(*ma, *layer));

  Material *copy = id_cast<Material *>(BKE_id_copy(bmain, &ma->id));
  ASSERT_NE(copy, nullptr);
  const MaterialPaintLayer *copied = static_cast<const MaterialPaintLayer *>(
      copy->paint_layers.first);
  ASSERT_NE(copied, nullptr);
  ASSERT_NE(copied->bake, nullptr);
  EXPECT_EQ(copied->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR], nullptr)
      << "re-baking the copy must not write into the original's maps";
  EXPECT_EQ(copied->bake->coverage, nullptr);
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*copy, *copied));
  EXPECT_TRUE(paint_layer_row_substituted(*ma, *layer)) << "the original keeps its bake";
}

TEST_F(PaintLayersBakeTest, a_removed_row_lets_go_of_its_bake)
{
  MaterialPaintLayer *layer = add_baked_row("Row");
  clear_active();
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma));
  Image *map = layer->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  Image *coverage = layer->bake->coverage;
  const int map_users = map->id.us;
  const int coverage_users = coverage->id.us;

  EXPECT_TRUE(BKE_paint_layers_remove(*ma, layer));
  EXPECT_EQ(map->id.us, map_users - 1);
  EXPECT_EQ(coverage->id.us, coverage_users - 1);
}

/**
 * "Use Row Result" renders a source row's channels off the main thread and commits them as the
 * target row's maps; the BKE job split is what the heavy operator path runs.
 */
TEST_F(PaintLayersBakeTest, a_row_result_job_gives_the_target_the_sources_channels)
{
  const int size = 4;
  MaterialPaintLayer *source = add_paint_layer("Source", add_image("SourceImg"));
  MaterialPaintLayer *target = BKE_paint_layers_add(
      *ma, "Target", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(target, nullptr);

  PaintLayersRowResultJob *job = BKE_paint_layers_row_result_job_create(
      *bmain, *ma, source->marker, target->marker, size);
  ASSERT_NE(job, nullptr);
  BKE_paint_layers_row_result_job_compute(*job);
  ASSERT_TRUE(BKE_paint_layers_row_result_job_commit(*job));
  BKE_paint_layers_row_result_job_free(*job);

  const MaterialPaintLayerChannel *record = nullptr;
  for (int i = 0; i < target->channels_num; i++) {
    if (target->channels[i].channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
      record = &target->channels[i];
    }
  }
  ASSERT_NE(record, nullptr);
  ASSERT_NE(record->image, nullptr);
  EXPECT_NE(record->image, source->channels[0].image) << "a render, not the source's own map";
  int width = 0;
  int height = 0;
  BKE_image_get_size(record->image, nullptr, &width, &height);
  EXPECT_EQ(width, size);
  EXPECT_EQ(height, size);
  EXPECT_EQ(source->channels_num, 1) << "the source is only read";
}

/**
 * A channel the GPU rendered is handed to the job and the compute step leaves it alone: the target
 * map carries the handed-in pixels, not the source row's own render.
 */
TEST_F(PaintLayersBakeTest, a_row_result_job_keeps_a_channel_handed_in_by_the_gpu)
{
  const int size = 4;
  MaterialPaintLayer *source = add_paint_layer("Source", add_image("SourceImg"));
  MaterialPaintLayer *target = BKE_paint_layers_add(
      *ma, "Target", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(target, nullptr);

  PaintLayersRowResultJob *job = BKE_paint_layers_row_result_job_create(
      *bmain, *ma, source->marker, target->marker, size);
  ASSERT_NE(job, nullptr);
  /* White with a full alpha: the same bytes whatever the colour space writes them in. */
  const Vector<float> white(int64_t(size) * size * 4, 1.0f);
  ASSERT_TRUE(BKE_paint_layers_row_result_job_channel_set(
      *job, PAINT_MATERIAL_CHANNEL_BASE_COLOR, white.data()));
  BKE_paint_layers_row_result_job_compute(*job);
  ASSERT_TRUE(BKE_paint_layers_row_result_job_commit(*job));
  BKE_paint_layers_row_result_job_free(*job);

  const MaterialPaintLayerChannel *color_record = nullptr;
  for (int i = 0; i < target->channels_num; i++) {
    if (target->channels[i].channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
      color_record = &target->channels[i];
    }
  }
  ASSERT_NE(color_record, nullptr);
  ASSERT_NE(color_record->image, nullptr);

  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(color_record->image, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  ASSERT_NE(ibuf->byte_data(), nullptr);
  for (const int64_t i : IndexRange(int64_t(size) * size * 4)) {
    EXPECT_EQ(int(ibuf->byte_data()[i]), 255) << "byte " << i;
  }
  BKE_image_release_ibuf(color_record->image, ibuf, lock);
}

/**
 * The whole row: a row carrying a CPU-computed effect renders as its content alone by default (its
 * mask and opacity are laid live over the map), while the row result asks for the finished picture.
 * A row with no such effect renders the same either way.
 */
TEST_F(PaintLayersBakeTest, whole_row_folds_the_opacity_of_a_cpu_effect_row_into_the_render)
{
  const int size = 8;
  ma->paint_layers_image_size = size;
  MaterialPaintLayer *plain = add_paint_layer("Plain", add_image("PlainImg"));
  MaterialPaintLayer *noisy = add_paint_layer("Noisy", add_image("NoisyImg"));
  ASSERT_NE(BKE_paint_layers_filter_add(*ma, noisy, MA_PAINT_LAYER_FILTER_NOISE), nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, plain, 0.5f));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, noisy, 0.5f));
  clear_active();

  struct Render {
    Vector<float> color = Vector<float>(8 * 8 * 4);
    Vector<float> coverage = Vector<float>(8 * 8);
  };
  const auto render = [&](const MaterialPaintLayer &row, const bool whole_row) {
    Render out;
    EXPECT_TRUE(BKE_paint_layers_bake_render_node(*ma,
                                                  row,
                                                  PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                                  size,
                                                  out.color.data(),
                                                  out.coverage.data(),
                                                  nullptr,
                                                  nullptr,
                                                  {},
                                                  whole_row));
    return out;
  };
  const auto coverage_sum = [](const Render &r) {
    double sum = 0.0;
    for (const float value : r.coverage) {
      sum += value;
    }
    return sum;
  };

  const Render plain_content = render(*plain, false);
  const Render plain_whole = render(*plain, true);
  EXPECT_EQ(plain_content.coverage, plain_whole.coverage)
      << "a row with no CPU effect is rendered whole either way";
  EXPECT_EQ(plain_content.color, plain_whole.color);

  const Render noisy_content = render(*noisy, false);
  const Render noisy_whole = render(*noisy, true);
  EXPECT_TRUE(noisy_content.coverage != noisy_whole.coverage ||
              noisy_content.color != noisy_whole.color)
      << "the whole row differs from the content alone";
  EXPECT_LE(coverage_sum(noisy_whole), coverage_sum(noisy_content) + 1e-4)
      << "the opacity is folded in, never added";
}

TEST_F(PaintLayersBakeTest, a_row_result_job_needs_both_rows_and_a_target_that_still_exists)
{
  MaterialPaintLayer *source = add_paint_layer("Source", add_image("SourceImg"));
  MaterialPaintLayer *target = BKE_paint_layers_add(
      *ma, "Target", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(target, nullptr);

  EXPECT_EQ(BKE_paint_layers_row_result_job_create(*bmain, *ma, source->marker, BLI_uuid_nil(), 4),
            nullptr);
  EXPECT_EQ(BKE_paint_layers_row_result_job_create(*bmain, *ma, BLI_uuid_nil(), target->marker, 4),
            nullptr);
  EXPECT_EQ(BKE_paint_layers_row_result_job_create(*bmain, *ma, source->marker, target->marker, 0),
            nullptr);

  /* The target is removed while the worker renders: the maps have nowhere to go. */
  PaintLayersRowResultJob *job = BKE_paint_layers_row_result_job_create(
      *bmain, *ma, source->marker, target->marker, 4);
  ASSERT_NE(job, nullptr);
  const bUUID target_marker = target->marker;
  BKE_paint_layers_row_result_job_compute(*job);
  ASSERT_TRUE(BKE_paint_layers_remove(*ma, target));
  EXPECT_FALSE(BKE_paint_layers_row_result_job_commit(*job));
  EXPECT_EQ(BKE_paint_layers_find(*ma, target_marker), nullptr);
  BKE_paint_layers_row_result_job_free(*job);
}

TEST_F(PaintLayersBakeTest, a_gradient_effect_invalidates_the_parent_bake_and_settles)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *row = add_baked_row("Row");
  clear_active();
  bool changed = false;
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  ASSERT_NE(row->bake, nullptr);

  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(
      *ma, row, "Grad", MA_PAINT_LAYER_SOURCE_GENERATOR);
  ASSERT_NE(effect, nullptr);
  /* A new gradient opens in object space; this test is about the parent's pixels, so the
   * correction starts UV and the row keeps baking. */
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(*ma, *effect, MA_PAINT_LAYER_GRADIENT_SPACE_UV));
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *row))
      << "a gradient effect is part of the parent's pixels";

  /* The re-bake settles: the following plans have nothing to do, so a value edit cannot loop. */
  changed = false;
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_TRUE(changed);
  changed = false;
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_FALSE(changed);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *row));

  const float start[3] = {0.0f, 0.0f, 0.0f};
  const float end[3] = {1.0f, 1.0f, 0.0f};
  EXPECT_TRUE(BKE_paint_layers_gradient_set_line(*ma, *effect, start, end));
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *row))
      << "moving the effect's line moves the parent's pixels";
  changed = false;
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  changed = false;
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_FALSE(changed) << "no infinite bake: the re-baked hash matches the description";
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *row));

  /* The midpoint is folded into the ramp position, so it moves the bake exactly once. */
  EXPECT_TRUE(BKE_paint_layers_gradient_set_midpoint(*ma, *effect, 0.75f));
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *row));
  changed = false;
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  changed = false;
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_FALSE(changed);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *row));
}

TEST_F(PaintLayersBakeTest, a_mask_gradient_item_invalidates_the_parent_bake_and_settles)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *row = add_baked_row("Row");
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, row, 1.0f), nullptr);
  clear_active();
  bool changed = false;
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  ASSERT_NE(row->bake, nullptr);

  MaterialPaintLayer *item = BKE_paint_layers_mask_item_add(
      *ma, row, MA_PAINT_LAYER_SOURCE_GENERATOR, "Mask Grad", 1.0f);
  ASSERT_NE(item, nullptr);
  /* A new gradient opens in object space; this test is about the parent's pixels, so the item
   * starts UV and the row keeps baking. */
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(*ma, *item, MA_PAINT_LAYER_GRADIENT_SPACE_UV));
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *row))
      << "a gradient mask item is part of the parent's pixels";

  changed = false;
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_TRUE(changed);
  changed = false;
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_FALSE(changed);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *row));

  /* A 3D mask item has no shared pixels: the parent stays live. Without a view layer there is
   * nothing to judge the objects by, so the plan drops nothing yet; the record keeps the ALWAYS
   * mode, and the maps the shared bake left are kept but no longer stand in. */
  EXPECT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *item, MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT));
  changed = false;
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_FALSE(changed);
  ASSERT_NE(row->bake, nullptr) << "the record keeps the ALWAYS mode";
  EXPECT_NE(row->bake->coverage, nullptr) << "a contextless plan drops nothing";
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *row));
  EXPECT_FALSE(paint_layer_row_substituted(*ma, *row));
  EXPECT_EQ(BKE_paint_layers_bake_mode_get(*row), MA_PAINT_LAYER_BAKE_ALWAYS);

  /* With a view layer the row's object has no UV map, the permanent reason: the stale shared bake
   * is released, and the record keeps the ALWAYS mode and its size. */
  Scene *scene = add_scene_with_uvless_object();
  ViewLayer *view_layer = static_cast<ViewLayer *>(scene->view_layers.first);
  changed = false;
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed, scene, view_layer));
  EXPECT_TRUE(changed);
  ASSERT_NE(row->bake, nullptr) << "the record keeps the ALWAYS mode";
  EXPECT_EQ(row->bake->coverage, nullptr) << "a stale shared bake is not kept";
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *row));
  EXPECT_FALSE(paint_layer_row_substituted(*ma, *row));
  EXPECT_EQ(BKE_paint_layers_bake_mode_get(*row), MA_PAINT_LAYER_BAKE_ALWAYS);

  /* The settled row has nothing left for the plan to do. */
  changed = false;
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed, scene, view_layer));
  EXPECT_FALSE(changed);
}

TEST_F(PaintLayersBakeTest, an_auto_row_under_a_gradient_correction_bakes_when_left)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *row = add_paint_layer("Row", add_image("Row"));
  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(
      *ma, row, "Grad", MA_PAINT_LAYER_SOURCE_GENERATOR);
  ASSERT_NE(effect, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(*ma, *effect, MA_PAINT_LAYER_GRADIENT_SPACE_UV));

  /* Working on the correction keeps its row live: the bake would fold the Gradient being edited.
   * The row still carries the canvas the space change precreated, but the plan bakes nothing into
   * it while the row is edited. */
  BKE_paint_layers_active_set(*ma, effect->marker);
  EXPECT_TRUE(BKE_paint_layers_bake_row_is_deferred(*ma, *row));
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma));
  ASSERT_NE(row->bake, nullptr) << "the precreated canvas stays";
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *row))
      << "the row is not baked while it is being edited";

  /* Leaving it is when the row's bake becomes due, AUTO or not: the Gradient folds into the row's
   * cache and stops being evaluated per texel for everything laid over the stack. */
  clear_active();
  bool changed = false;
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_TRUE(changed);
  ASSERT_NE(row->bake, nullptr);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *row));
  EXPECT_TRUE(paint_layer_row_substituted(*ma, *row));

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  ASSERT_EQ(layers.size(), 1);
  ASSERT_EQ(layers[0].content_corrections.size(), 0)
      << "a substituted row lays no Gradient correction of its own";
  EXPECT_NE(layers[0].color_image, nullptr);

  changed = false;
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_FALSE(changed);
}

TEST_F(PaintLayersBakeTest, an_auto_row_with_a_gradient_mask_item_bakes_when_left)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *row = add_paint_layer("Row", add_image("Row"));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, row, 1.0f), nullptr);
  MaterialPaintLayer *item = BKE_paint_layers_mask_item_add(
      *ma, row, MA_PAINT_LAYER_SOURCE_GENERATOR, "Mask Grad", 1.0f);
  ASSERT_NE(item, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(*ma, *item, MA_PAINT_LAYER_GRADIENT_SPACE_UV));

  BKE_paint_layers_active_set(*ma, item->marker);
  EXPECT_TRUE(BKE_paint_layers_bake_row_is_deferred(*ma, *row));
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma));

  clear_active();
  bool changed = false;
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_TRUE(changed);
  EXPECT_TRUE(paint_layer_row_substituted(*ma, *row));
  changed = false;
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_FALSE(changed);
}

TEST_F(PaintLayersBakeTest, a_row_under_a_gradient_correction_bakes_on_the_worker)
{
  ma->paint_layers_image_size = 2048;
  MaterialPaintLayer *row = add_paint_layer("Row", add_image("Row"));
  EXPECT_FALSE(BKE_paint_layers_bake_is_heavy(*ma, *row))
      << "a light painted row bakes inline, Gradient or not";

  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(
      *ma, row, "Grad", MA_PAINT_LAYER_SOURCE_GENERATOR);
  ASSERT_NE(effect, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(*ma, *effect, MA_PAINT_LAYER_GRADIENT_SPACE_UV));
  EXPECT_TRUE(BKE_paint_layers_generator_subtree_is_shared_bakeable(*row));
  EXPECT_TRUE(BKE_paint_layers_bake_is_heavy(*ma, *row))
      << "a bake that folds a Gradient in costs texels, not nodes";
}

/**
 * A Gradient child row caches itself, so the folder around it is not a per-texel row: the child's
 * cache already stands in for it in the folder's own bake.
 */
TEST_F(PaintLayersBakeTest, a_folder_around_a_gradient_row_is_not_made_heavy_by_it)
{
  ma->paint_layers_image_size = 2048;
  MaterialPaintLayer *gradient = BKE_paint_layers_gradient_add(
      *ma, "Gradient", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(gradient, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *gradient, MA_PAINT_LAYER_GRADIENT_SPACE_UV));
  MaterialPaintLayer *members[] = {gradient};
  MaterialPaintLayer *folder = BKE_paint_layers_group(*ma, Span<MaterialPaintLayer *>(members, 1));
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.9f));

  EXPECT_TRUE(BKE_paint_layers_bake_is_heavy(*ma, *gradient))
      << "the child's own bake pays for the canvas";
  EXPECT_FALSE(BKE_paint_layers_bake_is_heavy(*ma, *folder))
      << "the child caches itself: the folder's bake is not a texel one";
}

TEST_F(PaintLayersBakeTest, a_switched_off_gradient_correction_costs_the_row_nothing)
{
  ma->paint_layers_image_size = 2048;
  MaterialPaintLayer *row = add_paint_layer("Row", add_image("Row"));
  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(
      *ma, row, "Grad", MA_PAINT_LAYER_SOURCE_GENERATOR);
  ASSERT_NE(effect, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(*ma, *effect, MA_PAINT_LAYER_GRADIENT_SPACE_UV));
  EXPECT_TRUE(BKE_paint_layers_bake_is_heavy(*ma, *row));

  /* A correction switched off lays no color: the row's bake does not fold its Gradient in. */
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, effect, false));
  EXPECT_FALSE(BKE_paint_layers_bake_is_heavy(*ma, *row))
      << "a Gradient that is switched off costs no texels";

  clear_active();
  bool changed = false;
  /* The canvas the correction's space change precreated is not wanted by the light row anymore:
   * releasing it is the one change the plan makes, and nothing is baked into the row. */
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_TRUE(changed);
  EXPECT_EQ(row->bake, nullptr) << "a light AUTO row is evaluated live";

  /* With the record gone the switched-off state costs the plan nothing. */
  changed = false;
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_FALSE(changed);
}

TEST_F(PaintLayersBakeTest, a_3d_gradient_correction_keeps_the_row_live_and_undoes_its_bake)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *row = add_paint_layer("Row", add_image("Row"));
  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(
      *ma, row, "Grad", MA_PAINT_LAYER_SOURCE_GENERATOR);
  ASSERT_NE(effect, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(*ma, *effect, MA_PAINT_LAYER_GRADIENT_SPACE_UV));

  clear_active();
  bool changed = false;
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  ASSERT_NE(row->bake, nullptr);

  /* Object space reads per-object geometry: the shared bake no longer describes the row. */
  EXPECT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *effect, MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT));
  EXPECT_TRUE(BKE_paint_layers_bake_is_heavy(*ma, *row))
      << "a per-object row is rasterized on the main thread, so it always leaves it";
  changed = false;
  /* Without a view there is nothing to judge the objects by: the plan drops nothing yet, and the
   * record keeps the mode; the row stays live and unsubstituted. */
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_FALSE(changed);
  ASSERT_NE(row->bake, nullptr);
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *row));
  EXPECT_FALSE(paint_layer_row_substituted(*ma, *row));

  /* With a view layer the object has no UV map, the permanent reason: the maps the shared bake
   * left are released, and the record keeps the mode. */
  Scene *scene = add_scene_with_uvless_object();
  ViewLayer *view_layer = static_cast<ViewLayer *>(scene->view_layers.first);
  changed = false;
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed, scene, view_layer));
  EXPECT_TRUE(changed);
  ASSERT_NE(row->bake, nullptr);
  EXPECT_EQ(row->bake->coverage, nullptr) << "a shared bake is not kept for a 3D row";
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *row));
  EXPECT_FALSE(paint_layer_row_substituted(*ma, *row));

  changed = false;
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed, scene, view_layer));
  EXPECT_FALSE(changed);
}

TEST_F(PaintLayersBakeTest, a_gradient_in_never_mode_is_left_live)
{
  ma->paint_layers_image_size = 2048;
  MaterialPaintLayer *row = add_paint_layer("Row", add_image("Row"));
  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(
      *ma, row, "Grad", MA_PAINT_LAYER_SOURCE_GENERATOR);
  ASSERT_NE(effect, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(*ma, *effect, MA_PAINT_LAYER_GRADIENT_SPACE_UV));
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *row, MA_PAINT_LAYER_BAKE_NEVER));

  /* Never wins over the texel cost: the Gradient is laid live, whatever it would cost to bake. */
  clear_active();
  bool changed = false;
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_FALSE(changed);
  ASSERT_EQ(BKE_paint_layers_bake_mode_get(*row), MA_PAINT_LAYER_BAKE_NEVER);
  ASSERT_NE(row->bake, nullptr) << "the mode lives in the row's bake record";
  EXPECT_EQ(row->bake->coverage, nullptr) << "Never writes no cache";
  EXPECT_FALSE(paint_layer_row_substituted(*ma, *row));
}

/**
 * Leaving a row is when its bake becomes due, and that is the only switch that has to reach the
 * depsgraph: tagging every one of them would rebuild the material's shading for nothing.
 */
TEST_F(PaintLayersBakeTest, switching_rows_tags_the_material_only_when_a_bake_is_due)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *row = add_paint_layer("Row", add_image("Row"));
  MaterialPaintLayer *other = add_paint_layer("Other", add_image("Other"));
  BKE_paint_layers_active_set(*ma, other->marker);

  ma->id.recalc = 0;
  BKE_paint_layers_active_set(*ma, row->marker);
  EXPECT_EQ(ma->id.recalc & ID_RECALC_SHADING, 0)
      << "a light AUTO row left behind has no bake to start";

  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(
      *ma, row, "Grad", MA_PAINT_LAYER_SOURCE_GENERATOR);
  ASSERT_NE(effect, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(*ma, *effect, MA_PAINT_LAYER_GRADIENT_SPACE_UV));
  BKE_paint_layers_active_set(*ma, effect->marker);

  ma->id.recalc = 0;
  BKE_paint_layers_active_set(*ma, other->marker);
  EXPECT_NE(ma->id.recalc & ID_RECALC_SHADING, 0)
      << "the row left behind is due a bake that folds its Gradient in";
  EXPECT_NE(ma->id.recalc & ID_RECALC_SYNC_TO_EVAL, 0)
      << "the tag is the pair the rest of the module uses";
}

TEST_F(PaintLayersBakeTest, an_object_gradient_keeps_its_mode_without_a_surface)
{
  ma->paint_layers_image_size = 8;
  Scene *scene = add_scene_with_uvless_object();
  ViewLayer *view_layer = static_cast<ViewLayer *>(scene->view_layers.first);
  MaterialPaintLayer *row = BKE_paint_layers_gradient_add(
      *ma, "Object", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *row, MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT));

  for (const int mode : {int(MA_PAINT_LAYER_BAKE_NEVER), int(MA_PAINT_LAYER_BAKE_ALWAYS)}) {
    ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *row, mode));
    bool changed = false;
    /* A contextless plan cannot judge the objects and must not drop the record. */
    EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
    EXPECT_EQ(BKE_paint_layers_bake_mode_get(*row), mode);
    ASSERT_NE(row->bake, nullptr);
    EXPECT_EQ(row->bake->coverage, nullptr);

    /* With a view but no UV map the reason is permanent: the mode still survives. */
    EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed, scene, view_layer));
    EXPECT_EQ(BKE_paint_layers_bake_mode_get(*row), mode);
    EXPECT_EQ(BKE_paint_layers_bake_object_issue_get(*ma, *row),
              PAINT_LAYERS_BAKE_OBJECTS_NO_UV);
    ASSERT_NE(row->bake, nullptr);
    EXPECT_EQ(row->bake->coverage, nullptr);
  }
}

TEST_F(PaintLayersBakeTest, a_failed_surface_does_not_create_a_bake_record)
{
  ma->paint_layers_image_size = 8;
  Scene *scene = add_scene_with_uvless_object();
  ViewLayer *view_layer = static_cast<ViewLayer *>(scene->view_layers.first);
  MaterialPaintLayer *row = BKE_paint_layers_gradient_add(
      *ma, "Object", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *row, MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT));
  /* Remove the precreated record to test the planner's own surface gate: a row whose object has no
   * UV must not get a new record from the plan. */
  ASSERT_TRUE(BKE_paint_layers_bake_clear(*ma, *row));
  ASSERT_EQ(row->bake, nullptr);

  bool changed = false;
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed, scene, view_layer));
  EXPECT_FALSE(changed);
  EXPECT_EQ(row->bake, nullptr)
      << "the surface is resolved before the row's bake structure is allocated";
  EXPECT_EQ(BKE_paint_layers_bake_object_issue_get(*ma, *row),
            PAINT_LAYERS_BAKE_OBJECTS_NO_UV);
}

TEST_F(PaintLayersBakeTest, a_shared_gradient_precreates_its_maps_and_never_drops_them)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *row = BKE_paint_layers_gradient_add(
      *ma, "Gradient", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *row, MA_PAINT_LAYER_GRADIENT_SPACE_UV));
  ASSERT_NE(row->bake, nullptr) << "a shared Gradient creates its canvas up front";
  EXPECT_NE(row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR], nullptr);
  EXPECT_NE(row->bake->coverage, nullptr);

  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *row, MA_PAINT_LAYER_BAKE_NEVER));
  EXPECT_EQ(BKE_paint_layers_bake_mode_get(*row), MA_PAINT_LAYER_BAKE_NEVER);
  ASSERT_NE(row->bake, nullptr);
  EXPECT_EQ(row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR], nullptr);
  EXPECT_EQ(row->bake->coverage, nullptr);
}

/**
 * Precreate makes only the maps the row's first bake will fill: a channel the row lays nothing in
 * gets no full-size blank map. A record added later brings its map on the next ask -- the
 * idempotent branch of the space setter re-runs the precreate.
 */
TEST_F(PaintLayersBakeTest, a_shared_gradient_precreates_only_the_channels_it_lays)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *layer = BKE_paint_layers_gradient_add(
      *ma, "Gradient", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *layer, MA_PAINT_LAYER_GRADIENT_SPACE_UV));
  ASSERT_NE(layer->bake, nullptr);
  EXPECT_NE(layer->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR], nullptr);
  EXPECT_EQ(layer->bake->images[PAINT_MATERIAL_CHANNEL_ROUGHNESS], nullptr)
      << "a channel the row has no record for gets no map";
  EXPECT_NE(layer->bake->coverage, nullptr);

  ASSERT_NE(BKE_paint_layers_channel_add(*ma, layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS), nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *layer, MA_PAINT_LAYER_GRADIENT_SPACE_UV));
  EXPECT_NE(layer->bake->images[PAINT_MATERIAL_CHANNEL_ROUGHNESS], nullptr);
}

/**
 * The side the user set with #BKE_paint_layers_bake_size_set lives in the row's bake structure
 * (#MA_PAINT_LAYER_BAKE_SIZE_EXPLICIT), so it outranks the canvas a Gradient row would otherwise
 * follow and survives the bakes and the edits between. It dies with the structure: a release of
 * the cache takes it, and the row answers to the canvas again.
 */
TEST_F(PaintLayersBakeTest, an_explicit_bake_size_outranks_the_canvas_and_dies_with_the_record)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *layer = BKE_paint_layers_gradient_add(
      *ma, "Gradient", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *layer, MA_PAINT_LAYER_GRADIENT_SPACE_UV));
  ASSERT_TRUE(BKE_paint_layers_bake_size_set(*ma, *layer, 4));
  ASSERT_NE(layer->bake, nullptr);
  EXPECT_NE(layer->bake->flag & MA_PAINT_LAYER_BAKE_SIZE_EXPLICIT, 0);

  clear_active();
  bool changed = false;
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_EQ(layer->bake->size, 4) << "the explicit side outranks the canvas";
  int width = 0;
  int height = 0;
  BKE_image_get_size(
      layer->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR], nullptr, &width, &height);
  EXPECT_EQ(width, 4);
  EXPECT_EQ(height, 4);

  /* A resize of the canvas does not move the row's side: the re-bake lands at the choice again. */
  ma->paint_layers_image_size = 16;
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *layer))
      << "the canvas is what the hash vouches for";
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma));
  EXPECT_EQ(layer->bake->size, 4);

  /* Only Complex releases the light row's cache: the choice goes with the structure it lives in. */
  ma->paint_layers_gradient_bake = MA_PAINT_LAYERS_GRADIENT_BAKE_ONLY_COMPLEX;
  changed = false;
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_TRUE(changed);
  EXPECT_EQ(layer->bake, nullptr);
  ma->paint_layers_gradient_bake = MA_PAINT_LAYERS_GRADIENT_BAKE_ALWAYS;
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma));
  ASSERT_NE(layer->bake, nullptr);
  EXPECT_EQ(layer->bake->size, 16) << "without the record the row follows the canvas again";
  EXPECT_EQ(layer->bake->flag & MA_PAINT_LAYER_BAKE_SIZE_EXPLICIT, 0);
}

/**
 * The switch reads only the old marker's ancestor path; its answer stays the one a walk over every
 * row of the material gives. The pairs run over a deep tree, folders included, and over the marker
 * of a correction, whose owner row is the one the path ends at.
 */
TEST_F(PaintLayersBakeTest, the_switch_pending_answer_matches_a_walk_over_every_row)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *a = add_baked_row("A");
  MaterialPaintLayer *b = add_baked_row("B");
  MaterialPaintLayer *inner_members[] = {a, b};
  MaterialPaintLayer *inner = BKE_paint_layers_group(
      *ma, Span<MaterialPaintLayer *>(inner_members, 2));
  ASSERT_NE(inner, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, inner, 0.9f));
  MaterialPaintLayer *c = add_baked_row("C");
  MaterialPaintLayer *outer_members[] = {inner, c};
  MaterialPaintLayer *outer = BKE_paint_layers_group(
      *ma, Span<MaterialPaintLayer *>(outer_members, 2));
  ASSERT_NE(outer, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, outer, 0.9f));
  MaterialPaintLayer *d = add_baked_row("D");
  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(
      *ma, d, "Grad", MA_PAINT_LAYER_SOURCE_GENERATOR);
  ASSERT_NE(effect, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(*ma, *effect, MA_PAINT_LAYER_GRADIENT_SPACE_UV));

  /* The bakes settle with nothing active, so the switches answer about real caches. */
  clear_active();
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma));

  const Vector<bUUID> markers = {a->marker,
                                 b->marker,
                                 c->marker,
                                 d->marker,
                                 inner->marker,
                                 outer->marker,
                                 effect->marker};
  PaintLayersBakeView no_context;
  no_context.issue = PAINT_LAYERS_BAKE_OBJECTS_NO_CONTEXT;
  for (const bUUID &old_marker : markers) {
    for (const bUUID &new_marker : markers) {
      if (BLI_uuid_equal(old_marker, new_marker)) {
        continue;
      }
      /* The function reads after the marker moved, so the walk sees the same active row. */
      BKE_paint_layers_active_set(*ma, new_marker);
      bool expected = false;
      BKE_paint_layers_foreach(
          *ma, [&](const MaterialPaintLayer &layer, const MaterialPaintLayer *) -> bool {
            if (!BKE_paint_layers_subtree_contains(layer, old_marker) ||
                BKE_paint_layers_subtree_contains(layer, new_marker))
            {
              return true;
            }
            BakeRowRelease release = BakeRowRelease::Keep;
            expected = bake_row_is_due(*ma, layer, no_context, release);
            return !expected;
          });
      EXPECT_EQ(paint_layers_bake_pending_after_switch(*ma, old_marker, new_marker), expected);
    }
  }
}

TEST_F(PaintLayersBakeTest, an_object_gradient_precreates_its_first_tile)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *row = BKE_paint_layers_gradient_add(
      *ma, "Gradient", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  /* A new Gradient starts in Object space: its maps are one tiled image with tile 1001. */
  ASSERT_NE(row->bake, nullptr) << "an Object Gradient creates its canvas up front";
  Image *base = row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  Image *coverage = row->bake->coverage;
  ASSERT_NE(base, nullptr);
  ASSERT_NE(coverage, nullptr);
  EXPECT_EQ(base->source, IMA_SRC_TILED);
  EXPECT_EQ(coverage->source, IMA_SRC_TILED);
  EXPECT_NE(BKE_image_get_tile(base, 1001), nullptr);
  EXPECT_EQ(BKE_image_get_tile(base, 1002), nullptr);
  EXPECT_NE(BKE_image_get_tile(coverage, 1001), nullptr)
      << "the coverage map is tiled when the row is per object";
}

TEST_F(PaintLayersBakeTest, an_object_gradient_creates_no_maps_in_never_mode_or_above_the_cap)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *never = BKE_paint_layers_gradient_add(
      *ma, "Never", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(never, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *never, MA_PAINT_LAYER_BAKE_NEVER));
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *never, MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT));
  ASSERT_NE(never->bake, nullptr);
  EXPECT_EQ(never->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR], nullptr);
  EXPECT_EQ(never->bake->coverage, nullptr);
  EXPECT_TRUE(BLI_listbase_is_empty(&never->bake->tiles));

  /* The size cap is checked before the record: a row that could never bake gets no structure. */
  ma->paint_layers_image_size = 8192;
  MaterialPaintLayer *too_large = BKE_paint_layers_gradient_add(
      *ma, "Too Large", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(too_large, nullptr);
  EXPECT_EQ(too_large->bake, nullptr);
}

TEST_F(PaintLayersBakeTest, removing_an_object_gradient_releases_its_precreated_maps)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *row = BKE_paint_layers_gradient_add(
      *ma, "Gradient", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_NE(row->bake, nullptr);
  Image *base = row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  Image *coverage = row->bake->coverage;
  ASSERT_NE(base, nullptr);
  ASSERT_NE(coverage, nullptr);
  const int base_users = base->id.us;
  const int coverage_users = coverage->id.us;
  ASSERT_GT(base_users, 0);
  ASSERT_GT(coverage_users, 0);
  const bUUID marker = row->marker;

  ASSERT_TRUE(BKE_paint_layers_remove(*ma, row));
  EXPECT_EQ(BKE_paint_layers_find(*ma, marker), nullptr);
  EXPECT_EQ(base->id.us, base_users - 1);
  EXPECT_EQ(coverage->id.us, coverage_users - 1);
}

TEST_F(PaintLayersBakeTest, removing_a_material_slot_drops_the_stale_tile_index)
{
  ma->paint_layers_image_size = 8;
  Scene *scene = BKE_scene_add(bmain, "BakeScene");
  Object *object = add_uv_quad_object(*scene, "Object");
  /* The slot removal checks the object's own slots once the shared one is gone, so the material is
   * also assigned to the object: an obdata-only assignment never enters the slot array the removal
   * walks, and the object then keeps its index until the next plan. */
  BKE_object_material_assign(bmain, object, ma, 1, BKE_MAT_ASSIGN_OBJECT);
  const std::string path = paint_layers_bake_tile_attribute_path(*ma);
  IDProperty *properties = IDP_EnsureProperties(&object->id);
  IDP_AddToGroup(properties, IDP_NewInt(0, path.c_str()));
  ASSERT_NE(IDP_GetPropertyFromGroup(properties, path.c_str()), nullptr);

  object->actcol = 1;
  ASSERT_TRUE(BKE_object_material_slot_remove(bmain, object));
  properties = IDP_GetProperties(&object->id);
  EXPECT_TRUE(properties == nullptr ||
              IDP_GetPropertyFromGroup(properties, path.c_str()) == nullptr)
      << "an object that no longer uses the material keeps no tile index";
}

TEST_F(PaintLayersBakeTest, a_plan_prunes_tile_indices_of_deleted_materials_only)
{
  ma->paint_layers_image_size = 8;
  Scene *scene = BKE_scene_add(bmain, "BakeScene");
  Object *object = add_uv_quad_object(*scene, "Object");
  update_scene(*scene);
  ViewLayer *view_layer = static_cast<ViewLayer *>(scene->view_layers.first);

  /* The plan releases a tile index no baked row of a live material justifies anymore, so the live
   * material's key is made the honest way: one per-object bake, which writes the property itself. */
  MaterialPaintLayer *row = BKE_paint_layers_gradient_add(
      *ma, "Object", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *row, MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT));
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *row, MA_PAINT_LAYER_BAKE_ALWAYS));
  clear_active();
  PaintLayersBakeJob *job = BKE_paint_layers_bake_job_create(*bmain, *ma, scene, view_layer);
  ASSERT_NE(job, nullptr);
  BKE_paint_layers_bake_job_compute(*job, {}, {});
  ASSERT_TRUE(BKE_paint_layers_bake_job_commit(*job));
  BKE_paint_layers_bake_job_free(*job);

  /* A material that is gone by the time the planner walks the view: its key cannot map back. */
  Material *dead = BKE_material_add(bmain, "DeadTileKey");
  dead->paint_layers_flag |= MA_PAINT_LAYERED;
  const std::string dead_path = paint_layers_bake_tile_attribute_path(*dead);
  BKE_id_delete(bmain, &dead->id);
  IDProperty *properties = IDP_EnsureProperties(&object->id);
  IDP_AddToGroup(properties, IDP_NewInt(0, dead_path.c_str()));

  bool changed = false;
  BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed, scene, view_layer);

  const std::string live_path = paint_layers_bake_tile_attribute_path(*ma);
  properties = IDP_GetProperties(&object->id);
  ASSERT_NE(properties, nullptr);
  EXPECT_EQ(IDP_GetPropertyFromGroup(properties, dead_path.c_str()), nullptr)
      << "a key no layered material of the main database names is stale";
  EXPECT_NE(IDP_GetPropertyFromGroup(properties, live_path.c_str()), nullptr)
      << "a key of a live layered material must stay";
}

TEST_F(PaintLayersBakeTest, setting_an_object_space_on_a_correction_precreates_the_owner_row)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *row = add_paint_layer("Row", add_image("Row"));
  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(
      *ma, row, "Grad", MA_PAINT_LAYER_SOURCE_GENERATOR);
  ASSERT_NE(effect, nullptr);
  /* The correction's Gradient belongs to the row's cache, so the correction itself has no
   * record. */
  ASSERT_EQ(effect->bake, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_clear(*ma, *row));
  ASSERT_EQ(row->bake, nullptr);

  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *effect, MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT));
  EXPECT_EQ(effect->bake, nullptr);
  ASSERT_NE(row->bake, nullptr);
  Image *base = row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(base, nullptr);
  EXPECT_EQ(base->source, IMA_SRC_TILED);
  EXPECT_NE(BKE_image_get_tile(base, 1001), nullptr);
}

TEST_F(PaintLayersBakeTest, an_object_bake_writes_one_tile_per_object_and_geometry_invalidates_it)
{
  ma->paint_layers_image_size = 8;
  Scene *scene = BKE_scene_add(bmain, "BakeScene");
  Object *first = add_uv_quad_object(*scene, "First");
  add_uv_quad_object(*scene, "Second");
  update_scene(*scene);
  ViewLayer *view_layer = static_cast<ViewLayer *>(scene->view_layers.first);

  MaterialPaintLayer *row = BKE_paint_layers_gradient_add(
      *ma, "Object", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *row, MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT));
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *row, MA_PAINT_LAYER_BAKE_ALWAYS));
  clear_active();

  PaintLayersBakeJob *job = BKE_paint_layers_bake_job_create(*bmain, *ma, scene, view_layer);
  ASSERT_NE(job, nullptr);
  BKE_paint_layers_bake_job_compute(*job, {}, {});
  ASSERT_TRUE(BKE_paint_layers_bake_job_commit(*job));
  BKE_paint_layers_bake_job_free(*job);

  ASSERT_NE(row->bake, nullptr);
  EXPECT_EQ(row->bake->object_count, 2);
  EXPECT_EQ(BLI_listbase_count(&row->bake->tiles), 2);
  Image *base = row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(base, nullptr);
  EXPECT_EQ(base->source, IMA_SRC_TILED);
  EXPECT_NE(BKE_image_get_tile(base, 1001), nullptr);
  EXPECT_NE(BKE_image_get_tile(base, 1002), nullptr);

  /* Editing one mesh changes its surface identity and invalidates the row. */
  Mesh *mesh = id_cast<Mesh *>(first->data);
  mesh->vert_positions_for_write()[0].x += 0.5f;
  mesh->tag_positions_changed();
  DEG_id_tag_update(&mesh->id, ID_RECALC_GEOMETRY);
  update_scene(*scene);
  BKE_paint_layers_bake_plan_run(*bmain, *ma, nullptr, scene, view_layer);
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *row));
}

/**
 * The maps a job's commit writes are the ones a synchronous bake would: the worker holds its render
 * in the map's own format -- a byte map's quantized texels, a float map's premultiplied ones -- and
 * the reference here is that same encoding run by #bake_write_image over the straight render. The
 * coverage is the common factor the worker folded, quantized the way the coverage map is written.
 */
TEST_F(PaintLayersBakeTest, a_job_writes_what_the_maps_own_format_keeps)
{
  ma->paint_layers_image_size = 8;
  Scene *scene = BKE_scene_add(bmain, "BakeScene");
  Object *object = add_uv_quad_object(*scene, "BakeObject");
  update_scene(*scene);
  ViewLayer *view_layer = static_cast<ViewLayer *>(scene->view_layers.first);

  MaterialPaintLayer *row = BKE_paint_layers_gradient_add(
      *ma, "Object", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *row, MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT));
  /* Metallic is the float channel this generated row lays (a generated row bakes every non-color
   * channel float; Height keeps a value pair instead of a map, so it renders nothing here). Base
   * Color stays byte. */
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, row, PAINT_MATERIAL_CHANNEL_METALLIC), nullptr);
  const float metallic_value[4] = {0.75f, 0.75f, 0.75f, 1.0f};
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_value(*ma, row, PAINT_MATERIAL_CHANNEL_METALLIC, metallic_value));
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *row, MA_PAINT_LAYER_BAKE_ALWAYS));
  clear_active();

  /* The reference, rendered before any cache exists: the same surface the job would rasterize,
   * through the same render the worker runs. */
  const int size = 8;
  Mesh &mesh = *id_cast<Mesh *>(object->data);
  std::shared_ptr<const PaintMaterialCompositeSurface> surface =
      BKE_paint_layers_gradient_surface_build(*object, mesh, "UVMap", size, 1, false);
  ASSERT_NE(surface, nullptr);
  Vector<float> base_color(size * size * 4);
  Vector<float> base_coverage(size * size);
  ASSERT_TRUE(BKE_paint_layers_bake_render_node(*ma,
                                                *row,
                                                PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                                size,
                                                base_color.data(),
                                                base_coverage.data(),
                                                nullptr,
                                                surface));
  Vector<float> metallic_color(size * size * 4);
  Vector<float> metallic_coverage(size * size);
  ASSERT_TRUE(BKE_paint_layers_bake_render_node(*ma,
                                                *row,
                                                PAINT_MATERIAL_CHANNEL_METALLIC,
                                                size,
                                                metallic_color.data(),
                                                metallic_coverage.data(),
                                                nullptr,
                                                surface));
  /* The common factor the worker folds: the maximum over the channels rendered. */
  Vector<float> common(size * size, 0.0f);
  fold_channel_common(base_color, base_coverage, common);
  fold_channel_common(metallic_color, metallic_coverage, common);

  /* The reference writes, through the same code a synchronous bake runs. */
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *ref_base = BKE_image_add_generated(
      bmain, size, size, "Ref Base", 32, 0, IMA_GENTYPE_BLANK, black, false, false, false);
  ASSERT_NE(ref_base, nullptr);
  bake_write_image(*ref_base, base_color.data(), true, nullptr, 0);
  Image *ref_metallic = BKE_image_add_generated(
      bmain, size, size, "Ref Metallic", 32, 1, IMA_GENTYPE_BLANK, black, false, true, false);
  ASSERT_NE(ref_metallic, nullptr);
  bake_write_image(*ref_metallic, metallic_color.data(), false, nullptr, 0);
  Image *ref_coverage = BKE_image_add_generated(
      bmain, size, size, "Ref Coverage", 32, 0, IMA_GENTYPE_BLANK, black, false, true, false);
  ASSERT_NE(ref_coverage, nullptr);
  bake_write_coverage_image(*ref_coverage, common.data(), nullptr, 0);

  /* The job. */
  PaintLayersBakeJob *job = BKE_paint_layers_bake_job_create(*bmain, *ma, scene, view_layer);
  ASSERT_NE(job, nullptr);
  BKE_paint_layers_bake_job_compute(*job, {}, {});
  EXPECT_GT(BKE_paint_layers_bake_job_retained_bytes(*job), 0)
      << "the worker holds the results for the commit";
  EXPECT_TRUE(BKE_paint_layers_bake_job_commit(*job));
  EXPECT_EQ(BKE_paint_layers_bake_job_retained_bytes(*job), 0)
      << "a fully committed job holds no channel buffers";
  BKE_paint_layers_bake_job_free(*job);

  ASSERT_NE(row->bake, nullptr);
  Image *base = row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  Image *metallic = row->bake->images[PAINT_MATERIAL_CHANNEL_METALLIC];
  Image *coverage = row->bake->coverage;
  ASSERT_NE(base, nullptr);
  ASSERT_NE(metallic, nullptr);
  ASSERT_NE(coverage, nullptr);
  EXPECT_EQ(tile_pixel_bytes(*base, 1001), tile_pixel_bytes(*ref_base))
      << "the byte map's texels are the reference's, quantized the same way";
  EXPECT_EQ(tile_pixel_floats(*metallic, 1001), tile_pixel_floats(*ref_metallic))
      << "the float map's texels are the reference's, premultiplied the same way";
  EXPECT_EQ(tile_pixel_bytes(*coverage, 1001), tile_pixel_bytes(*ref_coverage))
      << "the coverage is the common factor the worker folded, quantized as the map is";

  BKE_id_free(bmain, &ref_base->id);
  BKE_id_free(bmain, &ref_metallic->id);
  BKE_id_free(bmain, &ref_coverage->id);
}

/**
 * A cancelled job writes nothing, and the results it held are let go the moment the cancellation is
 * noticed: at 4K they are hundreds of MiB the commit would never read.
 */
TEST_F(PaintLayersBakeTest, a_cancelled_job_writes_nothing_and_lets_go_of_its_results)
{
  ma->paint_layers_image_size = 8;
  Scene *scene = BKE_scene_add(bmain, "BakeScene");
  add_uv_quad_object(*scene, "BakeObject");
  update_scene(*scene);
  ViewLayer *view_layer = static_cast<ViewLayer *>(scene->view_layers.first);

  MaterialPaintLayer *row = BKE_paint_layers_gradient_add(
      *ma, "Object", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *row, MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT));
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *row, MA_PAINT_LAYER_BAKE_ALWAYS));
  clear_active();

  const size_t memory_before = MEM_get_memory_in_use();
  PaintLayersBakeJob *job = BKE_paint_layers_bake_job_create(*bmain, *ma, scene, view_layer);
  ASSERT_NE(job, nullptr);
  /* Cancel after the first channel rendered: one result is held when the notice lands. */
  int polls = 0;
  BKE_paint_layers_bake_job_compute(*job, {}, [&]() { return ++polls > 1; });
  /* `PaintLayersBakeJob` is private to the bake source, so the cancel is seen through the poll. */
  EXPECT_GT(polls, 1) << "the cancel notice was polled";
  EXPECT_EQ(BKE_paint_layers_bake_job_retained_bytes(*job), 0)
      << "the held results went with the cancellation";
  EXPECT_FALSE(BKE_paint_layers_bake_job_commit(*job)) << "a cancelled job writes nothing";
  ASSERT_NE(row->bake, nullptr) << "the record the precreated maps keep stays";
  EXPECT_EQ(row->bake->hash[0], 0) << "no hash stamped: the maps are still nobody's result";
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *row));

  BKE_paint_layers_bake_job_free(*job);
  /* The job's surfaces, results and localized copy are all back; the megabyte of slack is for the
   * caches the render of the first channel may have warmed around the buffers themselves. */
  EXPECT_LE(int64_t(MEM_get_memory_in_use()), int64_t(memory_before) + (int64_t(1) << 20));
}

TEST_F(PaintLayersBakeTest, a_permanently_blocked_object_row_does_not_tag_on_a_switch)
{
  ma->paint_layers_image_size = 8;
  Scene *scene = add_scene_with_uvless_object();
  ViewLayer *view_layer = static_cast<ViewLayer *>(scene->view_layers.first);
  MaterialPaintLayer *row = BKE_paint_layers_gradient_add(
      *ma, "Object", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *other = add_paint_layer("Other", add_image("Other"));
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *row, MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT));
  BKE_paint_layers_bake_plan_run(*bmain, *ma, nullptr, scene, view_layer);
  ASSERT_EQ(BKE_paint_layers_bake_object_issue_get(*ma, *row),
            PAINT_LAYERS_BAKE_OBJECTS_NO_UV);

  /* Leaving the blocked row changes no object: the permanent reason stays and no tag is due. */
  BKE_paint_layers_active_set(*ma, row->marker);
  ma->id.recalc = 0;
  BKE_paint_layers_active_set(*ma, other->marker);
  EXPECT_EQ(ma->id.recalc & ID_RECALC_SHADING, 0);
}

/** Append one tile record to \a layer's bake, as a completed per-object bake would. */
static MaterialPaintLayerBakeTile *add_bake_tile(MaterialPaintLayer &layer,
                                                 const uint32_t object_uid,
                                                 const int tile_number)
{
  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(layer);
  MaterialPaintLayerBakeTile *tile = MEM_new<MaterialPaintLayerBakeTile>(__func__);
  tile->object_uid = object_uid;
  tile->tile[0] = (tile_number - 1001) % 10;
  tile->tile[1] = (tile_number - 1001) / 10;
  BLI_addtail(&bake->tiles, tile);
  bake->object_count++;
  return tile;
}

TEST_F(PaintLayersBakeTest, a_tiled_bake_round_trips_without_carrying_tiles_to_a_new_session)
{
  MaterialPaintLayer *row = add_baked_row("RoundTrip");
  ASSERT_NE(row, nullptr);
  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(*row);
  bake->size = 8;
  add_bake_tile(*row, 1234, 1001);
  add_bake_tile(*row, 5678, 1002);
  ASSERT_EQ(BLI_listbase_count(&bake->tiles), 2);

  char filepath[FILE_MAX];
  BLI_path_join(
      filepath, sizeof(filepath), BKE_tempdir_session(), "paint_layers_bake_tiles.blend");
  BlendFileWriteParams write_params{};
  ASSERT_TRUE(BLO_write_file(bmain, filepath, 0, &write_params, nullptr));

  BlendFileReadReport read_report{};
  BlendFileData *bfd = BLO_read_from_file(filepath, BLO_READ_SKIP_NONE, &read_report);
  ASSERT_NE(bfd, nullptr);

  Material *reloaded = nullptr;
  for (Material &candidate : bfd->main->materials) {
    if (STREQ(candidate.id.name + 2, ma->id.name + 2)) {
      reloaded = &candidate;
      break;
    }
  }
  ASSERT_NE(reloaded, nullptr);
  MaterialPaintLayer *reloaded_row = BKE_paint_layers_find(*reloaded, row->marker);
  ASSERT_NE(reloaded_row, nullptr);
  ASSERT_NE(reloaded_row->bake, nullptr);
  /* The file is read without undo: the object uids belong to the old session, so the records go,
   * while the mode and size the user chose stay. */
  EXPECT_TRUE(BLI_listbase_is_empty(&reloaded_row->bake->tiles));
  EXPECT_EQ(reloaded_row->bake->object_count, 0);
  EXPECT_EQ(BKE_paint_layers_bake_mode_get(*reloaded_row), MA_PAINT_LAYER_BAKE_ALWAYS);
  EXPECT_EQ(reloaded_row->bake->size, 8);

  BLO_blendfiledata_free(bfd);
  BLI_delete(filepath, false, false);
}

TEST_F(PaintLayersBakeTest, copying_a_material_with_tiles_does_not_free_them_twice)
{
  MaterialPaintLayer *row = add_baked_row("CopyTiles");
  ASSERT_NE(row, nullptr);
  add_bake_tile(*row, 1234, 1001);
  add_bake_tile(*row, 5678, 1002);
  MaterialPaintLayerBake *bake = row->bake;
  ASSERT_NE(bake, nullptr);
  ASSERT_EQ(BLI_listbase_count(&bake->tiles), 2);

  Material *copy = id_cast<Material *>(BKE_id_copy(bmain, &ma->id));
  ASSERT_NE(copy, nullptr);
  MaterialPaintLayer *copied = static_cast<MaterialPaintLayer *>(copy->paint_layers.first);
  ASSERT_NE(copied, nullptr);
  ASSERT_NE(copied->bake, nullptr);
  EXPECT_TRUE(BLI_listbase_is_empty(&copied->bake->tiles));
  EXPECT_EQ(copied->bake->object_count, 0);
  EXPECT_EQ(BLI_listbase_count(&bake->tiles), 2) << "the original keeps its own records";

  BKE_id_free(bmain, &copy->id);
  EXPECT_EQ(BLI_listbase_count(&bake->tiles), 2)
      << "freeing the copy must not free the original's records";
}

TEST_F(PaintLayersBakeTest, clearing_a_tiled_bake_removes_the_object_tile_index)
{
  ma->paint_layers_image_size = 8;
  Scene *scene = BKE_scene_add(bmain, "BakeScene");
  Object *first = add_uv_quad_object(*scene, "First");
  Object *second = add_uv_quad_object(*scene, "Second");
  update_scene(*scene);
  ViewLayer *view_layer = static_cast<ViewLayer *>(scene->view_layers.first);

  MaterialPaintLayer *row = BKE_paint_layers_gradient_add(
      *ma, "Object", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *row, MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT));
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *row, MA_PAINT_LAYER_BAKE_ALWAYS));
  clear_active();

  PaintLayersBakeJob *job = BKE_paint_layers_bake_job_create(*bmain, *ma, scene, view_layer);
  ASSERT_NE(job, nullptr);
  BKE_paint_layers_bake_job_compute(*job, {}, {});
  ASSERT_TRUE(BKE_paint_layers_bake_job_commit(*job));
  BKE_paint_layers_bake_job_free(*job);

  const std::string path = paint_layers_bake_tile_attribute_path(*ma);
  for (Object *object : {first, second}) {
    IDProperty *properties = IDP_GetProperties(&object->id);
    ASSERT_NE(properties, nullptr);
    ASSERT_NE(IDP_GetPropertyFromGroup(properties, path.c_str()), nullptr);
  }

  /* Clearing the row drops its tiles; the next plan has no bake left to justify the property. */
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *row, MA_PAINT_LAYER_BAKE_NEVER));
  bool changed = false;
  BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed, scene, view_layer);
  for (Object *object : {first, second}) {
    IDProperty *properties = IDP_GetProperties(&object->id);
    if (properties != nullptr) {
      EXPECT_EQ(IDP_GetPropertyFromGroup(properties, path.c_str()), nullptr);
    }
  }
}

TEST_F(PaintLayersBakeTest, an_object_row_without_maps_keeps_never_and_always)
{
  ma->paint_layers_image_size = 8;
  Scene *scene = add_scene_with_uvless_object();
  ViewLayer *view_layer = static_cast<ViewLayer *>(scene->view_layers.first);
  MaterialPaintLayer *row = BKE_paint_layers_gradient_add(
      *ma, "Object", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *row, MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT));

  for (const int mode : {int(MA_PAINT_LAYER_BAKE_NEVER), int(MA_PAINT_LAYER_BAKE_ALWAYS)}) {
    ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *row, mode));
    bool changed = false;
    EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed, scene, view_layer));
    EXPECT_EQ(BKE_paint_layers_bake_mode_get(*row), mode);
    ASSERT_NE(row->bake, nullptr);
    EXPECT_EQ(row->bake->coverage, nullptr);
    EXPECT_TRUE(BLI_listbase_is_empty(&row->bake->tiles));
    EXPECT_EQ(row->bake->object_count, 0);
  }
}

/* -------------------------------------------------------------------- */
/** \name Mask effect bakes (a CPU-computed kind as a mask item)
 * \{ */

namespace {

/** A CPU-computed effect as an item of \a row's mask stack, over its base mask. */
MaterialPaintLayer *bake_test_add_mask_effect(Material &ma,
                                              MaterialPaintLayer &row,
                                              const eMaterialPaintLayerFilter filter)
{
  MaterialPaintLayer *effect = BKE_paint_layers_mask_filter_add(ma, &row, filter, nullptr);
  EXPECT_NE(effect, nullptr);
  EXPECT_EQ(effect->role, MA_PAINT_LAYER_ROLE_MASK_ITEM);
  return effect;
}

/** An 8x8 byte image whose grey runs from black at the left edge to white at the right one. */
Image *bake_test_add_ramp_image(Main &bmain, const char *name)
{
  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *image = BKE_image_add_generated(
      &bmain, 8, 8, name, 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  image->alpha_mode = IMA_ALPHA_STRAIGHT;
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
  uchar *pixels = ibuf->byte_data_for_write();
  for (const int y : IndexRange(8)) {
    for (const int x : IndexRange(8)) {
      uchar *p = pixels + (int64_t(y) * 8 + x) * 4;
      const uchar value = uchar(x * 255 / 7);
      p[0] = value;
      p[1] = value;
      p[2] = value;
      p[3] = 255;
    }
  }
  BKE_image_release_ibuf(image, ibuf, lock);
  return image;
}

/** The linear red of a bake map's texel, the encoding the bake wrote. */
float bake_test_map_red(Image &image, const int x, const int y)
{
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(&image, nullptr, &lock);
  const float value = srgb_to_linearrgb(ibuf->byte_data()[(int64_t(y) * ibuf->x + x) * 4] / 255.0f);
  BKE_image_release_ibuf(&image, ibuf, lock);
  return value;
}

/** A \a width x \a height byte image whose grey runs from black at the left edge to white at the
 * right one: the ramp the rectangular-canvas test needs. */
Image *bake_test_add_ramp_image_sized(Main &bmain,
                                      const char *name,
                                      const int width,
                                      const int height)
{
  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *image = BKE_image_add_generated(
      &bmain, width, height, name, 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  image->alpha_mode = IMA_ALPHA_STRAIGHT;
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
  uchar *pixels = ibuf->byte_data_for_write();
  for (const int y : IndexRange(height)) {
    for (const int x : IndexRange(width)) {
      uchar *p = pixels + (int64_t(y) * width + x) * 4;
      const uchar value = uchar(x * 255 / (width - 1));
      p[0] = value;
      p[1] = value;
      p[2] = value;
      p[3] = 255;
    }
  }
  BKE_image_release_ibuf(image, ibuf, lock);
  return image;
}

}  // namespace

TEST_F(PaintLayersBakeTest, a_mask_effect_item_bakes_the_grey_the_composite_lays)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_image("Row"));
  ASSERT_NE(BKE_paint_layers_mask_add_image(*ma, *row, *bake_test_add_ramp_image(*bmain, "Mask")),
            nullptr);
  MaterialPaintLayer *blur = bake_test_add_mask_effect(*ma, *row, MA_PAINT_LAYER_FILTER_BLUR);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_SHAPE, PAINT_LAYER_BLUR_BOX));
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_RADIUS, 1));
  clear_active();

  /* The grey the CPU composite lays before any bake: the describe computes it into the cache. */
  Vector<float> live(64);
  {
    Vector<PaintMaterialCompositeImageLayer> layers;
    ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
        *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
    const PaintMaterialCompositeMaskItem *laid = nullptr;
    for (const PaintMaterialCompositeImageLayer &layer : layers) {
      for (const PaintMaterialCompositeMaskItem &item : layer.mask_items) {
        if (item.effect_filter == MA_PAINT_LAYER_FILTER_BLUR) {
          laid = &item;
        }
      }
    }
    ASSERT_NE(laid, nullptr);
    ASSERT_NE(laid->grey_buffer, nullptr);
    for (const int i : IndexRange(64)) {
      live[i] = laid->grey_buffer->float_data()[i * 4];
    }
  }

  bool changed = false;
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_TRUE(changed);
  ASSERT_NE(blur->bake, nullptr);
  ASSERT_NE(blur->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR], nullptr)
      << "the item's grey is a map of its own";
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *blur));
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed)) << "nothing is left due";

  /* The row itself needed no bake: its mask, the item included, is live over its content, so the
   * item's grey is the only cache the mask effect asked for. */
  EXPECT_EQ(row->bake, nullptr);

  /* The bake holds the same grey the composite laid, texel for texel, within the byte rounding of
   * the map's own encoding. The describe reads this map back once the bake stands, so the picture
   * the composite shows does not move across the bake. */
  Image *baked = blur->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  for (const int y : IndexRange(8)) {
    for (const int x : IndexRange(8)) {
      /* The map stores the grey encoded into the channel's colorspace; decoding reads the scene
       * linear value the cache held back. */
      const float stored_linear = bake_test_map_red(*baked, x, y);
      EXPECT_NEAR(stored_linear, live[y * 8 + x], 2.0f / 255.0f)
          << "the bake and the composite agree, at " << x << "," << y;
    }
  }
}

TEST_F(PaintLayersBakeTest, editing_a_mask_effects_parameters_rebakes_it)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_image("Row"));
  ASSERT_NE(BKE_paint_layers_mask_add_image(*ma, *row, *bake_test_add_ramp_image(*bmain, "Mask")),
            nullptr);
  MaterialPaintLayer *blur = bake_test_add_mask_effect(*ma, *row, MA_PAINT_LAYER_FILTER_BLUR);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_SHAPE, PAINT_LAYER_BLUR_BOX));
  clear_active();
  bool changed = false;
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  ASSERT_NE(blur->bake, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *blur));

  /* A wider radius is a different grey: the hash moves with the parameter and the item is due. */
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_RADIUS, 3));
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *blur));
  changed = false;
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_TRUE(changed);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *blur));

  /* The item's own opacity is laid over the grey by the reader: it moves no hash and asks for no
   * re-bake. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, blur, 0.4f));
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *blur)) << "the opacity is live over the map";
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
}

TEST_F(PaintLayersBakeTest, a_stroke_into_the_base_mask_defers_the_mask_effects_bake_until_it_ends)
{
  Image *mask_map = bake_test_add_ramp_image(*bmain, "Mask");
  MaterialPaintLayer *row = add_paint_layer("Row", add_image("Row"));
  ASSERT_NE(BKE_paint_layers_mask_add_image(*ma, *row, *mask_map), nullptr);
  MaterialPaintLayer *blur = bake_test_add_mask_effect(*ma, *row, MA_PAINT_LAYER_FILTER_BLUR);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_SHAPE, PAINT_LAYER_BLUR_BOX));
  clear_active();
  bool changed = false;
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  ASSERT_NE(blur->bake, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *blur));

  /* The stroke: a pixel edit of the base mask's map, the stroke still running. The grey is
   * computed over the whole canvas, so the item is not re-rendered at every dab: it stays deferred
   * and the mask shows live, without the effect. */
  BKE_paint_layers_bake_stroke_active_set(true);
  BKE_image_partial_update_mark_full_update(mask_map);
  BKE_paint_layers_bake_notice_changes(*ma);

  EXPECT_TRUE(BKE_paint_layers_bake_row_is_deferred(*ma, *blur));
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed))
      << "the stroke defers the re-bake";
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *blur))
      << "stale: the old grey is kept and not substituted";

  /* The stroke is over: the grey is baked once, while no marker has moved at all. */
  BKE_paint_layers_bake_stroke_active_set(false);
  EXPECT_FALSE(BKE_paint_layers_bake_row_is_deferred(*ma, *blur));
  changed = false;
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_TRUE(changed);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *blur));
}

TEST_F(PaintLayersBakeTest, a_stroke_into_the_rows_colour_leaves_the_mask_effects_bake_valid)
{
  Image *row_map = add_image("Row");
  MaterialPaintLayer *row = add_paint_layer("Row", row_map);
  ASSERT_NE(BKE_paint_layers_mask_add_image(*ma, *row, *bake_test_add_ramp_image(*bmain, "Mask")),
            nullptr);
  MaterialPaintLayer *blur = bake_test_add_mask_effect(*ma, *row, MA_PAINT_LAYER_FILTER_BLUR);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_SHAPE, PAINT_LAYER_BLUR_BOX));
  clear_active();
  bool changed = false;
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  ASSERT_NE(blur->bake, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *blur));

  /* The grey reads the mask, not the row's colour: an edit of the colour map reaches the item
   * neither through its subscription nor its hash. */
  BKE_paint_layers_bake_stroke_active_set(true);
  BKE_image_partial_update_mark_full_update(row_map);
  BKE_paint_layers_bake_notice_changes(*ma);
  EXPECT_FALSE(BKE_paint_layers_bake_row_is_deferred(*ma, *blur))
      << "the colour is no map of the grey";
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *blur));
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed)) << "nothing is due for it";
  BKE_paint_layers_bake_stroke_active_set(false);
}

TEST_F(PaintLayersBakeTest, a_mask_effects_bake_keeps_its_cache_entry)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_image("Row"));
  ASSERT_NE(BKE_paint_layers_mask_add_image(*ma, *row, *bake_test_add_ramp_image(*bmain, "Mask")),
            nullptr);
  MaterialPaintLayer *blur = bake_test_add_mask_effect(*ma, *row, MA_PAINT_LAYER_FILTER_BLUR);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_SHAPE, PAINT_LAYER_BLUR_BOX));
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_RADIUS, 1));
  clear_active();

  /* The describe computes the grey into the cache: the entry is what the preview reads while no
   * bake stands. */
  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  ASSERT_EQ(paint_layer_mask_effect_cache_entry_count(), 1);

  bool changed = false;
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  ASSERT_NE(blur->bake, nullptr);
  ASSERT_NE(blur->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR], nullptr);
  /* The entry the write went through stays: the live grey is the base the next regional patch
   * builds on, and dropping it would turn every bake into a full recompute again. The cache's LRU
   * budget, not the bake, retires it -- the rule the owned effects keep (#bake_mask_effect_row_write). */
  EXPECT_EQ(paint_layer_mask_effect_cache_entry_count(), 1);

  /* An edit that stales the bake has the grey already in the cache: the preview does not wait for
   * the bake to catch up, and the recompute replaces the entry in place. */
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_RADIUS, 2));
  Vector<PaintMaterialCompositeImageLayer> moved_layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, moved_layers));
  const PaintMaterialCompositeMaskItem *laid = nullptr;
  for (const PaintMaterialCompositeImageLayer &layer : moved_layers) {
    for (const PaintMaterialCompositeMaskItem &item : layer.mask_items) {
      if (item.effect_filter == MA_PAINT_LAYER_FILTER_BLUR) {
        laid = &item;
      }
    }
  }
  ASSERT_NE(laid, nullptr);
  EXPECT_NE(laid->grey_buffer, nullptr) << "the grey is computed while the bake is stale";
  EXPECT_EQ(paint_layer_mask_effect_cache_entry_count(), 1);
}

TEST_F(PaintLayersBakeTest, a_mask_effect_over_a_rectangular_mask_stretches_into_the_square_map)
{
  /* A 64x32 row map: the grey the effect reads is rectangular, while the bake map is square -- the
   * side the content's width names, the height stretched into it, the way a row's own bake lays
   * any source of its proportions into the square map. */
  MaterialPaintLayer *row = add_paint_layer(
      "Row", bake_test_add_ramp_image_sized(*bmain, "Row", 64, 32));
  ASSERT_NE(BKE_paint_layers_mask_add_image(*ma, *row, *bake_test_add_ramp_image(*bmain, "Mask")),
            nullptr);
  MaterialPaintLayer *blur = bake_test_add_mask_effect(*ma, *row, MA_PAINT_LAYER_FILTER_BLUR);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_SHAPE, PAINT_LAYER_BLUR_BOX));
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_RADIUS, 1));
  clear_active();

  /* The grey the composite lays, texel by texel: the base the bake is checked against. */
  Vector<float> live(64 * 32);
  {
    Vector<PaintMaterialCompositeImageLayer> layers;
    ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
        *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
    const PaintMaterialCompositeMaskItem *laid = nullptr;
    for (const PaintMaterialCompositeImageLayer &layer : layers) {
      for (const PaintMaterialCompositeMaskItem &item : layer.mask_items) {
        if (item.effect_filter == MA_PAINT_LAYER_FILTER_BLUR) {
          laid = &item;
        }
      }
    }
    ASSERT_NE(laid, nullptr);
    ASSERT_NE(laid->grey_buffer, nullptr);
    for (const int64_t i : IndexRange(64 * 32)) {
      live[i] = laid->grey_buffer->float_data()[i * 4];
    }
  }

  bool changed = false;
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  ASSERT_NE(blur->bake, nullptr);
  Image *baked = blur->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(baked, nullptr);
  {
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(baked, nullptr, &lock);
    ASSERT_NE(ibuf, nullptr);
    EXPECT_EQ(ibuf->x, 64) << "the map's side is the content's width";
    EXPECT_EQ(ibuf->y, 64) << "the map is square, whatever the grey's proportions";
    BKE_image_release_ibuf(baked, ibuf, lock);
  }

  /* The nearest sample per axis, at the corners and the centre: src_x = x (the widths agree),
   * src_y = y * 32 / 64 = y / 2. */
  constexpr int grey_width = 64;
  constexpr int grey_height = 32;
  const std::array<std::pair<int, int>, 7> probes = {
      std::make_pair(0, 0),
      std::make_pair(63, 0),
      std::make_pair(0, 63),
      std::make_pair(63, 63),
      std::make_pair(32, 32),
      std::make_pair(1, 1),
      std::make_pair(31, 17),
  };
  for (const auto &[x, y] : probes) {
    const int src_x = min_ii(int(int64_t(x) * grey_width / 64), grey_width - 1);
    const int src_y = min_ii(int(int64_t(y) * grey_height / 64), grey_height - 1);
    EXPECT_NEAR(bake_test_map_red(*baked, x, y), live[int64_t(src_y) * grey_width + src_x],
                2.0f / 255.0f)
        << "the bake samples the grey per axis, at " << x << "," << y;
  }
}

/** \} */

TEST_F(PaintLayersBakeTest, a_mask_item_contrast_edit_invalidates_the_parent_bake_once)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *row = add_baked_row("Row");
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, row, 1.0f), nullptr);
  MaterialPaintLayer *item = BKE_paint_layers_mask_item_add(
      *ma, row, MA_PAINT_LAYER_SOURCE_CONSTANT, "Mask Fill", 0.75f);
  ASSERT_NE(item, nullptr);
  /* A fresh Always row owns no map until the planner has baked it. */
  clear_active();
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma));
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *row));

  /* The contrast is part of the parent's pixels, like the item's opacity is. */
  EXPECT_TRUE(BKE_paint_layers_mask_contrast_set(*ma, item, 3.0f));
  EXPECT_FLOAT_EQ(item->mask_contrast, 2.0f) << "the stored value is the offset from 1";
  EXPECT_FLOAT_EQ(BKE_paint_layers_mask_contrast_get(*item), 3.0f);
  EXPECT_FLOAT_EQ(BKE_paint_layers_mask_contrast_apply(0.75f, 1.0f), 0.75f);
  EXPECT_FLOAT_EQ(BKE_paint_layers_mask_contrast_apply(0.75f, 3.0f), 1.0f);
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *row));
  bool changed = false;
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  changed = false;
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_FALSE(changed) << "no infinite bake: the re-baked hash matches the description";
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *row));
}

/* -------------------------------------------------------------------- */
/** \name A mask effect that reads the surface
 *
 * A mask effect whose grey is computed from a projected item before it is baked per object, like a
 * row (#paint_layers_bake_node_reads_surface); any other mask effect stays the one shared map.
 * \{ */

namespace {

/** A Fill mask item with a map, appended to \a row's mask stack; projected when \a space says so. */
MaterialPaintLayer *bake_test_add_mask_fill(Main &bmain,
                                            Material &ma,
                                            MaterialPaintLayer &row,
                                            const int16_t space)
{
  MaterialPaintLayer *fill = BKE_paint_layers_mask_item_add(
      ma, &row, MA_PAINT_LAYER_SOURCE_CONSTANT, "Mask Fill", 1.0f);
  EXPECT_NE(fill, nullptr);
  EXPECT_TRUE(BKE_paint_layers_channel_set_image(
      ma, fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR, bake_test_add_ramp_image(bmain, "Fill Map")));
  if (space != MA_PAINT_LAYER_MAPPING_SPACE_UV) {
    EXPECT_TRUE(BKE_paint_layers_projection_set_space(ma, fill, space));
  }
  return fill;
}

}  // namespace

TEST_F(PaintLayersBakeTest, a_mask_effect_reads_the_surface_only_after_a_projected_item)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_image("Row"));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, row, 1.0f), nullptr);
  MaterialPaintLayer *early = bake_test_add_mask_effect(*ma, *row, MA_PAINT_LAYER_FILTER_BLUR);
  MaterialPaintLayer *fill = bake_test_add_mask_fill(
      *bmain, *ma, *row, MA_PAINT_LAYER_MAPPING_SPACE_UV);
  MaterialPaintLayer *late = bake_test_add_mask_effect(*ma, *row, MA_PAINT_LAYER_FILTER_BLUR);
  ASSERT_NE(early, nullptr);
  ASSERT_NE(late, nullptr);
  ASSERT_NE(fill, nullptr);

  EXPECT_FALSE(paint_layers_bake_node_reads_surface(*ma, *early));
  EXPECT_FALSE(paint_layers_bake_node_reads_surface(*ma, *late)) << "a Fill in UV reads no surface";

  ASSERT_TRUE(BKE_paint_layers_projection_set_space(*ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_BOX));
  EXPECT_FALSE(paint_layers_bake_node_reads_surface(*ma, *early))
      << "an item after the effect is no part of its input";
  EXPECT_TRUE(paint_layers_bake_node_reads_surface(*ma, *late));

  /* A stack row keeps the answer it always had, whatever its mask holds. */
  EXPECT_EQ(paint_layers_bake_node_reads_surface(*ma, *row),
            paint_layers_bake_subtree_needs_surface(*row));

  ASSERT_TRUE(BKE_paint_layers_projection_set_space(*ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_UV));
  EXPECT_FALSE(paint_layers_bake_node_reads_surface(*ma, *late))
      << "the projection stopped applying";
}

TEST_F(PaintLayersBakeTest, a_mask_effect_without_a_projected_item_keeps_its_shared_map_and_hash)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_image("Row"));
  ASSERT_NE(BKE_paint_layers_mask_add_image(*ma, *row, *bake_test_add_ramp_image(*bmain, "Mask")),
            nullptr);
  MaterialPaintLayer *blur = bake_test_add_mask_effect(*ma, *row, MA_PAINT_LAYER_FILTER_BLUR);
  ASSERT_NE(blur, nullptr);
  clear_active();

  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma));
  ASSERT_NE(blur->bake, nullptr);
  Image *grey = blur->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(grey, nullptr);
  EXPECT_NE(grey->source, IMA_SRC_TILED) << "one map for every object";
  EXPECT_TRUE(BLI_listbase_is_empty(&blur->bake->tiles));
  EXPECT_EQ(blur->bake->object_count, 0);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *blur));

  /* A projected item after the effect is no part of what it reads: the bake stays current. */
  uint32_t before[2];
  BKE_paint_layers_bake_hash(*ma, *blur, before);
  MaterialPaintLayer *fill = bake_test_add_mask_fill(
      *bmain, *ma, *row, MA_PAINT_LAYER_MAPPING_SPACE_BOX);
  ASSERT_NE(fill, nullptr);
  uint32_t after[2];
  BKE_paint_layers_bake_hash(*ma, *blur, after);
  EXPECT_EQ(before[0], after[0]);
  EXPECT_EQ(before[1], after[1]);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *blur));
}

/**
 * An older file could carry Mirror bits on a row that never owned the fold; before the setters
 * refused them, a script could park them there too. Every reader treats them as off, so they must
 * not make a visually equal row hash apart and force a re-bake.
 */
TEST_F(PaintLayersBakeTest, stale_mirror_bits_in_a_fill_row_do_not_move_the_bake_hash)
{
  /* The projection fold is where a content row's raw mapping flag reaches the hash. */
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "Projected", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_source(*ma, row, MA_PAINT_LAYER_SOURCE_CONSTANT));
  add_channel(*row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("Projected"));
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(*ma, row, MA_PAINT_LAYER_MAPPING_SPACE_BOX));
  ASSERT_NE(BKE_paint_layers_filter_add(*ma, row, MA_PAINT_LAYER_FILTER_NOISE), nullptr);
  ASSERT_TRUE(paint_layer_row_folds_into_bake(*ma, *row));
  clear_active();

  uint32_t clean[2];
  BKE_paint_layers_bake_hash(*ma, *row, clean);

  /* Written straight into the DNA: how a file from before the fold moved to the effect reads. */
  row->mapping.flag |= MA_PAINT_LAYER_MAPPING_MIRROR_U | MA_PAINT_LAYER_MAPPING_MIRROR_V |
                       MA_PAINT_LAYER_MAPPING_MIRROR_U_NEGATIVE |
                       MA_PAINT_LAYER_MAPPING_MIRROR_ENABLED;
  uint32_t stale[2];
  BKE_paint_layers_bake_hash(*ma, *row, stale);
  EXPECT_EQ(clean[0], stale[0]);
  EXPECT_EQ(clean[1], stale[1]);
}

TEST_F(PaintLayersBakeTest, a_mask_effect_over_a_projected_item_waits_for_objects)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_image("Row"));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, row, 1.0f), nullptr);
  ASSERT_NE(bake_test_add_mask_fill(*bmain, *ma, *row, MA_PAINT_LAYER_MAPPING_SPACE_BOX), nullptr);
  MaterialPaintLayer *blur = bake_test_add_mask_effect(*ma, *row, MA_PAINT_LAYER_FILTER_BLUR);
  ASSERT_NE(blur, nullptr);
  clear_active();

  /* No scene: there is no surface to read the projection over, so nothing is baked, and what a
   * shared bake would have held is not written. */
  BKE_paint_layers_bake_plan_run(*bmain, *ma);
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *blur));
  EXPECT_TRUE(blur->bake == nullptr || blur->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR] == nullptr);
}

TEST_F(PaintLayersBakeTest, a_mask_effect_over_a_projected_item_bakes_one_tile_per_object)
{
  Scene *scene = BKE_scene_add(bmain, "BakeScene");
  Object *first = add_uv_quad_object(*scene, "First");
  update_scene(*scene);
  ViewLayer *view_layer = static_cast<ViewLayer *>(scene->view_layers.first);

  MaterialPaintLayer *row = add_paint_layer("Row", add_image("Row"));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, row, 1.0f), nullptr);
  ASSERT_NE(bake_test_add_mask_fill(*bmain, *ma, *row, MA_PAINT_LAYER_MAPPING_SPACE_BOX), nullptr);
  MaterialPaintLayer *blur = bake_test_add_mask_effect(*ma, *row, MA_PAINT_LAYER_FILTER_BLUR);
  ASSERT_NE(blur, nullptr);
  clear_active();

  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, nullptr, scene, view_layer));
  ASSERT_NE(blur->bake, nullptr);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *blur));
  EXPECT_EQ(blur->bake->object_count, 1);
  ASSERT_EQ(BLI_listbase_count(&blur->bake->tiles), 1);
  Image *grey = blur->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(grey, nullptr);
  EXPECT_EQ(grey->source, IMA_SRC_TILED);
  EXPECT_NE(BKE_image_get_tile(grey, 1001), nullptr);

  /* The tile carries the surface identity: an edited mesh is another tile hash after the re-bake. */
  const MaterialPaintLayerBakeTile *tile = static_cast<const MaterialPaintLayerBakeTile *>(
      blur->bake->tiles.first);
  const uint32_t hash_before[2] = {tile->hash[0], tile->hash[1]};
  Mesh *mesh = id_cast<Mesh *>(first->data);
  mesh->vert_positions_for_write()[0].x += 0.5f;
  mesh->tag_positions_changed();
  DEG_id_tag_update(&mesh->id, ID_RECALC_GEOMETRY);
  update_scene(*scene);
  BKE_paint_layers_bake_plan_run(*bmain, *ma, nullptr, scene, view_layer);
  tile = static_cast<const MaterialPaintLayerBakeTile *>(blur->bake->tiles.first);
  ASSERT_NE(tile, nullptr);
  EXPECT_TRUE(tile->hash[0] != hash_before[0] || tile->hash[1] != hash_before[1]);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *blur));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Coverage culling of reported edit regions
 * \{ */

namespace {

/** Mark \a region as changed in \a image's partial-update log, the way a paint writer reports its
 * edits, on the image's first tile. */
void bake_test_mark_region(Image &image, const rcti &region)
{
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(&image, nullptr, &lock);
  EXPECT_NE(ibuf, nullptr);
  if (ibuf == nullptr) {
    return;
  }
  /* The log keys its changesets by tile, so a null tile is not a valid stand-in for the first. */
  const ImageTile *tile = BKE_image_get_tile(&image, 1001);
  EXPECT_NE(tile, nullptr);
  BKE_image_partial_update_mark_region(&image, tile, ibuf, &region);
  BKE_image_release_ibuf(&image, ibuf, lock);
}

/**
 * A map two partial-update chunks wide. The log reports an edit as the chunks it touches (256 px
 * squares, not clamped to the image), so a region can only lie outside a half of the map when that
 * half is a chunk of its own; the regions the culling tests read back are therefore compared as
 * "covers the edit", not as the edit's exact rectangle.
 */
Image *bake_test_add_wide_image(Main &bmain, const char *name)
{
  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  return BKE_image_add_generated(
      &bmain, 512, 256, name, 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
}

/**
 * Make the right half of \a image's map transparent, so the row covers only its left half: a
 * reported edit region on the right lies outside everything the row paints. Returns the
 * transparent rectangle.
 */
rcti bake_test_clear_right_half_alpha(Image &image)
{
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(&image, nullptr, &lock);
  EXPECT_NE(ibuf, nullptr);
  if (ibuf == nullptr) {
    return rcti{0, 0, 0, 0};
  }
  const int width = ibuf->x;
  const int height = ibuf->y;
  const int half = width / 2;
  uchar *pixels = ibuf->byte_data_for_write();
  EXPECT_NE(pixels, nullptr);
  if (pixels != nullptr) {
    for (const int y : IndexRange(height)) {
      for (int x = half; x < width; x++) {
        pixels[(int64_t(y) * width + x) * 4 + 3] = 0;
      }
    }
  }
  BKE_image_release_ibuf(&image, ibuf, lock);
  return rcti{half, width, 0, height};
}

}  // namespace

/**
 * A pointwise bake (Noise reads only its own texel) cannot move past the edited rectangle, so a
 * reported region that lies outside the row's content coverage is dropped before it can mark the
 * row stale. The same mark over a covered part of the map is kept: the culling answers "is there
 * coverage here", not "drop what I do not recognise".
 */
TEST_F(PaintLayersBakeTest, an_uncovered_edit_region_is_dropped_for_a_pointwise_bake)
{
  Image *map = bake_test_add_wide_image(*bmain, "Row");
  MaterialPaintLayer *row = add_paint_layer("Row", map);
  ASSERT_NE(BKE_paint_layers_filter_add(*ma, row, MA_PAINT_LAYER_FILTER_NOISE), nullptr);
  clear_active();
  const rcti uncovered = bake_test_clear_right_half_alpha(*map);
  ASSERT_GT(uncovered.xmax, uncovered.xmin);

  /* Prime the journal before the subscription so its baseline drain sees the priming mark and
   * later reports only what this test marks. */
  bake_test_mark_region(*map, rcti{0, 0, 0, 0});
  BKE_paint_layers_bake_subscribe(*ma, *row);
  /* The setup's structural edits mark the material stale on their own; the culling is what this
   * test reads. */
  ma->paint_layers_flag &= ~MA_PAINT_LAYERS_BAKE_STALE;

  bake_test_mark_region(*map, uncovered);
  BKE_paint_layers_bake_notice_changes(*ma);
  int region[4] = {};
  EXPECT_FALSE(BKE_paint_layers_bake_changed_region(*ma, *row, region))
      << "a pointwise bake cannot move through an uncovered region, so it is dropped";
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_BAKE_STALE, 0)
      << "a dropped region must not make the material stale";

  /* The covered half is a real edit: the same row keeps it. */
  const rcti covered{0, uncovered.xmin, uncovered.ymin, uncovered.ymax};
  bake_test_mark_region(*map, covered);
  BKE_paint_layers_bake_notice_changes(*ma);
  ASSERT_TRUE(BKE_paint_layers_bake_changed_region(*ma, *row, region));
  EXPECT_LE(region[0], covered.xmin);
  EXPECT_GE(region[1], covered.xmax);
  EXPECT_LE(region[2], covered.ymin);
  EXPECT_GE(region[3], covered.ymax);
}

/**
 * A neighbourhood bake (Blur reads past the edited texel) keeps an uncovered region: a pixel edit
 * just outside the coverage can still change the blur inside it, so dropping the region would
 * leave the covered neighbours stale in the bake.
 */
TEST_F(PaintLayersBakeTest, an_uncovered_edit_region_is_kept_for_a_neighbourhood_bake)
{
  Image *map = add_image("Row");
  MaterialPaintLayer *row = add_paint_layer("Row", map);
  ASSERT_NE(BKE_paint_layers_filter_add(*ma, row, MA_PAINT_LAYER_FILTER_BLUR), nullptr);
  clear_active();
  const rcti uncovered = bake_test_clear_right_half_alpha(*map);
  ASSERT_GT(uncovered.xmax, uncovered.xmin);

  bake_test_mark_region(*map, rcti{0, 0, 0, 0});
  BKE_paint_layers_bake_subscribe(*ma, *row);

  bake_test_mark_region(*map, uncovered);
  BKE_paint_layers_bake_notice_changes(*ma);
  int region[4] = {};
  ASSERT_TRUE(BKE_paint_layers_bake_changed_region(*ma, *row, region))
      << "a blur reaches past the edited rectangle, so its region is kept whole";
  EXPECT_LE(region[0], uncovered.xmin);
  EXPECT_GE(region[1], uncovered.xmax);
  EXPECT_LE(region[2], uncovered.ymin);
  EXPECT_GE(region[3], uncovered.ymax);
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_BAKE_STALE, 0);
}

namespace {

/**
 * A chain instance laid on \a row: a Smart Filter definition whose effect members are \a filters,
 * saved and inserted through the asset path. Null when the definition or the landing failed.
 */
MaterialPaintLayer *bake_test_add_chain_instance(
    Main &bmain,
    Material &ma,
    MaterialPaintLayer &row,
    const char *name,
    const Span<const eMaterialPaintLayerFilter> filters)
{
  Material *src = BKE_material_add(&bmain, name);
  src->paint_layers_flag |= MA_PAINT_LAYERED;
  MaterialPaintLayer *holder = BKE_paint_layers_add(
      *src, "Members", nullptr, PaintLayerPlace::Above);
  if (holder == nullptr) {
    return nullptr;
  }
  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  if (BKE_paint_layers_channel_add(*src, holder, PAINT_MATERIAL_CHANNEL_BASE_COLOR) == nullptr ||
      !BKE_paint_layers_channel_set_value(*src, holder, PAINT_MATERIAL_CHANNEL_BASE_COLOR, white))
  {
    return nullptr;
  }
  Vector<const MaterialPaintLayer *> members;
  for (const eMaterialPaintLayerFilter filter : filters) {
    MaterialPaintLayer *member = BKE_paint_layers_filter_add(*src, holder, filter, nullptr);
    if (member == nullptr) {
      return nullptr;
    }
    members.append(member);
  }
  Material *definition = BKE_paint_layers_asset_create(
      bmain, *src, members.as_span(), PaintLayerAssetType::Filter, name);
  if (definition == nullptr) {
    return nullptr;
  }
  PaintLayerImportResult result;
  if (!BKE_paint_layers_asset_insert(
          bmain, ma, *definition, &row, PaintLayerPlace::Above, &result))
  {
    return nullptr;
  }
  for (MaterialPaintLayer &effect : row.effects) {
    if (effect.kind_subtype == MA_PAINT_LAYER_FILTER_CHAIN) {
      return &effect;
    }
  }
  return nullptr;
}

}  // namespace

/**
 * A chain whose members are all pointwise corrections is a pointwise bake: the composite lays each
 * member's filter per texel, so an uncovered region cannot move the chain's result.
 */
TEST_F(PaintLayersBakeTest, an_uncovered_edit_region_is_dropped_for_a_pointwise_chain)
{
  /* The wide map, so the right half is a partial-update chunk of its own: a narrow map reports the
   * half edit as the whole 256-px chunk it falls in, which is covered. */
  Image *map = bake_test_add_wide_image(*bmain, "Row");
  MaterialPaintLayer *row = add_paint_layer("Row", map);
  const eMaterialPaintLayerFilter filters[] = {MA_PAINT_LAYER_FILTER_EXPOSURE,
                                               MA_PAINT_LAYER_FILTER_CONTRAST_LUMINOSITY};
  ASSERT_NE(bake_test_add_chain_instance(*bmain,
                                         *ma,
                                         *row,
                                         "PointwiseChain",
                                         Span<const eMaterialPaintLayerFilter>(
                                             filters, int(std::size(filters)))),
            nullptr);
  clear_active();
  const rcti uncovered = bake_test_clear_right_half_alpha(*map);
  ASSERT_GT(uncovered.xmax, uncovered.xmin);

  bake_test_mark_region(*map, rcti{0, 0, 0, 0});
  BKE_paint_layers_bake_subscribe(*ma, *row);
  ma->paint_layers_flag &= ~MA_PAINT_LAYERS_BAKE_STALE;

  bake_test_mark_region(*map, uncovered);
  BKE_paint_layers_bake_notice_changes(*ma);
  int region[4] = {};
  EXPECT_FALSE(BKE_paint_layers_bake_changed_region(*ma, *row, region))
      << "a pointwise chain cannot move through an uncovered region, so it is dropped";
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_BAKE_STALE, 0)
      << "a dropped region must not make the material stale";
}

/**
 * A chain with a neighbourhood member (a Blur) reads past the edited texel through that member, so
 * the whole chain keeps an uncovered region, as the member alone would.
 */
TEST_F(PaintLayersBakeTest, an_uncovered_edit_region_is_kept_for_a_neighbourhood_chain)
{
  Image *map = add_image("Row");
  MaterialPaintLayer *row = add_paint_layer("Row", map);
  const eMaterialPaintLayerFilter filters[] = {MA_PAINT_LAYER_FILTER_EXPOSURE,
                                               MA_PAINT_LAYER_FILTER_BLUR};
  ASSERT_NE(bake_test_add_chain_instance(*bmain,
                                         *ma,
                                         *row,
                                         "BlurChain",
                                         Span<const eMaterialPaintLayerFilter>(
                                             filters, int(std::size(filters)))),
            nullptr);
  clear_active();
  const rcti uncovered = bake_test_clear_right_half_alpha(*map);
  ASSERT_GT(uncovered.xmax, uncovered.xmin);

  bake_test_mark_region(*map, rcti{0, 0, 0, 0});
  BKE_paint_layers_bake_subscribe(*ma, *row);

  bake_test_mark_region(*map, uncovered);
  BKE_paint_layers_bake_notice_changes(*ma);
  int region[4] = {};
  ASSERT_TRUE(BKE_paint_layers_bake_changed_region(*ma, *row, region))
      << "a blur member reaches past the edited rectangle, so the region is kept whole";
  EXPECT_LE(region[0], uncovered.xmin);
  EXPECT_GE(region[1], uncovered.xmax);
  EXPECT_LE(region[2], uncovered.ymin);
  EXPECT_GE(region[3], uncovered.ymax);
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_BAKE_STALE, 0);
}

/**
 * A chain with no definition has no answer to fold: nothing is known about what it would lay, so
 * an uncovered region is kept rather than culled on a guess.
 */
TEST_F(PaintLayersBakeTest, an_uncovered_edit_region_is_kept_for_a_chain_without_a_definition)
{
  Image *map = add_image("Row");
  MaterialPaintLayer *row = add_paint_layer("Row", map);
  MaterialPaintLayer *chain = BKE_paint_layers_filter_add(
      *ma, row, MA_PAINT_LAYER_FILTER_CHAIN);
  ASSERT_NE(chain, nullptr);
  EXPECT_EQ(chain->material, nullptr);
  clear_active();
  const rcti uncovered = bake_test_clear_right_half_alpha(*map);
  ASSERT_GT(uncovered.xmax, uncovered.xmin);

  bake_test_mark_region(*map, rcti{0, 0, 0, 0});
  BKE_paint_layers_bake_subscribe(*ma, *row);

  bake_test_mark_region(*map, uncovered);
  BKE_paint_layers_bake_notice_changes(*ma);
  int region[4] = {};
  ASSERT_TRUE(BKE_paint_layers_bake_changed_region(*ma, *row, region))
      << "a chain with no definition is not known to be pointwise, so its region is kept";
  EXPECT_LE(region[0], uncovered.xmin);
  EXPECT_GE(region[1], uncovered.xmax);
  EXPECT_LE(region[2], uncovered.ymin);
  EXPECT_GE(region[3], uncovered.ymax);
}

/**
 * A whole-map bake (a Color Match reads a statistic of the map) cannot be narrowed to the edited
 * rectangle: its result spans the canvas from every texel, so an uncovered region is kept.
 */
TEST_F(PaintLayersBakeTest, an_uncovered_edit_region_is_kept_for_a_whole_map_effect)
{
  Image *map = add_image("Row");
  MaterialPaintLayer *row = add_paint_layer("Row", map);
  ASSERT_NE(BKE_paint_layers_filter_add(*ma, row, MA_PAINT_LAYER_FILTER_COLOR_MATCH), nullptr);
  clear_active();
  const rcti uncovered = bake_test_clear_right_half_alpha(*map);
  ASSERT_GT(uncovered.xmax, uncovered.xmin);

  bake_test_mark_region(*map, rcti{0, 0, 0, 0});
  BKE_paint_layers_bake_subscribe(*ma, *row);
  ma->paint_layers_flag &= ~MA_PAINT_LAYERS_BAKE_STALE;

  bake_test_mark_region(*map, uncovered);
  BKE_paint_layers_bake_notice_changes(*ma);
  int region[4] = {};
  ASSERT_TRUE(BKE_paint_layers_bake_changed_region(*ma, *row, region))
      << "a whole-map effect reads every texel, so its region is kept whole";
  EXPECT_LE(region[0], uncovered.xmin);
  EXPECT_GE(region[1], uncovered.xmax);
  EXPECT_LE(region[2], uncovered.ymin);
  EXPECT_GE(region[3], uncovered.ymax);
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_BAKE_STALE, 0);
}

/**
 * A mapping folded into the bake (a rotation over a row with a CPU effect) moves the edited pixel to
 * the mapped rectangle, which the plane's rectangle cannot see: the region is kept.
 */
TEST_F(PaintLayersBakeTest, an_uncovered_edit_region_is_kept_for_a_folded_mapping)
{
  /* Only a Fill's own map supports a mapping: a painted map is written at the raw UV. */
  Image *map = add_image("Row");
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "Row", nullptr, PaintLayerPlace::Above);
  ASSERT_TRUE(BKE_paint_layers_set_source(*ma, row, MA_PAINT_LAYER_SOURCE_CONSTANT));
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, row, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_image(*ma, row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map));
  ASSERT_NE(BKE_paint_layers_filter_add(*ma, row, MA_PAINT_LAYER_FILTER_NOISE), nullptr);
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, row, true));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_rotation(*ma, row, 0.5f));
  clear_active();
  const rcti uncovered = bake_test_clear_right_half_alpha(*map);
  ASSERT_GT(uncovered.xmax, uncovered.xmin);

  bake_test_mark_region(*map, rcti{0, 0, 0, 0});
  BKE_paint_layers_bake_subscribe(*ma, *row);
  ma->paint_layers_flag &= ~MA_PAINT_LAYERS_BAKE_STALE;

  bake_test_mark_region(*map, uncovered);
  BKE_paint_layers_bake_notice_changes(*ma);
  int region[4] = {};
  ASSERT_TRUE(BKE_paint_layers_bake_changed_region(*ma, *row, region))
      << "a folded mapping moves the edit to the mapped rectangle, so the region is kept";
  EXPECT_LE(region[0], uncovered.xmin);
  EXPECT_GE(region[1], uncovered.xmax);
  EXPECT_LE(region[2], uncovered.ymin);
  EXPECT_GE(region[3], uncovered.ymax);
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_BAKE_STALE, 0);
}

/**
 * A projected row reads its map through the surface: the bake pixels an edit moves live in the
 * frame's space, which the plane's rectangle cannot see, so the region is kept.
 */
TEST_F(PaintLayersBakeTest, an_uncovered_edit_region_is_kept_for_a_projection)
{
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "Projected", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_source(*ma, row, MA_PAINT_LAYER_SOURCE_CONSTANT));
  Image *map = add_image("Projected");
  add_channel(*row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map);
  ASSERT_TRUE(
      BKE_paint_layers_projection_set_space(*ma, row, MA_PAINT_LAYER_MAPPING_SPACE_TRIPLANAR));
  clear_active();
  const rcti uncovered = bake_test_clear_right_half_alpha(*map);
  ASSERT_GT(uncovered.xmax, uncovered.xmin);

  bake_test_mark_region(*map, rcti{0, 0, 0, 0});
  BKE_paint_layers_bake_subscribe(*ma, *row);
  ma->paint_layers_flag &= ~MA_PAINT_LAYERS_BAKE_STALE;

  bake_test_mark_region(*map, uncovered);
  BKE_paint_layers_bake_notice_changes(*ma);
  int region[4] = {};
  ASSERT_TRUE(BKE_paint_layers_bake_changed_region(*ma, *row, region))
      << "a projection reads through the surface, so the plane's rectangle cannot narrow it";
  EXPECT_LE(region[0], uncovered.xmin);
  EXPECT_GE(region[1], uncovered.xmax);
  EXPECT_LE(region[2], uncovered.ymin);
  EXPECT_GE(region[3], uncovered.ymax);
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_BAKE_STALE, 0);
}

/**
 * The band is scaled by one channel's dimensions; a row whose source maps differ in size has no
 * single scale, so the helper refuses the band and the row keeps its whole/footprint path.
 */
TEST_F(PaintLayersBakeTest, a_mixed_size_row_refuses_the_band)
{
  Image *base = bake_test_add_ramp_image_sized(*bmain, "MixedBase", 16, 16);
  Image *metallic = bake_test_add_ramp_image_sized(*bmain, "MixedMetallic", 8, 8);
  MaterialPaintLayer *mixed = add_paint_layer("Mixed", base, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  add_channel(*mixed, PAINT_MATERIAL_CHANNEL_METALLIC, metallic);
  EXPECT_FALSE(bake_row_band_dimensions_match(*ma, *mixed, 16, 16));

  Image *even_base = bake_test_add_ramp_image_sized(*bmain, "EvenBase", 16, 16);
  Image *even_metal = bake_test_add_ramp_image_sized(*bmain, "EvenMetallic", 16, 16);
  MaterialPaintLayer *even = add_paint_layer("Even", even_base, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  add_channel(*even, PAINT_MATERIAL_CHANNEL_METALLIC, even_metal);
  /* The positive half asks the row for its own dimensions, so a row whose map is not 16x16 cannot
   * pass by asserting a size it does not have. */
  int even_width = 0;
  int even_height = 0;
  ASSERT_TRUE(BKE_paint_layers_row_dimensions(
      *ma, *even, PAINT_MATERIAL_CHANNEL_BASE_COLOR, even_width, even_height));
  EXPECT_EQ(even_width, 16);
  EXPECT_EQ(even_height, 16);
  EXPECT_TRUE(bake_row_band_dimensions_match(*ma, *even, even_width, even_height));
}

/**
 * A freed bake map leaves a dangling pointer in the runtime slot. The validity record the
 * subscription takes must store no map for it, so the bake stops counting as current instead of
 * dereferencing freed memory.
 */
TEST_F(PaintLayersBakeTest, a_freed_bake_slot_is_recorded_as_no_map)
{
  MaterialPaintLayer *row = add_baked_row("FreedSlot");
  clear_active();
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma));
  ASSERT_NE(row->bake, nullptr);
  const int channel = int(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  Image *map = row->bake->images[channel];
  ASSERT_NE(map, nullptr);

  /* An undo or a file reset can free the map while the slot keeps the pointer. */
  BKE_id_free(bmain, map);

  BKE_paint_layers_bake_subscribe(*ma, *row);
  {
    std::lock_guard lock(bake_runtime_mutex());
    const BakeMaterialRuntime *runtime = bake_runtime().lookup_ptr(ma->id.session_uid);
    ASSERT_NE(runtime, nullptr);
    const auto *maps = runtime->baked_maps.lookup_ptr(row->marker);
    ASSERT_NE(maps, nullptr);
    ASSERT_EQ((*maps)[channel].image, nullptr)
        << "a dead bake slot is recorded as no map, never as a dangling pointer";
    EXPECT_EQ((*maps)[channel].session_uid, 0u);
  }
  EXPECT_FALSE(BKE_paint_layers_bake_maps_are_current(*ma, *row))
      << "a bake whose map was freed is not current";
  /* Leave no dangling pointer in the runtime slot for teardown or a later check. */
  row->bake->images[channel] = nullptr;
}

TEST_F(PaintLayersBakeTest, a_tiled_map_missing_a_tile_is_not_a_service_reuse)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *row = BKE_paint_layers_gradient_add(
      *ma, "Gradient", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *row, MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT));
  Image *base = row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(base, nullptr);
  ASSERT_NE(BKE_image_get_tile(base, 1001), nullptr);
  ASSERT_EQ(BKE_image_get_tile(base, 1002), nullptr);

  /* Service equivalence, not a band-safety property: the predicate answers what
   * #bake_service_image would hand back. The band-safety gate is the plan-time uid snapshot, which
   * is tested below. */
  EXPECT_FALSE(bake_service_image_would_reuse(
      *bmain, *row, int(PAINT_MATERIAL_CHANNEL_BASE_COLOR), 8, false, 2));

  ASSERT_TRUE(BKE_paint_layers_map_coverage_tile_ensure(*base, 1002));
  EXPECT_TRUE(bake_service_image_would_reuse(
      *bmain, *row, int(PAINT_MATERIAL_CHANNEL_BASE_COLOR), 8, false, 2));
}

TEST_F(PaintLayersBakeTest, a_tiled_map_missing_a_tile_records_no_plan_uid)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *row = BKE_paint_layers_gradient_add(
      *ma, "Gradient", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *row, MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT));
  Image *base = row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  Image *coverage = row->bake->coverage;
  ASSERT_NE(base, nullptr);
  ASSERT_NE(coverage, nullptr);
  ASSERT_NE(BKE_image_get_tile(base, 1001), nullptr);
  ASSERT_EQ(BKE_image_get_tile(base, 1002), nullptr);
  ASSERT_EQ(BKE_image_get_tile(coverage, 1002), nullptr);

  /* The plan asks for two objects: tile 1002 is missing now, so neither the channel map nor the
   * coverage may be recorded as a map the band can write into. Another object's service call would
   * grow the tile, which is exactly why the snapshot is taken before any object renders. */
  uint32_t coverage_uid = 0;
  std::array<uint32_t, PAINT_MATERIAL_CHANNEL_NUM> map_uid =
      bake_plan_standing_map_uids(*bmain, *row, 8, 2, {}, coverage_uid);
  EXPECT_EQ(map_uid[PAINT_MATERIAL_CHANNEL_BASE_COLOR], 0u)
      << "a map missing a requested tile records no identity for the band";
  EXPECT_EQ(coverage_uid, 0u);

  ASSERT_TRUE(BKE_paint_layers_map_coverage_tile_ensure(*base, 1002));
  ASSERT_TRUE(BKE_paint_layers_map_coverage_tile_ensure(*coverage, 1002));
  map_uid = bake_plan_standing_map_uids(*bmain, *row, 8, 2, {}, coverage_uid);
  EXPECT_NE(map_uid[PAINT_MATERIAL_CHANNEL_BASE_COLOR], 0u);
  EXPECT_NE(coverage_uid, 0u);
}

TEST_F(PaintLayersBakeTest, a_new_object_tile_is_filled_after_the_first_bake)
{
  ma->paint_layers_image_size = 8;
  Scene *scene = BKE_scene_add(bmain, "BakeScene");
  add_uv_quad_object(*scene, "First");
  add_uv_quad_object(*scene, "Second");
  update_scene(*scene);
  ViewLayer *view_layer = static_cast<ViewLayer *>(scene->view_layers.first);

  MaterialPaintLayer *row = BKE_paint_layers_gradient_add(
      *ma, "Object", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *row, MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT));
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *row, MA_PAINT_LAYER_BAKE_ALWAYS));
  clear_active();

  PaintLayersBakeJob *job = BKE_paint_layers_bake_job_create(*bmain, *ma, scene, view_layer);
  ASSERT_NE(job, nullptr);
  BKE_paint_layers_bake_job_compute(*job, {}, {});
  ASSERT_TRUE(BKE_paint_layers_bake_job_commit(*job));
  BKE_paint_layers_bake_job_free(*job);

  Image *base = row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(base, nullptr);
  ASSERT_NE(BKE_image_get_tile(base, 1002), nullptr);

  /* A third object grows a new tile. Measured: adding it makes the row due with no subscription
   * band (the object list is part of the bake, not a pixel edit), so the bake is whole and the new
   * tile is filled like the ones a first bake laid. */
  add_uv_quad_object(*scene, "Third");
  update_scene(*scene);
  int band[4] = {};
  EXPECT_FALSE(BKE_paint_layers_bake_changed_region(*ma, *row, band))
      << "a new object has no standing-region edit to narrow the bake";

  job = BKE_paint_layers_bake_job_create(*bmain, *ma, scene, view_layer);
  ASSERT_NE(job, nullptr);
  BKE_paint_layers_bake_job_compute(*job, {}, {});
  ASSERT_TRUE(BKE_paint_layers_bake_job_commit(*job));
  BKE_paint_layers_bake_job_free(*job);

  Image *after = row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  ASSERT_NE(after, nullptr);
  ASSERT_NE(BKE_image_get_tile(after, 1003), nullptr)
      << "the new object's tile was grown";
  ASSERT_NE(row->bake->coverage, nullptr);
  EXPECT_NE(BKE_image_get_tile(row->bake->coverage, 1003), nullptr);

  /* The three objects are identical quads and the gradient reads object space, so their tiles
   * carry the same picture: a rectangle-only write into the blank new tile would differ. */
  const Vector<uchar> first = tile_pixel_bytes(*after, 1001);
  const Vector<uchar> third = tile_pixel_bytes(*after, 1003);
  ASSERT_EQ(first.size(), third.size());
  ASSERT_FALSE(first.is_empty());
  EXPECT_EQ(first, third) << "the new tile is filled by the bake, not left blank outside a band";
}

/**
 * The coverage plane is the row's content map, not whichever source reported the edit: a region of
 * another of the row's images has no rectangle in the plane's pixel space to test, so it is kept.
 */
TEST_F(PaintLayersBakeTest, an_edit_region_of_another_image_is_kept)
{
  Image *base = add_image("Base");
  Image *roughness = add_image("Roughness");
  MaterialPaintLayer *row = add_paint_layer("Row", base, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  add_channel(*row, PAINT_MATERIAL_CHANNEL_ROUGHNESS, roughness);
  ASSERT_NE(BKE_paint_layers_filter_add(*ma, row, MA_PAINT_LAYER_FILTER_NOISE), nullptr);
  clear_active();
  const rcti uncovered = bake_test_clear_right_half_alpha(*roughness);
  ASSERT_GT(uncovered.xmax, uncovered.xmin);

  bake_test_mark_region(*roughness, rcti{0, 0, 0, 0});
  BKE_paint_layers_bake_subscribe(*ma, *row);
  ma->paint_layers_flag &= ~MA_PAINT_LAYERS_BAKE_STALE;

  bake_test_mark_region(*roughness, uncovered);
  BKE_paint_layers_bake_notice_changes(*ma);
  int region[4] = {};
  ASSERT_TRUE(BKE_paint_layers_bake_changed_region(*ma, *row, region))
      << "the coverage plane is another image, so the region cannot be tested against it";
  EXPECT_LE(region[0], uncovered.xmin);
  EXPECT_GE(region[1], uncovered.xmax);
  EXPECT_LE(region[2], uncovered.ymin);
  EXPECT_GE(region[3], uncovered.ymax);
}

/**
 * A mask item that bakes a CPU effect owns its grey: its coverage is the mask it reads, which this
 * consumer does not walk to, so every region is kept (the plane stays #BakeCoveragePlane::Ignore).
 */
TEST_F(PaintLayersBakeTest, an_edit_region_of_a_mask_effect_item_is_kept)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_image("Row"));
  Image *mask = bake_test_add_ramp_image(*bmain, "Mask");
  ASSERT_NE(BKE_paint_layers_mask_add_image(*ma, *row, *mask), nullptr);
  MaterialPaintLayer *blur = bake_test_add_mask_effect(*ma, *row, MA_PAINT_LAYER_FILTER_BLUR);
  clear_active();

  bake_test_mark_region(*mask, rcti{0, 0, 0, 0});
  BKE_paint_layers_bake_subscribe(*ma, *blur);
  ma->paint_layers_flag &= ~MA_PAINT_LAYERS_BAKE_STALE;

  const rcti edited{1, 3, 2, 5};
  bake_test_mark_region(*mask, edited);
  BKE_paint_layers_bake_notice_changes(*ma);
  int region[4] = {};
  ASSERT_TRUE(BKE_paint_layers_bake_changed_region(*ma, *blur, region))
      << "a mask effect item keeps every region of its input";
  EXPECT_LE(region[0], edited.xmin);
  EXPECT_GE(region[1], edited.xmax);
  EXPECT_LE(region[2], edited.ymin);
  EXPECT_GE(region[3], edited.ymax);
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_BAKE_STALE, 0);
}

/**
 * A row removed while its regions wait for the re-lock: the coverage lookup (Phase 2) misses, the
 * regions fall through to the re-lock (Phase 3), and the drain must not crash. The interleaving is
 * played by removing the row between two drains, the closest a public sequence can get to the gap
 * between the phases.
 */
TEST_F(PaintLayersBakeTest, a_row_removed_before_the_regions_are_applied_does_not_crash)
{
  Image *map = add_image("Row");
  MaterialPaintLayer *row = add_paint_layer("Row", map);
  ASSERT_NE(BKE_paint_layers_filter_add(*ma, row, MA_PAINT_LAYER_FILTER_NOISE), nullptr);
  clear_active();
  bake_test_mark_region(*map, rcti{0, 0, 0, 0});
  BKE_paint_layers_bake_subscribe(*ma, *row);
  const bUUID marker = row->marker;

  bake_test_mark_region(*map, rcti{1, 2, 1, 2});
  ASSERT_TRUE(BKE_paint_layers_remove(*ma, row));
  /* The drain looks the row up again for its coverage (Phase 2); with the row gone that lookup
   * misses, and the pending region must not crash the re-lock (Phase 3) or resurrect the row. */
  EXPECT_EQ(BKE_paint_layers_find(*ma, marker), nullptr);
  BKE_paint_layers_bake_notice_changes(*ma);
}

/** \} */

}  // namespace blender::bke::tests
