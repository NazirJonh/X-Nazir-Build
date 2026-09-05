/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BKE_gtest_base.hh"
#include "BKE_image.hh"
#include "BKE_image_partial_update.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_paint_material_combined.hh"

#include <array>

#include "BLI_index_range.hh"
#include "BLI_math_vector.hh"
#include "BLI_rect.h"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

namespace blender::bke::tests {

/* -------------------------------------------------------------------- */
/** \name Combined Cache
 * \{ */

class CombinedCacheTest : public bke::BlenderGTestBase {
 public:
  static constexpr int size = 8;

  Main *bmain = nullptr;
  Material *material_ = nullptr;
  Material *other_material_ = nullptr;
  Image *image_a_ = nullptr;
  Image *image_b_ = nullptr;

  CombinedInputs inputs_;
  CombinedPreviewLighting lighting_;

  void SetUp() override
  {
    bmain = BKE_main_new();
    material_ = BKE_material_add(bmain, "Mat");
    other_material_ = BKE_material_add(bmain, "Other");

    const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    image_a_ = BKE_image_add_generated(
        bmain, size, size, "A", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
    image_b_ = BKE_image_add_generated(
        bmain, size, size, "B", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);

    inputs_.width = size;
    inputs_.height = size;
    for (const int i : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
      inputs_.channels[i].constant = BKE_paint_material_combined_default_value(
          eMaterialPaintChannel(i));
    }
    lighting_ = BKE_paint_material_combined_lighting_default();
  }

  void TearDown() override
  {
    BKE_paint_material_combined_cache_free_all();
    BKE_main_free(bmain);
  }

  /**
   * #BKE_paint_material_combined_cache_ensure on #material_ with the fixture's inputs.
   *
   * Every test varies one or two fields of the request and leaves the rest at their defaults, so
   * naming them at the call site is what keeps a test readable: `request.clip` says which
   * rectangle is meant, where the positional form said only `empty_region_, {}, clip`.
   */
  ImBuf *ensure(const CombinedCacheRequest &request,
                uint64_t *r_revision = nullptr,
                CombinedEvalStats *r_stats = nullptr)
  {
    return BKE_paint_material_combined_cache_ensure(
        *material_, inputs_, lighting_, request, r_revision, nullptr, r_stats);
  }

  /** #ensure, for the tests that are about the reported rectangle rather than the pixel count. */
  ImBuf *ensure_reporting(const CombinedCacheRequest &request, rcti &r_changed_region)
  {
    return BKE_paint_material_combined_cache_ensure(
        *material_, inputs_, lighting_, request, nullptr, &r_changed_region, nullptr);
  }

  /**
   * Overwrite \a rect of \a image *and record the edit*, as the paint path does.
   *
   * The recording is the point: the cache learns what changed by polling the image's own
   * partial-update log, so a test that only writes pixels is testing the case it must deliberately
   * *not* notice. #imapaint_image_update makes the same call.
   */
  void paint_region(Image &image, const rcti &rect, const uchar value = 200)
  {
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(&image, nullptr, &lock);
    ASSERT_NE(ibuf, nullptr);
    uchar *pixels = ibuf->byte_data_for_write();
    for (const int y : IndexRange(rect.ymin, BLI_rcti_size_y(&rect))) {
      for (const int x : IndexRange(rect.xmin, BLI_rcti_size_x(&rect))) {
        uchar *p = pixels + (int64_t(y) * ibuf->x + x) * 4;
        p[0] = p[1] = p[2] = value;
        p[3] = 255;
      }
    }
    BKE_image_partial_update_mark_region(
        &image, static_cast<ImageTile *>(image.tiles.first), ibuf, &rect);
    BKE_image_release_ibuf(&image, ibuf, lock);
  }

  /** The request every test starts from: a structural hash and nothing else declared. */
  static CombinedCacheRequest make_request(const uint64_t inputs_hash = 1234)
  {
    CombinedCacheRequest request;
    request.inputs_hash = inputs_hash;
    return request;
  }
};

TEST_F(CombinedCacheTest, unchanged_inputs_are_not_re_evaluated)
{
  CombinedEvalStats stats;
  uint64_t first_revision = 0;
  ImBuf *a = this->ensure(make_request(), &first_revision, &stats);
  ASSERT_NE(a, nullptr);
  EXPECT_GT(stats.pixels_processed, 0);

  uint64_t second_revision = 0;
  stats = CombinedEvalStats{};
  ImBuf *b = this->ensure(make_request(), &second_revision, &stats);
  /* The steady state during pan and zoom: same buffer, same revision, no work at all. */
  EXPECT_EQ(a, b);
  EXPECT_EQ(first_revision, second_revision);
  EXPECT_EQ(stats.pixels_processed, 0);
}

TEST_F(CombinedCacheTest, structural_change_forces_a_full_rebuild)
{
  uint64_t revision_a = 0;
  uint64_t revision_b = 0;
  this->ensure(make_request(1234), &revision_a);
  CombinedEvalStats stats;
  this->ensure(make_request(5678), &revision_b, &stats);
  EXPECT_NE(revision_a, revision_b);
  EXPECT_EQ(stats.pixels_processed, int64_t(inputs_.width) * inputs_.height);
}

TEST_F(CombinedCacheTest, lighting_change_forces_a_full_rebuild)
{
  uint64_t revision_a = 0;
  uint64_t revision_b = 0;
  this->ensure(make_request(), &revision_a);
  CombinedPreviewLighting moved = lighting_;
  moved.lights[0].direction = math::normalize(float3(1.0f, 1.0f, 1.0f));
  ASSERT_NE(moved.hash(), lighting_.hash());
  lighting_ = moved;
  CombinedEvalStats stats;
  this->ensure(make_request(), &revision_b, &stats);
  EXPECT_NE(revision_a, revision_b);
  EXPECT_EQ(stats.pixels_processed, int64_t(inputs_.width) * inputs_.height);
}

TEST_F(CombinedCacheTest, changed_region_rebuilds_only_that_rectangle)
{
  this->ensure(make_request());

  CombinedCacheRequest request = make_request();
  BLI_rcti_init(&request.changed_region, 2, 6, 2, 6);
  CombinedEvalStats stats;
  this->ensure(request, nullptr, &stats);
  EXPECT_EQ(stats.pixels_processed, 16);
}

/* An edit is found by polling the dependency's own log, and only a declared dependency is polled.
 * The second half is what stands between an unrelated paint stroke and a re-shade of every preview
 * in the file. */
TEST_F(CombinedCacheTest, edits_are_detected_only_for_declared_dependencies)
{
  CombinedCacheRequest request = make_request();
  request.dependency_images = Span<Image *>(&image_a_, 1);
  /* The first call subscribes, and a fresh subscription always asks for a full rebuild. The one
   * after it is the steady state this test measures against. */
  this->ensure(request);
  this->ensure(request);

  rcti dab;
  BLI_rcti_init(&dab, 0, 4, 0, 4);
  CombinedEvalStats stats;

  this->paint_region(*image_b_, dab); /* Not a dependency. */
  this->ensure(request, nullptr, &stats);
  EXPECT_EQ(stats.pixels_processed, 0);

  this->paint_region(*image_a_, dab); /* Is a dependency. */
  stats = CombinedEvalStats{};
  this->ensure(request, nullptr, &stats);
  /* The log works in 256-pixel chunks, so on an 8x8 image the dab rounds out to the whole buffer.
   * What has to hold is that the edit was noticed at all, and that the other image's was not. */
  EXPECT_GT(stats.pixels_processed, 0);
}

/* Nobody reports a paint stroke to this cache any more, and nothing has to: an edit recorded in
 * the image's own log is found by the next resolve. This is the regression that mattered -- being
 * told by the depsgraph flush instead turned every dab into a full re-shade of the canvas. */
TEST_F(CombinedCacheTest, an_edit_needs_no_tag)
{
  CombinedCacheRequest request = make_request();
  request.dependency_images = Span<Image *>(&image_a_, 1);
  this->ensure(request);
  this->ensure(request);

  CombinedEvalStats stats;
  this->ensure(request, nullptr, &stats);
  ASSERT_EQ(stats.pixels_processed, 0) << "the steady state must shade nothing";

  rcti dab;
  BLI_rcti_init(&dab, 0, 4, 0, 4);
  this->paint_region(*image_a_, dab);

  stats = CombinedEvalStats{};
  this->ensure(request, nullptr, &stats);
  EXPECT_GT(stats.pixels_processed, 0);
}

TEST_F(CombinedCacheTest, free_material_drops_only_that_entry)
{
  this->ensure(make_request());
  BKE_paint_material_combined_cache_ensure(*other_material_, inputs_, lighting_, make_request());
  EXPECT_TRUE(BKE_paint_material_combined_cache_contains(*material_));
  BKE_paint_material_combined_cache_free_material(*material_);
  EXPECT_FALSE(BKE_paint_material_combined_cache_contains(*material_));
  EXPECT_TRUE(BKE_paint_material_combined_cache_contains(*other_material_));
  BKE_paint_material_combined_cache_free_all();
  EXPECT_FALSE(BKE_paint_material_combined_cache_contains(*other_material_));
}

TEST_F(CombinedCacheTest, freeing_an_evaluated_copy_keeps_the_entry)
{
  /* The counterpart of the composite cache's test of the same name, and for the same reason: a
   * copy-on-evaluation datablock carries the original's session UID, which is the whole of this
   * cache's key. The depsgraph frees one on every relation rebuild, and dropping the entry there
   * would re-shade the entire preview for a material nothing happened to. */
  ASSERT_NE(this->ensure(make_request()), nullptr);
  ASSERT_TRUE(BKE_paint_material_combined_cache_contains(*material_));

  ID *eval_id = nullptr;
  BKE_id_copy_ex(nullptr, &material_->id, &eval_id, LIB_ID_COPY_LOCALIZE);
  ASSERT_NE(eval_id, nullptr);
  eval_id->tag |= ID_TAG_COPIED_ON_EVAL;
  eval_id->session_uid = material_->id.session_uid;

  BKE_id_free(nullptr, eval_id);

  EXPECT_TRUE(BKE_paint_material_combined_cache_contains(*material_));
}

TEST_F(CombinedCacheTest, a_dropped_dependency_stops_being_polled)
{
  /* Subscriptions are pruned against the images actually declared, so an image that leaves the
   * dependency list must stop being able to dirty the entry. Without the pruning the cache keeps
   * an allocation and a poll per frame for something it no longer reads -- and, worse, re-shades
   * for edits that cannot reach the preview. */
  std::array<Image *, 2> both = {image_a_, image_b_};
  CombinedCacheRequest request = make_request();
  request.dependency_images = Span<Image *>(both);
  ASSERT_NE(this->ensure(request), nullptr);
  /* The first poll of a new subscription always asks for everything; settle it. */
  this->ensure(request);

  CombinedCacheRequest narrowed = make_request();
  narrowed.dependency_images = Span<Image *>(&image_a_, 1);
  this->ensure(narrowed);
  this->ensure(narrowed);

  rcti rect;
  BLI_rcti_init(&rect, 0, 2, 0, 2);
  this->paint_region(*image_b_, rect);

  CombinedEvalStats stats;
  this->ensure(narrowed, nullptr, &stats);
  EXPECT_EQ(stats.pixels_processed, 0);
}

TEST_F(CombinedCacheTest, invalidate_marks_rather_than_drops)
{
  ImBuf *before = this->ensure(make_request());
  ASSERT_NE(before, nullptr);
  BKE_paint_material_combined_cache_invalidate(material_);
  /* Marking, not dropping: a caller asking in between must get the previous pixels rather than
   * nothing, exactly as the composite cache behaves. */
  EXPECT_TRUE(BKE_paint_material_combined_cache_contains(*material_));
  CombinedEvalStats stats;
  this->ensure(make_request(), nullptr, &stats);
  EXPECT_EQ(stats.pixels_processed, int64_t(inputs_.width) * inputs_.height);
}

TEST_F(CombinedCacheTest, zero_sized_inputs_create_no_entry)
{
  CombinedInputs empty = inputs_;
  empty.width = 0;
  empty.height = 0;
  EXPECT_EQ(BKE_paint_material_combined_cache_ensure(*material_, empty, lighting_, make_request()),
            nullptr);
  EXPECT_FALSE(BKE_paint_material_combined_cache_contains(*material_));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Visible-Region Clipping
 * \{ */

/* A clipped call shades the clip and nothing else: the point of the whole exercise. */
TEST_F(CombinedCacheTest, clip_limits_a_full_rebuild_to_the_clip)
{
  inputs_.width = 64;
  inputs_.height = 64;
  CombinedCacheRequest request = make_request();
  BLI_rcti_init(&request.clip, 8, 24, 8, 24);

  CombinedEvalStats stats;
  ImBuf *ibuf = this->ensure(request, nullptr, &stats);
  ASSERT_NE(ibuf, nullptr);
  EXPECT_EQ(stats.pixels_processed, 16 * 16);
}

/* Asking again for the same clip with nothing dirty must shade nothing at all. */
TEST_F(CombinedCacheTest, clip_already_valid_shades_nothing)
{
  inputs_.width = 64;
  inputs_.height = 64;
  CombinedCacheRequest request = make_request();
  BLI_rcti_init(&request.clip, 8, 24, 8, 24);
  this->ensure(request);

  CombinedEvalStats stats;
  this->ensure(request, nullptr, &stats);
  EXPECT_EQ(stats.pixels_processed, 0);
}

/* Panning to a rectangle that was never shaded must shade it, not serve stale pixels. */
TEST_F(CombinedCacheTest, clip_moving_outside_valid_region_shades_the_new_part)
{
  inputs_.width = 64;
  inputs_.height = 64;
  CombinedCacheRequest first = make_request();
  BLI_rcti_init(&first.clip, 0, 16, 0, 16);
  this->ensure(first);

  CombinedCacheRequest second = make_request();
  BLI_rcti_init(&second.clip, 48, 64, 48, 64);
  CombinedEvalStats stats;
  this->ensure(second, nullptr, &stats);
  /* The two rectangles are disjoint, so the second one is shaded whole. */
  EXPECT_EQ(stats.pixels_processed, 16 * 16);
}

/* A dab outside the clip must stay dirty, so that scrolling to it later still shades it. */
TEST_F(CombinedCacheTest, dirty_outside_the_clip_survives_until_it_is_visible)
{
  inputs_.width = 64;
  inputs_.height = 64;
  CombinedCacheRequest whole = make_request();
  BLI_rcti_init(&whole.clip, 0, 64, 0, 64);
  this->ensure(whole);

  CombinedCacheRequest far_away = make_request();
  BLI_rcti_init(&far_away.changed_region, 50, 54, 50, 54);
  BLI_rcti_init(&far_away.clip, 0, 16, 0, 16);
  CombinedEvalStats stats;
  /* The dab is reported while the viewport is looking somewhere else. */
  this->ensure(far_away, nullptr, &stats);
  EXPECT_EQ(stats.pixels_processed, 0);

  /* Now the viewport looks at it. The corner itself is already valid from the first call, so the
   * only thing shaded is the dab. */
  CombinedCacheRequest nearby = make_request();
  BLI_rcti_init(&nearby.clip, 48, 64, 48, 64);
  this->ensure(nearby, nullptr, &stats);
  EXPECT_EQ(stats.pixels_processed, 4 * 4);
}

/* Two overlapping clips must not make their bounding box valid: the corners it adds were never
 * shaded, and a later clip landing in one of them would be served stale pixels. */
TEST_F(CombinedCacheTest, overlapping_clips_do_not_validate_the_corners_between_them)
{
  inputs_.width = 64;
  inputs_.height = 64;
  CombinedCacheRequest a = make_request();
  BLI_rcti_init(&a.clip, 0, 32, 0, 32);
  this->ensure(a);
  CombinedCacheRequest b = make_request();
  BLI_rcti_init(&b.clip, 16, 48, 16, 48);
  this->ensure(b);

  /* The union of `a` and `b` is not a rectangle, so `valid_region` is `b` alone and this corner --
   * inside their bounding box, inside neither rectangle -- must still be shaded. */
  CombinedCacheRequest corner = make_request();
  BLI_rcti_init(&corner.clip, 0, 16, 32, 48);
  CombinedEvalStats stats;
  this->ensure(corner, nullptr, &stats);
  EXPECT_GT(stats.pixels_processed, 0);
}

/* A size query must answer from the cache without shading a single texel, because it is on the
 * redraw path several times a frame -- #ED_space_image_get_size alone reaches it from
 * #image_main_region_set_view2d and from the preview's own clip calculation. */
TEST_F(CombinedCacheTest, size_query_does_not_shade)
{
  int width = -1;
  int height = -1;
  EXPECT_FALSE(BKE_paint_material_combined_cache_size_get(*material_, width, height));
  EXPECT_EQ(width, -1);
  EXPECT_EQ(height, -1);

  this->ensure(make_request());

  EXPECT_TRUE(BKE_paint_material_combined_cache_size_get(*material_, width, height));
  EXPECT_EQ(width, inputs_.width);
  EXPECT_EQ(height, inputs_.height);

  /* And asking again shades nothing: the query must not have disturbed the cache. */
  CombinedEvalStats stats;
  this->ensure(make_request(), nullptr, &stats);
  EXPECT_EQ(stats.pixels_processed, 0);
}

/* The cached buffer takes the output resolution, not the input one. */
TEST_F(CombinedCacheTest, cache_entry_takes_the_output_resolution)
{
  inputs_.width = 64;
  inputs_.height = 64;
  inputs_.output_width = 16;
  inputs_.output_height = 16;
  CombinedEvalStats stats;
  ImBuf *ibuf = this->ensure(make_request(), nullptr, &stats);
  ASSERT_NE(ibuf, nullptr);
  EXPECT_EQ(ibuf->x, 16);
  EXPECT_EQ(ibuf->y, 16);
  EXPECT_EQ(stats.pixels_processed, 16 * 16);
}

/* The reported rectangle is what was shaded, in the buffer's own texels -- which is what a
 * consumer refreshing its copy of those pixels needs, and is not the same as canvas texels once
 * the output is reduced. */
TEST_F(CombinedCacheTest, changed_region_is_reported_in_buffer_texels)
{
  inputs_.width = 64;
  inputs_.height = 64;
  inputs_.output_width = 16;
  inputs_.output_height = 16;

  rcti changed;
  BLI_rcti_init(&changed, -1, -1, -1, -1);
  ASSERT_NE(this->ensure_reporting(make_request(), changed), nullptr);
  /* The first call shades the whole buffer, so the report is the buffer, not the canvas. */
  EXPECT_EQ(changed.xmin, 0);
  EXPECT_EQ(changed.xmax, 16);
  EXPECT_EQ(changed.ymin, 0);
  EXPECT_EQ(changed.ymax, 16);

  /* Nothing to do on the second call, and the report has to say so rather than repeat itself. */
  this->ensure_reporting(make_request(), changed);
  EXPECT_TRUE(BLI_rcti_is_empty(&changed));
}

/* A clipped shade reports the clip, so a consumer refreshes that much of its copy and no more. */
TEST_F(CombinedCacheTest, changed_region_follows_the_clip)
{
  inputs_.width = 64;
  inputs_.height = 64;
  CombinedCacheRequest request = make_request();
  BLI_rcti_init(&request.clip, 8, 24, 8, 24);

  rcti changed;
  BLI_rcti_init(&changed, 0, 0, 0, 0);
  ASSERT_NE(this->ensure_reporting(request, changed), nullptr);
  EXPECT_EQ(changed.xmin, 8);
  EXPECT_EQ(changed.xmax, 24);
  EXPECT_EQ(changed.ymin, 8);
  EXPECT_EQ(changed.ymax, 24);
}

/* The output-resolution query answers with the buffer, where the size query answers with the
 * canvas. The gather picks its octave from the first and builds the editor's view from the second,
 * so the two must not be confused for one another. */
TEST_F(CombinedCacheTest, output_size_query_reports_the_buffer_not_the_canvas)
{
  int width = -1;
  int height = -1;
  EXPECT_FALSE(BKE_paint_material_combined_cache_output_size_get(*material_, width, height));
  EXPECT_EQ(width, -1);

  inputs_.width = 64;
  inputs_.height = 64;
  inputs_.output_width = 16;
  inputs_.output_height = 16;
  this->ensure(make_request());

  ASSERT_TRUE(BKE_paint_material_combined_cache_output_size_get(*material_, width, height));
  EXPECT_EQ(width, 16);
  EXPECT_EQ(height, 16);

  int canvas_width = 0;
  int canvas_height = 0;
  ASSERT_TRUE(BKE_paint_material_combined_cache_size_get(*material_, canvas_width, canvas_height));
  EXPECT_EQ(canvas_width, 64);
  EXPECT_EQ(canvas_height, 64);

  /* And it must not have disturbed the cache either. */
  CombinedEvalStats stats;
  this->ensure(make_request(), nullptr, &stats);
  EXPECT_EQ(stats.pixels_processed, 0);
}

/* Changing the output resolution is a full rebuild, and the buffer is replaced. */
TEST_F(CombinedCacheTest, output_resolution_change_rebuilds)
{
  inputs_.width = 64;
  inputs_.height = 64;
  inputs_.output_width = 16;
  inputs_.output_height = 16;
  this->ensure(make_request());

  inputs_.output_width = 32;
  inputs_.output_height = 32;
  CombinedEvalStats stats;
  ImBuf *ibuf = this->ensure(make_request(), nullptr, &stats);
  ASSERT_NE(ibuf, nullptr);
  EXPECT_EQ(ibuf->x, 32);
  EXPECT_EQ(stats.pixels_processed, 32 * 32);
}

/* A dirty region the clip sits inside must be shaded once, not once per call.
 *
 * The remainder -- everything but the clip -- is not a rectangle, and keeping it as a superset
 * made the next call in the same frame shade the same pixels again. The Image Editor resolves the
 * preview three times per redraw, so that was a threefold cost on the first dab of every stroke.
 */
TEST_F(CombinedCacheTest, dirty_covering_the_clip_is_shaded_once)
{
  inputs_.width = 64;
  inputs_.height = 64;
  CombinedCacheRequest request = make_request();
  BLI_rcti_init(&request.clip, 16, 48, 16, 48);
  this->ensure(request);

  CombinedCacheRequest dirtied = request;
  BLI_rcti_init(&dirtied.changed_region, 0, 64, 0, 64);
  CombinedEvalStats stats;
  this->ensure(dirtied, nullptr, &stats);
  EXPECT_EQ(stats.pixels_processed, 32 * 32);

  /* The second and third resolves of the same frame must find it current. */
  this->ensure(request, nullptr, &stats);
  EXPECT_EQ(stats.pixels_processed, 0);
}

/** \} */

}  // namespace blender::bke::tests
