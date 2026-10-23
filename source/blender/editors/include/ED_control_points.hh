/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 *
 * The shared screen-space layer of every Bézier control-point editor: what a point looks like
 * on screen, which point / handle is under the cursor, and the drag protocol on top of it.
 *
 * A document plugs in through #Source, which answers in REGION PIXELS with the handles already
 * resolved (auto handles evaluated). That is the only thing the two data models -- a
 * `bke::CurvesGeometry` of a Curve Patch and the splines of a #PaintShape -- have in common;
 * where the points come from, how an auto handle is computed and how a mutation is persisted
 * stay with the document.
 *
 * The draw data (#PaintCurveScreenHandles) is then drawn by one implementation, the overlay
 * engine's #PaintCurveCursor, so the two systems cannot drift apart on colors, marker shapes or
 * hit thresholds.
 */

#pragma once

#include <cstdint>

#include "BLI_function_ref.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "DNA_curves_types.h" /* BEZIER_HANDLE_* */

#include "ED_paint_curve_draw.hh"

namespace blender {
struct wmEvent;
}

namespace blender::ed::sculpt_paint::control_points {

/** Bit values of a control point's selection: left handle, pivot, right handle. The curves
 * editor's `.selection*` attributes use the same bits. */
#define SEL_F1 (1 << 0)
#define SEL_F2 (1 << 1)
#define SEL_F3 (1 << 2)

/** Which part of a control point a hit-test landed on. */
enum class Part : int8_t {
  None = 0,
  Left = 1,
  Point = 2,
  Right = 3,
};

/**
 * Distance metric of the point pick. The Curve Patch has always picked with Manhattan distance;
 * a flat canvas with round knobs reads better with Euclidean. The rule -- the closest of the
 * pivot and both handles wins, ties keep the pivot -- is shared either way.
 */
enum class Metric : int8_t {
  Manhattan = 0,
  Euclidean = 1,
};

/**
 * One editable curve as the shared layer sees it.
 *
 * All positions are region pixels: the source projects its own space, so a 3D Curve Patch and a
 * flat 2D shape feed the same builder and the same picker.
 */
class Source {
 public:
  virtual ~Source() = default;

  virtual int points_num() const = 0;
  virtual float2 point_position(int point) const = 0;
  /** Both handles with the auto / aligned handle rule of the document already applied. */
  virtual void point_handles(int point, float2 &r_left, float2 &r_right) const = 0;
  /** `BEZIER_HANDLE_*` type of the stored handle ( left selects the left or the right one);
   * drives the knob shape and its theme color. */
  virtual int8_t handle_type(int point, bool left) const = 0;
  /** Bit mask of #SEL_F1 / #SEL_F2 / #SEL_F3. */
  virtual uint8_t selection(int point) const = 0;
  /**
   * False for a collapsed handle the editor does not want drawn or grabbed (a Vector point of an
   * outline whose handles are hidden). The pivot stays reachable through the point itself.
   */
  virtual bool handle_visible(int /*point*/, bool /*left*/) const
  {
    return true;
  }

  virtual int segments_num() const = 0;
  /** Tessellated segment \a segment in region pixels. */
  virtual void segment_polyline(int segment, Vector<float2> &r_out) const = 0;
};

/** Which optional affordances the builder computes. */
struct BuildOptions {
  float2 cursor = float2(0.0f);
  bool hover_point = false;
  bool hover_segment = false;
  float point_hover_radius = 12.0f;
  float segment_hover_radius = 30.0f;
  Metric hover_metric = Metric::Manhattan;
  /** Resolve the theme colors of the handle knobs. Picking does not need them, and skipping
   * them keeps a non-drawing caller out of the theme; the colors then stay zero. */
  bool compute_colors = true;
};

/**
 * Fill the shared draw data of \a source: one #PaintCurveHandleDrawData per point (resolved
 * handles, selection flags, theme color by handle type) and one #PaintCurveSegmentDrawData per
 * segment. The radius / strength / rotation handles of a Curve Patch are not touched; the
 * caller fills those itself.
 */
void handles_build(const Source &source,
                   const BuildOptions &options,
                   PaintCurveScreenHandles &r_out);

/** Result of a point / handle pick. */
struct Pick {
  int point = -1;
  Part part = Part::None;
  float distance = 0.0f;
};

/** One control point as the picker sees it, in region pixels. */
struct PointGeometry {
  float2 position;
  float2 handle_left;
  float2 handle_right;
  /** False for a handle that must be neither drawn nor grabbed. */
  bool show_left = true;
  bool show_right = true;
};

/**
 * The closest pivot or handle under \a pos within \a threshold, or an empty #Pick, for points
 * produced on demand by \a point_get. This is the single implementation behind every point pick
 * (the draw-data overload below and #paintcurve_find_in_screen_points), so a caller whose points
 * live in another layout does not need to convert them first.
 * With \a ignore_pivot a hit on the pivot redirects to the nearer handle, which is what a
 * "drag the tangent" tool wants.
 */
Pick pick_point(int points_num,
                FunctionRef<PointGeometry(int)> point_get,
                float2 pos,
                float threshold,
                Metric metric,
                bool ignore_pivot = false);

/**
 * Draw-data overload of the pick above.
 * With \a ignore_pivot a hit on the pivot redirects to the nearer handle, which is what a
 * "drag the tangent" tool wants.
 */
Pick pick_point(Span<PaintCurveHandleDrawData> points,
                float2 pos,
                float threshold,
                Metric metric,
                bool ignore_pivot = false);

/** Pick straight from a #Source, without building the draw data first. */
Pick pick_point(const Source &source,
                float2 pos,
                float threshold,
                Metric metric,
                bool ignore_pivot = false);

/**
 * The closest segment under \a pos within \a threshold. \a r_t is the normalized position along
 * the picked polyline (0 at its first vertex, 1 at its last). That is an ARC-LENGTH fraction of
 * the tessellation, not the Bezier parameter of the segment, so it must not feed a point insertion
 * that expects the latter.
 */
bool pick_segment(Span<PaintCurveSegmentDrawData> segments,
                  float2 pos,
                  float threshold,
                  int &r_segment,
                  float &r_t,
                  float *r_distance_sq = nullptr);

/** Theme color of one handle knob, selected or not, by #BEZIER_HANDLE_* type. */
void handle_color_get(int8_t handle_type, bool selected, float r_color[4]);

/** Translate a picked #Part to the #SEL_F* bit the selection attributes use. */
inline uint8_t part_to_sel_flag(const Part part)
{
  switch (part) {
    case Part::Left:
      return SEL_F1;
    case Part::Point:
      return SEL_F2;
    case Part::Right:
      return SEL_F3;
    case Part::None:
      break;
  }
  return 0;
}

}  // namespace blender::ed::sculpt_paint::control_points
