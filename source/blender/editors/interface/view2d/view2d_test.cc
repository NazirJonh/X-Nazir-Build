/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Invariants of the #View2D coordinate conversions under canvas rotation.
 *
 * The rotation splits the API in two frames (see #UI_view2d.hh): the *navigation* frame is the
 * axis-aligned `cur <-> mask` map, the *display* frame additionally applies the rotation about the
 * pivot. Several call sites depend on properties that hold across both, and which are easy to break
 * silently because a wrong rotation still looks plausible on screen. Pin them here.
 */

#include "DNA_view2d_types.h"

#include "BLI_math_constants.h"
#include "BLI_rect.h"

#include "UI_view2d.hh"

#include "testing/testing.h"

namespace blender::ui::tests {

/** A region of `width x height` pixels showing `cur`, rotated by `rotation` about `pivot`. */
static View2D make_view2d(const int width,
                          const int height,
                          const rctf &cur,
                          const float rotation,
                          const float pivot_x = 0.5f,
                          const float pivot_y = 0.5f)
{
  View2D v2d = {};
  BLI_rcti_init(&v2d.mask, 0, width - 1, 0, height - 1);
  v2d.cur = cur;
  v2d.tot = cur;
  v2d.rotation = rotation;
  v2d.rotation_pivot[0] = pivot_x;
  v2d.rotation_pivot[1] = pivot_y;
  return v2d;
}

static rctf unit_cur()
{
  rctf cur;
  BLI_rctf_init(&cur, 0.0f, 1.0f, 0.0f, 1.0f);
  return cur;
}

/** A panned and zoomed `cur`, so the tests do not accidentally pass on a symmetric special case. */
static rctf panned_cur()
{
  rctf cur;
  BLI_rctf_init(&cur, -0.25f, 0.4f, 0.1f, 0.6f);
  return cur;
}

static const float test_rotations[] = {
    0.0f, DEG2RADF(30.0f), DEG2RADF(90.0f), DEG2RADF(-45.0f), DEG2RADF(180.0f), DEG2RADF(-137.0f)};

/* -------------------------------------------------------------------- */
/** \name Round Trips
 * \{ */

TEST(view2d_rotation, view_to_region_round_trip)
{
  const float points[][2] = {{0.0f, 0.0f}, {0.5f, 0.5f}, {1.0f, 0.25f}, {-0.3f, 0.9f}};

  const rctf curs[] = {unit_cur(), panned_cur()};

  for (const float rotation : test_rotations) {
    for (const rctf &cur : curs) {
      const View2D v2d = make_view2d(800, 600, cur, rotation, 0.35f, 0.7f);
      for (const float(&p)[2] : points) {
        float region_x, region_y;
        view2d_view_to_region_fl(&v2d, p[0], p[1], &region_x, &region_y);

        float view_x, view_y;
        view2d_region_to_view(&v2d, region_x, region_y, &view_x, &view_y);

        EXPECT_NEAR(view_x, p[0], 1e-4f) << "rotation " << rotation;
        EXPECT_NEAR(view_y, p[1], 1e-4f) << "rotation " << rotation;
      }
    }
  }
}

TEST(view2d_rotation, region_to_view_round_trip)
{
  const float pixels[][2] = {{0.0f, 0.0f}, {400.0f, 300.0f}, {799.0f, 0.0f}, {-50.0f, 620.0f}};

  for (const float rotation : test_rotations) {
    const View2D v2d = make_view2d(800, 600, panned_cur(), rotation, 0.35f, 0.7f);
    for (const float(&px)[2] : pixels) {
      float view_x, view_y;
      view2d_region_to_view(&v2d, px[0], px[1], &view_x, &view_y);

      float region_x, region_y;
      view2d_view_to_region_fl(&v2d, view_x, view_y, &region_x, &region_y);

      EXPECT_NEAR(region_x, px[0], 1e-2f) << "rotation " << rotation;
      EXPECT_NEAR(region_y, px[1], 1e-2f) << "rotation " << rotation;
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Frame Separation
 * \{ */

/** With no rotation the display frame and the navigation frame must be the same mapping. */
TEST(view2d_rotation, frames_agree_without_rotation)
{
  const View2D v2d = make_view2d(800, 600, panned_cur(), 0.0f);

  float display_x, display_y, navigation_x, navigation_y;
  view2d_view_to_region_fl(&v2d, 0.2f, 0.8f, &display_x, &display_y);
  view2d_view_to_region_navigation_fl(&v2d, 0.2f, 0.8f, &navigation_x, &navigation_y);

  EXPECT_FLOAT_EQ(display_x, navigation_x);
  EXPECT_FLOAT_EQ(display_y, navigation_y);
}

/**
 * The pivot is the fixed point of the display rotation: it must land on the same pixel whatever the
 * rotation is. Zoom and pan code relies on this (see #view2d_region_to_view_zoom_anchor).
 */
TEST(view2d_rotation, pivot_pixel_is_rotation_invariant)
{
  const float pivot_x = 0.35f, pivot_y = 0.7f;

  const View2D unrotated = make_view2d(800, 600, panned_cur(), 0.0f, pivot_x, pivot_y);
  float expected_x, expected_y;
  view2d_view_to_region_fl(&unrotated, pivot_x, pivot_y, &expected_x, &expected_y);

  for (const float rotation : test_rotations) {
    const View2D v2d = make_view2d(800, 600, panned_cur(), rotation, pivot_x, pivot_y);
    float region_x, region_y;
    view2d_view_to_region_fl(&v2d, pivot_x, pivot_y, &region_x, &region_y);

    EXPECT_NEAR(region_x, expected_x, 1e-3f) << "rotation " << rotation;
    EXPECT_NEAR(region_y, expected_y, 1e-3f) << "rotation " << rotation;
  }
}

/**
 * Border zoom adopts `cur' = A^-1(rect)`, which is defined in the navigation frame. Its result must
 * therefore not move when the canvas is rotated, unlike #view2d_region_to_view_rctf.
 */
TEST(view2d_rotation, zoom_bounds_ignore_rotation)
{
  rctf rect;
  BLI_rctf_init(&rect, 100.0f, 500.0f, 80.0f, 420.0f);

  const View2D unrotated = make_view2d(800, 600, panned_cur(), 0.0f, 0.35f, 0.7f);
  rctf expected;
  view2d_region_to_view_rctf_zoom_bounds(&unrotated, &rect, &expected);

  for (const float rotation : test_rotations) {
    const View2D v2d = make_view2d(800, 600, panned_cur(), rotation, 0.35f, 0.7f);
    rctf bounds;
    view2d_region_to_view_rctf_zoom_bounds(&v2d, &rect, &bounds);

    EXPECT_FLOAT_EQ(bounds.xmin, expected.xmin) << "rotation " << rotation;
    EXPECT_FLOAT_EQ(bounds.xmax, expected.xmax) << "rotation " << rotation;
    EXPECT_FLOAT_EQ(bounds.ymin, expected.ymin) << "rotation " << rotation;
    EXPECT_FLOAT_EQ(bounds.ymax, expected.ymax) << "rotation " << rotation;
  }
}

/** Under rotation a screen rect maps to a quad; the rect form must be exactly its bounding box. */
TEST(view2d_rotation, region_to_view_rctf_bounds_the_quad)
{
  const View2D v2d = make_view2d(800, 600, panned_cur(), DEG2RADF(30.0f), 0.35f, 0.7f);

  rcti rect_i;
  BLI_rcti_init(&rect_i, 100, 500, 80, 420);
  rctf rect_f;
  BLI_rctf_rcti_copy(&rect_f, &rect_i);

  float corners[4][2];
  view2d_region_to_view_quad(&v2d, &rect_i, corners);

  rctf expected;
  BLI_rctf_init_minmax(&expected);
  for (const float(&corner)[2] : corners) {
    BLI_rctf_do_minmax_v(&expected, corner);
  }

  rctf bounds;
  view2d_region_to_view_rctf(&v2d, &rect_f, &bounds);

  EXPECT_NEAR(bounds.xmin, expected.xmin, 1e-5f);
  EXPECT_NEAR(bounds.xmax, expected.xmax, 1e-5f);
  EXPECT_NEAR(bounds.ymin, expected.ymin, 1e-5f);
  EXPECT_NEAR(bounds.ymax, expected.ymax, 1e-5f);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name GPU Matrix Agreement
 * \{ */

/** Project a view point the way the GPU pipeline does: rotation matrix, then the ortho map. */
static void project_via_rotation_matrix(const View2D *v2d,
                                        const float x,
                                        const float y,
                                        float *r_region_x,
                                        float *r_region_y)
{
  float mat[4][4];
  view2d_view_rotation_matrix(v2d, mat);

  /* Blender matrices are column-major: `mat[col][row]`. */
  const float rotated_x = mat[0][0] * x + mat[1][0] * y + mat[3][0];
  const float rotated_y = mat[0][1] * x + mat[1][1] * y + mat[3][1];

  view2d_view_to_region_navigation_fl(v2d, rotated_x, rotated_y, r_region_x, r_region_y);
}

/**
 * #view2d_view_rotation_matrix feeds the GPU (through #view2d_view_ortho) while
 * #view2d_view_to_region_fl feeds every interactive tool and overlay. If the two ever disagree,
 * the drawn image drifts away from what the user can click on.
 *
 * A square region is used so the aspect correction is exactly 1 on both paths: the matrix derives
 * its scale from `BLI_rcti_size + 1` while the projection uses `BLI_rcti_size`, which differ by one
 * pixel of aspect on a non-square region (see the non-square case below).
 */
TEST(view2d_rotation, gpu_matrix_matches_projection_square_region)
{
  const float points[][2] = {{0.0f, 0.0f}, {0.5f, 0.5f}, {1.0f, 0.25f}, {-0.3f, 0.9f}};

  for (const float rotation : test_rotations) {
    const View2D v2d = make_view2d(600, 600, panned_cur(), rotation, 0.35f, 0.7f);
    for (const float(&p)[2] : points) {
      float expected_x, expected_y;
      view2d_view_to_region_fl(&v2d, p[0], p[1], &expected_x, &expected_y);

      float matrix_x, matrix_y;
      project_via_rotation_matrix(&v2d, p[0], p[1], &matrix_x, &matrix_y);

      EXPECT_NEAR(matrix_x, expected_x, 1e-2f) << "rotation " << rotation;
      EXPECT_NEAR(matrix_y, expected_y, 1e-2f) << "rotation " << rotation;
    }
  }
}

/**
 * Same agreement on a non-square region. The tolerance covers the one-pixel difference in how the
 * two paths measure the region: it is deliberately far tighter than the tens of pixels a wrong
 * rotation direction, pivot or aspect coupling would produce.
 */
TEST(view2d_rotation, gpu_matrix_matches_projection_wide_region)
{
  const float points[][2] = {{0.0f, 0.0f}, {0.5f, 0.5f}, {1.0f, 0.25f}, {-0.3f, 0.9f}};

  for (const float rotation : test_rotations) {
    const View2D v2d = make_view2d(800, 600, panned_cur(), rotation, 0.35f, 0.7f);
    for (const float(&p)[2] : points) {
      float expected_x, expected_y;
      view2d_view_to_region_fl(&v2d, p[0], p[1], &expected_x, &expected_y);

      float matrix_x, matrix_y;
      project_via_rotation_matrix(&v2d, p[0], p[1], &matrix_x, &matrix_y);

      EXPECT_NEAR(matrix_x, expected_x, 1.5f) << "rotation " << rotation;
      EXPECT_NEAR(matrix_y, expected_y, 1.5f) << "rotation " << rotation;
    }
  }
}

/** The matrix must be exactly the identity when nothing is rotated. */
TEST(view2d_rotation, gpu_matrix_is_identity_without_rotation)
{
  const View2D v2d = make_view2d(800, 600, panned_cur(), 0.0f);

  float mat[4][4];
  view2d_view_rotation_matrix(&v2d, mat);

  for (int col = 0; col < 4; col++) {
    for (int row = 0; row < 4; row++) {
      EXPECT_FLOAT_EQ(mat[col][row], col == row ? 1.0f : 0.0f);
    }
  }
}

/** \} */

}  // namespace blender::ui::tests
