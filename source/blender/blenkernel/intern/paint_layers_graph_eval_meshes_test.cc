/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */


/** \file
 * Paint Layers graph-vs-CPU tests: mesh maps and mapping (plan 9.3).
 *
 * Fixture and shared helpers live in
 * `paint_layers_graph_eval_test_fixture.hh` /
 * `paint_layers_graph_eval_test_shared.hh`; this TU holds only its own tests.
 */

#include "intern/paint_layers_graph_eval_test_fixture.hh"
#include "intern/paint_layers_graph_eval_test_shared.hh"

namespace blender::bke::tests {

TEST_F(PaintLayersGraphEvalTest, mesh_map_mask_and_correction_contribute_nothing)
{
  const int size = 4;
  const float tolerance = 1e-4f;
  const int channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;

  ma = BKE_material_add(bmain, "MeshMapNoContribution");
  add_layer("Bottom",
            MA_PAINT_LAYER_SOURCE_IMAGE,
            add_solid_image("MmBottom", size, 200, 40, 10, 255),
            eMaterialPaintChannel(channel));
  MaterialPaintLayer *top = add_layer(
      "Top", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("MmTop", size, 20, 180, 90, 200),
      eMaterialPaintChannel(channel));

  /* A normal Paint mask item and a normal content correction: these do change the result. */
  ASSERT_NE(set_mask_image(*top, add_solid_image("MmMask", size, 128, 128, 128, 255)), nullptr);
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, top, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "MmCorr");
  ASSERT_NE(correction, nullptr);
  MaterialPaintLayerChannel *correction_record = BKE_paint_layers_channel_add(
      *ma, correction, eMaterialPaintChannel(channel));
  ASSERT_NE(correction_record, nullptr);
  correction_record->image = add_solid_image("MmCorrMap", size, 40, 40, 220, 255);
  correction_record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  ASSERT_TRUE(BKE_paint_layers_correction_source_set(
      *ma, correction, MA_PAINT_LAYER_SOURCE_IMAGE));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(interpreter.instance, nullptr);
  interpreter.tree->ensure_topology_cache();
  const RGBA graph_before = eval_channel_result(interpreter, eMaterialPaintChannel(channel));
  const RGBA cpu_before = cpu_pixel(channel);

  /* A MESH_MAP mask item and a MESH_MAP content correction with no atlas assigned (the slot has no
   * image) read nothing and contribute nothing on either side (spec M2 item 4). */
  ASSERT_NE(BKE_paint_layers_correction_add(
                *ma, top, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_MESH_MAP, "MmMaskMap"),
            nullptr);
  ASSERT_NE(BKE_paint_layers_correction_add(
                *ma, top, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_MESH_MAP, "MmMapCorr"),
            nullptr);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter after;
  after.instance = find_instance();
  after.tree = ma->paint_layers_tree;
  ASSERT_NE(after.instance, nullptr);
  after.tree->ensure_topology_cache();
  const RGBA graph_after = eval_channel_result(after, eMaterialPaintChannel(channel));
  const RGBA cpu_after = cpu_pixel(channel);

  /* The stack without the MESH_MAP elements is the same picture, graph and CPU. */
  EXPECT_NEAR(graph_after.r, graph_before.r, tolerance);
  EXPECT_NEAR(graph_after.g, graph_before.g, tolerance);
  EXPECT_NEAR(graph_after.b, graph_before.b, tolerance);
  EXPECT_NEAR(graph_after.a, graph_before.a, tolerance);
  EXPECT_NEAR(cpu_after.r, cpu_before.r, tolerance);
  EXPECT_NEAR(cpu_after.g, cpu_before.g, tolerance);
  EXPECT_NEAR(cpu_after.b, cpu_before.b, tolerance);
  EXPECT_NEAR(cpu_after.a, cpu_before.a, tolerance);
  /* And the two sides still agree with each other. */
  EXPECT_NEAR(graph_after.r, cpu_after.r, tolerance);
  EXPECT_NEAR(graph_after.g, cpu_after.g, tolerance);
  EXPECT_NEAR(graph_after.b, cpu_after.b, tolerance);
  EXPECT_NEAR(graph_after.a, cpu_after.a, tolerance);
}

/**
 * The manual bilinear read both sides are specified to perform on a MESH_MAP atlas: the reference
 * texel centre `(x + 0.5) / ref_w` is looked up in the atlas at `u * W_atlas - 0.5`, blended over
 * the four neighbours and clamped to the edge (Linear + Extend). Written out here rather than
 * reused, so it independently pins the formula the graph and the CPU have to agree on.
 */
static float manual_atlas_red(const Image &atlas,
                              const int x,
                              const int y,
                              const int ref_w,
                              const int ref_h)
{
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(const_cast<Image *>(&atlas), nullptr, &lock);
  EXPECT_NE(ibuf, nullptr);
  if (ibuf == nullptr || ibuf->float_buffer.data == nullptr) {
    if (ibuf != nullptr) {
      BKE_image_release_ibuf(const_cast<Image *>(&atlas), ibuf, lock);
    }
    return 0.0f;
  }
  const float su = (float(x) + 0.5f) / float(ref_w) * float(ibuf->x) - 0.5f;
  const float sv = (float(y) + 0.5f) / float(ref_h) * float(ibuf->y) - 0.5f;
  const int x1 = int(floorf(su));
  const int y1 = int(floorf(sv));
  const float a = su - float(x1);
  const float b = sv - float(y1);
  const int x1c = min_ii(max_ii(x1, 0), ibuf->x - 1);
  const int x2c = min_ii(max_ii(x1 + 1, 0), ibuf->x - 1);
  const int y1c = min_ii(max_ii(y1, 0), ibuf->y - 1);
  const int y2c = min_ii(max_ii(y1 + 1, 0), ibuf->y - 1);
  const float *p11 = ibuf->float_buffer.data + (int64_t(y1c) * ibuf->x + x1c) * 4;
  const float *p21 = ibuf->float_buffer.data + (int64_t(y1c) * ibuf->x + x2c) * 4;
  const float *p12 = ibuf->float_buffer.data + (int64_t(y2c) * ibuf->x + x1c) * 4;
  const float *p22 = ibuf->float_buffer.data + (int64_t(y2c) * ibuf->x + x2c) * 4;
  const float red = (1.0f - a) * (1.0f - b) * p11[0] + a * (1.0f - b) * p21[0] +
                    (1.0f - a) * b * p12[0] + a * b * p22[0];
  BKE_image_release_ibuf(const_cast<Image *>(&atlas), ibuf, lock);
  return red;
}

namespace {

/** The interpreter pointed at the channel result of \a ma at pixel (px, py) of a ref_w x ref_h
 * reference grid. */
GraphInterpreter make_interpreter(Material &ma, const int px, const int py, const int ref_w, const int ref_h)
{
  GraphInterpreter interpreter;
  interpreter.instance = nullptr;
  for (bNode &node : ma.nodetree->nodes) {
    if (node.id == &ma.paint_layers_tree->id) {
      interpreter.instance = &node;
    }
  }
  interpreter.tree = ma.paint_layers_tree;
  interpreter.x = px;
  interpreter.y = py;
  interpreter.ref_width = ref_w;
  interpreter.ref_height = ref_h;
  interpreter.tree->ensure_topology_cache();
  return interpreter;
}

}  // namespace

/** Guard: the MESH_MAP row over a Paint row (graph: the Extend atlas chain in the generator; CPU:
 * the resampled grey in #composite_layer_build); revert either and the two part, and with no atlas
 * both sides must still drop the row entirely.
 */
TEST_F(PaintLayersGraphEvalTest, mesh_map_row_atlas_matches_the_cpu)
{
  const int size = 4;
  const float tolerance = 1e-4f;
  const int channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;

  ma = BKE_material_add(bmain, "MeshMapAtlasRow");
  add_layer("Bottom",
            MA_PAINT_LAYER_SOURCE_IMAGE,
            add_solid_image("MmBottom", size, 200, 40, 10, 128),
            eMaterialPaintChannel(channel));

  const RGBA baseline = cpu_pixel(channel);

  /* The row, before any atlas: the slot has no image, so the row is exactly absent (M2 item 4). */
  MaterialPaintLayer *row = add_mesh_map_layer("AO Row", MA_MESH_MAP_AO, 0.5f);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter before = make_interpreter(*ma, 1, 1, size, size);
  const RGBA graph_before = eval_channel_result(before, eMaterialPaintChannel(channel));
  const RGBA cpu_before = cpu_pixel(channel);
  EXPECT_NEAR(graph_before.r, baseline.r, tolerance);
  EXPECT_NEAR(graph_before.g, baseline.g, tolerance);
  EXPECT_NEAR(graph_before.b, baseline.b, tolerance);
  EXPECT_NEAR(graph_before.a, baseline.a, tolerance);
  EXPECT_NEAR(cpu_before.r, baseline.r, tolerance);
  EXPECT_NEAR(cpu_before.g, baseline.g, tolerance);
  EXPECT_NEAR(cpu_before.b, baseline.b, tolerance);
  EXPECT_NEAR(cpu_before.a, baseline.a, tolerance);

  /* With the atlas the row mixes its grey over the stack at its opacity: Mix at 0.5. */
  Image *atlas = add_gradient_atlas("MmAtlas", 8, 8);
  ASSERT_TRUE(BKE_mesh_maps_slot_image_set(*ma, MA_MESH_MAP_AO, atlas));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter after = make_interpreter(*ma, 1, 1, size, size);
  const RGBA graph_after = eval_channel_result(after, eMaterialPaintChannel(channel));
  const RGBA cpu_after = cpu_pixel(channel);

  const float grey = manual_atlas_red(*atlas, 1, 1, size, size);
  EXPECT_NEAR(graph_after.r, cpu_after.r, tolerance);
  EXPECT_NEAR(graph_after.g, cpu_after.g, tolerance);
  EXPECT_NEAR(graph_after.b, cpu_after.b, tolerance);
  EXPECT_NEAR(graph_after.a, cpu_after.a, tolerance);
  EXPECT_NEAR(cpu_after.r, baseline.r + 0.5f * (grey - baseline.r), tolerance);
  EXPECT_NEAR(cpu_after.g, baseline.g + 0.5f * (grey - baseline.g), tolerance);
  EXPECT_NEAR(cpu_after.b, baseline.b + 0.5f * (grey - baseline.b), tolerance);
  EXPECT_NEAR(cpu_after.a, baseline.a + 0.5f * (1.0f - baseline.a), tolerance);
}

/** Guard: a MESH_MAP mask item reads the atlas R as its coverage (the generator's Separate-X grey
 * and the CPU's #mesh_map_mask_reads_red); revert either and the factor is wrong on one side. */
TEST_F(PaintLayersGraphEvalTest, mesh_map_mask_item_atlas_matches_the_cpu)
{
  const int size = 4;
  const float tolerance = 1e-4f;
  const int channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;

  ma = BKE_material_add(bmain, "MeshMapAtlasMask");
  MaterialPaintLayer *owner = add_layer(
      "Owner", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("MmOwner", size, 30, 200, 90, 255),
      eMaterialPaintChannel(channel));
  const RGBA baseline = cpu_pixel(channel);
  /* The mask item lays the atlas R over the owner's factor at full opacity. */
  MaterialPaintLayer *item = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_MESH_MAP, "AO Mask");
  ASSERT_NE(item, nullptr);
  Image *atlas = add_gradient_atlas("MmMaskAtlas", 8, 8);
  ASSERT_TRUE(BKE_mesh_maps_slot_image_set(*ma, MA_MESH_MAP_AO, atlas));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter interpreter = make_interpreter(*ma, 1, 1, size, size);
  const RGBA graph = eval_channel_result(interpreter, eMaterialPaintChannel(channel));
  const RGBA cpu = cpu_pixel(channel);
  const float red = manual_atlas_red(*atlas, 1, 1, size, size);
  /* The factor is the atlas R: the result is the owner's colour scaled by it over the channel's
   * bottom constant (black), and the coverage reports it too. */
  EXPECT_NEAR(graph.r, cpu.r, tolerance);
  EXPECT_NEAR(graph.g, cpu.g, tolerance);
  EXPECT_NEAR(graph.b, cpu.b, tolerance);
  EXPECT_NEAR(graph.a, cpu.a, tolerance);
  EXPECT_NEAR(cpu.r, baseline.r * red, tolerance);
  EXPECT_NEAR(cpu.g, baseline.g * red, tolerance);
  EXPECT_NEAR(cpu.b, baseline.b * red, tolerance);
  EXPECT_NEAR(cpu.a, 1.0f, tolerance);
}

/** Guard: a MESH_MAP content correction spreads the atlas R to grey (the generator's
 * Separate/Combine and the CPU's scalar resample); revert either and the colour is wrong. */
TEST_F(PaintLayersGraphEvalTest, mesh_map_effect_atlas_matches_the_cpu)
{
  const int size = 4;
  const float tolerance = 1e-4f;
  const int channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;

  ma = BKE_material_add(bmain, "MeshMapAtlasEffect");
  MaterialPaintLayer *owner = add_layer(
      "Owner", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("MmOwner", size, 30, 200, 90, 255),
      eMaterialPaintChannel(channel));
  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_MESH_MAP, "AO Effect");
  ASSERT_NE(effect, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, effect, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  Image *atlas = add_gradient_atlas("MmEffectAtlas", 8, 8);
  ASSERT_TRUE(BKE_mesh_maps_slot_image_set(*ma, MA_MESH_MAP_AO, atlas));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter interpreter = make_interpreter(*ma, 1, 1, size, size);
  const RGBA graph = eval_channel_result(interpreter, eMaterialPaintChannel(channel));
  const RGBA cpu = cpu_pixel(channel);
  const float grey = manual_atlas_red(*atlas, 1, 1, size, size);
  /* The correction replaces the colour at full opacity: the grey the atlas R spreads to. */
  EXPECT_NEAR(graph.r, cpu.r, tolerance);
  EXPECT_NEAR(graph.g, cpu.g, tolerance);
  EXPECT_NEAR(graph.b, cpu.b, tolerance);
  EXPECT_NEAR(graph.a, cpu.a, tolerance);
  EXPECT_NEAR(cpu.r, grey, tolerance);
  EXPECT_NEAR(cpu.g, grey, tolerance);
  EXPECT_NEAR(cpu.b, grey, tolerance);
  EXPECT_NEAR(cpu.a, 1.0f, tolerance);
}

/**
 * Guard: the CPU acquires the atlas' buffer on demand (#composite_image_acquire, like every
 * painted map) instead of the resolver's old loaded-buffer check; restore that check and an
 * assigned-but-unloaded atlas drops out of the CPU side while the graph keeps drawing it.
 */
TEST_F(PaintLayersGraphEvalTest, mesh_map_atlas_buffer_is_acquired_on_demand)
{
  const int size = 4;
  const float tolerance = 1e-4f;
  const int channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;

  ma = BKE_material_add(bmain, "MeshMapAtlasReload");
  add_layer("Bottom",
            MA_PAINT_LAYER_SOURCE_IMAGE,
            add_solid_image("MmBottom", size, 200, 40, 10, 128),
            eMaterialPaintChannel(channel));
  const RGBA baseline = cpu_pixel(channel);
  add_mesh_map_layer("AO Row", MA_MESH_MAP_AO, 0.5f);
  Image *atlas = add_gradient_atlas("MmAtlas", 8, 8);
  ASSERT_TRUE(BKE_mesh_maps_slot_image_set(*ma, MA_MESH_MAP_AO, atlas));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter interpreter = make_interpreter(*ma, 1, 1, size, size);
  const RGBA graph_loaded = eval_channel_result(interpreter, eMaterialPaintChannel(channel));
  const RGBA cpu_loaded = cpu_pixel(channel);
  const float grey = manual_atlas_red(*atlas, 1, 1, size, size);
  /* The row mixes its grey over the stack at its opacity, graph and CPU alike. */
  EXPECT_NEAR(graph_loaded.r, cpu_loaded.r, tolerance);
  EXPECT_NEAR(graph_loaded.a, cpu_loaded.a, tolerance);
  EXPECT_NEAR(cpu_loaded.r, baseline.r + 0.5f * (grey - baseline.r), tolerance);
  EXPECT_NEAR(cpu_loaded.g, baseline.g + 0.5f * (grey - baseline.g), tolerance);
  EXPECT_NEAR(cpu_loaded.b, baseline.b + 0.5f * (grey - baseline.b), tolerance);

  /* Unload the buffer and re-evaluate: the CPU has to load it back itself, and the two sides have
   * to agree on whatever the re-acquired buffer holds. */
  BKE_image_free_buffers(atlas);
  ASSERT_FALSE(BKE_image_has_loaded_ibuf(atlas));

  /* The CPU goes first: its own acquire has to re-populate the cache, so the graph (which loads
   * the buffer the same way) cannot mask a CPU that drops the row instead. */
  const RGBA cpu_unloaded = cpu_pixel(channel);
  /* The CPU's acquire re-populated the cache. */
  EXPECT_TRUE(BKE_image_has_loaded_ibuf(atlas));
  const float grey_reloaded = manual_atlas_red(*atlas, 1, 1, size, size);
  GraphInterpreter reloaded = make_interpreter(*ma, 1, 1, size, size);
  const RGBA graph_unloaded = eval_channel_result(reloaded, eMaterialPaintChannel(channel));
  EXPECT_NEAR(graph_unloaded.r, cpu_unloaded.r, tolerance);
  EXPECT_NEAR(graph_unloaded.g, cpu_unloaded.g, tolerance);
  EXPECT_NEAR(graph_unloaded.b, cpu_unloaded.b, tolerance);
  EXPECT_NEAR(graph_unloaded.a, cpu_unloaded.a, tolerance);
  /* The re-acquired atlas holds the generated fill (black), so the mix collapses towards it. */
  EXPECT_NEAR(cpu_unloaded.r, baseline.r + 0.5f * (grey_reloaded - baseline.r), tolerance);
  EXPECT_NEAR(cpu_unloaded.g, baseline.g + 0.5f * (grey_reloaded - baseline.g), tolerance);
  EXPECT_NEAR(cpu_unloaded.b, baseline.b + 0.5f * (grey_reloaded - baseline.b), tolerance);
  EXPECT_NEAR(cpu_unloaded.a, cpu_loaded.a, tolerance);
}

/**
 * Guard: the parent chain's `row_one` (a row without a Content Alpha output counts as opaque) --
 * a Material row over a partially transparent Paint row must raise the result alpha by the over
 * model, its source Alpha sitting in the row's coverage, exactly as the CPU computes it.
 */
TEST_F(PaintLayersGraphEvalTest, material_row_source_alpha_blends_the_content_alpha_over_the_below)
{
  const int size = 4;
  const float tolerance = 1e-3f;
  const int channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;

  ma = BKE_material_add(bmain, "MaterialRowOverPartial");
  add_layer("Bottom",
            MA_PAINT_LAYER_SOURCE_IMAGE,
            add_solid_image("MrBottom", size, 200, 40, 10, 128),
            eMaterialPaintChannel(channel));
  const RGBA baseline = cpu_pixel(channel);
  ASSERT_NEAR(baseline.a, 0.75f, 5e-3f);

  /* A specular source with its Principled Alpha at 0.5: the row's coverage, not a content alpha.
   * The spec is source_spec_b with the alpha swapped; the colour itself is not checked absolutely. */
  const HybridSourceSpec source_alpha_half = {
      {0.15f, 0.30f, 0.60f}, 0.80f, 0.42f, 0.20f, 0.50f, {0.05f, 0.10f, 0.40f}};
  Material *source = build_hybrid_source(*bmain, "MrSource", "MrNormal", source_alpha_half);
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  /* The source Alpha is the row's coverage, and Alpha is outside the build default set. */
  channel_set_extend(*ma, channel_bit(PAINT_MATERIAL_CHANNEL_ALPHA));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  BKE_paint_layers_values_sync(*ma);

  GraphInterpreter interpreter = make_interpreter(*ma, 1, 1, size, size);
  const RGBA graph = eval_channel_result(interpreter, eMaterialPaintChannel(channel));
  const RGBA cpu = cpu_pixel(channel);
  /* The over model with an opaque row at coverage 0.5: a = a + f * (1 - a). */
  EXPECT_NEAR(graph.a, cpu.a, tolerance);
  EXPECT_NEAR(graph.r, cpu.r, tolerance);
  EXPECT_NEAR(graph.g, cpu.g, tolerance);
  EXPECT_NEAR(graph.b, cpu.b, tolerance);
  EXPECT_NEAR(cpu.a, baseline.a + 0.5f * (1.0f - baseline.a), 5e-3f);
}

/**
 * The Alpha set bit gates the Principled Alpha output, never the Material row's coverage factor. A
 * source whose Alpha is a live constant drives the row's coverage with Alpha out of the set -- the
 * factor is not a channel output -- and the graph and the CPU agree. Naming Alpha in the set only
 * adds `Result Alpha`.
 */
TEST_F(PaintLayersGraphEvalTest, material_row_coverage_ignores_the_alpha_set_bit)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  const float tolerance = 1e-4f;

  ma = BKE_material_add(bmain, "CoverageAlphaSet");
  add_layer("Bottom",
            MA_PAINT_LAYER_SOURCE_IMAGE,
            add_solid_image("CovAlphaBottom", size, 200, 40, 10, 128),
            bc);
  const RGBA baseline = cpu_pixel(bc);

  const HybridSourceSpec spec = {
      {0.15f, 0.30f, 0.60f}, 0.80f, 0.42f, 0.20f, 0.50f, {0.05f, 0.10f, 0.40f}};
  Material *source = build_hybrid_source(*bmain, "CovAlphaSource", "CovAlphaNormal", spec);
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));

  /* Alpha is not part of the build default set, yet the live source Alpha is the row's coverage. */
  EXPECT_FALSE(BKE_paint_layers_channel_in_set(*ma, PAINT_MATERIAL_CHANNEL_ALPHA));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_FALSE(result_output_exists(*ma->paint_layers_tree, PAINT_MATERIAL_CHANNEL_ALPHA));

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = eval_channel_result(interpreter, bc);
  const RGBA cpu = cpu_pixel(bc);
  EXPECT_NEAR(graph.a, cpu.a, tolerance);
  EXPECT_NEAR(graph.a, baseline.a + 0.5f * (1.0f - baseline.a), 5e-3f);

  /* Naming Alpha in the set adds the Principled Alpha output; the coverage is unchanged. */
  ASSERT_TRUE(BKE_paint_layers_channel_set_enable(*ma, PAINT_MATERIAL_CHANNEL_ALPHA, true));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_TRUE(result_output_exists(*ma->paint_layers_tree, PAINT_MATERIAL_CHANNEL_ALPHA));

  GraphInterpreter interpreter_set;
  interpreter_set.instance = find_instance();
  interpreter_set.tree = ma->paint_layers_tree;
  interpreter_set.x = 1;
  interpreter_set.y = 1;
  ASSERT_NE(interpreter_set.instance, nullptr);
  const RGBA graph_set = eval_channel_result(interpreter_set, bc);
  EXPECT_NEAR(graph_set.a, graph.a, tolerance);
}

/** Guard: the same over model for a MESH_MAP row over a partially transparent Paint row -- the
 * row_one constant feeds the chain, and the atlas' ignored alpha never clips the coverage. */
TEST_F(PaintLayersGraphEvalTest, mesh_map_row_over_partial_alpha_reports_the_over_alpha)
{
  const int size = 4;
  const float tolerance = 1e-4f;
  const int channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;

  ma = BKE_material_add(bmain, "MeshMapOverPartial");
  add_layer("Bottom",
            MA_PAINT_LAYER_SOURCE_IMAGE,
            add_solid_image("MmBottom", size, 200, 40, 10, 128),
            eMaterialPaintChannel(channel));
  const RGBA baseline = cpu_pixel(channel);
  ASSERT_NEAR(baseline.a, 0.75f, 5e-3f);

  add_mesh_map_layer("AO Row", MA_MESH_MAP_AO, 0.5f);
  /* A constant atlas: the grey value cannot muddy the alpha check. */
  Image *atlas = add_gradient_atlas("MmAtlas", 2, 1);
  {
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(atlas, nullptr, &lock);
    ASSERT_NE(ibuf, nullptr);
    ASSERT_NE(ibuf->float_data(), nullptr);
    float *pixels = ibuf->float_data_for_write();
    for (const int64_t i : IndexRange(int64_t(2))) {
      pixels[i * 4 + 0] = 0.5f;
      pixels[i * 4 + 1] = 0.0f;
      pixels[i * 4 + 2] = 0.0f;
      pixels[i * 4 + 3] = 1.0f;
    }
    BKE_image_release_ibuf(atlas, ibuf, lock);
  }
  ASSERT_TRUE(BKE_mesh_maps_slot_image_set(*ma, MA_MESH_MAP_AO, atlas));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter interpreter = make_interpreter(*ma, 1, 1, size, size);
  const RGBA graph = eval_channel_result(interpreter, eMaterialPaintChannel(channel));
  const RGBA cpu = cpu_pixel(channel);
  /* The over model at coverage 1, opacity 0.5: a = a + f * (1 - a). */
  EXPECT_NEAR(graph.a, cpu.a, tolerance);
  EXPECT_NEAR(graph.r, cpu.r, tolerance);
  EXPECT_NEAR(graph.g, cpu.g, tolerance);
  EXPECT_NEAR(graph.b, cpu.b, tolerance);
  EXPECT_NEAR(cpu.a, baseline.a + 0.5f * (1.0f - baseline.a), tolerance);
}

/** Guard: the bilinear atlas read (the CPU's resample and the graph's Extend sampling) and the
 * shared resolver; a differently sized atlas must resample, not reject the stack, and the formula
 * is pinned by the manual calculation. */
TEST_F(PaintLayersGraphEvalTest, mesh_map_atlas_other_resolutions_match_the_manual_bilinear)
{
  const int size = 4;
  const float tolerance = 1e-4f;
  const int channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;

  ma = BKE_material_add(bmain, "MeshMapAtlasSizes");
  add_layer("Bottom",
            MA_PAINT_LAYER_SOURCE_IMAGE,
            add_solid_image("MmBottom", size, 200, 40, 10, 255),
            eMaterialPaintChannel(channel));
  add_mesh_map_layer("AO Row", MA_MESH_MAP_AO, 1.0f);
  Image *double_atlas = add_gradient_atlas("MmAtlas2x", 8, 8);
  ASSERT_TRUE(BKE_mesh_maps_slot_image_set(*ma, MA_MESH_MAP_AO, double_atlas));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* Twice the resolution of the Paint map: the grey replaces the stack at full coverage. */
  for (const int px : {0, 1, 2, 3}) {
    GraphInterpreter interpreter = make_interpreter(*ma, px, px, size, size);
    const RGBA graph = eval_channel_result(interpreter, eMaterialPaintChannel(channel));
    const RGBA cpu = cpu_pixel_at(channel, px, px);
    const float red = manual_atlas_red(*double_atlas, px, px, size, size);
    EXPECT_NEAR(graph.r, cpu.r, tolerance) << px;
    EXPECT_NEAR(graph.g, cpu.g, tolerance) << px;
    EXPECT_NEAR(graph.b, cpu.b, tolerance) << px;
    EXPECT_NEAR(graph.a, cpu.a, tolerance) << px;
    EXPECT_NEAR(cpu.r, red, tolerance) << px;
    EXPECT_NEAR(cpu.g, red, tolerance) << px;
    EXPECT_NEAR(cpu.b, red, tolerance) << px;
  }

  /* An odd-sized atlas: the same formula, now off a 37 x 23 grid. */
  Image *odd_atlas = add_gradient_atlas("MmAtlasOdd", 37, 23);
  ASSERT_TRUE(BKE_mesh_maps_slot_image_set(*ma, MA_MESH_MAP_AO, odd_atlas));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  for (const int px : {0, 1, 2, 3}) {
    GraphInterpreter interpreter = make_interpreter(*ma, px, px, size, size);
    const RGBA graph = eval_channel_result(interpreter, eMaterialPaintChannel(channel));
    const RGBA cpu = cpu_pixel_at(channel, px, px);
    const float red = manual_atlas_red(*odd_atlas, px, px, size, size);
    EXPECT_NEAR(graph.r, cpu.r, tolerance) << px;
    EXPECT_NEAR(graph.g, cpu.g, tolerance) << px;
    EXPECT_NEAR(graph.b, cpu.b, tolerance) << px;
    EXPECT_NEAR(graph.a, cpu.a, tolerance) << px;
    EXPECT_NEAR(cpu.r, red, tolerance) << px;
    EXPECT_NEAR(cpu.g, red, tolerance) << px;
    EXPECT_NEAR(cpu.b, red, tolerance) << px;
  }
}

/** Guard: the sampling formula degenerates to the direct texel when the atlas matches the
 * reference grid; a half-texel offset or a needless resample would show here. */
TEST_F(PaintLayersGraphEvalTest, mesh_map_atlas_equal_size_reads_the_texel_directly)
{
  const int size = 4;
  const float tolerance = 1e-4f;
  const int channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;

  ma = BKE_material_add(bmain, "MeshMapAtlasEqual");
  add_layer("Bottom",
            MA_PAINT_LAYER_SOURCE_IMAGE,
            add_solid_image("MmBottom", size, 200, 40, 10, 255),
            eMaterialPaintChannel(channel));
  add_mesh_map_layer("AO Row", MA_MESH_MAP_AO, 1.0f);
  Image *atlas = add_gradient_atlas("MmAtlas1x", size, size);
  ASSERT_TRUE(BKE_mesh_maps_slot_image_set(*ma, MA_MESH_MAP_AO, atlas));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  for (const int px : {0, 1, 2, 3}) {
    GraphInterpreter interpreter = make_interpreter(*ma, px, px, size, size);
    const RGBA graph = eval_channel_result(interpreter, eMaterialPaintChannel(channel));
    const RGBA cpu = cpu_pixel_at(channel, px, px);
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(atlas, nullptr, &lock);
    ASSERT_NE(ibuf, nullptr);
    const float direct = ibuf->float_buffer.data[(int64_t(px) * size + px) * 4];
    BKE_image_release_ibuf(atlas, ibuf, lock);
    EXPECT_NEAR(graph.r, cpu.r, tolerance) << px;
    EXPECT_NEAR(graph.g, cpu.g, tolerance) << px;
    EXPECT_NEAR(graph.b, cpu.b, tolerance) << px;
    EXPECT_NEAR(graph.a, cpu.a, tolerance) << px;
    EXPECT_NEAR(cpu.r, direct, tolerance) << px;
    EXPECT_NEAR(cpu.g, direct, tolerance) << px;
    EXPECT_NEAR(cpu.b, direct, tolerance) << px;
  }
}

/**
 * The reference for a tangent-space normal read through a UV mapping `uv' = R * (S * uv) + t`: the
 * covariant counterpart `S * R^T` of the transform, with only the sign of the scale kept (a
 * magnitude does not turn a direction).
 */
static void normal_remap_reference(const float n[3],
                                   const float rotation,
                                   const float scale_x,
                                   const float scale_y,
                                   float r_out[3])
{
  const float cos_rot = cosf(rotation);
  const float sin_rot = sinf(rotation);
  const float sign_x = scale_x < 0.0f ? -1.0f : 1.0f;
  const float sign_y = scale_y < 0.0f ? -1.0f : 1.0f;
  r_out[0] = sign_x * (cos_rot * n[0] + sin_rot * n[1]);
  r_out[1] = sign_y * (-sin_rot * n[0] + cos_rot * n[1]);
  r_out[2] = n[2];
}

/** Evaluate the generated Normal Remap chain on the decoded normal \a n; returns its node count. */
static int normal_remap_chain_eval(Main &bmain,
                                   const float n[3],
                                   const float rotation,
                                   const float scale[3],
                                   float r_out[3])
{
  bNodeTree *tree = bke::node_tree_add_tree(&bmain, "NormalRemapChain", "ShaderNodeTree");
  bNode *source = bke::node_add_static_node(nullptr, *tree, SH_NODE_RGB);
  bNode *rotation_node = bke::node_add_static_node(nullptr, *tree, SH_NODE_RGB);
  bNode *scale_node = bke::node_add_static_node(nullptr, *tree, SH_NODE_RGB);
  bNodeSocket *source_out = bke::node_find_socket(*source, SOCK_OUT, "Color"_ustr);
  bNodeSocket *rotation_out = bke::node_find_socket(*rotation_node, SOCK_OUT, "Color"_ustr);
  bNodeSocket *scale_out = bke::node_find_socket(*scale_node, SOCK_OUT, "Color"_ustr);
  const float encoded[4] = {n[0] * 0.5f + 0.5f, n[1] * 0.5f + 0.5f, n[2] * 0.5f + 0.5f, 1.0f};
  const float rotation_value[4] = {0.0f, 0.0f, rotation, 1.0f};
  const float scale_value[4] = {scale[0], scale[1], scale[2], 1.0f};
  copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(source_out->default_value)->value, encoded);
  copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(rotation_out->default_value)->value,
             rotation_value);
  copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(scale_out->default_value)->value, scale_value);
  bNode *out_node = nullptr;
  bNodeSocket *out = bke::paint_layers::normal_remap_nodes_add(*tree,
                                                               *source,
                                                               *source_out,
                                                               {rotation_node, rotation_out},
                                                               {scale_node, scale_out},
                                                               0.0f,
                                                               0.0f,
                                                               out_node);
  tree->ensure_topology_cache();
  GraphInterpreter interpreter;
  interpreter.tree = tree;
  const RGBA encoded_out = interpreter.eval_output(*out_node, out->identifier);
  r_out[0] = encoded_out.r * 2.0f - 1.0f;
  r_out[1] = encoded_out.g * 2.0f - 1.0f;
  r_out[2] = encoded_out.b * 2.0f - 1.0f;
  return BLI_listbase_count(&tree->nodes);
}

/**
 * The generated Normal Remap chain: identity for a zero rotation and unit scale, a rotation by the
 * mapping angle, and a flip by the sign of each scale axis (never its magnitude). The order is the
 * covariant `S * R^T`, so a rotation followed by a mirror differs from the reverse.
 */
TEST_F(PaintLayersGraphEvalTest, normal_remap_chain_rotates_and_mirrors_the_vectors)
{
  const float tolerance = 1e-5f;
  const float half_pi = 1.5707964f;
  struct Case {
    float n[3];
    float rotation;
    float scale[3];
  };
  const Case cases[] = {
      /* (a) Identity. */
      {{0.3f, -0.5f, 0.8f}, 0.0f, {1.0f, 1.0f, 1.0f}},
      /* (b) Quarter and half turns on the axes. */
      {{1.0f, 0.0f, 0.0f}, half_pi, {1.0f, 1.0f, 1.0f}},
      {{0.0f, 1.0f, 0.0f}, half_pi, {1.0f, 1.0f, 1.0f}},
      {{1.0f, 0.0f, 0.0f}, 2.0f * half_pi, {1.0f, 1.0f, 1.0f}},
      {{0.0f, 1.0f, 0.0f}, 2.0f * half_pi, {1.0f, 1.0f, 1.0f}},
      /* (c) Mirror by U, by V and by both. */
      {{0.3f, 0.5f, 0.8f}, 0.0f, {-1.0f, 1.0f, 1.0f}},
      {{0.3f, 0.5f, 0.8f}, 0.0f, {1.0f, -1.0f, 1.0f}},
      {{0.3f, 0.5f, 0.8f}, 0.0f, {-1.0f, -1.0f, 1.0f}},
      /* (d) Rotation and mirror together fix the operation order. */
      {{1.0f, 0.0f, 0.0f}, half_pi, {-1.0f, 1.0f, 1.0f}},
      {{0.0f, 1.0f, 0.0f}, half_pi, {-1.0f, 1.0f, 1.0f}},
      {{0.6f, -0.2f, 0.7f}, 0.7f, {1.0f, -1.0f, 1.0f}},
      /* The magnitude of the scale and a free angle do not change the sign rule. */
      {{0.6f, -0.2f, 0.7f}, -1.1f, {3.0f, -0.25f, 1.0f}},
  };
  int index = 0;
  for (const Case &test : cases) {
    SCOPED_TRACE(::testing::Message() << "case " << index++);
    float expected[3];
    normal_remap_reference(test.n, test.rotation, test.scale[0], test.scale[1], expected);
    float got[3];
    normal_remap_chain_eval(*bmain, test.n, test.rotation, test.scale, got);
    EXPECT_NEAR(got[0], expected[0], tolerance);
    EXPECT_NEAR(got[1], expected[1], tolerance);
    EXPECT_NEAR(got[2], expected[2], tolerance);
  }
  /* The identity case is the input itself, and the quarter turn sends +Y to +X. */
  float identity[3];
  const float unit_scale[3] = {1.0f, 1.0f, 1.0f};
  const float tilted[3] = {0.3f, -0.5f, 0.8f};
  normal_remap_chain_eval(*bmain, tilted, 0.0f, unit_scale, identity);
  EXPECT_NEAR(identity[0], 0.3f, tolerance);
  EXPECT_NEAR(identity[1], -0.5f, tolerance);
  EXPECT_NEAR(identity[2], 0.8f, tolerance);
  float turned[3];
  const float up[3] = {0.0f, 1.0f, 0.0f};
  normal_remap_chain_eval(*bmain, up, half_pi, unit_scale, turned);
  EXPECT_NEAR(turned[0], 1.0f, tolerance);
  EXPECT_NEAR(turned[1], 0.0f, tolerance);
}

/** Changing the scale or the rotation values never changes the chain's node count. */
TEST_F(PaintLayersGraphEvalTest, normal_remap_chain_topology_ignores_the_values)
{
  const float n[3] = {0.3f, 0.5f, 0.8f};
  const float unit[3] = {1.0f, 1.0f, 1.0f};
  const float other[3] = {-4.0f, 0.5f, 1.0f};
  float out[3];
  const int base = normal_remap_chain_eval(*bmain, n, 0.0f, unit, out);
  EXPECT_EQ(normal_remap_chain_eval(*bmain, n, 1.3f, other, out), base);
}

/**
 * A mapped Fill row on the Normal channel: the graph and the CPU both rotate and mirror the map's
 * vectors before they are combined, and the closed form agrees. A Base Color row of the same
 * mapping reads its texels unchanged apart from the read point.
 */
TEST_F(PaintLayersGraphEvalTest, mapped_fill_normal_follows_rotation_and_mirror_in_graph_and_cpu)
{
  const int size = 4;
  const float tolerance = 1e-4f;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_NORMAL;
  const float rotation = 1.5707964f;
  const float scale[2] = {-2.0f, 1.0f};

  ma = BKE_material_add(bmain, "MappedFillNormal");
  Image *bottom_map = add_solid_image("MappedFillNormalBottom", size, 128, 128, 255, 255);
  make_image_data(bottom_map);
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, bottom_map, channel);

  Image *detail_map = add_solid_image("MappedFillNormalDetail", size, 200, 100, 220, 255);
  make_image_data(detail_map);
  MaterialPaintLayer *fill = add_layer("MappedFill", MA_PAINT_LAYER_SOURCE_CONSTANT, detail_map, channel);
  ASSERT_NE(fill, nullptr);
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, fill, true));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_rotation(*ma, fill, rotation));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(*ma, fill, scale));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA bottom_enc = interpreter.sample_image(bottom_map, "Color");
  const RGBA detail_enc = interpreter.sample_image(detail_map, "Color");
  const float detail[3] = {detail_enc.r * 2.0f - 1.0f,
                           detail_enc.g * 2.0f - 1.0f,
                           detail_enc.b * 2.0f - 1.0f};
  float remapped[3];
  normal_remap_reference(detail, rotation, scale[0], scale[1], remapped);
  float expected[3];
  normal_result_reference(bottom_enc, remapped, 1.0f, expected);
  float unmapped[3];
  normal_result_reference(bottom_enc, detail, 1.0f, unmapped);
  EXPECT_GT(fabsf(expected[0] - unmapped[0]) + fabsf(expected[1] - unmapped[1]), 0.05f);

  for (int x = 0; x < size; x++) {
    interpreter.x = x;
    interpreter.y = 0;
    const RGBA graph = interpreter.eval_result(result_name(channel));
    const RGBA cpu = cpu_pixel_at(channel, x, 0);
    EXPECT_NEAR(graph.r, expected[0], tolerance) << "graph x=" << x;
    EXPECT_NEAR(graph.g, expected[1], tolerance) << "graph x=" << x;
    EXPECT_NEAR(graph.b, expected[2], tolerance) << "graph x=" << x;
    EXPECT_NEAR(cpu.r, expected[0], tolerance) << "cpu x=" << x;
    EXPECT_NEAR(cpu.g, expected[1], tolerance) << "cpu x=" << x;
    EXPECT_NEAR(cpu.b, expected[2], tolerance) << "cpu x=" << x;
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/** Mapping nodes of \a type_custom1 in \a tree and every wrapper copy under it. */
static int mapping_nodes_of_type(const bNodeTree &tree, const int mapping_type)
{
  int count = 0;
  for (const bNode &node : tree.nodes) {
    if (node.type_legacy == SH_NODE_MAPPING && node.custom1 == mapping_type) {
      count++;
    }
  }
  return count;
}

/**
 * A mapped SourceGroup row whose Normal comes from a Normal Map over a texture: the wrapper
 * re-orients the Color it exposes by the row's rotation and the sign of its scale, fed by the
 * wrapper's mapping inputs.
 */
TEST_F(PaintLayersGraphEvalTest, source_group_normal_map_color_follows_the_row_mapping)
{
  const int size = 4;
  const float tolerance = 1e-4f;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_NORMAL;
  const float rotation = 1.5707964f;
  const float scale[2] = {-1.0f, 1.0f};

  ma = BKE_material_add(bmain, "SGMappedNormalMap");
  Image *bottom_map = add_solid_image("SGMappedNormalMapBottom", size, 128, 128, 255, 255);
  make_image_data(bottom_map);
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, bottom_map, channel);
  Image *source_normal = add_solid_image("SGMappedNormalMapTexture", size, 200, 100, 220, 255);
  make_image_data(source_normal);

  bNode *principled = nullptr;
  Material *source = make_wrapper_forced_source(*bmain, "SGMappedNormalMapSource", &principled);
  bNodeTree &ntree = *source->nodetree;
  bNode *texture = bke::node_add_static_node(nullptr, ntree, SH_NODE_TEX_IMAGE);
  ASSERT_NE(texture, nullptr);
  texture->id = &source_normal->id;
  id_us_plus(&source_normal->id);
  bNode *normal_map = bke::node_add_static_node(nullptr, ntree, SH_NODE_NORMAL_MAP);
  ASSERT_NE(normal_map, nullptr);
  bke::node_add_link(ntree,
                     *texture,
                     *bke::node_find_socket(*texture, SOCK_OUT, "Color"_ustr),
                     *normal_map,
                     *bke::node_find_socket(*normal_map, SOCK_IN, "Color"_ustr));
  bke::node_add_link(ntree,
                     *normal_map,
                     *bke::node_find_socket(*normal_map, SOCK_OUT, "Normal"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Normal"_ustr));

  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, row, true));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_rotation(*ma, row, rotation));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(*ma, row, scale));
  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);

  bNodeTree *wrapper = wrapper_tree_find(*bmain, "SGMappedNormalMapSource");
  ASSERT_NE(wrapper, nullptr);
  /* One Texture-type Mapping is the remap; the source's UV Mapping is of the Point type. */
  EXPECT_EQ(mapping_nodes_of_type(*wrapper, NODE_MAPPING_TYPE_TEXTURE), 1);

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA bottom_enc = interpreter.sample_image(bottom_map, "Color");
  const RGBA row_enc = interpreter.sample_image(source_normal, "Color");
  const float detail[3] = {row_enc.r * 2.0f - 1.0f, row_enc.g * 2.0f - 1.0f, row_enc.b * 2.0f - 1.0f};
  float remapped[3];
  normal_remap_reference(detail, rotation, scale[0], scale[1], remapped);
  float expected[3];
  normal_result_reference(bottom_enc, remapped, 1.0f, expected);
  for (int x = 0; x < size; x++) {
    interpreter.x = x;
    interpreter.y = 0;
    const RGBA graph = interpreter.eval_result(result_name(channel));
    EXPECT_NEAR(graph.r, expected[0], tolerance) << "x=" << x;
    EXPECT_NEAR(graph.g, expected[1], tolerance) << "x=" << x;
    EXPECT_NEAR(graph.b, expected[2], tolerance) << "x=" << x;
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * The Bump / computed-normal branch of a mapped wrapper encodes a world-space-like vector, not a
 * tangent-space map read through the UVs, so the remap must not touch it.
 */
TEST_F(PaintLayersGraphEvalTest, source_group_computed_normal_gets_no_remap_when_mapped)
{
  const int size = 4;
  const float tolerance = 1e-4f;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_NORMAL;
  const float raw[3] = {2.0f, 1.0f, 0.5f};

  ma = BKE_material_add(bmain, "SGMappedComputed");
  Image *bottom_map = add_solid_image("SGMappedComputedBottom", size, 128, 128, 255, 255);
  make_image_data(bottom_map);
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, bottom_map, channel);

  bNode *principled = nullptr;
  Material *source = make_wrapper_forced_source(*bmain, "SGMappedComputedSource", &principled);
  bNode *normal = bke::node_add_static_node(nullptr, *source->nodetree, SH_NODE_VECTOR_MATH);
  ASSERT_NE(normal, nullptr);
  normal->custom1 = NODE_VECTOR_MATH_NORMALIZE;
  bNodeSocket *normal_in = bke::node_find_socket(*normal, SOCK_IN, "Vector"_ustr);
  copy_v3_v3(static_cast<bNodeSocketValueVector *>(normal_in->default_value)->value, raw);
  bke::node_add_link(*source->nodetree,
                     *normal,
                     *bke::node_find_socket(*normal, SOCK_OUT, "Vector"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Normal"_ustr));

  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, row, true));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_rotation(*ma, row, 1.5707964f));
  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);

  bNodeTree *wrapper = wrapper_tree_find(*bmain, "SGMappedComputedSource");
  ASSERT_NE(wrapper, nullptr);
  EXPECT_EQ(mapping_nodes_of_type(*wrapper, NODE_MAPPING_TYPE_TEXTURE), 0);

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA bottom_enc = interpreter.sample_image(bottom_map, "Color");
  float detail[3] = {raw[0], raw[1], raw[2]};
  normalize_v3(detail);
  float expected[3];
  normal_result_reference(bottom_enc, detail, 1.0f, expected);
  interpreter.x = 0;
  interpreter.y = 0;
  const RGBA graph = interpreter.eval_result(result_name(channel));
  EXPECT_NEAR(graph.r, expected[0], tolerance);
  EXPECT_NEAR(graph.g, expected[1], tolerance);
  EXPECT_NEAR(graph.b, expected[2], tolerance);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

}  // namespace blender::bke::tests
