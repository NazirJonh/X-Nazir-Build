/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BKE_curves.hh"

#include "DNA_curves_types.h"

#include "paint_curve_intern.hh"

namespace blender::ed::sculpt_paint::tests {

TEST(paint_curve_geometry, init_bezier_single_curve)
{
  bke::CurvesGeometry geom;
  paintcurve_geometry_init_bezier(geom, 3);
  EXPECT_EQ(geom.points_num(), 3);
  EXPECT_EQ(geom.curves_num(), 1);
  EXPECT_EQ(geom.curve_types()[0], CURVE_TYPE_BEZIER);
}

}  // namespace blender::ed::sculpt_paint::tests
