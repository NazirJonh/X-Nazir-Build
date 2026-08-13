/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "ED_curve_patch.hh"

#include "paint_curve_patch_session.hh"

namespace blender::ed::sculpt_paint::tests {

TEST(paint_curve_patch_session, active_source_curve_maps_to_filtered_patch)
{
  CurvePatchSession session;
  session.patches.resize(2);
  session.patches[0].source_curve_index = 1;
  session.patches[1].source_curve_index = 3;

  EXPECT_EQ(curve_patch_index_for_source_curve(session, 1), 0);
  EXPECT_EQ(curve_patch_index_for_source_curve(session, 3), 1);
}

TEST(paint_curve_patch_session, missing_source_curve_is_reported_as_unmapped)
{
  CurvePatchSession session;
  session.patches.resize(2);
  session.patches[0].source_curve_index = 2;
  session.patches[1].source_curve_index = 4;

  EXPECT_EQ(curve_patch_index_for_source_curve(session, 0), -1);
  EXPECT_EQ(curve_patch_index_for_source_curve(session, 5), -1);
}

TEST(paint_curve_patch_session, missing_source_curve_has_no_fallback_for_empty_session)
{
  const CurvePatchSession session;
  EXPECT_EQ(curve_patch_index_for_source_curve(session, 0), -1);
}

TEST(paint_curve_patch_session, overlay_data_is_empty_without_session)
{
  blender::CurvePatchOverlayData data;
  data.splines.append(nullptr);
  data.active_index = 4;

  blender::ED_curve_patch_overlay_data_get(static_cast<const CurvePatchSession *>(nullptr), data);

  EXPECT_TRUE(data.splines.is_empty());
  EXPECT_EQ(data.active_index, -1);
}

TEST(paint_curve_patch_session, overlay_data_lists_all_splines_and_marks_active)
{
  CurvePatchSession session;
  session.patches.resize(2);
  session.active_patch = 1;

  blender::CurvePatchOverlayData data;
  blender::ED_curve_patch_overlay_data_get(&session, data);

  ASSERT_EQ(data.splines.size(), 2);
  EXPECT_EQ(data.splines[0], &session.patches[0].control_curve);
  EXPECT_EQ(data.splines[1], &session.patches[1].control_curve);
  EXPECT_EQ(data.active_index, 1);
}

TEST(paint_curve_patch_session, overlay_data_has_no_active_on_half_built_session)
{
  CurvePatchSession session;
  session.active_patch = 0;

  blender::CurvePatchOverlayData data;
  blender::ED_curve_patch_overlay_data_get(&session, data);

  EXPECT_TRUE(data.splines.is_empty());
  EXPECT_EQ(data.active_index, -1);
}

}  // namespace blender::ed::sculpt_paint::tests
