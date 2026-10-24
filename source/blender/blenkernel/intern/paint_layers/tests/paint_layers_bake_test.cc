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

#include "BKE_lib_id.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_layers_gradient.hh"

#include "../paint_layers_intern.hh"

namespace blender::bke::tests {

class PaintLayersBakeTest : public PaintLayersGenerateTest {
 public:
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
  ASSERT_EQ(layer->gradient->space, MA_PAINT_LAYER_GRADIENT_SPACE_UV);
  EXPECT_TRUE(BKE_paint_layers_gradient_is_shared_bakeable(*layer));
  EXPECT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *layer, MA_PAINT_LAYER_BAKE_ALWAYS));
  clear_active();

  bool changed = false;
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_TRUE(changed);
  ASSERT_NE(layer->bake, nullptr);
  EXPECT_NE(layer->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR], nullptr);
  EXPECT_NE(layer->bake->coverage, nullptr);
}

TEST_F(PaintLayersBakeTest, switching_a_uv_gradient_to_object_releases_its_bake)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *layer = BKE_paint_layers_gradient_add(
      *ma, "Gradient", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *layer, MA_PAINT_LAYER_BAKE_ALWAYS));
  clear_active();
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma));
  ASSERT_NE(layer->bake, nullptr);

  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *layer, MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT));
  bool changed = false;
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_TRUE(changed);
  EXPECT_EQ(layer->bake, nullptr) << "a shared UV bake cannot stand in for a 3D row";

  /* A later pass has nothing to create again. */
  changed = false;
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_FALSE(changed);
  EXPECT_EQ(layer->bake, nullptr);
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
    EXPECT_FALSE(BKE_paint_layers_gradient_is_shared_bakeable(*layer));
    EXPECT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *layer, MA_PAINT_LAYER_BAKE_ALWAYS));
  }
  clear_active();

  bool changed = false;
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_FALSE(changed);
  for (MaterialPaintLayer *layer : {object, generated}) {
    EXPECT_EQ(layer->bake, nullptr) << "no shared maps are made for a 3D gradient";
    Vector<float> color(8 * 8 * 4);
    Vector<float> coverage(8 * 8);
    EXPECT_FALSE(BKE_paint_layers_bake_render_node(*ma,
                                                   *layer,
                                                   PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                                   8,
                                                   color.data(),
                                                   coverage.data()));
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
  EXPECT_EQ(folder->bake, nullptr) << "an object-specific child keeps the whole folder live";
  EXPECT_EQ(gradient->bake, nullptr);

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
  EXPECT_TRUE(BKE_paint_layers_bake_mode_set(*ma, never, MA_PAINT_LAYER_BAKE_NEVER));
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
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, layer, MA_PAINT_LAYER_BAKE_ALWAYS));
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

TEST_F(PaintLayersBakeTest, the_generator_reads_the_bake_instead_of_the_row)
{
  Image *source = add_image("Row");
  MaterialPaintLayer *layer = add_paint_layer("Row", source);
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, layer, MA_PAINT_LAYER_BAKE_ALWAYS));
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
  const MaterialPaintLayer *copied = copy->paint_layers.first;
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
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *row))
      << "a gradient mask item is part of the parent's pixels";

  changed = false;
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_TRUE(changed);
  changed = false;
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_FALSE(changed);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *row));

  /* A 3D mask item has no shared pixels at all: the parent's bake is released, not re-made. */
  EXPECT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, item, MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT));
  changed = false;
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_EQ(row->bake, nullptr) << "a 3D gradient item keeps the parent live";
  changed = false;
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_FALSE(changed);
  EXPECT_EQ(row->bake, nullptr);
}

}  // namespace blender::bke::tests
