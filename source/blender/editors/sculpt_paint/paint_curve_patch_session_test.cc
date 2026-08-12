/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

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

}  // namespace blender::ed::sculpt_paint::tests
