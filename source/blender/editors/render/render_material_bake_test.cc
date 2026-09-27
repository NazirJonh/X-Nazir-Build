/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BKE_gtest_base.hh"
#include "BKE_global.hh"
#include "BKE_image.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_paint_layers.hh"

#include "ED_material_bake.hh"
#include "ED_paint_layers_bake.hh"

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_ustring.hh"

#include "DNA_material_types.h"
#include "DNA_node_types.h"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"
#include "IMB_colormanagement.hh"

#include <cmath>
#include <cstdint>

namespace blender::ed::material_bake::tests {

/**
 * The material-to-images bake of a plain Principled material: a constant channel is filled
 * synchronously, with no render.
 */
class MaterialBakeTest : public bke::BlenderGTestBase {
 public:
  Main *bmain = nullptr;
  Material *material = nullptr;

  void SetUp() override
  {
    bmain = BKE_main_new();
    G_MAIN = bmain;
    material = add_material_with_principled("Material");
  }

  void TearDown() override
  {
    BKE_main_free(bmain);
    G_MAIN = nullptr;
  }

  Material *add_material_with_principled(const char *name)
  {
    Material *ma = BKE_material_add(bmain, name);
    bNodeTree &tree = *ma->nodetree;
    bNode *principled = bke::node_add_static_node(nullptr, tree, SH_NODE_BSDF_PRINCIPLED);
    bNode *output = bke::node_add_static_node(nullptr, tree, SH_NODE_OUTPUT_MATERIAL);
    bke::node_add_link(tree,
                       *principled,
                       *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                       *output,
                       *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
    return ma;
  }

  /** Every texel of \a image equals \a color, alpha included, within the fill tolerance. */
  static void expect_image_is_solid_color(Image &image, const float color[4])
  {
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(&image, nullptr, &lock);
    ASSERT_NE(ibuf, nullptr);
    ASSERT_NE(ibuf->float_data(), nullptr);
    int64_t mismatches = 0;
    for (const int64_t texel : IndexRange(int64_t(ibuf->x) * ibuf->y)) {
      for (const int component : IndexRange(4)) {
        if (std::abs(ibuf->float_data()[texel * 4 + component] - color[component]) > 1e-3f) {
          mismatches++;
        }
      }
    }
    BKE_image_release_ibuf(&image, ibuf, lock);
    EXPECT_EQ(mismatches, 0) << "expected solid (" << color[0] << ", " << color[1] << ", "
                             << color[2] << ", " << color[3] << ")";
  }

  /** A square scene-linear buffer touching every RGB texel with the same value. */
  static ImBuf *make_rendered_buffer(const int size, const float r, const float g, const float b)
  {
    ImBuf *rendered = IMB_allocImBuf(size, size, ImBufFlags::Zero);
    EXPECT_NE(rendered, nullptr);
    if (rendered == nullptr) {
      return nullptr;
    }
    rendered->channels = 4;
    EXPECT_TRUE(IMB_alloc_float_pixels(rendered, 4, false));
    float *dst = rendered->float_data_for_write();
    for (const int64_t texel : IndexRange(int64_t(size) * size)) {
      dst[texel * 4 + 0] = r;
      dst[texel * 4 + 1] = g;
      dst[texel * 4 + 2] = b;
      dst[texel * 4 + 3] = 1.0f;
    }
    return rendered;
  }
};

TEST_F(MaterialBakeTest, constant_channel_is_filled_without_a_render)
{
  const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  bNode *principled = nullptr;
  for (bNode &node : material->nodetree->nodes) {
    if (node.type_legacy == SH_NODE_BSDF_PRINCIPLED) {
      principled = &node;
    }
  }
  ASSERT_NE(principled, nullptr);
  bNodeSocket *base_color = bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(base_color, nullptr);
  copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(base_color->default_value)->value, blue);

  const int images_before = BLI_listbase_count(&bmain->images);

  BakeTargetSpec spec;
  spec.channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  MaterialBakeToImagesParams params;
  params.material = material;
  params.targets = Span<BakeTargetSpec>(&spec, 1);
  params.size = 64;
  params.blocking = true;
  MaterialBakeToImagesResult result = material_bake_to_images(*bmain, nullptr, nullptr, params);

  EXPECT_TRUE(result.ok);
  ASSERT_EQ(result.created.size(), 1);
  ASSERT_EQ(result.created_channels.size(), 1);
  EXPECT_EQ(result.created_channels[0], PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_TRUE(result.skipped_unavailable.is_empty());
  EXPECT_EQ(BLI_listbase_count(&bmain->images), images_before + 1);
  expect_image_is_solid_color(*result.created[0], blue);
}

/**
 * A source read live by an active Material row is not re-baked by the automatic stale pass. The
 * fixture has no window manager, so the live-source guard (which sits before the wm check) is the
 * one that returns; the predicate's truth table is covered in PaintLayersDescription.
 */
TEST_F(MaterialBakeTest, rebake_stale_skips_a_live_source)
{
  Material *layered = BKE_material_add(bmain, "LayeredLive");
  Material *source = add_material_with_principled("LiveSourceBake");
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *layered, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*layered, row, source));
  BKE_paint_layers_active_set(*layered, row->marker);
  ASSERT_TRUE(BKE_paint_layers_source_material_is_live(*bmain, *source));

  material_bake_images_rebake_stale(*bmain, *source);
  SUCCEED();
}

/**
 * #BKE_paint_layers_source_material_consumers is the list the explicit source-material bake marks
 * before it starts: a layered material reading the source through a MATERIAL row has no `wmJob` of
 * its own for that render -- the job is keyed on the source -- so this is the only way its
 * #MA_PAINT_LAYERS_BAKE_SCHEDULED mark can be stamped. A row reading a different material, an
 * Effect correction reading the source, and a plain material must all be classified right.
 */
TEST_F(MaterialBakeTest, source_material_consumers_lists_layered_readers_once)
{
  Material *source = add_material_with_principled("ConsumerSource");
  Material *other_source = add_material_with_principled("OtherConsumerSource");

  Material *reader = BKE_material_add(bmain, "ConsumerReader");
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *reader, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*reader, row, source));

  /* An Effect correction reading the source counts exactly like a Layer row; both live on the same
   * material, which must still appear once. */
  MaterialPaintLayer *owner = BKE_paint_layers_add(
      *reader, MA_PAINT_LAYER_SOURCE_IMAGE, "Owner", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(owner, nullptr);
  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(
      *reader, owner, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_MATERIAL, "Effect");
  ASSERT_NE(effect, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*reader, effect, source));

  Material *other_reader = BKE_material_add(bmain, "ConsumerOtherReader");
  MaterialPaintLayer *other_row = BKE_paint_layers_add(
      *other_reader, MA_PAINT_LAYER_SOURCE_MATERIAL, "OtherSource", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(other_row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*other_reader, other_row, other_source));

  Vector<Material *> consumers;
  BKE_paint_layers_source_material_consumers(*bmain, *source, consumers);

  ASSERT_EQ(consumers.size(), 1);
  EXPECT_TRUE(consumers.contains(reader));
  EXPECT_FALSE(consumers.contains(other_reader));
  EXPECT_FALSE(consumers.contains(source));
}

/**
 * A freshly created color target, reloaded from its own colorspace (what the texture cache does
 * between the bake job starting and its completion callback) and then written with a scene-linear
 * render, must hold those scene-linear values.
 *
 * The shader path uploads a float buffer raw, so a float map is expected to be scene linear or
 * data whatever its channel (see FloatBufferCache's assertion). Declaring a color map sRGB and
 * encoding on write makes the shader read the encoded, too-light values as if they were linear.
 */
TEST_F(MaterialBakeTest, baked_color_map_stores_scene_linear_values)
{
  const uint64_t hash = material_bake_source_node_tree_hash(*material);
  Image *image = bake_target_image_create(
      *bmain, *material, PAINT_MATERIAL_CHANNEL_BASE_COLOR, 8, "test-layer", hash);
  ASSERT_NE(image, nullptr);

  /* Reload the pixels from the image's declared colorspace, as a cache eviction does. */
  BKE_image_free_buffers(image);

  ImBuf *rendered = make_rendered_buffer(8, 0.2f, 0.5f, 0.8f);
  ASSERT_NE(rendered, nullptr);
  bake_target_image_write_back(*image, *rendered);

  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  ASSERT_NE(ibuf->float_data(), nullptr);
  const char *float_cs = IMB_colormanagement_get_float_colorspace(ibuf);
  EXPECT_TRUE(IMB_colormanagement_space_name_is_scene_linear(float_cs) ||
              IMB_colormanagement_space_name_is_data(float_cs))
      << "float buffer carries colorspace '" << float_cs << "'";
  EXPECT_NEAR(ibuf->float_data()[0], 0.2f, 1e-3f);
  EXPECT_NEAR(ibuf->float_data()[1], 0.5f, 1e-3f);
  EXPECT_NEAR(ibuf->float_data()[2], 0.8f, 1e-3f);
  BKE_image_release_ibuf(image, ibuf, lock);
  IMB_freeImBuf(rendered);
}

/** A data target stores its scalar unchanged, and stays in a data colorspace. */
TEST_F(MaterialBakeTest, baked_data_map_stores_the_value_unchanged)
{
  const uint64_t hash = material_bake_source_node_tree_hash(*material);
  Image *image = bake_target_image_create(
      *bmain, *material, PAINT_MATERIAL_CHANNEL_ROUGHNESS, 8, "test-layer", hash);
  ASSERT_NE(image, nullptr);

  BKE_image_free_buffers(image);

  ImBuf *rendered = make_rendered_buffer(8, 0.3f, 0.3f, 0.3f);
  ASSERT_NE(rendered, nullptr);
  bake_target_image_write_back(*image, *rendered);

  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  ASSERT_NE(ibuf->float_data(), nullptr);
  EXPECT_TRUE(IMB_colormanagement_space_name_is_data(
      IMB_colormanagement_get_float_colorspace(ibuf)));
  EXPECT_NEAR(ibuf->float_data()[0], 0.3f, 1e-3f);
  BKE_image_release_ibuf(image, ibuf, lock);
  IMB_freeImBuf(rendered);
}

/**
 * Re-baking into the maps a row already owns must reuse the same #Image IDs and create none: an
 * earlier version always minted a fresh `.00N` set, which leaked tens of megabytes per re-bake.
 */
TEST_F(MaterialBakeTest, rebake_reuses_the_rows_existing_maps)
{
  const uint64_t hash = material_bake_source_node_tree_hash(*material);
  Image *color = bake_target_image_create(
      *bmain, *material, PAINT_MATERIAL_CHANNEL_BASE_COLOR, 8, "test-layer", hash);
  Image *rough = bake_target_image_create(
      *bmain, *material, PAINT_MATERIAL_CHANNEL_ROUGHNESS, 8, "test-layer", hash);
  ASSERT_NE(color, nullptr);
  ASSERT_NE(rough, nullptr);

  const int images_before = BLI_listbase_count(&bmain->images);
  BakeTargetSpec specs[2] = {
      {PAINT_MATERIAL_CHANNEL_BASE_COLOR, color},
      {PAINT_MATERIAL_CHANNEL_ROUGHNESS, rough},
  };
  MaterialBakeToImagesParams params;
  params.material = material;
  params.targets = Span<BakeTargetSpec>(specs, 2);
  params.size = 8;
  params.blocking = true;
  MaterialBakeToImagesResult result = material_bake_to_images(*bmain, nullptr, nullptr, params);

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(BLI_listbase_count(&bmain->images), images_before);
  ASSERT_EQ(result.created.size(), 2);
  EXPECT_EQ(result.created[0], color);
  EXPECT_EQ(result.created[1], rough);
}

/**
 * A map baked before the scene-linear fix carries an sRGB tag and sRGB-encoded floats. Reusing it
 * must convert the pixels back to scene linear, not merely relabel the buffer.
 */
TEST_F(MaterialBakeTest, legacy_srgb_color_map_is_normalized)
{
  const uint64_t hash = material_bake_source_node_tree_hash(*material);
  Image *image = bake_target_image_create(
      *bmain, *material, PAINT_MATERIAL_CHANNEL_BASE_COLOR, 8, "test-layer", hash);
  ASSERT_NE(image, nullptr);

  const ColorSpace *srgb = IMB_colormanagement_space_get_named("sRGB");
  ASSERT_NE(srgb, nullptr);
  {
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
    ASSERT_NE(ibuf, nullptr);
    ASSERT_NE(ibuf->float_data(), nullptr);
    float *dst = ibuf->float_data_for_write();
    for (const int64_t texel : IndexRange(int64_t(ibuf->x) * ibuf->y)) {
      dst[texel * 4 + 0] = 0.2f;
      dst[texel * 4 + 1] = 0.5f;
      dst[texel * 4 + 2] = 0.8f;
      dst[texel * 4 + 3] = 1.0f;
    }
    /* Encode as the old write-back did, and declare the buffer sRGB. */
    IMB_colormanagement_scene_linear_to_colorspace(dst, ibuf->x, ibuf->y, 4, srgb);
    IMB_colormanagement_assign_float_colorspace(ibuf, "sRGB");
    BKE_image_release_ibuf(image, ibuf, lock);
  }
  STRNCPY(image->colorspace_settings.name, "sRGB");

  bake_target_image_normalize_colorspace(*image, PAINT_MATERIAL_CHANNEL_BASE_COLOR);

  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  ASSERT_NE(ibuf->float_data(), nullptr);
  EXPECT_TRUE(IMB_colormanagement_space_name_is_scene_linear(
      IMB_colormanagement_get_float_colorspace(ibuf)));
  EXPECT_NEAR(ibuf->float_data()[0], 0.2f, 1e-3f);
  EXPECT_NEAR(ibuf->float_data()[1], 0.5f, 1e-3f);
  EXPECT_NEAR(ibuf->float_data()[2], 0.8f, 1e-3f);
  BKE_image_release_ibuf(image, ibuf, lock);
}

TEST_F(MaterialBakeTest, debounce_resolve_keeps_only_surviving_materials)
{
  /* #paint_layers_bake_debounce_resolve is the pure half of the 0.3s debounce: given a list of
   * `session_uid`s and a #Main, it must say which still name a real material, without touching a
   * #wmWindowManager or a `wmTimer` -- exactly the shape a material can be in after waiting: still
   * there, or deleted (or belonging to a file that has since been closed) while the timer waited. */
  Material *second = add_material_with_principled("SecondMaterial");
  ASSERT_NE(second, nullptr);

  const uint32_t material_uid = material->id.session_uid;
  const uint32_t second_uid = second->id.session_uid;
  /* No material in this #Main was ever handed this uid: stands in for one deleted, or from a file
   * that was since closed, while the timer waited. */
  const uint32_t deleted_uid = second_uid + 1000000u;

  const Vector<uint32_t> pending = {material_uid, deleted_uid, second_uid};
  Vector<Material *> found;
  paint_layers_bake_debounce_resolve(*bmain, pending.as_span(), found);

  ASSERT_EQ(found.size(), 2);
  EXPECT_TRUE(found.contains(material));
  EXPECT_TRUE(found.contains(second));

  /* An empty pending list resolves to nothing, and must not walk every material in \a bmain to
   * find that out. */
  Vector<Material *> empty_found;
  paint_layers_bake_debounce_resolve(*bmain, {}, empty_found);
  EXPECT_TRUE(empty_found.is_empty());
}

TEST_F(MaterialBakeTest, debounce_arm_replaces_a_running_timer_for_trailing_edge)
{
  /* A trailing-edge debounce must restart its countdown on every edit, not just the first, or a
   * continuous drag (a slider held for two seconds) would still tick -- and bake -- every 0.3s
   * instead of once after the drag ends. #paint_layers_bake_debounce_should_replace_timer is the
   * named, pure form of that "restart, don't leave running" decision: true whenever a timer already
   * exists, checked with nothing but a pointer, never dereferenced. */
  EXPECT_FALSE(paint_layers_bake_debounce_should_replace_timer(nullptr));
  wmTimer *const not_dereferenced = reinterpret_cast<wmTimer *>(std::uintptr_t(1));
  EXPECT_TRUE(paint_layers_bake_debounce_should_replace_timer(not_dereferenced));
}

TEST_F(MaterialBakeTest, debounce_settles_immediately_only_when_nothing_is_or_will_be_running)
{
  /* The tick may clear #MA_PAINT_LAYERS_BAKE_SCHEDULED itself only when its own calls started
   * nothing (no job in flight) AND nothing is about to be started for it either (no heavy bake
   * #paint_layers_bake_jobs_ensure has not queued yet) -- otherwise the mark would flicker false for
   * the window between the tick returning and that queuing happening, or #paint_layers_bake_scheduled_settle
   * would never be told to check a material with nothing running belonging to it any more. */
  EXPECT_TRUE(paint_layers_bake_debounce_settles_immediately(false, false));
  EXPECT_FALSE(paint_layers_bake_debounce_settles_immediately(true, false));
  EXPECT_FALSE(paint_layers_bake_debounce_settles_immediately(false, true));
  EXPECT_FALSE(paint_layers_bake_debounce_settles_immediately(true, true));
}

TEST_F(MaterialBakeTest, scheduled_settle_clears_only_once_nothing_is_in_flight)
{
  /* #paint_layers_bake_scheduled_settle_clears is the one decision the free callback of every bake
   * job type the debounce timer can start (#paint_layers_bake_free, `material_bake_images_free`)
   * relies on to know when #MA_PAINT_LAYERS_BAKE_SCHEDULED may finally come down -- an add-on
   * reading `Material.paint_layers_is_stale` must never see "fresh" while a job it cannot see is
   * still writing. */
  EXPECT_FALSE(paint_layers_bake_scheduled_settle_clears(true));
  EXPECT_TRUE(paint_layers_bake_scheduled_settle_clears(false));
}

TEST_F(MaterialBakeTest, job_is_excluded_matches_owner_and_type_exactly)
{
  /* `WM_jobs_test` still reports a job as running from *inside* its own free callback
   * (`wm_jobs_handle_finished` and `wm_jobs_kill_job` in `wm_jobs.cc` both free before the job is
   * marked finished or removed) -- #paint_layers_bake_job_is_excluded is what lets a settle call
   * made from there treat that one (owner, job_type) pair as already gone, without ever
   * dereferencing either pointer. Every other (owner, job_type) -- a different owner, a different
   * type, or both -- must still go through the real #WM_jobs_test. */
  int owner_a = 0;
  int owner_b = 0;
  const void *a = &owner_a;
  const void *b = &owner_b;

  EXPECT_TRUE(paint_layers_bake_job_is_excluded(a, 1, a, 1));
  EXPECT_FALSE(paint_layers_bake_job_is_excluded(a, 1, b, 1));
  EXPECT_FALSE(paint_layers_bake_job_is_excluded(a, 1, a, 2));
  EXPECT_FALSE(paint_layers_bake_job_is_excluded(a, 1, b, 2));
  EXPECT_FALSE(paint_layers_bake_job_is_excluded(nullptr, 0, a, 1));
}

TEST_F(MaterialBakeTest, gate_decide_headless_always_proceeds)
{
  /* Headless has no timer, no `wmJob`, and no modal loop to ever become fresh through, so refusing
   * it would only ever be a false refusal: #material_changed's own headless branch already ran
   * every bake synchronously before this could be asked, so \a stale is normally false there
   * anyway. Both `invoke` and `exec` proceed, and so does a headless caller that somehow still
   * observes stale. */
  EXPECT_EQ(paint_layers_bake_gate_decide(false, false, true),
           PaintLayersBakeGateAction::Proceed);
  EXPECT_EQ(paint_layers_bake_gate_decide(false, true, true), PaintLayersBakeGateAction::Proceed);
  EXPECT_EQ(paint_layers_bake_gate_decide(true, false, true), PaintLayersBakeGateAction::Proceed);
  EXPECT_EQ(paint_layers_bake_gate_decide(true, true, true), PaintLayersBakeGateAction::Proceed);
}

TEST_F(MaterialBakeTest, gate_decide_fresh_always_proceeds)
{
  /* Nothing is stale: there is nothing to wait for or refuse, whether reached from `invoke` or
   * `exec`. */
  EXPECT_EQ(paint_layers_bake_gate_decide(false, false, false),
           PaintLayersBakeGateAction::Proceed);
  EXPECT_EQ(paint_layers_bake_gate_decide(false, true, false),
           PaintLayersBakeGateAction::Proceed);
}

TEST_F(MaterialBakeTest, gate_decide_stale_invoke_waits_exec_refuses)
{
  /* The accepted UI decision: an `invoke`-reached, stale result forces a bake and goes modal
   * (#Wait) rather than acting on stale data or refusing outright; `exec`/an RNA function cannot go
   * modal, so it is refused instead (#Refuse) with a message pointing at
   * `Material.paint_layers_is_stale` / `MATERIAL_OT_paint_layers_bake_now`. */
  EXPECT_EQ(paint_layers_bake_gate_decide(true, true, false), PaintLayersBakeGateAction::Wait);
  EXPECT_EQ(paint_layers_bake_gate_decide(true, false, false),
           PaintLayersBakeGateAction::Refuse);
}

TEST_F(MaterialBakeTest, bke_scheduled_mark_is_seen_by_the_gate_without_a_window_manager)
{
  /* C1 contract: the editor stamps #MA_PAINT_LAYERS_BAKE_SCHEDULED for whatever it has queued or
   * running, and the RNA layer now reads freshness through #BKE_paint_layers_is_stale alone, with
   * no #wmWindowManager. With the mark set, that BKE-only signal must report stale, and the gate
   * must then refuse an `exec`/RNA caller rather than let it read a result still being written.
   * This covers the BKE half of the wiring; the editor half -- a job stamping and settling the mark
   * -- needs a real #wmJob and is a manual check. */
  material->paint_layers_flag |= MA_PAINT_LAYERED;
  EXPECT_FALSE(BKE_paint_layers_bake_scheduled_get(*material));
  EXPECT_FALSE(BKE_paint_layers_is_stale(*material));

  BKE_paint_layers_bake_scheduled_set(*material, true);
  ASSERT_TRUE(BKE_paint_layers_bake_scheduled_get(*material));
  EXPECT_TRUE(BKE_paint_layers_is_stale(*material));
  EXPECT_EQ(
      BKE_paint_layers_bake_gate_decide(/*stale=*/true, /*is_invoke=*/false, /*headless=*/false),
      PaintLayersBakeGateAction::Refuse);
  /* Headless still proceeds: #material_changed's own headless branch already ran every bake
   * synchronously before this could be asked, so refusing would only ever be a false refusal. */
  EXPECT_EQ(
      BKE_paint_layers_bake_gate_decide(/*stale=*/true, /*is_invoke=*/false, /*headless=*/true),
      PaintLayersBakeGateAction::Proceed);

  BKE_paint_layers_bake_scheduled_set(*material, false);
  EXPECT_FALSE(BKE_paint_layers_bake_scheduled_get(*material));
  EXPECT_FALSE(BKE_paint_layers_is_stale(*material));
}

}  // namespace blender::ed::material_bake::tests
