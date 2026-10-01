/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BKE_appdir.hh"
#include "BKE_gtest_base.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_paint.hh"
#include "BKE_paint_vector.hh"

#include "BLI_fileops.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"


#include "BLO_readfile.hh"
#include "BLO_writefile.hh"

#include "DNA_paint_vector_types.h"
#include "DNA_scene_types.h"

namespace blender {

namespace {

/** Two items with points, a spline and the full owned style profiles, so copy / save must handle
 * every owning pointer an item can have. */
PaintVector *make_two_item_vector(Main *bmain, const char *name)
{
  PaintVector *pv = BKE_paint_vector_add(bmain, name);
  /* Unused IDs are not saved; the test opens a fresh file. */
  id_fake_user_set(&pv->id);

  PaintVectorItem &a = BKE_paint_vector_item_add(*pv);
  a.type = PAINT_VECTOR_ITEM_SHAPE;
  a.shape_type = PAINT_SHAPE_LINE;
  BKE_paint_vector_item_style_init(a);
  a.style.stroke_width = 12.0f;
  MutableSpan<PaintVectorPoint> points = BKE_paint_vector_item_points_alloc(a, 2);
  points[0].co[0] = 1.0f;
  points[1].co[0] = 2.0f;
  MutableSpan<PaintVectorSpline> splines = BKE_paint_vector_item_splines_alloc(a, 1);
  splines[0].point_offset = 0;
  splines[0].point_num = 2;

  PaintVectorItem &b = BKE_paint_vector_item_add(*pv);
  b.type = PAINT_VECTOR_ITEM_SHAPE;
  b.shape_type = PAINT_SHAPE_ELLIPSE;
  BKE_paint_vector_item_style_init(b);
  b.style.fill_type = PAINT_SHAPE_FILL_GRADIENT;

  pv->active_item = 1;
  return pv;
}

class PaintVectorTest : public bke::BlenderGTestBase {
 public:
  Main *bmain = nullptr;

  void SetUp() override
  {
    bmain = BKE_main_new();
  }
  void TearDown() override
  {
    BKE_main_free(bmain);
  }
};

TEST_F(PaintVectorTest, CopyIsDeepAndIndependent)
{
  PaintVector *pv = make_two_item_vector(bmain, "PVCopy");

  ID *copy_id = BKE_id_copy(bmain, &pv->id);
  ASSERT_NE(copy_id, nullptr);
  PaintVector *copy = id_cast<PaintVector *>(copy_id);

  ASSERT_EQ(copy->items_num, 2);
  EXPECT_EQ(copy->active_item, 1);
  /* Deep copy: no owning pointer is shared. */
  EXPECT_NE(copy->items, pv->items);
  EXPECT_NE(copy->items[0].points, pv->items[0].points);
  EXPECT_NE(copy->items[0].splines, pv->items[0].splines);
  EXPECT_NE(copy->items[0].style.stroke_profile, pv->items[0].style.stroke_profile);
  EXPECT_NE(copy->items[0].style.stroke_ramp, pv->items[0].style.stroke_ramp);

  /* Mutating the copy leaves the original untouched. */
  copy->items[0].style.stroke_width = 99.0f;
  copy->items[0].points[0].co[0] = -1.0f;
  EXPECT_FLOAT_EQ(pv->items[0].style.stroke_width, 12.0f);
  EXPECT_FLOAT_EQ(pv->items[0].points[0].co[0], 1.0f);

  BKE_id_free(bmain, copy_id);
}

TEST_F(PaintVectorTest, NeedsRebakeTracksRevision)
{
  PaintVector *pv = make_two_item_vector(bmain, "PVRev");

  /* A freshly built document has not been baked, but nothing changed since "revision 0". */
  EXPECT_FALSE(BKE_paint_vector_needs_rebake(*pv));

  BKE_paint_vector_tag_changed(*pv);
  EXPECT_TRUE(BKE_paint_vector_needs_rebake(*pv));
  BKE_paint_vector_tag_baked(*pv);
  EXPECT_FALSE(BKE_paint_vector_needs_rebake(*pv));

  /* Every further change is seen again after a bake. */
  BKE_paint_vector_tag_changed(*pv);
  BKE_paint_vector_tag_changed(*pv);
  EXPECT_TRUE(BKE_paint_vector_needs_rebake(*pv));
  BKE_paint_vector_tag_baked(*pv);
  EXPECT_FALSE(BKE_paint_vector_needs_rebake(*pv));
}

TEST_F(PaintVectorTest, WasBakedFlagIsExplicit)
{
  /* Until Stack Layers, re-editing an already baked PaintVector writes only the document; the
   * decision is the explicit DNA flag, not the revision counters (O45). */
  PaintVector *pv = make_two_item_vector(bmain, "PVBaked");
  EXPECT_FALSE(BKE_paint_vector_was_baked(*pv));
  BKE_paint_vector_tag_changed(*pv);
  EXPECT_FALSE(BKE_paint_vector_was_baked(*pv));
  BKE_paint_vector_tag_baked(*pv);
  EXPECT_TRUE(BKE_paint_vector_was_baked(*pv));
  BKE_paint_vector_tag_changed(*pv);
  EXPECT_TRUE(BKE_paint_vector_was_baked(*pv));
}

TEST_F(PaintVectorTest, StyleSnapshotDeepCopyRoundTrip)
{
  PaintShapeSettings original = {};
  BKE_paint_shape_settings_init(&original);
  original.stroke_width = 5.0f;
  original.fill_type = PAINT_SHAPE_FILL_GRADIENT;
  original.stroke_channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].color[0] = 0.25f;

  /* This is exactly the save/restore path of the tool settings around a `vector_edit` session. */
  PaintShapeSettings copy = {};
  BKE_paint_vector_style_copy(copy, original);
  EXPECT_FLOAT_EQ(copy.stroke_width, 5.0f);
  EXPECT_EQ(copy.fill_type, PAINT_SHAPE_FILL_GRADIENT);
  EXPECT_FLOAT_EQ(copy.stroke_channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].color[0], 0.25f);
  EXPECT_NE(copy.stroke_profile, nullptr);
  EXPECT_NE(copy.fill_profile, nullptr);
  EXPECT_NE(copy.stroke_ramp, nullptr);
  EXPECT_NE(copy.stroke_profile, original.stroke_profile);
  EXPECT_NE(copy.stroke_ramp, original.stroke_ramp);

  /* The copy is independent: the session edits its own block, leaving the original untouched
   * (Variant A isolation). */
  copy.stroke_width = 99.0f;
  copy.stroke_channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].color[0] = 0.9f;
  EXPECT_FLOAT_EQ(original.stroke_width, 5.0f);
  EXPECT_FLOAT_EQ(original.stroke_channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].color[0], 0.25f);

  BKE_paint_vector_style_free(copy);
  BKE_paint_vector_style_free(original);
}

TEST_F(PaintVectorTest, MaterialLinkRoundTrip)
{
  Material *ma = BKE_material_add(bmain, "PVMat");
  PaintVector *pv = BKE_paint_vector_add(bmain, "PVLink");

  EXPECT_EQ(BKE_paint_vector_material_count(*ma), 0);
  EXPECT_EQ(BKE_paint_vector_material_get(*ma, "PVLink"), nullptr);

  BKE_paint_vector_material_link(*ma, *pv);
  /* The "create + assign" pattern drops the user `BKE_id_new` added, leaving the material link as
   * the single owner. */
  id_us_min(&pv->id);
  EXPECT_EQ(pv->id.us, 1);
  EXPECT_EQ(BKE_paint_vector_material_count(*ma), 1);
  EXPECT_EQ(BKE_paint_vector_material_get(*ma, "PVLink"), pv);
  EXPECT_EQ(BKE_paint_vector_material_get_index(*ma, 0), pv);

  /* Linking again is idempotent, matched by ID pointer even after a rename. */
  BKE_paint_vector_material_link(*ma, *pv);
  EXPECT_EQ(BKE_paint_vector_material_count(*ma), 1);
  BLI_strncpy(pv->id.name + 2, "PVRenamed", sizeof(pv->id.name) - 2);
  EXPECT_EQ(BKE_paint_vector_material_get(*ma, "PVRenamed"), pv);
  EXPECT_EQ(BKE_paint_vector_material_get(*ma, "PVLink"), nullptr);
  BKE_paint_vector_material_link(*ma, *pv);
  EXPECT_EQ(BKE_paint_vector_material_count(*ma), 1);

  BKE_paint_vector_material_unlink(*ma, *pv);
  EXPECT_EQ(BKE_paint_vector_material_count(*ma), 0);

  BKE_id_free(bmain, &pv->id);
  BKE_id_free(bmain, ma);
}

TEST_F(PaintVectorTest, SaveReloadKeepsAllItems)
{
  PaintVector *pv = make_two_item_vector(bmain, "PVSaveTest");
  pv->items[0].style.stroke_width = 13.0f;

  char filepath[FILE_MAX];
  BLI_path_join(filepath, sizeof(filepath), BKE_tempdir_session(), "paint_vector_test.blend");
  BlendFileWriteParams write_params{};
  ASSERT_TRUE(BLO_write_file(bmain, filepath, 0, &write_params, nullptr));

  BlendFileReadReport read_report{};
  BlendFileData *bfd = BLO_read_from_file(filepath, BLO_READ_SKIP_NONE, &read_report);
  ASSERT_NE(bfd, nullptr);

  PaintVector *loaded = nullptr;
  for (PaintVector &cur : bfd->main->paint_vectors) {
    if (STREQ(cur.id.name + 2, "PVSaveTest")) {
      loaded = &cur;
      break;
    }
  }
  ASSERT_NE(loaded, nullptr);

  /* The failing case of the old writer: >1 item read back as more than one. */
  ASSERT_EQ(loaded->items_num, 2);
  EXPECT_EQ(loaded->active_item, 1);
  EXPECT_EQ(loaded->items[0].points_num, 2);
  EXPECT_EQ(loaded->items[0].splines_num, 1);
  EXPECT_FLOAT_EQ(loaded->items[0].points[0].co[0], 1.0f);
  EXPECT_FLOAT_EQ(loaded->items[0].style.stroke_width, 13.0f);
  EXPECT_NE(loaded->items[0].style.stroke_profile, nullptr);
  EXPECT_EQ(loaded->items[1].shape_type, PAINT_SHAPE_ELLIPSE);

  BLO_blendfiledata_free(bfd);
  BLI_delete(filepath, false, false);
}

}  // namespace

}  // namespace blender
