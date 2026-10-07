/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */


/** \file
 * Paint Layers graph-vs-CPU tests: bake render, jobs and material rows (plan 9.3).
 *
 * Fixture and shared helpers live in
 * `paint_layers_graph_eval_test_fixture.hh` /
 * `paint_layers_graph_eval_test_shared.hh`; this TU holds only its own tests.
 */

#include "intern/paint_layers_graph_eval_test_fixture.hh"
#include "intern/paint_layers_graph_eval_test_shared.hh"

namespace blender::bke::tests {


/**
 * The same invariant as the previous test, for the Alpha convention (mask_channel=Alpha reads the
 * subtree's own coverage as a flat-opacity grey, always through Base Color -- like every other
 * Paint mask's #paint_layer_mask_correction_image, which reads Base Color regardless of the
 * channel argument it is passed). The mask's child carries a Base Color record only (no Roughness
 * one), so a build keyed to the owner's current channel would find no content at all while
 * generating Roughness -- the mask would vanish there (coverage 0, and with mask_gray_mode::Alpha
 * forcing the item's own factor flat, that reads as `r_factor = 0`, hiding the row completely) --
 * while Base Color would coincidentally still show the right, partial value. Because both the CPU
 * and the generator shared exactly this bug before the fix, a plain graph-vs-cpu comparison on one
 * channel would not have caught it (both sides would agree, wrongly); the cross-channel factor
 * comparison below is the check that does.
 */
TEST_F(PaintLayersGraphEvalTest, stack_mask_alpha_reads_base_color_on_every_owner_channel_matches_the_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  const eMaterialPaintChannel rough = PAINT_MATERIAL_CHANNEL_ROUGHNESS;
  ma = BKE_material_add(bmain, "StackMaskFixedChannelAlpha");

  MaterialPaintLayer *row = add_layer(
      "Row", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("RowBC", size, 200, 100, 50, 255), bc);
  MaterialPaintLayerChannel *row_rough = BKE_paint_layers_channel_add(*ma, row, rough);
  ASSERT_NE(row_rough, nullptr);
  row_rough->image = add_solid_image("RowRough", size, 90, 90, 90, 255);
  row_rough->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  /* The reference: the row alone, before any mask exists, so the CPU's fully-unmasked value (F=1)
   * is known for both channels without depending on any of the code this test exercises. */
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const RGBA unmasked_bc = cpu_pixel(bc);
  const RGBA unmasked_rough = cpu_pixel(rough);

  MaterialPaintLayer *mask = BKE_paint_layers_correction_add(
      *ma, row, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_STACK, "StackMask");
  ASSERT_NE(mask, nullptr);
  mask->mask_channel = int8_t(PAINT_MATERIAL_CHANNEL_ALPHA);

  /* A Base-Color-only child at half coverage: no Roughness record at all. */
  MaterialPaintLayer *child = add_layer_into(
      mask, "MaskChild", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("MaskChild", size, 128, 128, 128, 128), bc);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);

  const RGBA graph_bc = interpreter.eval_result(result_name(bc));
  const RGBA graph_rough = interpreter.eval_result(
      (std::string("Result ") + BKE_paint_material_channel_info(rough).ui_name).c_str());
  const RGBA cpu_bc = cpu_pixel(bc);
  const RGBA cpu_rough = cpu_pixel(rough);

  /* Base Color's own bottom is black (component 0); Roughness's is a neutral 0.5
   * (#BKE_paint_layers_channel_bottom_color). Inverting the plain lerp against the row's own,
   * mask-free value gives the mask's implied factor without hand-computing any sRGB conversion. */
  const float f_bc_cpu = cpu_bc.r / unmasked_bc.r;
  const float f_rough_cpu = (cpu_rough.r - 0.5f) / (unmasked_rough.r - 0.5f);
  const float f_bc_graph = graph_bc.r / unmasked_bc.r;
  const float f_rough_graph = (graph_rough.r - 0.5f) / (unmasked_rough.r - 0.5f);

  /* The key invariant (task's point (b)): the same Alpha-mask coverage, read from Base Color,
   * applies to every owner channel -- not just the one that happens to be Base Color itself. */
  EXPECT_NEAR(f_rough_cpu, f_bc_cpu, 0.02f);
  EXPECT_NEAR(f_rough_graph, f_bc_graph, 0.02f);
  EXPECT_GT(f_bc_cpu, 0.3f);
  EXPECT_LT(f_bc_cpu, 0.7f);

  EXPECT_NEAR(graph_bc.r, cpu_bc.r, 1e-4f);
  EXPECT_NEAR(graph_bc.g, cpu_bc.g, 1e-4f);
  EXPECT_NEAR(graph_bc.b, cpu_bc.b, 1e-4f);
  EXPECT_NEAR(graph_rough.r, cpu_rough.r, 1e-4f);
  EXPECT_NEAR(graph_rough.g, cpu_rough.g, 1e-4f);
  EXPECT_NEAR(graph_rough.b, cpu_rough.b, 1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

TEST_F(PaintLayersGraphEvalTest, bake_render_node_folder_minimal)
{
  const int size = 4;
  ma = BKE_material_add(bmain, "FolderRender");
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Child", folder, PaintLayerPlace::Into);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
      *ma, child, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  record->image = add_solid_image("Child", size, 0, 0, 255, 255);
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  Vector<float> color(int64_t(size) * size * 4);
  Vector<float> coverage(int64_t(size) * size);
  ASSERT_TRUE(BKE_paint_layers_bake_render_node(
      *ma, *folder, PAINT_MATERIAL_CHANNEL_BASE_COLOR, size, color.data(), coverage.data()));
  EXPECT_NEAR(coverage[0], 1.0f, 1e-4f);
  /* Blue in scene linear (byte 255 -> 1.0). */
  EXPECT_NEAR(color[0], 0.0f, 1e-4f);
  EXPECT_NEAR(color[1], 0.0f, 1e-4f);
  EXPECT_NEAR(color[2], 1.0f, 1e-4f);

  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(*folder);
  bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR] = add_solid_image("BakedColor", size, 0, 0, 255, 255);
  bake->coverage = add_solid_image("BakedCov", size, 255, 255, 255, 255);
  uint32_t hash[2];
  BKE_paint_layers_bake_hash(*folder, hash);
  bake->hash[0] = hash[0];
  bake->hash[1] = hash[1];
  Image *out = nullptr;
  EXPECT_TRUE(BKE_paint_layers_bake_substitute(*ma, *folder, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &out));
  EXPECT_EQ(out, bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR]);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result("Result Base Color");
  EXPECT_NEAR(graph.r, 0.0f, 1e-4f);
  EXPECT_NEAR(graph.g, 0.0f, 1e-4f);
  EXPECT_NEAR(graph.b, 1.0f, 1e-4f);
}

TEST_F(PaintLayersGraphEvalTest, bake_render_node_region_matches_full)
{
  const int size = 8;
  ma = BKE_material_add(bmain, "RegionRender");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Layer", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
      *ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  Image *image = add_solid_image("Gradient", size, 0, 0, 0, 255);
  /* A per-pixel gradient so a wrong region crop cannot pass by accident. */
  {
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
    uchar *pixels = ibuf->byte_data_for_write();
    for (int y = 0; y < size; y++) {
      for (int x = 0; x < size; x++) {
        const int64_t i = int64_t(y) * size + x;
        pixels[i * 4 + 0] = uchar(x * 255 / (size - 1));
        pixels[i * 4 + 1] = uchar(y * 255 / (size - 1));
        pixels[i * 4 + 2] = uchar(64);
        pixels[i * 4 + 3] = 255;
      }
    }
    BKE_image_release_ibuf(image, ibuf, lock);
  }
  record->image = image;
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  Vector<float> full_color(int64_t(size) * size * 4);
  Vector<float> full_coverage(int64_t(size) * size);
  ASSERT_TRUE(BKE_paint_layers_bake_render_node(
      *ma, *layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, size, full_color.data(), full_coverage.data()));

  /* A rectangle that does not start at a tile or map boundary. */
  const int rect[4] = {2, 1, 6, 7};
  Vector<float> part_color(int64_t(size) * size * 4, -1.0f);
  Vector<float> part_coverage(int64_t(size) * size, -1.0f);
  ASSERT_TRUE(BKE_paint_layers_bake_render_node(*ma,
                                                *layer,
                                                PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                                size,
                                                part_color.data(),
                                                part_coverage.data(),
                                                rect));
  for (int y = rect[1]; y < rect[3]; y++) {
    for (int x = rect[0]; x < rect[2]; x++) {
      const int64_t i = int64_t(y) * size + x;
      for (const int k : IndexRange(4)) {
        EXPECT_NEAR(part_color[i * 4 + k], full_color[i * 4 + k], 1e-6f) << "at " << x << "," << y;
      }
      EXPECT_NEAR(part_coverage[i], full_coverage[i], 1e-6f) << "at " << x << "," << y;
    }
  }
  /* Outside the rectangle nothing was computed. */
  EXPECT_FLOAT_EQ(part_color[0], -1.0f);
  EXPECT_FLOAT_EQ(part_coverage[int64_t(size) * size - 1], -1.0f);
}

TEST_F(PaintLayersGraphEvalTest, bake_row_to_image_writes_row_content)
{
  const int size = 4;
  ma = BKE_material_add(bmain, "RowToImage");
  MaterialPaintLayer *layer = add_layer(
      "Source", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Source", size, 0, 255, 0, 255));

  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *dst = BKE_image_add_generated(
      bmain, size, size, "RowResult", 32, false, IMA_GENTYPE_BLANK, black, false, false, false);
  dst->alpha_mode = IMA_ALPHA_STRAIGHT;
  ASSERT_TRUE(BKE_paint_layers_bake_row_to_image(
      *ma, *layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, size, *dst));

  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(dst, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  const uchar *pixels = ibuf->byte_buffer.data;
  ASSERT_NE(pixels, nullptr);
  EXPECT_EQ(pixels[0], 0);
  EXPECT_EQ(pixels[1], 255);
  EXPECT_EQ(pixels[2], 0);
  BKE_image_release_ibuf(dst, ibuf, lock);
}

TEST_F(PaintLayersGraphEvalTest, heavy_bake_job_computes_and_commits)
{
  const int size = 4;
  ma = BKE_material_add(bmain, "HeavyJob");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Layer", nullptr, PaintLayerPlace::Above);
  /* Six channels weigh 40, over the 36-node AUTO/worker threshold. */
  for (const eMaterialPaintChannel channel : {PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                              PAINT_MATERIAL_CHANNEL_ROUGHNESS,
                                              PAINT_MATERIAL_CHANNEL_METALLIC,
                                              PAINT_MATERIAL_CHANNEL_SPECULAR,
                                              PAINT_MATERIAL_CHANNEL_NORMAL,
                                              PAINT_MATERIAL_CHANNEL_AO})
  {
    MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, layer, channel);
    ASSERT_NE(record, nullptr);
    record->image = add_solid_image("Map", size, 128, 64, 32, 255);
    record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  }
  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(*layer);
  bake->mode = MA_PAINT_LAYER_BAKE_ALWAYS;
  bake->size = size;
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *layer));
  EXPECT_TRUE(BKE_paint_layers_bake_heavy_pending(*ma));

  PaintLayersBakeJob *job = BKE_paint_layers_bake_job_create(*bmain, *ma);
  ASSERT_NE(job, nullptr);
  BKE_paint_layers_bake_job_compute(*job);
  EXPECT_TRUE(BKE_paint_layers_bake_job_commit(*job));
  BKE_paint_layers_bake_job_free(*job);

  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *layer));
  Image *baked = nullptr;
  EXPECT_TRUE(
      BKE_paint_layers_bake_substitute(*ma, *layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &baked));
  EXPECT_NE(baked, nullptr);
}

/**
 * Spec-29, test #3: unlike #heavy_bake_job_computes_and_commits, this row is never given an explicit
 * bake structure -- AUTO mode, #MaterialPaintLayer::bake null. #BKE_paint_layers_bake_job_create is
 * now the only place allowed to allocate one, and only for a row that passed every gate and is
 * about to be queued. Before the fix, the job-create gate required `layer->bake != nullptr` up
 * front, so a freshly authored heavy row with no bake was invisible to the async planner forever;
 * this test must fail on that old code.
 */
TEST_F(PaintLayersGraphEvalTest, heavy_bake_job_allocates_bake_for_a_row_with_none)
{
  const int size = 4;
  ma = BKE_material_add(bmain, "HeavyJobNoBake");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Layer", nullptr, PaintLayerPlace::Above);
  /* Six channels weigh 40, over the 36-node AUTO/worker threshold, exactly like the job-create test
   * above. */
  for (const eMaterialPaintChannel channel : {PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                              PAINT_MATERIAL_CHANNEL_ROUGHNESS,
                                              PAINT_MATERIAL_CHANNEL_METALLIC,
                                              PAINT_MATERIAL_CHANNEL_SPECULAR,
                                              PAINT_MATERIAL_CHANNEL_NORMAL,
                                              PAINT_MATERIAL_CHANNEL_AO})
  {
    MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, layer, channel);
    ASSERT_NE(record, nullptr);
    record->image = add_solid_image("Map", size, 128, 64, 32, 255);
    record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  }
  ASSERT_EQ(layer->bake, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_is_heavy(*ma, *layer));
  ASSERT_FALSE(BKE_paint_layers_bake_row_is_deferred(*ma, *layer));
  ASSERT_TRUE(BKE_paint_layers_bake_heavy_pending(*ma));

  PaintLayersBakeJob *job = BKE_paint_layers_bake_job_create(*bmain, *ma);
  ASSERT_NE(job, nullptr);
  ASSERT_NE(layer->bake, nullptr)
      << "job_create must allocate the bake structure for a row it is about to queue";
  BKE_paint_layers_bake_job_compute(*job);
  EXPECT_TRUE(BKE_paint_layers_bake_job_commit(*job));
  BKE_paint_layers_bake_job_free(*job);

  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *layer));
  EXPECT_NE(layer->bake, nullptr);
  Image *baked = nullptr;
  EXPECT_TRUE(
      BKE_paint_layers_bake_substitute(*ma, *layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &baked));
  EXPECT_NE(baked, nullptr);
}

/**
 * Spec-29, test #6: #BKE_paint_layers_bake_heavy_pending is a pure predicate -- it must see a row that
 * is heavy purely by subtree weight and has no bake structure at all, without allocating anything.
 * Before the fix, its combined gate required `layer->bake != nullptr` before it would even look at
 * `is_heavy`, so this row was invisible to it; this test must fail on that old code.
 */
TEST_F(PaintLayersGraphEvalTest, heavy_pending_sees_a_weight_heavy_row_with_no_bake)
{
  ma = BKE_material_add(bmain, "HeavyNoBake");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Layer", nullptr, PaintLayerPlace::Above);
  for (const eMaterialPaintChannel channel : {PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                              PAINT_MATERIAL_CHANNEL_ROUGHNESS,
                                              PAINT_MATERIAL_CHANNEL_METALLIC,
                                              PAINT_MATERIAL_CHANNEL_SPECULAR,
                                              PAINT_MATERIAL_CHANNEL_NORMAL,
                                              PAINT_MATERIAL_CHANNEL_AO})
  {
    MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, layer, channel);
    ASSERT_NE(record, nullptr);
    record->image = add_solid_image("Map", 4, 128, 64, 32, 255);
    record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  }
  ASSERT_EQ(layer->bake, nullptr);
  EXPECT_TRUE(BKE_paint_layers_bake_heavy_pending(*ma));
  EXPECT_EQ(layer->bake, nullptr) << "the predicate is read-only and must not allocate anything";
}

TEST_F(PaintLayersGraphEvalTest, heavy_bake_job_drops_removed_row)
{
  const int size = 4;
  ma = BKE_material_add(bmain, "HeavyJobGone");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Layer", nullptr, PaintLayerPlace::Above);
  for (const eMaterialPaintChannel channel : {PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                              PAINT_MATERIAL_CHANNEL_ROUGHNESS,
                                              PAINT_MATERIAL_CHANNEL_METALLIC,
                                              PAINT_MATERIAL_CHANNEL_SPECULAR,
                                              PAINT_MATERIAL_CHANNEL_NORMAL,
                                              PAINT_MATERIAL_CHANNEL_AO})
  {
    MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, layer, channel);
    record->image = add_solid_image("Map", size, 128, 64, 32, 255);
    record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  }
  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(*layer);
  bake->mode = MA_PAINT_LAYER_BAKE_ALWAYS;
  bake->size = size;

  PaintLayersBakeJob *job = BKE_paint_layers_bake_job_create(*bmain, *ma);
  ASSERT_NE(job, nullptr);
  ASSERT_TRUE(BKE_paint_layers_remove(*ma, layer));
  BKE_paint_layers_bake_job_compute(*job);
  EXPECT_FALSE(BKE_paint_layers_bake_job_commit(*job));
  BKE_paint_layers_bake_job_free(*job);
}

TEST_F(PaintLayersGraphEvalTest, height_uses_mix_and_bump)
{
  const int size = 4;
  ma = BKE_material_add(bmain, "HeightEval");
  add_layer("Bottom",
            MA_PAINT_LAYER_SOURCE_IMAGE,
            add_solid_image("Bottom", size, 192, 64, 32, 255),
            PAINT_MATERIAL_CHANNEL_HEIGHT);
  MaterialPaintLayer *top = add_layer("Top",
                                      MA_PAINT_LAYER_SOURCE_IMAGE,
                                      add_solid_image("Top", size, 64, 192, 224, 255),
                                      PAINT_MATERIAL_CHANNEL_HEIGHT);
  /* Height is a scalar stack like any other: a plain Mix, never the Normal combine. */
  BKE_paint_layers_set_blend(*ma, top, MA_PAINT_LAYER_BLEND_MULTIPLY);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result("Result Height");
  const RGBA cpu = cpu_pixel(PAINT_MATERIAL_CHANNEL_HEIGHT);
  EXPECT_NEAR(graph.r, cpu.r, 1e-4f);
  EXPECT_NEAR(graph.g, cpu.g, 1e-4f);
  EXPECT_NEAR(graph.b, cpu.b, 1e-4f);

  /* The material reads the Height result through a Bump, not through the Result Normal chain. */
  bNode *bump = nullptr;
  for (bNode &node : ma->nodetree->nodes) {
    if (node.type_legacy == SH_NODE_BUMP) {
      bump = &node;
      break;
    }
  }
  ASSERT_NE(bump, nullptr);
  bNodeSocket *height_in = bke::node_find_socket(*bump, SOCK_IN, "Height"_ustr);
  ASSERT_NE(height_in, nullptr);
  EXPECT_FALSE(height_in->directly_linked_links().is_empty());
}

TEST_F(PaintLayersGraphEvalTest, bake_planner_writes_maps_and_substitutes)
{
  const int size = 4;
  ma = BKE_material_add(bmain, "BakePlanner");
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Bottom", size, 192, 64, 32, 255));
  MaterialPaintLayer *top = add_layer(
      "Top", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Top", size, 64, 192, 224, 255));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA live = interpreter.eval_result("Result Base Color");

  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(*top);
  bake->size = size;
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *top, MA_PAINT_LAYER_BAKE_ALWAYS));
  bool changed = false;
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_TRUE(changed);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *top));
  EXPECT_NE(bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR], nullptr);
  EXPECT_NE(bake->coverage, nullptr);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA baked = interpreter.eval_result("Result Base Color");
  EXPECT_NEAR(baked.r, live.r, 1e-2f);
  EXPECT_NEAR(baked.g, live.g, 1e-2f);
  EXPECT_NEAR(baked.b, live.b, 1e-2f);
}

/**
 * A synthetic bake of \a layer, built the way the planner will: composite the node's subtree through
 * #BKE_paint_layers_bake_render_node, write the straight colour and grey coverage into two images,
 * and stamp the description hash. Then the substituted tree must equal the live one.
 */
TEST_F(PaintLayersGraphEvalTest, baked_node_equals_the_live_node)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;

  enum class Variant { Layer, Multiply, Mask, Correction, Folder, NestedFolder };
  const Variant variants[] = {Variant::Layer,
                              Variant::Multiply,
                              Variant::Mask,
                              Variant::Correction,
                              Variant::Folder,
                              Variant::NestedFolder};

  auto make_float_image = [&](const char *name, const float *rgba, const float *gray) {
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    Image *image = BKE_image_add_generated(
        bmain, size, size, name, 32, 32, IMA_GENTYPE_BLANK, black, false, false, false);
    STRNCPY(image->colorspace_settings.name, "Non-Color");
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
    float *pixels = const_cast<float *>(ibuf->float_buffer.data);
    for (const int64_t i : IndexRange(int64_t(size) * size)) {
      if (rgba != nullptr) {
        pixels[i * 4 + 0] = rgba[i * 4 + 0];
        pixels[i * 4 + 1] = rgba[i * 4 + 1];
        pixels[i * 4 + 2] = rgba[i * 4 + 2];
        pixels[i * 4 + 3] = 1.0f;
      }
      else {
        pixels[i * 4 + 0] = gray[i];
        pixels[i * 4 + 1] = gray[i];
        pixels[i * 4 + 2] = gray[i];
        pixels[i * 4 + 3] = 1.0f;
      }
    }
    BKE_image_release_ibuf(image, ibuf, lock);
    return image;
  };

  auto add_paint = [&](const char *name, Image *image, MaterialPaintLayer *anchor) {
    MaterialPaintLayer *layer = BKE_paint_layers_add(*ma,
                                                     MA_PAINT_LAYER_SOURCE_IMAGE,
                                                     name,
                                                     anchor,
                                                     anchor != nullptr ? PaintLayerPlace::Into :
                                                                          PaintLayerPlace::Above);
    MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, layer, channel);
    record->image = image;
    record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
    return layer;
  };

  for (const Variant variant : variants) {
    ma = BKE_material_add(bmain, "BakeEquiv");
    add_paint("Bottom", add_solid_image("Bottom", size, 192, 64, 32, 255), nullptr);

    MaterialPaintLayer *baked_node = nullptr;
    switch (variant) {
      case Variant::Layer:
        baked_node = add_paint("Top", add_solid_image("Top", size, 64, 192, 224, 255), nullptr);
        break;
      case Variant::Multiply:
        baked_node = add_paint("Top", add_solid_image("Top", size, 64, 192, 224, 255), nullptr);
        BKE_paint_layers_set_blend(*ma, baked_node, MA_PAINT_LAYER_BLEND_MULTIPLY);
        break;
      case Variant::Mask:
        baked_node = add_paint("Top", add_solid_image("Top", size, 64, 192, 224, 255), nullptr);
        set_mask_image(*baked_node, add_solid_image("Mask", size, 200, 200, 200, 255));
        break;
      case Variant::Correction: {
        baked_node = add_paint("Top", add_solid_image("Top", size, 64, 192, 224, 255), nullptr);
        MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
            *ma, baked_node, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_CONSTANT, "C");
        const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
        BKE_paint_layers_set_fill_color(*ma, correction, green);
        BKE_paint_layers_set_opacity(*ma, correction, 0.4f);
        break;
      }
      case Variant::Folder: {
        MaterialPaintLayer *folder = BKE_paint_layers_add(
            *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
        add_paint("Child", add_solid_image("Child", size, 0, 0, 255, 255), folder);
        BKE_paint_layers_set_opacity(*ma, folder, 0.7f);
        baked_node = folder;
        break;
      }
      case Variant::NestedFolder: {
        MaterialPaintLayer *outer = BKE_paint_layers_add(
            *ma, MA_PAINT_LAYER_SOURCE_STACK, "Outer", nullptr, PaintLayerPlace::Above);
        MaterialPaintLayer *inner = BKE_paint_layers_add(
            *ma, MA_PAINT_LAYER_SOURCE_STACK, "Inner", outer, PaintLayerPlace::Into);
        add_paint("Child", add_solid_image("Child", size, 0, 255, 0, 255), inner);
        BKE_paint_layers_set_opacity(*ma, inner, 0.6f);
        BKE_paint_layers_set_opacity(*ma, outer, 0.8f);
        baked_node = outer;
        break;
      }
    }
    ASSERT_NE(baked_node, nullptr);

    /* Reference: the live stack. */
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
    GraphInterpreter interpreter;
    interpreter.instance = find_instance();
    interpreter.tree = ma->paint_layers_tree;
    interpreter.x = 1;
    interpreter.y = 1;
    ASSERT_NE(interpreter.instance, nullptr);
    const RGBA graph_live = interpreter.eval_result(result_name(channel));
    const RGBA cpu_live = cpu_pixel(channel);

    /* Synthetic bake: the node's own CPU composite. */
    Vector<float> color(int64_t(size) * size * 4);
    Vector<float> coverage(int64_t(size) * size);
    ASSERT_TRUE(BKE_paint_layers_bake_render_node(
        *ma, *baked_node, channel, size, color.data(), coverage.data()));
    MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(*baked_node);
    bake->images[channel] = make_float_image("BakeColor", color.data(), nullptr);
    bake->coverage = make_float_image("BakeCoverage", nullptr, coverage.data());
    uint32_t hash[2];
    BKE_paint_layers_bake_hash(*baked_node, hash);
    bake->hash[0] = hash[0];
    bake->hash[1] = hash[1];
    ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *baked_node));

    /* The substituted stack. */
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
    interpreter.instance = find_instance();
    interpreter.tree = ma->paint_layers_tree;
    ASSERT_NE(interpreter.instance, nullptr);
    const RGBA graph_baked = interpreter.eval_result(result_name(channel));
    const RGBA cpu_baked = cpu_pixel(channel);

    const float tol = 1e-4f;
    EXPECT_NEAR(graph_baked.r, graph_live.r, tol) << int(variant);
    EXPECT_NEAR(graph_baked.g, graph_live.g, tol) << int(variant);
    EXPECT_NEAR(graph_baked.b, graph_live.b, tol) << int(variant);
    EXPECT_NEAR(cpu_baked.r, cpu_live.r, tol) << int(variant);
    EXPECT_NEAR(cpu_baked.g, cpu_live.g, tol) << int(variant);
    EXPECT_NEAR(cpu_baked.b, cpu_live.b, tol) << int(variant);

    BKE_id_free(bmain, ma);
    ma = nullptr;
  }
}

/**
 * A Material layer has no live subtree: its channels exist only as its bake, and those maps are the
 * row's content in both the generator and the CPU composite.
 */
TEST_F(PaintLayersGraphEvalTest, material_layer_bake_is_its_content_in_graph_and_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "MaterialLayer");
  Material *source = BKE_material_add(bmain, "MaterialSource");

  MaterialPaintLayer *material = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Material", nullptr, PaintLayerPlace::Above);
  material->material = source;
  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(*material);
  bake->size = size;
  bake->mode = MA_PAINT_LAYER_BAKE_ALWAYS;

  Image *color = add_solid_image("MaterialBakeColor", size, 0, 255, 0, 255);
  Image *coverage = add_solid_image("MaterialBakeCoverage", size, 255, 255, 255, 255);
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *material, channel, color));
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *material, -1, coverage));
  BKE_paint_layers_bake_finalize(*ma, *material);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *material));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result(result_name(channel));
  const RGBA cpu = cpu_pixel(channel);

  /* Coverage is one, so the baked colour shows through unblended. */
  EXPECT_NEAR(graph.g, 1.0f, 1e-2f);
  EXPECT_NEAR(graph.r, 0.0f, 1e-2f);
  EXPECT_NEAR(graph.b, 0.0f, 1e-2f);
  EXPECT_NEAR(cpu.r, graph.r, 1e-2f);
  EXPECT_NEAR(cpu.g, graph.g, 1e-2f);
  EXPECT_NEAR(cpu.b, graph.b, 1e-2f);
}

/**
 * A Custom layer has no live CPU subtree either: with a valid bake it substitutes in both the
 * generator and the CPU composite exactly like a Material layer, and without one it drops out.
 */
TEST_F(PaintLayersGraphEvalTest, custom_layer_bake_substitutes_in_graph_and_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "CustomLayer");
  MaterialPaintLayer *custom = BKE_paint_layers_custom_layer_add(
      *bmain, *ma, "Custom", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(custom, nullptr);

  /* Without a bake the row contributes nothing: the result is the channel bottom. */
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA without_bake = interpreter.eval_result(result_name(channel));

  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(*custom);
  bake->size = size;
  bake->mode = MA_PAINT_LAYER_BAKE_ALWAYS;
  Image *color = add_solid_image("CustomBakeColor", size, 0, 255, 0, 255);
  Image *coverage = add_solid_image("CustomBakeCoverage", size, 255, 255, 255, 255);
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *custom, channel, color));
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *custom, -1, coverage));
  BKE_paint_layers_bake_finalize(*ma, *custom);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *custom));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result(result_name(channel));
  const RGBA cpu = cpu_pixel(channel);

  EXPECT_NEAR(graph.g, 1.0f, 1e-2f);
  EXPECT_NEAR(graph.r, 0.0f, 1e-2f);
  EXPECT_NEAR(cpu.r, graph.r, 1e-2f);
  EXPECT_NEAR(cpu.g, graph.g, 1e-2f);
  EXPECT_NEAR(cpu.b, graph.b, 1e-2f);
  /* The unbaked result differed: the layer did take part once baked. */
  EXPECT_GT(std::abs(graph.g - without_bake.g), 1e-2f);
}

/**
 * C-7: a Custom row has no live CPU expression, so a saved bake that is stale (its hash no longer
 * matches) is still shown instead of the row dropping to black -- both sides substitute it.
 */
TEST_F(PaintLayersGraphEvalTest, custom_stale_bake_still_substitutes)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "CustomStale");
  MaterialPaintLayer *custom = BKE_paint_layers_custom_layer_add(
      *bmain, *ma, "Custom", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(custom, nullptr);

  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(*custom);
  bake->size = size;
  bake->mode = MA_PAINT_LAYER_BAKE_ALWAYS;
  Image *color = add_solid_image("StaleCustomColor", size, 0, 255, 0, 255);
  Image *coverage = add_solid_image("StaleCustomCoverage", size, 255, 255, 255, 255);
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *custom, channel, color));
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *custom, -1, coverage));
  /* No finalize: the stored hash stays zero, so the maps are there but stale. */
  ASSERT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *custom));
  {
    Image *sub = nullptr;
    bool stale = false;
    ASSERT_TRUE(
        BKE_paint_layers_bake_substitute_custom(*ma, *custom, channel, &sub, &stale));
    ASSERT_EQ(sub, color);
    ASSERT_TRUE(stale);
  }

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result(result_name(channel));
  const RGBA cpu = cpu_pixel(channel);

  /* Coverage is one, so the stale baked green shows through. */
  EXPECT_NEAR(graph.g, 1.0f, 1e-2f);
  EXPECT_NEAR(graph.r, 0.0f, 1e-2f);
  EXPECT_NEAR(cpu.r, graph.r, 1e-2f);
  EXPECT_NEAR(cpu.g, graph.g, 1e-2f);
  EXPECT_NEAR(cpu.b, graph.b, 1e-2f);
}

/**
 * Spec-F2-C3: a Custom row inside an isolating folder has no live path at all -- it only ever reaches
 * the generator through the substituted branch (#row_channel_substituted). Before the fix that
 * branch built no content-alpha chain for any kind, so the folder's own "Content Alpha Base Color"
 * output was always null and the row's partial alpha never reached the Result's alpha. With the
 * fix, the substituted branch starts the chain from the same Image Texture Alpha the live Paint/Fill
 * leaf uses, so a single Custom child's straight alpha passes through the isolating folder's
 * divide-by-coverage unchanged (coverage is full here), exactly like the existing Fill case.
 */
TEST_F(PaintLayersGraphEvalTest, custom_layer_isolating_folder_partial_alpha_matches_cpu)
{
  const int size = 4;
  const float folder_opacity = 0.5f;
  const float content_alpha = 0.4f;
  const float tolerance = 1e-2f;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;

  ma = BKE_material_add(bmain, "CustomIso");
  ASSERT_NE(ma, nullptr);

  MaterialPaintLayer *bottom = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Bottom", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(bottom, nullptr);
  MaterialPaintLayerChannel *bottom_record = BKE_paint_layers_channel_add(*ma, bottom, channel);
  ASSERT_NE(bottom_record, nullptr);
  bottom_record->image = add_solid_image("CustomIsoBottom", size, 255, 0, 0, 255);
  ASSERT_NE(bottom_record->image, nullptr);
  bottom_record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "CustomFolder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, folder_opacity));
  ASSERT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));

  MaterialPaintLayer *custom = BKE_paint_layers_custom_layer_add(
      *bmain, *ma, "CustomChild", folder, PaintLayerPlace::Into);
  ASSERT_NE(custom, nullptr);

  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(*custom);
  bake->size = size;
  bake->mode = MA_PAINT_LAYER_BAKE_ALWAYS;
  /* Green, partial straight alpha 0.4 -- the leaf's own content alpha. */
  Image *color = add_solid_image(
      "CustomIsoColor", size, 0, 255, 0, uchar(content_alpha * 255.0f + 0.5f));
  /* Full coverage, so the folder's own factor is exactly the folder opacity, like the existing
   * Fill isolating-folder case, and the divide-by-coverage leaves the leaf's alpha unchanged. */
  Image *coverage = add_solid_image("CustomIsoCoverage", size, 255, 255, 255, 255);
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *custom, int(channel), color));
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *custom, -1, coverage));
  BKE_paint_layers_bake_finalize(*ma, *custom);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *custom));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(ma->paint_layers_tree, nullptr);

  bNodeTree *folder_tree = nested_folder_tree_find(*bmain, "CustomFolder");
  ASSERT_NE(folder_tree, nullptr);
  bNode *folder_instance = nested_group_instance_find(*ma->paint_layers_tree, *folder_tree);
  ASSERT_NE(folder_instance, nullptr);
  EXPECT_TRUE(folder_interface_has_socket(*folder_tree, "Content Alpha Base Color"));

  GraphInterpreter root_interpreter;
  root_interpreter.instance = find_instance();
  root_interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(root_interpreter.instance, nullptr);
  root_interpreter.tree->ensure_topology_cache();
  folder_tree->ensure_topology_cache();

  const float bottom_color[3] = {1.0f, 0.0f, 0.0f};
  const float leaf_color[3] = {0.0f, 1.0f, 0.0f};
  for (int x = 0; x < size; x++) {
    const int y = 0;
    const float factor = folder_opacity;
    const RGBA expected = {bottom_color[0] * (1.0f - factor) + leaf_color[0] * factor,
                           bottom_color[1] * (1.0f - factor) + leaf_color[1] * factor,
                           bottom_color[2] * (1.0f - factor) + leaf_color[2] * factor,
                           1.0f * (1.0f - factor) + content_alpha * factor};

    root_interpreter.x = x;
    root_interpreter.y = y;
    const RGBA graph = eval_channel_result(root_interpreter, channel);
    const RGBA cpu = cpu_pixel_at(channel, x, y);
    const float graph_cov = nested_folder_named_output_eval(
        root_interpreter, *folder_tree, *folder_instance, "Coverage Base Color");
    const float graph_content_a = nested_folder_named_output_eval(
        root_interpreter, *folder_tree, *folder_instance, "Content Alpha Base Color");

    EXPECT_NEAR(graph.r, expected.r, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.g, expected.g, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.b, expected.b, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.a, expected.a, tolerance) << "x=" << x;
    EXPECT_NEAR(cpu.r, expected.r, tolerance) << "x=" << x;
    EXPECT_NEAR(cpu.g, expected.g, tolerance) << "x=" << x;
    EXPECT_NEAR(cpu.b, expected.b, tolerance) << "x=" << x;
    EXPECT_NEAR(cpu.a, expected.a, tolerance) << "x=" << x;
    EXPECT_NEAR(graph_cov, folder_opacity, tolerance) << "x=" << x;
    EXPECT_NEAR(graph_content_a, content_alpha, tolerance) << "x=" << x;
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * Spec-F2-C3 regression: the same hole also hit a Paint row that switches to substituted mode via its
 * own valid cache bake (an "inactive" row shown from its bake instead of its live map). Before the
 * fix its content alpha was dropped exactly like the Custom case, even though the row's kind is one
 * the live path already tracks. Unlike the Custom sibling, a Paint row's content enters the
 * coverage (folder_opacity * content_alpha), as in #check_iso_folder_two_map_channel.
 */
TEST_F(PaintLayersGraphEvalTest, substituted_paint_row_isolating_folder_partial_alpha_matches_cpu)
{
  const int size = 4;
  const float folder_opacity = 0.5f;
  const float content_alpha = 0.4f;
  const float tolerance = 1e-2f;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;

  ma = BKE_material_add(bmain, "SubPaintIso");
  ASSERT_NE(ma, nullptr);

  MaterialPaintLayer *bottom = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Bottom", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(bottom, nullptr);
  MaterialPaintLayerChannel *bottom_record = BKE_paint_layers_channel_add(*ma, bottom, channel);
  ASSERT_NE(bottom_record, nullptr);
  bottom_record->image = add_solid_image("SubPaintBottom", size, 255, 0, 0, 255);
  ASSERT_NE(bottom_record->image, nullptr);
  bottom_record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "SubPaintFolder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, folder_opacity));
  ASSERT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));

  MaterialPaintLayer *paint = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "PaintChild", folder, PaintLayerPlace::Into);
  ASSERT_NE(paint, nullptr);
  MaterialPaintLayerChannel *paint_record = BKE_paint_layers_channel_add(*ma, paint, channel);
  ASSERT_NE(paint_record, nullptr);
  /* The row's live map -- a different colour and full alpha -- must not show: once the row has a
   * valid cache bake, #row_channel_substituted replaces it whole, same as row_is_substituted says. */
  paint_record->image = add_solid_image("SubPaintLive", size, 0, 0, 255, 255);
  paint_record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(*paint);
  bake->size = size;
  bake->mode = MA_PAINT_LAYER_BAKE_ALWAYS;
  /* Green, partial straight alpha 0.4 -- the row's cached content alpha, distinct from the live
   * map's colour and full alpha above, so a leftover live read would fail this test's colour and
   * alpha checks alike. */
  Image *color = add_solid_image(
      "SubPaintColor", size, 0, 255, 0, uchar(content_alpha * 255.0f + 0.5f));
  Image *coverage = add_solid_image("SubPaintCoverage", size, 255, 255, 255, 255);
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *paint, int(channel), color));
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *paint, -1, coverage));
  BKE_paint_layers_bake_finalize(*ma, *paint);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *paint));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(ma->paint_layers_tree, nullptr);

  bNodeTree *folder_tree = nested_folder_tree_find(*bmain, "SubPaintFolder");
  ASSERT_NE(folder_tree, nullptr);
  bNode *folder_instance = nested_group_instance_find(*ma->paint_layers_tree, *folder_tree);
  ASSERT_NE(folder_instance, nullptr);
  EXPECT_TRUE(folder_interface_has_socket(*folder_tree, "Content Alpha Base Color"));

  GraphInterpreter root_interpreter;
  root_interpreter.instance = find_instance();
  root_interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(root_interpreter.instance, nullptr);
  root_interpreter.tree->ensure_topology_cache();
  folder_tree->ensure_topology_cache();

  const float bottom_color[3] = {1.0f, 0.0f, 0.0f};
  const float leaf_color[3] = {0.0f, 1.0f, 0.0f};
  for (int x = 0; x < size; x++) {
    const int y = 0;
    const float factor = folder_opacity * content_alpha;
    const RGBA expected = {bottom_color[0] * (1.0f - factor) + leaf_color[0] * factor,
                           bottom_color[1] * (1.0f - factor) + leaf_color[1] * factor,
                           bottom_color[2] * (1.0f - factor) + leaf_color[2] * factor,
                           1.0f * (1.0f - factor) + content_alpha * factor};

    root_interpreter.x = x;
    root_interpreter.y = y;
    const RGBA graph = eval_channel_result(root_interpreter, channel);
    const RGBA cpu = cpu_pixel_at(channel, x, y);
    const float graph_cov = nested_folder_named_output_eval(
        root_interpreter, *folder_tree, *folder_instance, "Coverage Base Color");
    const float graph_content_a = nested_folder_named_output_eval(
        root_interpreter, *folder_tree, *folder_instance, "Content Alpha Base Color");

    EXPECT_NEAR(graph.r, expected.r, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.g, expected.g, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.b, expected.b, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.a, expected.a, tolerance) << "x=" << x;
    EXPECT_NEAR(cpu.r, expected.r, tolerance) << "x=" << x;
    EXPECT_NEAR(cpu.g, expected.g, tolerance) << "x=" << x;
    EXPECT_NEAR(cpu.b, expected.b, tolerance) << "x=" << x;
    EXPECT_NEAR(cpu.a, expected.a, tolerance) << "x=" << x;
    EXPECT_NEAR(graph_cov, folder_opacity * content_alpha, tolerance) << "x=" << x;
    EXPECT_NEAR(graph_content_a, content_alpha, tolerance) << "x=" << x;
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * "Use Row Result" renders a source row's channels off the main thread and commits them as the
 * target row's maps; the BKE job split is what the heavy operator path runs.
 */
TEST_F(PaintLayersGraphEvalTest, row_result_job_bakes_source_into_target_channels)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "RowResult");

  MaterialPaintLayer *source = add_layer(
      "Source", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("SourceImg", size, 0, 255, 0, 255));
  MaterialPaintLayer *target = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Target", nullptr, PaintLayerPlace::Above);

  PaintLayersRowResultJob *job = BKE_paint_layers_row_result_job_create(
      *bmain, *ma, source->marker, target->marker, size);
  ASSERT_NE(job, nullptr);
  BKE_paint_layers_row_result_job_compute(*job);
  ASSERT_TRUE(BKE_paint_layers_row_result_job_commit(*job));
  BKE_paint_layers_row_result_job_free(*job);

  MaterialPaintLayer *live_target = BKE_paint_layers_find(*ma, target->marker);
  ASSERT_NE(live_target, nullptr);
  const MaterialPaintLayerChannel *record = nullptr;
  for (int i = 0; i < live_target->channels_num; i++) {
    if (live_target->channels[i].channel == channel) {
      record = &live_target->channels[i];
    }
  }
  ASSERT_NE(record, nullptr);
  ASSERT_NE(record->image, nullptr);

  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(record->image, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  const uchar *pixels = ibuf->byte_data();
  ASSERT_NE(pixels, nullptr);
  const int64_t i = (int64_t(1) * size + 1) * 4;
  /* Green survived decode/encode back into the map's own sRGB space. */
  EXPECT_LT(int(pixels[i + 0]), 8);
  EXPECT_GT(int(pixels[i + 1]), 247);
  EXPECT_LT(int(pixels[i + 2]), 8);
  BKE_image_release_ibuf(record->image, ibuf, lock);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * A semi-transparent material bake limits the row through its coverage map: the generator and the
 * CPU both blend the baked colour over what is below by that factor, so they have to agree.
 */
TEST_F(PaintLayersGraphEvalTest, material_layer_semi_transparent_bake_matches_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "MaterialSemi");
  Material *source = BKE_material_add(bmain, "MaterialSemiSource");

  MaterialPaintLayer *material = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Material", nullptr, PaintLayerPlace::Above);
  material->material = source;
  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(*material);
  bake->size = size;
  bake->mode = MA_PAINT_LAYER_BAKE_ALWAYS;

  Image *color = add_solid_image("SemiBakeColor", size, 0, 255, 0, 255);
  Image *coverage = add_solid_image("SemiBakeCoverage", size, 128, 128, 128, 255);
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *material, channel, color));
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *material, -1, coverage));
  BKE_paint_layers_bake_finalize(*ma, *material);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *material));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result(result_name(channel));
  const RGBA cpu = cpu_pixel(channel);

  /* Half coverage: between the bottom and the full baked green, on both sides. */
  EXPECT_GT(graph.g, 0.05f);
  EXPECT_LT(graph.g, 0.98f);
  EXPECT_NEAR(cpu.r, graph.r, 1e-2f);
  EXPECT_NEAR(cpu.g, graph.g, 1e-2f);
  EXPECT_NEAR(cpu.b, graph.b, 1e-2f);
}

/**
 * Spec-25c: a Material row whose source's Alpha is a plain constant below 1. The real bake path
 * (#BKE_paint_layers_material_bake_apply) diverts that channel into the row's #coverage, never
 * into #MaterialPaintLayerBake::images[ALPHA] -- no pipeline fills that slot. Before the fix
 * #paint_layer_material_source_map read Alpha from `images[]` regardless, so it always answered
 * null and the channel stayed "live" forever (#material_live_row_eligible): a fully finalized,
 * not-deferred Material row could never settle on Baked, and stayed Hybrid indefinitely instead.
 * This bakes every channel through the real entry point, including Alpha, and checks the row lands
 * on Baked once finalized, with the graph and the CPU composite agreeing with the reference blend.
 */
TEST_F(PaintLayersGraphEvalTest, material_layer_constant_alpha_bake_settles_on_baked)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "MatAlphaBaked");
  Image *bottom_image = add_solid_image("Bottom", size, 255, 0, 0, 255);
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, bottom_image, bc);

  const float row_color[4] = {0.0f, 1.0f, 0.0f, 1.0f};
  const float alpha_value = 0.5f;
  Material *source = BKE_material_add(bmain, "MatAlphaSource");
  bNodeTree &tree = *source->nodetree;
  bNode *principled = bke::node_add_static_node(nullptr, tree, SH_NODE_BSDF_PRINCIPLED);
  bNode *output = bke::node_add_static_node(nullptr, tree, SH_NODE_OUTPUT_MATERIAL);
  bke::node_add_link(tree,
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                     *output,
                     *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
  bNodeSocket *color_socket = bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(color_socket, nullptr);
  copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(color_socket->default_value)->value, row_color);
  bNodeSocket *alpha_socket = bke::node_find_socket(*principled, SOCK_IN, "Alpha"_ustr);
  ASSERT_NE(alpha_socket, nullptr);
  static_cast<bNodeSocketValueFloat *>(alpha_socket->default_value)->value = alpha_value;
  BKE_ntree_update_tag_all(&tree);
  BKE_ntree_update_after_single_tree_change(*bmain, tree);

  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Mat", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));

  /* Not active: the row is free to be baked and read from its maps like the planner would. */
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());

  /* Bake through the real entry point: every resolvable channel gets a solid map at the source's
   * constant value, and Alpha (the constant 0.5) is diverted into the row's coverage exactly as a
   * real material bake hand-over does. */
  const MaterialSourceResolve resolve = BKE_paint_material_source_resolve(source);
  Vector<int> channels;
  Vector<Image *> images;
  Image *color_image = nullptr;
  for (const eMaterialPaintChannel channel : BKE_paint_material_bakeable_channels()) {
    if (channel == PAINT_MATERIAL_CHANNEL_ALPHA ||
        resolve.channels[channel] == ChannelResolution::Unavailable)
    {
      continue;
    }
    Image *map = (channel == bc) ? add_solid_image("MatAlphaColor", size, 0, 255, 0, 255) :
                                   add_solid_image("MatAlphaOther", size, 0, 255, 0, 255);
    if (channel == bc) {
      color_image = map;
    }
    channels.append(int(channel));
    images.append(map);
  }
  ASSERT_NE(color_image, nullptr);
  channels.append(int(PAINT_MATERIAL_CHANNEL_ALPHA));
  Image *coverage_image = add_solid_image("MatAlphaCoverage", size, 128, 128, 128, 255);
  images.append(coverage_image);
  BKE_paint_layers_material_bake_apply(
      *bmain, *ma, *row, size, channels.as_span(), images.as_span());
  BKE_paint_layers_bake_finalize(*ma, *row);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *row));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Baked)
      << "Alpha must be read from the row's coverage, or the channel stays perpetually unmapped "
         "and the row never leaves Hybrid";

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result(result_name(bc));
  const RGBA cpu = cpu_pixel(bc);

  /* The reference blend: bottom under the baked colour, mixed by the decoded coverage factor --
   * built from the same maps the bake wrote, read back through the same colour management the
   * generator and the CPU both apply, so this does not assume a particular gamma convention. */
  const RGBA bottom_sample = interpreter.sample_image(bottom_image, "Color");
  const RGBA color_sample = interpreter.sample_image(color_image, "Color");
  const RGBA coverage_sample = interpreter.sample_image(coverage_image, "Color");
  const float factor = (coverage_sample.r + coverage_sample.g + coverage_sample.b) / 3.0f;
  ASSERT_GT(factor, 0.01f);
  ASSERT_LT(factor, 0.99f);
  const float expected_g = bottom_sample.g + (color_sample.g - bottom_sample.g) * factor;
  EXPECT_NEAR(graph.g, expected_g, 1e-2f);
  EXPECT_NEAR(cpu.r, graph.r, 1e-2f);
  EXPECT_NEAR(cpu.g, graph.g, 1e-2f);
  EXPECT_NEAR(cpu.b, graph.b, 1e-2f);
}

/**
 * A mask on a Material layer applies live on top of its source's bake: adding it leaves the bake
 * valid (no EEVEE re-render of the source), and a black mask hides the row in both the generator
 * and the CPU composite.
 */
TEST_F(PaintLayersGraphEvalTest, material_layer_mask_is_live_and_keeps_the_bake)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "MaterialMasked");
  Material *source = BKE_material_add(bmain, "MaterialMaskedSource");

  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Bottom", size, 255, 0, 0, 255));
  MaterialPaintLayer *material = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Material", nullptr, PaintLayerPlace::Above);
  material->material = source;
  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(*material);
  bake->size = size;
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(
      *ma, *material, channel, add_solid_image("MaskedBakeColor", size, 0, 255, 0, 255)));
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(
      *ma, *material, -1, add_solid_image("MaskedBakeCoverage", size, 255, 255, 255, 255)));
  BKE_paint_layers_bake_finalize(*ma, *material);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *material));

  /* The row's own settings are not what the source was baked from. */
  set_mask_image(*material, add_solid_image("MaskedMask", size, 0, 0, 0, 255));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, material, 0.75f));
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *material));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result(result_name(channel));
  const RGBA cpu = cpu_pixel(channel);

  /* The black mask hides the Material row: the red bottom shows, on both sides. */
  EXPECT_NEAR(graph.r, 1.0f, 1e-2f);
  EXPECT_NEAR(graph.g, 0.0f, 1e-2f);
  EXPECT_NEAR(cpu.r, graph.r, 1e-2f);
  EXPECT_NEAR(cpu.g, graph.g, 1e-2f);
  EXPECT_NEAR(cpu.b, graph.b, 1e-2f);
}

/**
 * A Paint correction on a Material row: its opacity is a value of the row group's interface and must
 * reach the correction's Mix factor in every row mode. Baked here (the row is not active).
 */
TEST_F(PaintLayersGraphEvalTest, material_layer_paint_correction_opacity_is_live)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "MaterialCorrOpacity");
  Material *source = BKE_material_add(bmain, "MaterialCorrOpacitySource");

  MaterialPaintLayer *material = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Material", nullptr, PaintLayerPlace::Above);
  material->material = source;
  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(*material);
  bake->size = size;
  bake->mode = MA_PAINT_LAYER_BAKE_ALWAYS;
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(
      *ma, *material, channel, add_solid_image("CorrOpacityBase", size, 255, 0, 0, 255)));
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(
      *ma, *material, -1, add_solid_image("CorrOpacityCov", size, 255, 255, 255, 255)));
  BKE_paint_layers_bake_finalize(*ma, *material);
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *material), PaintLayerMaterialMode::Baked);

  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, material, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "C");
  ASSERT_NE(correction, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, correction, channel);
  ASSERT_NE(record, nullptr);
  Image *blue = add_solid_image("CorrOpacityBlue", size, 0, 0, 255, 255);
  blue->alpha_mode = IMA_ALPHA_STRAIGHT;
  blue->flag |= IMA_GPU_LINEAR_PREMUL;
  record->image = blue;
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  auto eval_channel = [&]() {
    GraphInterpreter interpreter;
    interpreter.instance = find_instance();
    interpreter.tree = ma->paint_layers_tree;
    interpreter.x = 1;
    interpreter.y = 1;
    EXPECT_NE(interpreter.instance, nullptr);
    return interpreter.eval_result(result_name(channel));
  };

  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, correction, 0.0f));
  BKE_paint_layers_values_sync(*ma);
  const RGBA off = eval_channel();

  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, correction, 1.0f));
  BKE_paint_layers_values_sync(*ma);
  const RGBA on = eval_channel();

  /* Off: the baked red row. On: the correction's blue dominates. */
  EXPECT_GT(off.r, 0.5f);
  EXPECT_LT(off.b, 0.1f);
  EXPECT_GT(on.b, off.b + 0.1f);
  EXPECT_LT(on.r, off.r - 0.1f);
}

/**
 * The "below" a Custom row is fed during its GPU bake is the stack under it only: the rows above it
 * must not leak in, and the result is straight scene-linear with the below coverage in alpha.
 */
TEST_F(PaintLayersGraphEvalTest, below_image_composites_only_rows_under_the_custom_row)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "BelowImage");

  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Bottom", size, 255, 0, 0, 255));
  add_layer(
      "Middle", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Middle", size, 255, 255, 0, 255));
  MaterialPaintLayer *custom = BKE_paint_layers_custom_layer_add(
      *bmain, *ma, "Custom", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(custom, nullptr);
  /* A row above the custom one must not leak into its "below". */
  add_layer("Top", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Top", size, 0, 0, 255, 255));

  Image *below = BKE_paint_layers_below_image(*bmain, *ma, *custom, channel, size);
  ASSERT_NE(below, nullptr);
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(below, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  ASSERT_EQ(ibuf->x, size);
  const float *pixels = ibuf->float_data();
  ASSERT_NE(pixels, nullptr);
  const int64_t i = (int64_t(1) * size + 1) * 4;
  /* Middle (sRGB yellow) over Bottom (sRGB red) fully covers: straight linear yellow. */
  EXPECT_NEAR(pixels[i + 0], 1.0f, 1e-3f);
  EXPECT_NEAR(pixels[i + 1], 1.0f, 1e-3f);
  EXPECT_NEAR(pixels[i + 2], 0.0f, 1e-3f);
  EXPECT_NEAR(pixels[i + 3], 1.0f, 1e-3f);
  BKE_image_release_ibuf(below, ibuf, lock);
  BKE_id_free(bmain, below);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

}  // namespace blender::bke::tests
