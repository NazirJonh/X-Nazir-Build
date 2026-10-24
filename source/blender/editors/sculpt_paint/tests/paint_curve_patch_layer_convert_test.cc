/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** Round-trips of the dedicated PaintVector <-> CurvePatchDocument converter over a MIXED
 * container: Shape items must survive untouched and in order, curves map through their remembered
 * item indices, and a session-created patch appends. */

#include "../paint_curve_patch_document.hh"
#include "../paint_curve_patch_layer.hh"
#include "../shapes/paint_vector_document.hh"

#include "DNA_curves_types.h"
#include "DNA_paint_vector_types.h"

#include "BKE_curves.hh"
#include "BKE_gtest_base.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_paint_vector.hh"

#include "BLI_index_range.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_string.h"

#include "MEM_guardedalloc.h"

#include "testing/testing.h"

namespace blender::ed::sculpt_paint {

namespace {

class CurvePatchLayerConvertTest : public bke::BlenderGTestBase {
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

/** A CurvePatch item with a distinctive two-point curve and frozen parameters. */
PaintVectorItem &make_curve_item(PaintVector &pv, const char *name)
{
  PaintVectorItem &item = BKE_paint_vector_item_add(pv);
  item.type = PAINT_VECTOR_ITEM_CURVE_PATCH;
  BLI_strncpy(item.curve_name, name, sizeof(item.curve_name));

  bke::CurvePatchParams params;
  params.radius = 0.4f;
  params.stamp_seed = 77;
  item.curve_patch_params = BKE_paint_vector_curve_params_encode(params);

  bke::CurvesGeometry &geom = item.curve_patch_geometry.wrap();
  geom = bke::CurvesGeometry(2, 1);
  geom.offsets_for_write()[0] = 0;
  geom.offsets_for_write()[1] = 2;
  geom.fill_curve_types(CURVE_TYPE_BEZIER);
  const MutableSpan<float3> positions = geom.positions_for_write();
  positions[0] = float3(-1.0f, 0.0f, 0.0f);
  positions[1] = float3(1.0f, 0.0f, 0.0f);
  geom.tag_positions_changed();
  return item;
}

/** A Shape item that only carries a marker value, enough to identify it after a round-trip. */
PaintVectorItem &make_shape_item(PaintVector &pv, const float marker_x)
{
  PaintVectorItem &item = BKE_paint_vector_item_add(pv);
  item.type = PAINT_VECTOR_ITEM_SHAPE;
  item.shape_type = PAINT_SHAPE_RECT;
  item.center[0] = marker_x;
  return item;
}

}  // namespace

TEST_F(CurvePatchLayerConvertTest, MixedRoundTripKeepsShapeItems)
{
  PaintVector *pv = BKE_paint_vector_add(bmain, "PVConvert");
  make_shape_item(*pv, 1.0f);
  make_curve_item(*pv, "Curve A");
  make_shape_item(*pv, 3.0f);
  pv->active_item = 1;

  CurvePatchDocument doc;
  curve_patch_document_from_vector(*pv, doc);
  /* Only the curve item became a patch; its remembered index is the item's. */
  ASSERT_EQ(doc.patches.size(), 1);
  EXPECT_EQ(doc.patches[0].source_curve_index, 1);
  EXPECT_EQ(doc.active_patch, 0);
  EXPECT_FLOAT_EQ(doc.patches[0].params.radius, 0.4f);
  EXPECT_EQ(doc.patches[0].params.stamp_seed, 77u);
  EXPECT_EQ(doc.patches[0].control_curve.positions()[0], float3(-1.0f, 0.0f, 0.0f));

  /* The write-back touches only the curve item: the two Shape items keep their indices, order and
   * data. */
  doc.patches[0].control_curve.positions_for_write()[0] = float3(-2.0f, 0.0f, 0.0f);
  curve_patch_document_to_vector(doc, *pv);

  ASSERT_EQ(pv->items_num, 3);
  EXPECT_EQ(pv->items[0].type, PAINT_VECTOR_ITEM_SHAPE);
  EXPECT_FLOAT_EQ(pv->items[0].center[0], 1.0f);
  EXPECT_EQ(pv->items[2].type, PAINT_VECTOR_ITEM_SHAPE);
  EXPECT_FLOAT_EQ(pv->items[2].center[0], 3.0f);
  EXPECT_EQ(pv->active_item, 1);
  EXPECT_EQ(pv->items[1].curve_patch_geometry.wrap().positions()[0], float3(-2.0f, 0.0f, 0.0f));

  BKE_id_free(bmain, &pv->id);
}

TEST_F(CurvePatchLayerConvertTest, SessionCreatedPatchAppends)
{
  PaintVector *pv = BKE_paint_vector_add(bmain, "PVConvertAppend");
  make_curve_item(*pv, "Curve A");

  CurvePatchDocument doc;
  curve_patch_document_from_vector(*pv, doc);
  ASSERT_EQ(doc.patches.size(), 1);

  /* A patch created inside the session has no source item: it appends after every existing item. */
  CurvePatchItem created;
  created.source_curve_index = -1;
  created.params.radius = 0.9f;
  doc.patches.append(std::move(created));
  doc.active_patch = 1;

  curve_patch_document_to_vector(doc, *pv);
  ASSERT_EQ(pv->items_num, 2);
  EXPECT_EQ(pv->items[0].type, PAINT_VECTOR_ITEM_CURVE_PATCH);
  EXPECT_STREQ(pv->items[0].curve_name, "Curve A");
  EXPECT_EQ(pv->items[1].type, PAINT_VECTOR_ITEM_CURVE_PATCH);
  EXPECT_FLOAT_EQ(pv->items[1].curve_patch_params.radius, 0.9f);
  /* The active patch is the appended one: the container's active item follows. */
  EXPECT_EQ(pv->active_item, 1);

  BKE_id_free(bmain, &pv->id);
}

}  // namespace blender::ed::sculpt_paint
