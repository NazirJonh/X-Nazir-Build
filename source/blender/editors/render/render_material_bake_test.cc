/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BKE_gtest_base.hh"
#include "BKE_global.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_paint_layers.hh"

#include "ED_material_bake.hh"
#include "ED_material_combined.hh"
#include "ED_paint_layers_bake.hh"

#include "render_material_bake_intern.hh"

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_timer.h"
#include "BLI_ustring.hh"
#include "BLI_vector.hh"

#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

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
 * A repeated print always fills the target again: the bake path has no "already current" gate that
 * would answer from the node-tree hash alone, so a different bake option always re-prints.
 */
TEST_F(MaterialBakeTest, a_repeated_bake_is_not_skipped_on_a_matching_hash)
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

  BakeTargetSpec spec;
  spec.channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  MaterialBakeToImagesParams params;
  params.material = material;
  params.targets = Span<BakeTargetSpec>(&spec, 1);
  params.size = 64;
  params.blocking = true;
  MaterialBakeToImagesResult first = material_bake_to_images(*bmain, nullptr, nullptr, params);
  ASSERT_TRUE(first.ok);
  ASSERT_EQ(first.created.size(), 1);
  Image *image = first.created[0];
  expect_image_is_solid_color(*image, blue);

  {
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
    ASSERT_NE(ibuf, nullptr);
    ASSERT_NE(ibuf->float_data(), nullptr);
    float *dst = ibuf->float_data_for_write();
    for (const int64_t texel : IndexRange(int64_t(ibuf->x) * ibuf->y)) {
      dst[texel * 4 + 0] = 1.0f;
      dst[texel * 4 + 1] = 0.0f;
      dst[texel * 4 + 2] = 0.0f;
    }
    BKE_image_release_ibuf(image, ibuf, lock);
  }

  params.reuse_existing = true;
  MaterialBakeToImagesResult second = material_bake_to_images(*bmain, nullptr, nullptr, params);
  EXPECT_TRUE(second.ok);
  expect_image_is_solid_color(*image, blue);
}

/* A freshly created color target, reloaded from its own colorspace (what the texture cache does
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

TEST_F(MaterialBakeTest, the_debounce_finds_the_materials_still_there_and_drops_the_others)
{
  Material *other = add_material_with_principled("Other");
  Material *gone = add_material_with_principled("Gone");
  const uint32_t gone_uid = gone->id.session_uid;
  BKE_id_delete(bmain, gone);

  const uint32_t pending[] = {material->id.session_uid, gone_uid};
  Vector<Material *> found;
  paint_layers_bake_debounce_resolve(*bmain, Span<uint32_t>(pending, 2), found);
  ASSERT_EQ(found.size(), 1);
  EXPECT_EQ(found[0], material);
  EXPECT_NE(found[0], other);

  found.clear();
  paint_layers_bake_debounce_resolve(*bmain, {}, found);
  EXPECT_TRUE(found.is_empty());
}

TEST_F(MaterialBakeTest, arming_marks_the_material_and_restarts_a_running_timer)
{
  paint_layers_bake_debounce_reset();
  EXPECT_FALSE(paint_layers_bake_debounce_should_replace_timer());

  paint_layers_bake_debounce_arm(*material);
  EXPECT_TRUE(BKE_paint_layers_bake_scheduled_get(*material));
  EXPECT_TRUE(paint_layers_bake_debounce_should_replace_timer())
      << "a second edit must restart the countdown, not let the first one fire";

  /* Arming again keeps exactly one timer: it is removed and registered anew. */
  paint_layers_bake_debounce_arm(*material);
  EXPECT_TRUE(paint_layers_bake_debounce_should_replace_timer());

  paint_layers_bake_debounce_reset();
  EXPECT_FALSE(paint_layers_bake_debounce_should_replace_timer())
      << "a file about to go away leaves no timer behind";
  BKE_paint_layers_bake_scheduled_set(*material, false);
}

TEST_F(MaterialBakeTest, a_mark_settles_only_once_nothing_is_left_running)
{
  EXPECT_FALSE(paint_layers_bake_scheduled_settle_clears(true));
  EXPECT_TRUE(paint_layers_bake_scheduled_settle_clears(false));

  /* A job inside its own free callback still counts as running, so it is the one excluded. */
  int owner = 0;
  int other = 0;
  constexpr int job_type = 7;
  EXPECT_TRUE(paint_layers_bake_job_is_excluded(&owner, job_type, &owner, job_type));
  EXPECT_FALSE(paint_layers_bake_job_is_excluded(&owner, job_type, &other, job_type));
  EXPECT_FALSE(paint_layers_bake_job_is_excluded(&owner, job_type, &owner, job_type + 1));
  EXPECT_FALSE(paint_layers_bake_job_is_excluded(&owner, job_type, nullptr, 0));
}

TEST_F(MaterialBakeTest, gradient_preview_throttle_has_a_trailing_edge)
{
  using namespace blender::ed::material_combined;

  /* The first frame of a drag is never delayed: there is nothing on screen to keep. */
  EXPECT_TRUE(gradient_preview_refresh_due(10.0, 0.0, true));

  /* During a drag, steps closer together than the interval keep the previous preview. */
  EXPECT_FALSE(gradient_preview_refresh_due(10.05, 10.0, true));
  EXPECT_TRUE(gradient_preview_refresh_due(10.0 + GRADIENT_PREVIEW_REFRESH_SECONDS + 0.001, 10.0, true));

  /* Release always rebuilds, even one millisecond after the last interactive frame. */
  EXPECT_TRUE(gradient_preview_refresh_due(10.001, 10.0, false));
}

/** A material with no paint layer stack has no owned Bump, so a Height target is skipped as
 * unavailable rather than rendered into an empty map. */
TEST_F(MaterialBakeTest, height_target_without_a_stack_is_skipped)
{
  BakeTargetSpec spec;
  spec.channel = PAINT_MATERIAL_CHANNEL_HEIGHT;
  MaterialBakeToImagesParams params;
  params.material = material;
  params.targets = Span<BakeTargetSpec>(&spec, 1);
  params.size = 64;
  params.blocking = true;
  MaterialBakeToImagesResult result = material_bake_to_images(*bmain, nullptr, nullptr, params);

  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(result.created.is_empty());
  ASSERT_EQ(result.skipped_unavailable.size(), 1);
  EXPECT_EQ(result.skipped_unavailable[0], PAINT_MATERIAL_CHANNEL_HEIGHT);
}

/** Height is skipped on its own, but does not take an available channel of the same bake down. */
TEST_F(MaterialBakeTest, height_target_is_skipped_beside_an_available_channel)
{
  BakeTargetSpec specs[2];
  specs[0].channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  specs[1].channel = PAINT_MATERIAL_CHANNEL_HEIGHT;
  MaterialBakeToImagesParams params;
  params.material = material;
  params.targets = Span<BakeTargetSpec>(specs, 2);
  params.size = 64;
  params.blocking = true;
  MaterialBakeToImagesResult result = material_bake_to_images(*bmain, nullptr, nullptr, params);

  EXPECT_TRUE(result.ok);
  ASSERT_EQ(result.created.size(), 1);
  ASSERT_EQ(result.created_channels.size(), 1);
  EXPECT_EQ(result.created_channels[0], PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_EQ(result.skipped_unavailable.size(), 1);
  EXPECT_EQ(result.skipped_unavailable[0], PAINT_MATERIAL_CHANNEL_HEIGHT);
}

/**
 * The Height request is asked for explicitly: a material whose stack owns a Bump contributes it
 * only when the caller includes Height, and the request reads that Bump's Height input.
 */
TEST_F(MaterialBakeTest, height_request_follows_include_height)
{
  Material *layered = add_material_with_principled("Layered");
  layered->paint_layers_flag = MA_PAINT_LAYERED;
  ASSERT_TRUE(BKE_paint_layers_channel_set_mask_set(
      *layered, uint16_t(uint16_t(1) << PAINT_MATERIAL_CHANNEL_HEIGHT)));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *layered, "Paint", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  BKE_paint_layers_default_channels_apply(*layered, *row);

  const float blank[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *height_image = BKE_image_add_generated(
      bmain, 8, 8, "Height", 32, false, IMA_GENTYPE_BLANK, blank, false, false, false);
  ASSERT_NE(height_image, nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *layered, row, PAINT_MATERIAL_CHANNEL_HEIGHT, height_image));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *layered));
  ASSERT_TRUE(BKE_paint_layers_has_bump(*layered));

  const MaterialSourceResolve resolve = BKE_paint_material_source_resolve(layered);
  Vector<BakeSocketRequest> requests;
  Vector<int> request_channels;

  EXPECT_FALSE(bake_channel_requests_build(
      *layered, resolve, /*include_image_resolution=*/true, requests, request_channels, false));
  EXPECT_TRUE(request_channels.is_empty());

  ASSERT_TRUE(bake_channel_requests_build(
      *layered, resolve, /*include_image_resolution=*/true, requests, request_channels, true));
  ASSERT_EQ(requests.size(), 1);
  ASSERT_EQ(request_channels.size(), 1);
  EXPECT_EQ(request_channels[0], PAINT_MATERIAL_CHANNEL_HEIGHT);
  EXPECT_TRUE(requests[0].is_color);
  EXPECT_FALSE(requests[0].encode_vector);
}

/** The Height map is data: a float buffer in a data colorspace, whatever values land in it. */
TEST_F(MaterialBakeTest, height_target_is_a_data_float_map)
{
  const uint64_t hash = material_bake_source_node_tree_hash(*material);
  Image *image = bake_target_image_create(
      *bmain, *material, PAINT_MATERIAL_CHANNEL_HEIGHT, 8, "test-layer", hash);
  ASSERT_NE(image, nullptr);

  ImBuf *rendered = make_rendered_buffer(8, 0.25f, 0.25f, 0.25f);
  ASSERT_NE(rendered, nullptr);
  bake_target_image_write_back(*image, *rendered);

  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  ASSERT_NE(ibuf->float_data(), nullptr);
  EXPECT_TRUE(IMB_colormanagement_space_name_is_data(
      IMB_colormanagement_get_float_colorspace(ibuf)));
  EXPECT_NEAR(ibuf->float_data()[0], 0.25f, 1e-3f);
  BKE_image_release_ibuf(image, ibuf, lock);
  IMB_freeImBuf(rendered);
}

/**
 * The two halves of a bake, driven without the render.
 *
 * A plain material whose Base Color is a plain texture: that channel is a graph the bake has to
 * render, and its source -- the texture's Color output -- sits in the material's own tree, which is
 * the tree #bake_requests_attach adds its AOV nodes to. Reading that socket's owner after the AOV
 * was added is the read that trips `BLI_assert(topology_cache_is_available)` when the cache is not
 * refreshed in between (see #route_socket_to_root).
 *
 * NOTE: with asserts compiled out (a plain Release build, i.e. `NDEBUG`) the stale read is silent
 * and this test passes either way; it fails only in a build with asserts on -- `WITH_ASSERT_RELEASE`
 * (the Lite configuration) or a Debug one. The render is never run: a unit test binary has no GPU
 * context, which the render asserts on before it does anything.
 */
TEST_F(MaterialBakeTest, a_source_in_the_root_tree_attaches_without_an_assert)
{
  bNodeTree &tree = *material->nodetree;
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  const bNode *principled = BKE_paint_material_principled_find(*material, reason);
  ASSERT_NE(principled, nullptr);
  bNode &principled_mutable = const_cast<bNode &>(*principled);

  const float blank[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *image = BKE_image_add_generated(
      bmain, 8, 8, "Map", 32, false, IMA_GENTYPE_BLANK, blank, false, false, false);
  ASSERT_NE(image, nullptr);
  bNode *texture = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
  ASSERT_NE(texture, nullptr);
  texture->id = &image->id;
  bke::node_add_link(
      tree,
      *texture,
      *bke::node_find_socket(*texture, SOCK_OUT, "Color"_ustr),
      principled_mutable,
      *bke::node_find_socket(principled_mutable, SOCK_IN, "Base Color"_ustr));

  /* The images bake job's own call: a plain texture is an Image resolution, which this argument
   * turns into a request to render. */
  const MaterialSourceResolve resolve = BKE_paint_material_source_resolve(material);
  ASSERT_EQ(resolve.channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR], ChannelResolution::Image);
  Vector<BakeSocketRequest> requests;
  Vector<int> request_channels;
  ASSERT_TRUE(bake_channel_requests_build(
      *material, resolve, /*include_image_resolution=*/true, requests, request_channels, false));
  ASSERT_EQ(requests.size(), 1);
  EXPECT_EQ(request_channels[0], PAINT_MATERIAL_CHANNEL_BASE_COLOR);

  /* The precondition of the defect: the source is a socket of the tree the AOVs go into, not one
   * of a group nested in it. */
  tree.ensure_topology_cache();
  ASSERT_NE(requests[0].source, nullptr);
  EXPECT_EQ(requests[0].source->owner_node().type_legacy, SH_NODE_TEX_IMAGE);
  EXPECT_TRUE(requests[0].group_path.is_empty());

  ASSERT_TRUE(bake_requests_attach(*bmain, tree, requests));

  /* The attach ran to its end rather than stopping at the first request: one AOV per request. */
  int aovs = 0;
  for (const bNode &node : tree.nodes) {
    if (node.type_legacy == SH_NODE_OUTPUT_AOV) {
      aovs++;
    }
  }
  EXPECT_EQ(aovs, int(requests.size()));
}

}  // namespace blender::ed::material_bake::tests
