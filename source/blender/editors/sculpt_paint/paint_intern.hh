/* SPDX-FileCopyrightText: 2008 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#pragma once

#include "BLI_index_mask_fwd.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_rand.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "BKE_curve_patch.hh"

#include "DNA_object_enums.h"
#include "DNA_scene_enums.h"
#include "DNA_scene_types.h"
#include "DNA_vec_types.h"
#include "DNA_windowmanager_enums.h"

#include "ED_view3d.hh"

#include <memory>
#include <optional>

namespace blender {

enum class PaintMode : int8_t;

struct ARegion;
struct bContext;
struct Brush;
struct Depsgraph;
struct Image;
struct ImagePool;
struct ImageUser;
struct ImBuf;
struct Main;
struct MTex;
struct Object;
struct Paint;
struct PointerRNA;
struct RegionView3D;
struct ReportList;
struct Scene;
struct ScrArea;
struct SculptSession;
struct SpaceImage;
struct ToolSettings;
struct VertProjHandle;
struct ViewContext;
struct VPaint;
struct wmEvent;
struct wmKeyConfig;
struct wmKeyMap;
struct wmOperator;
struct wmOperatorType;

namespace bke::pbvh {
class Node;
}

namespace ed::sculpt_paint {
class PaintModeData;
struct PaintStroke;
struct PaintSample;
struct StrokeCache;

}  // namespace ed::sculpt_paint

namespace ocio {
class Display;
}
using ColorManagedDisplay = ocio::Display;

/* paint_stroke.cc */

enum class BrushStrokeMode : int8_t {
  Normal = 0,
  Invert = 1,
};

/* Indicates a brush that the stroke will switch to for the duration of the stroke */
enum class BrushSwitchMode : int8_t {
  None = 0,
  Smooth = 1,
  Erase = 2,
  Mask = 3,
};

namespace ed::sculpt_paint {

using StrokeDone = void (*)(PaintStroke *stroke, bool is_cancel);

/**
 * The message to report when a live Curve Patch session must be finished before anything else may
 * paint, or null when no session is live.
 *
 * There are two independent sessions -- 3D Sculpt Mode's (`SculptSession::curve_patch_session`)
 * and the Image Editor's module singleton -- and only one of them may be live at a time, so that
 * the user can always tell what an edit is about to change. Every stroke operator asks this at
 * invoke, BEFORE the stroke starts: refusing after the drag would throw away work the user has
 * already done. Declared here rather than in `paint_curve_patch_session.hh` because the callers
 * are four unrelated stroke operators that have no other reason to pull in a Curve Patch header.
 *
 * The returned string is a static literal naming WHICH editor holds the session, and how to end
 * it; it is never owned by the caller.
 */
const char *curve_patch_active_session_message(const bContext &C);

/* stroke operator */

struct PaintSample {
  float2 mouse = float2(0.0f, 0.0f);
  float pressure = 0.0f;
};

/**
 * Per-dab record for the Roll stroke method's ring buffer. Distinct from #PaintSample (the raw
 * per-event input-averaging sample): this carries the richer state a rolled dab needs -- the 3D
 * hit location, the frozen surface normal, tilt, size and pen flip.
 */
struct PaintStrokePoint {
  float2 mouse_in = float2(0.0f, 0.0f);
  float2 mouse_out = float2(0.0f, 0.0f);
  float3 location = float3(0.0f);
  float3 surface_normal = float3(0.0f, 0.0f, 1.0f); /* sculpt_normal at recording time. */
  float pressure = 0.0f;
  float x_tilt = 0.0f;
  float y_tilt = 0.0f;
  bool pen_flip = false;
  float size = 0.0f;
};

/**
 * Per-dab input contract for 2D Roll mapping in the Image Editor (SPACE_IMAGE + SI_MODE_PAINT,
 * `PaintMode::Texture2D`).
 *
 * Canonical units: `center_uv` is authoritative in image UV space (unbounded, so UDIM tiles are
 * supported), and `radius`/`arc_length` are expressed in the same UV units that were in effect
 * when the dab was fixed -- a finished patch must not depend on the later view zoom. The
 * concrete dab is rasterized in tile-local canvas pixels; the UV -> tile pixel conversion is
 * the existing `paint_2d_uv_to_coord()` (`paint_image_2d.cc`).
 *
 * Deliberately carries no `float3` location, surface normal or #StrokeCache: the 2D mapping
 * path is flat by contract and never touches 3D Roll state.
 */
struct ImagePaintRollDab {
  float2 center_uv = float2(0.0f, 0.0f); /* dab center in canonical image UV space. */
  float2 tangent = float2(1.0f, 0.0f);   /* unit-length path tangent in the image plane. */
  float arc_length = 0.0f;               /* along-path arc length in UV units. */
  float radius = 0.0f;                   /* brush radius in UV units at dab time. */
  bool frame_valid = false;              /* false when no stable tangent exists (first dab). */
};

/**
 * Live 2D trajectory for the Image Editor Roll mapping path
 * (`SPACE_IMAGE + SI_MODE_PAINT + PaintMode::Texture2D`).
 *
 * Records one canonical UV sample per dab: `uv_traj[i]` is the dab center converted through the
 * active `View2D` at the time of the dab, and `arc_lengths[i]` is the cumulative distance in
 * UV units up to and including sample i. The struct owns no extra state beyond these two
 * parallel vectors: tangent / frame_valid for the *current* dab are derived on demand in
 * #compute_roll_dab() at the point of dispatch to `paint_2d_stroke()`, because the BrushPainter
 * mapping consumes them per dab and we cannot move the path origin forward without recording
 * every input sample (3D Roll's deferred dab is the opposite trade-off; 2D Roll does not need
 * it).
 *
 * Lifetime is bound to a single 2D stroke: created empty inside the `PaintOperation`, grown by
 * exactly one entry per `ImagePaintStroke::update_step()` call when `enabled` is true, and
 * cleared at `done()` / on cancel. Independent of #RollSpline (3D) and of any tooling.
 */
struct ImagePaintRollTraj2D {
  /* Per-dab UV centers (image plane, unbounded so UDIM tiles fit). */
  Vector<float2> uv_traj;
  /* Cumulative arc length up to and including `uv_traj[i]`. First entry is always 0. */
  Vector<float> arc_lengths;

  /* True when the active stroke method requires Roll mapping (BRUSH_STROKE_ROLL, or
   * BRUSH_STROKE_CURVE with MTEX_MAP_MODE_ROLL on mtex/mask_mtex). The 2D path is opt-in: a
   * Space/Anchored/Curve brush that does not use Roll mapping leaves `enabled = false` and
   * never touches `uv_traj`. */
  bool enabled = false;

  /* Reset trajectory buffers; `enabled` is left unchanged. */
  void clear()
  {
    uv_traj.clear();
    arc_lengths.clear();
  }

  /* Append the dab at UV `uv`. The new sample is always accepted here; degenerate duplicates
   * (zero-length segments) are still kept so that exact same-position dabs -- typical of
   * BRUSH_STROKE_SPACE on stationary input -- retain a frame_valid=false fallback until motion
   * resumes. */
  void append(const float2 &uv)
  {
    uv_traj.append(uv);
    if (uv_traj.size() == 1) {
      arc_lengths.append(0.0f);
    }
    else {
      const float2 prev_uv = uv_traj[uv_traj.size() - 2];
      arc_lengths.append(arc_lengths.last() + math::length(uv - prev_uv));
    }
  }

  /* True when at least one sample has been appended. */
  bool is_empty() const
  {
    return uv_traj.is_empty();
  }

  /* True when `enabled` and non-empty. */
  bool has_active_dab() const
  {
    return enabled && !uv_traj.is_empty();
  }

  /* Compute the #ImagePaintRollDab for the last appended sample. Tangent is a forward
   * difference from the previous sample, normalized; when the segment is shorter than
   * `1e-6f` UV units or no previous sample exists, tangent falls back to unit (1, 0) and
   * `frame_valid = false`. The output extends the Stage 3 brush_painter_2d_tex_mapping
   * contract unchanged.
   *
   * `radius_uv` is the brush radius in canonical UV units, used for future consumers; the
   * current Stage 3 mapping reads `start_pixel_radius` directly, so the caller may pass
   * any conversion it computed (typically `pixel_radius / tile_size[uidx]`). */
  void compute_roll_dab(const float radius_uv, ImagePaintRollDab &r_dab) const
  {
    BLI_assert(!uv_traj.is_empty());
    const int idx = uv_traj.size() - 1;
    r_dab.center_uv = uv_traj[idx];
    r_dab.arc_length = arc_lengths[idx];
    r_dab.radius = radius_uv;

    /* Stable fallback for the first sample and any zero-length segment: tangent is unit (1, 0)
     * and the frame is flagged invalid so consumers can distinguish "first dab" from "moving
     * along the path". */
    r_dab.tangent = float2(1.0f, 0.0f);
    r_dab.frame_valid = false;
    if (idx >= 1) {
      const float2 prev_uv = uv_traj[idx - 1];
      const float2 delta = uv_traj[idx] - prev_uv;
      const float len = math::length(delta);
      /* `1e-6f` UV units is well below a single pixel even at the worst zoom and is the
       * threshold `BrushPainterMapping2D` recognizes as "no motion" when scaling the basis. */
      if (len > 1e-6f) {
        r_dab.tangent = delta / len;
        r_dab.frame_valid = true;
      }
    }
  }
};

/**
 * Live, arc-length-parameterized polyline for the Roll stroke method. Owns the recorded knots
 * (3D `poly_3d`, screen `poly_2d`, per-knot `pressures`/`normals`) and delegates all 3D
 * arc-length/closest-point math to a contained #CurvePatchSpline (`core`), rebuilt by
 * #update_lengths(). Roll-only smoothing (virtual extension, CC grid, LUT) lives in
 * `paint_stroke_roll.cc`.
 */
struct RollSpline {
  /** Geometric core used only for #closest_point_3d and #evaluate_3d. Its internal
   * `lengths_3d`/`tangents_3d` use CurvePatchSpline's leading-zero convention and are NOT read by
   * the roll code directly -- the roll grid/LUT math relies on the #length_parameterize convention
   * below (no leading zero, one entry per segment). */
  bke::CurvePatchSpline core;
  Vector<float3> poly_3d; /* Recorded 3D knots (authoritative). */
  Vector<float2> poly_2d; /* Recorded screen-space knots. */
  /** Cumulative arc lengths in #length_parameterize convention: one entry per segment (size ==
   * knots - 1), `lengths_3d.last() == total`. NOT the leading-zero convention `core` uses. */
  Vector<float> lengths_2d;
  Vector<float> lengths_3d;
  Vector<float3> tangents_3d; /* Central-difference tangent at each knot (size == knots). */
  Vector<float> pressures;    /* Per-knot pen pressure (0..1). */
  Vector<float3> normals;     /* Per-knot frozen surface normal. */

  void clear();
  bool is_empty() const;
  float total_length_3d() const; /* -> core.total_length(). */
  float total_length_2d() const;

  /** Rebuild `core` from `poly_3d` and recompute `lengths_2d`. Call after mutating the knots. */
  void update_lengths();

  /** Length of 3D segment `seg_idx` (difference of consecutive `core.lengths_3d`). */
  float segment_length_3d(int seg_idx) const;

  float3 evaluate_3d(float s) const;              /* -> core.evaluate(s). */
  float2 tangent_2d_at_index(int poly_idx) const; /* 2D direction from `poly_2d`. */

  /** Closest point on the 3D polyline: arc-length, tangent, distance. -> core.closest_point_dist.
   */
  void closest_point_3d(const float3 &query, float &r_s, float3 &r_tan, float &r_dis) const;
};

/**
 * Everything `BRUSH_STROKE_ROLL` needs for the life of one stroke, and nothing any other stroke
 * method reads.
 *
 * Held by #PaintStroke as a `std::unique_ptr` that stays null for every other stroke method, so
 * an ordinary Draw stroke no longer carries a #RollSpline and two `Vector`s it never touches.
 * Its presence IS the "roll mapping is active" flag -- see #PaintStroke::need_roll_mapping.
 *
 * The implementation lives in `paint_stroke_roll.cc` as #PaintStroke methods rather than as
 * methods here, deliberately: they read the general stroke's own dab ring buffer (`points_`,
 * `num_points_`, `cur_point_`) and its cached brush/view context throughout. Only the state that
 * is exclusively Roll's moved out; the code that drives it stays where it can still see the
 * stroke it belongs to.
 */
struct RollStrokeState {
  /** Raw arc length of stroke knots dropped from the head of the ring buffer, subtracted in
   * `spline_uv()` so the texture V coordinate stays continuous as the polyline shrinks. */
  float stroke_distance_world = 0.0f;
  /** True after virtual backward segments are prepended. */
  bool roll_virtual_prepended = false;
  /** Polyline points in virtual backward extension. */
  int n_virtual_poly_points = 0;
  /** Arc length of virtual extension (subtracted from V). */
  float roll_virtual_length = 0.0f;
  /** `cache.initial_radius`, captured on first dab. */
  float roll_initial_radius = 0.0f;
  /** Projection normal, frozen on first dab. */
  float3 roll_proj_normal = float3(0.0f);
  /** Pressure-normalized V offset for consumed knots. */
  float stroke_distance_normalized = 0.0f;
  /** Normalized arc length of virtual extension. */
  float roll_virtual_length_normalized = 0.0f;
  /** `backward_ext` knot count at creation, for budget. */
  int initial_backward_ext_count = 0;
  /** Always-on preview of unflushed spline portion. */
  void *roll_cursor = nullptr;
  void *debug_cursor = nullptr;
  /** Ring buffer index of the last deferred dab placed. */
  int last_painted_roll_idx = -1;
  RollSpline roll_spline;
  /** Virtual backward extension knots (screen space). */
  Vector<float2> backward_ext_2d;
  /** Virtual backward extension knots (world space). */
  Vector<float3> backward_ext_3d;
};

/**
 * Common structure for various paint operators (e.g. Sculpt, Grease Pencil, Curves Sculpt)
 *
 * Callback functions defined and stored on this struct (e.g. `StrokeGetLocation`) allow each of
 * these modes to customize specific behavior while still sharing other common handing.
 *
 * See #paint_stroke_modal for the majority of the paint operator logic.
 */
struct PaintStroke : NonCopyable, NonMovable {
 public:
  /* TODO: Temporary, used to assist removing usage of bContext in PaintStroke callbacks.
   * See #149378 */
  bContext *evil_C = nullptr;

  /* Cached values */
  ViewContext vc = {};
  Depsgraph *depsgraph = nullptr;
  Object *object = nullptr;
  Scene *scene = nullptr;
  Paint *paint = nullptr;
  Brush *brush = nullptr;
  UnifiedPaintSettings *ups = nullptr;

  /* TODO: These are only public so that cursor drawing code can use them. Find a better place. */
  float2 last_mouse_position = float2(0.0f, 0.0f);
  bool constrain_line = false;
  float2 constrained_pos = float2(0.0f, 0.0f);

 protected:
  std::unique_ptr<PaintModeData> mode_data_ = nullptr;

 private:
  void *stroke_cursor_ = nullptr;

  wmTimer *timer_ = nullptr;
  std::optional<RandomNumberGenerator> rng_ = std::nullopt;

  /* Paint stroke can use up to PAINT_MAX_INPUT_SAMPLES prior inputs
   * to smooth the stroke */
  PaintSample samples_[PAINT_MAX_INPUT_SAMPLES];
  int num_samples_ = 0;
  int cur_sample_ = 0;
  int tot_samples_ = 0;

  float3 last_world_space_position_ = float3(0.0f, 0.0f, 0.0f);
  float3 last_scene_spacing_delta_ = float3(0.0f, 0.0f, 0.0f);

  bool stroke_over_mesh_ = false;
  /* space distance covered so far */
  float stroke_distance_ = 0.0f;

  /* Set whether any stroke step has yet occurred
   * e.g. in sculpt mode, stroke doesn't start until cursor
   * passes over the mesh */
  bool stroke_started_ = false;
  /* Set when enough motion was found for rake rotation */
  bool rake_started_ = false;
  /* event that started stroke, for modal() return */
  int event_type_ = 0;
  /* check if stroke variables have been initialized */
  bool stroke_init_ = false;
  /** Check if input variables have been initialized (e.g. cursor position & pressure). */
  bool input_init_ = false;
  float2 initial_mouse_ = float2(0.0f, 0.0f);
  float cached_size_pressure_ = 0.0f;
  /* last pressure will store last pressure value for use in interpolation for space strokes */
  float last_pressure_ = 0.0f;
  BrushStrokeMode stroke_mode_ = BrushStrokeMode::Normal;
  BrushSwitchMode brush_switch_mode_ = BrushSwitchMode::None;

  float last_tablet_event_pressure_ = 0.0f;

  float zoom_2d_ = 0.0f;
  bool pen_flip_ = false;

  /* Tilt, as read from the event. */
  float2 tilt_ = float2(0.0f, 0.0f);

  bool original_ = false; /* Ray-cast original mesh at start of stroke. */

  /* Anchored brush repositioning via Space key. */
  bool anchored_repositioning_ = false;
  /* Original event type that entered repositioning (#EVENT_NONE if unused). */
  short anchored_reposition_event_type_ = 0;
  /* Vector from current mouse position to anchor point, saved when entering repositioning. */
  float2 anchored_visual_offset_ = float2(0.0f, 0.0f);
  /* Brush radius in screen pixels (before zoom_2d_ adjustment), locked during repositioning. */
  float anchored_saved_radius_ = 0.0f;
  /* Brush rotation at the time Space was pressed, locked during repositioning. */
  float anchored_saved_rotation_ = 0.0f;

  void anchored_reposition_begin(bContext *C, const float2 &mouse, short event_type);
  void anchored_reposition_end(bContext *C);
  /* last smoothed mouse position (for stabilize stroke finalization) */
  float2 last_smoothed_mouse_ = float2(0.0f, 0.0f);

  /* Index of the current input sample within the stroke, counted from 0. */
  int stroke_sample_index_ = 0;
  /* `Brush::spacing` as a fraction. Recomputed for every brush, not only for Roll. */
  float spacing_raw_ = 0.0f;

  /* Recorded dabs, oldest to newest within the ring. EVERY stroke method uses this, not just
   * Roll: an ordinary stroke takes the point just recorded, while Roll defers by
   * `roll_half_points()` so its spline has forward context (see #add_step). These three sat under
   * a "Roll stroke method" heading that did not describe them. */
  PaintStrokePoint points_[PAINT_MAX_INPUT_SAMPLES];
  int num_points_ = 0;
  int cur_point_ = 0;

  /* --- Roll stroke method (BRUSH_STROKE_ROLL) --- */
  /* Null for every other stroke method; its presence is what #need_roll_mapping reports. Created
   * by #stroke_init when the brush asks for roll mapping, and never afterwards. */
  std::unique_ptr<RollStrokeState> roll_ = nullptr;

 public:
  PaintStroke() = delete;

  /**
   * The main modal callback shared by any custom operator that implements a form of painting.
   *
   * At a high level, this function performs the following steps for interactive stroke types:
   * 1. Initialization of necessary common `PaintStroke` values.
   * 2. Custom paint initialization via `StrokeTestStart`>
   * 3. Create an `OperatorStrokeElement` for a given mouse position by calling `StrokeGetLocation`
   *    to potentially turn screen space coordinates into object space coordinates.
   * 4. Call `StrokeUpdateStep` to perform custom paint operation on the most recent
   *    `OperatorStrokeElement` data.
   * 5. Tag extra redraws if necessary via `StrokeRedraw`.
   * 6. Return to step 3 while stroke is ongoing.
   * 7. Call `StrokeDone` when finished to perform any cleanup or finalization.
   */
  wmOperatorStatus modal(bContext *C, wmOperator *op, const wmEvent *event);
  wmOperatorStatus exec(bContext *C, wmOperator *op);

  /** Cancel a stroke and return to the initial state.
   *
   * \note Typically handled as part of modal operator actions. Consumers of this API may need
   * to call this if returning OPERATOR_CANCELLED during the `invoke` operator callback.
   */
  void cancel(bContext *C);
  /** Finish a stroke, performing any necessary cleanup actions.
   *
   * \note Typically handled as part of modal operator actions. Consumers of this API may need
   * to call this if returning OPERATOR_FINISHED during the `invoke` operator callback.
   */
  void finish(bContext *C);

  /* TODO: The following accessors should all be parameters passed into various callbacks */
  bool stroke_flipped() const
  {
    return pen_flip_;
  }

  bool stroke_inverted() const
  {
    return stroke_mode_ == BrushStrokeMode::Invert;
  }

  float stroke_distance() const
  {
    return stroke_distance_;
  }

  /**
   * Compute roll-mapping UV coordinates for a 3D point along the stroke spline.
   * \param cache: The sculpt stroke cache (for the roll LUT/view axes).
   * \param co: Object-space position to project.
   * \param r_out: Output `[U, V, 0]` (U negated to match texture orientation).
   * \param r_tan: Output tangent at the sampled spline point.
   */
  void spline_uv(const StrokeCache &cache,
                 const float co[3],
                 float r_out[3],
                 float r_tan[3]) const;

  float spline_length() const;

  /** Returns true when roll texture mapping is active for this stroke. */
  bool need_roll_mapping() const
  {
    return roll_ != nullptr;
  }

  /**
   * Precompute the arc length, position, tangent, surface grid and UV LUT for the brush center.
   * Call once per dab (single-threaded) before the per-vertex parallel loop; results are stored on
   * `cache` for #spline_uv() to read.
   */
  void compute_roll_center(StrokeCache &cache);

  /** Debug: draw the roll spline overlay in the viewport (developer Paint Debug option). */
  void draw_debug_roll(bContext *C) const;

  /** Draw the unflushed portion of the roll spline as an always-on preview. */
  void draw_roll_preview(bContext *C) const;

  /** Resample the recorded real roll knots (excluding virtual extensions) to ~14 object-space
   * control points with a per-point radius derived from pen pressure, for the Curve Patch handoff.
   * Fills `r_positions`/`r_radii` (always the same length). Emits nothing when fewer than 2 knots
   * exist. */
  void extract_roll_control_points(Vector<float3> &r_positions, Vector<float> &r_radii) const;

  /** The projection normal frozen on the first roll dab, used as the Curve Patch plane normal in
   * the handoff. Zero for a stroke that never rolled, which the handoff already treats as "no
   * frozen normal" and replaces with the stroke's own surface normal. */
  float3 roll_plane_normal() const
  {
    return roll_ ? roll_->roll_proj_normal : float3(0.0f);
  }

 protected:
  ~PaintStroke() = default;
  PaintStroke(bContext *C, wmOperator *op, int event_type);

  /**
   * Callback function to retrieve the object space coordinates based on screen space coordinates.
   * \param location: resulting object space coordinates
   * \returns whether or not a value was actually found & the value in location is usable
   */
  virtual bool get_location(float location[3], const float mouse[2], bool force_original) = 0;

  /**
   * Callback function to determine whether a stroke has started, and performing initialization.
   *
   * In many cases, this is a check to whether the stroke is over the active mesh.
   */
  virtual bool test_start(wmOperator *op, const float mouse[2]) = 0;

  /**
   * Callback function for performing a paint stroke for a new step.
   */
  virtual void update_step(wmOperator *op, PointerRNA *itemptr) = 0;

  /**
   * Callback function for performing necessary redraw functions based on the stroke.
   */
  virtual void redraw(bool final) = 0;

  /**
   * Callback function for dynamically determining if a stroke can be cancelled.
   */
  virtual bool test_cancel() = 0;

  /**
   * Callback function for cleaning up and finalizing data after a stroke has finished.
   *
   * \param is_cancel: Some paint modes support cancelling a stroke and returning to the initial
   * state. This parameter indicates this case so that appropriate cleanup actions can be taken.
   * \param stroke_started: Whether the stroke started. Subclasses can use this to determine if
   * undo steps should be created. See \test_start.
   */
  virtual void done(bool is_cancel, bool stroke_started) = 0;

  /**
   * Called from #PaintStroke::done after #done returns. Used to run work that must execute outside
   * TBB-isolated regions created during stroke cleanup (e.g. VDM insert mesh).
   */
  virtual void post_done(bContext * /*C*/, bool /*is_cancel*/, bool /*stroke_started*/) {}

  /* TODO: This can probably be private, but `paint_image_ops_paint` depends on this */
  bool update(bContext *C,
              const Brush &brush,
              PaintMode mode,
              const float mouse_init[2],
              float mouse[2],
              float pressure,
              float r_location[3],
              bool *r_location_is_set);

 private:
  int roll_max_points() const;
  int roll_half_points() const
  {
    return (roll_max_points() * 3) / 5 + 2;
  }
  void add_roll_point(const float2 &mouse_in,
                      const float2 &mouse_out,
                      const float3 &loc,
                      const float3 &surface_normal,
                      float size,
                      float pressure,
                      bool pen_flip,
                      float x_tilt,
                      float y_tilt);
  void prepend_virtual_roll_points();
  void make_roll_spline(bContext *C);
  void finish_roll_stroke(bContext *C, wmOperator *op, const float2 &mouse_up, float pressure);
  void init_roll_cursors();

  void done(bContext *C, bool is_cancel);
  void add_step(bContext *C,
                wmOperator *op,
                float2 mval,
                float pressure,
                std::optional<float> curve_point_radius = std::nullopt);

  void add_sample(int input_samples, float x, float y, float pressure);
  void calc_average_sample(PaintSample *average);

  void lines_spacing(bContext *C,
                     wmOperator *op,
                     float spacing,
                     float *length_residue,
                     float2 old_pos,
                     float2 new_pos,
                     std::optional<float> old_curve_radius = std::nullopt,
                     std::optional<float> new_curve_radius = std::nullopt);
  int space_stroke(bContext *C, wmOperator *op, float2 final_mouse, float final_pressure);

  void line_end(bContext *C, wmOperator *op, float2 mouse);
  bool curve_end(bContext *C, wmOperator *op);
};

float2 paint_stroke_jitter_pos(Paint *paint,
                               PaintMode mode,
                               const Brush &brush,
                               float pressure,
                               BrushStrokeMode stroke_mode,
                               float zoom_2d,
                               const float2 &mval);

/**
 * Returns zero if the stroke dots should not be spaced, non-zero otherwise.
 */
bool paint_space_stroke_enabled(const Brush &br, PaintMode mode);
/**
 * Computes the overlap-compensation factor used to scale brush strength so that closely spaced
 * dabs don't over-apply relative to widely spaced ones. Callers store the result in
 * `Paint::runtime->overlap_factor`, which `brush_strength()` reads for every dab. `factor` scales
 * `Brush::spacing` before the integration (e.g. `1.0f` for a normal stroke).
 */
float paint_stroke_integrate_overlap(const Brush &br, const float factor);
/**
 * Return true if the brush size can change during paint (normally used for pressure).
 */
bool paint_supports_dynamic_size(const Brush &br, PaintMode mode);
/**
 * Return true if the brush size can change during paint (normally used for pressure).
 */
bool paint_supports_dynamic_tex_coords(const Brush &br, PaintMode mode);
bool paint_supports_smooth_stroke(const Brush &brush,
                                  PaintMode mode,
                                  BrushSwitchMode brush_switch_mode);
bool paint_supports_texture(PaintMode mode);

/**
 * Called in paint_ops.cc, on each regeneration of key-maps.
 */
wmKeyMap *paint_stroke_modal_keymap(wmKeyConfig *keyconf);

class PaintModeData {
 public:
  virtual ~PaintModeData() = default;
};

/** Returns true if the active tool uses brushes. */
bool paint_brush_tool_poll(bContext *C);
bool paint_brush_tool_poll(const ScrArea *area,
                           const ARegion *region,
                           const Paint *paint,
                           const Object *ob);
/** Returns true if the brush cursor should be activated. */
bool paint_brush_cursor_poll(bContext *C);

void BRUSH_OT_asset_activate(wmOperatorType *ot);
void BRUSH_OT_asset_save_as(wmOperatorType *ot);
void BRUSH_OT_asset_edit_metadata(wmOperatorType *ot);
void BRUSH_OT_asset_load_preview(wmOperatorType *ot);
void BRUSH_OT_asset_delete(wmOperatorType *ot);
void BRUSH_OT_asset_save(wmOperatorType *ot);
void BRUSH_OT_asset_revert(wmOperatorType *ot);

/**
 * Delete overlay cursor textures to preserve memory and invalidate all overlay flags.
 */
void paint_cursor_delete_textures();

}  // namespace ed::sculpt_paint

/* `paint_vertex.cc` */

bool weight_paint_poll(bContext *C);
bool weight_paint_poll_ignore_tool(bContext *C);
bool weight_paint_mode_poll(bContext *C);
bool weight_paint_mode_region_view3d_poll(bContext *C);
bool vertex_paint_poll(bContext *C);
bool vertex_paint_poll_ignore_tool(bContext *C);
/**
 * Returns true if vertex paint mode is active.
 */
bool vertex_paint_mode_poll(bContext *C);

using VPaintTransform_Callback = void (*)(const float col[3],
                                          const void *user_data,
                                          float r_col[3]);

void PAINT_OT_weight_paint_toggle(wmOperatorType *ot);
void PAINT_OT_weight_paint(wmOperatorType *ot);
void PAINT_OT_weight_set(wmOperatorType *ot);

enum {
  WPAINT_GRADIENT_TYPE_LINEAR,
  WPAINT_GRADIENT_TYPE_RADIAL,
};
void PAINT_OT_weight_gradient(wmOperatorType *ot);

void PAINT_OT_vertex_paint_toggle(wmOperatorType *ot);
void PAINT_OT_vertex_paint(wmOperatorType *ot);

/**
 * \note weight-paint has an equivalent function: #ED_wpaint_blend_tool
 */
unsigned int ED_vpaint_blend_tool(int tool, uint col, uint paintcol, int alpha_i);

/* `paint_vertex_weight_utils.cc` */

/**
 * \param weight: Typically the current weight: #MDeformWeight.weight
 *
 * \return The final weight, note that this is _not_ clamped from [0-1].
 * Clamping must be done on the final #MDeformWeight.weight
 *
 * \note vertex-paint has an equivalent function: #ED_vpaint_blend_tool
 */
float ED_wpaint_blend_tool(int tool, float weight, float paintval, float alpha);
/* Utility for tools to ensure vertex groups exist before they begin. */
enum eWPaintFlag {
  WPAINT_ENSURE_MIRROR = (1 << 0),
};
struct WPaintVGroupIndex {
  int active;
  int mirror;
};
/**
 * Ensure we have data on wpaint start, add if needed.
 */
bool ED_wpaint_ensure_data(bContext *C,
                           ReportList *reports,
                           eWPaintFlag flag,
                           WPaintVGroupIndex *vgroup_index);
bool ED_wpaint_ensure_data(bContext *C,
                           Main *bmain,
                           Object *object,
                           ReportList *reports,
                           eWPaintFlag flag,
                           WPaintVGroupIndex *vgroup_index);
/** Return -1 when invalid. */
int ED_wpaint_mirror_vgroup_ensure(Object *ob, int vgroup_active);

/* `paint_vertex_color_ops.cc` */

void PAINT_OT_vertex_color_set(wmOperatorType *ot);
void PAINT_OT_vertex_color_from_weight(wmOperatorType *ot);
void PAINT_OT_vertex_color_smooth(wmOperatorType *ot);
void PAINT_OT_vertex_color_brightness_contrast(wmOperatorType *ot);
void PAINT_OT_vertex_color_hsv(wmOperatorType *ot);
void PAINT_OT_vertex_color_invert(wmOperatorType *ot);
void PAINT_OT_vertex_color_levels(wmOperatorType *ot);

/* `paint_vertex_weight_ops.cc` */

void PAINT_OT_weight_from_bones(wmOperatorType *ot);
void PAINT_OT_weight_sample(wmOperatorType *ot);
void PAINT_OT_weight_sample_group(wmOperatorType *ot);

/* `paint_image.cc` */

struct ImagePaintPartialRedraw {
  rcti dirty_region;
};

bool image_texture_paint_poll(bContext *C);
bool image_paint_poll_ignore_tool(bContext *C);
void imapaint_image_update(
    SpaceImage *sima, Image *image, ImBuf *ibuf, ImageUser *iuser, short texpaint);
ImagePaintPartialRedraw *get_imapaintpartial();
void set_imapaintpartial(ImagePaintPartialRedraw *ippr);
void imapaint_region_tiles(
    ImBuf *ibuf, int x, int y, int w, int h, int *tx, int *ty, int *tw, int *th);
bool get_imapaint_zoom(bContext *C, float *zoomx, float *zoomy);
void *paint_2d_new_stroke(bContext *, wmOperator *, BrushStrokeMode mode);
void paint_2d_redraw(const bContext *C, void *ps, bool final);
void paint_2d_stroke_done(void *ps);
void paint_2d_stroke(void *ps,
                     const float prev_mval[2],
                     const float mval[2],
                     bool eraser,
                     float pressure,
                     float distance,
                     float base_size,
                     const ed::sculpt_paint::ImagePaintRollDab *roll_dab = nullptr);
/**
 * This function expects sRGB space color values.
 */
void paint_2d_bucket_fill(const bContext *C,
                          const float color[3],
                          Brush *br,
                          const float mouse_init[2],
                          const float mouse_final[2],
                          void *ps);
/**
 * 3D Texture Paint Face/Island fill.
 *
 * Ray-casts the original mesh / Edit BMesh of \a ob (not the evaluated mesh) so the hit
 * face index matches 2D Image Editor fill and selection expand. Rasterizes in UV space
 * onto `imapaint.canvas` or the hit face's material image.
 *
 * \return true if any tile was written.
 */
bool paint_image_proj_geometry_fill(
    const bContext *C, const float color[3], Brush *br, Object *ob, const float mouse[2]);
/**
 * Single-shot texture fill from 3D viewport screen coords.
 * Solid color only (v1). Geometry expand or pixel flood depending on \a brush.fill_expand.
 */
bool paint_image_viewport_fill_at_mouse(const bContext *C,
                                        const Paint *paint,
                                        Brush *brush,
                                        Object *ob,
                                        bool stroke_inverted,
                                        const float mouse[2]);
void paint_2d_gradient_fill(
    const bContext *C, Brush *br, const float mouse_init[2], const float mouse_final[2], void *ps);
void *paint_proj_new_stroke(bContext *C,
                            Object *ob,
                            const float mouse[2],
                            BrushStrokeMode mode,
                            BrushSwitchMode brush_switch_mode);
void paint_proj_stroke(const bContext *C,
                       void *ps_handle_p,
                       const float prev_pos[2],
                       const float pos[2],
                       bool eraser,
                       float pressure,
                       float distance,
                       float size);
void paint_proj_redraw(const bContext *C, void *ps_handle_p, bool final);
void paint_proj_stroke_done(void *ps_handle_p);

void paint_brush_color_get(const Paint *paint,
                           Brush *br,
                           std::optional<float3> &initial_hsv_jitter,
                           bool invert,
                           float distance,
                           float pressure,
                           float r_color[3]);
bool paint_use_opacity_masking(const Paint *paint, const Brush *brush);
void paint_brush_init_tex(Brush *brush);
void paint_brush_exit_tex(Brush *brush);

void PAINT_OT_grab_clone(wmOperatorType *ot);
namespace ed::sculpt_paint {
void PAINT_OT_sample_color(wmOperatorType *ot);
}
void PAINT_OT_brush_colors_flip(wmOperatorType *ot);
void PAINT_OT_material_paint_brush_ensure(wmOperatorType *ot);
void PAINT_OT_material_paint_images_ensure(wmOperatorType *ot);
void PAINT_OT_material_paint_brush_sync(wmOperatorType *ot);
void PAINT_OT_material_channel_value_invert(wmOperatorType *ot);
void PAINT_OT_material_channel_source_clear(wmOperatorType *ot);
void PAINT_OT_texture_paint_toggle(wmOperatorType *ot);
void PAINT_OT_project_image(wmOperatorType *ot);
void PAINT_OT_image_from_view(wmOperatorType *ot);
void PAINT_OT_add_texture_paint_slot(wmOperatorType *ot);
void PAINT_OT_image_paint(wmOperatorType *ot);
void PAINT_OT_add_simple_uvs(wmOperatorType *ot);
void PAINT_OT_brush_group_override_toggle(wmOperatorType *ot);

/* paint_texture_ops.cc */
void BRUSH_OT_texture_slot_assign_image(wmOperatorType *ot);

/* paint_image_select_mask.cc, paint_image_select_move.cc, paint_image_select_transform.cc */
void PAINT_OT_image_select_all(wmOperatorType *ot);
void PAINT_OT_image_select_none(wmOperatorType *ot);
void PAINT_OT_image_select_box(wmOperatorType *ot);
void PAINT_OT_image_select_lasso(wmOperatorType *ot);
void PAINT_OT_image_select_circle(wmOperatorType *ot);
void PAINT_OT_image_select_polyline(wmOperatorType *ot);
void PAINT_OT_image_select_invert(wmOperatorType *ot);
void PAINT_OT_image_select_move(wmOperatorType *ot);
void PAINT_OT_image_select_move_confirm(wmOperatorType *ot);
void PAINT_OT_image_select_move_cancel(wmOperatorType *ot);
void PAINT_OT_image_select_move_undo_step(wmOperatorType *ot);
void PAINT_OT_image_select_copy(wmOperatorType *ot);
void PAINT_OT_image_select_paste(wmOperatorType *ot);
void PAINT_OT_image_select_transform(wmOperatorType *ot);
void PAINT_OT_image_select_transform_confirm(wmOperatorType *ot);
void PAINT_OT_image_select_transform_cancel(wmOperatorType *ot);
void PAINT_OT_image_select_transform_drag(wmOperatorType *ot);
void PAINT_OT_image_select_gradient(wmOperatorType *ot);
void PAINT_OT_image_select_gradient_apply(wmOperatorType *ot);
void PAINT_OT_image_select_gradient_cancel(wmOperatorType *ot);
void PAINT_OT_image_select_warp(wmOperatorType *ot);
void PAINT_OT_image_select_warp_confirm(wmOperatorType *ot);
void PAINT_OT_image_select_warp_cancel(wmOperatorType *ot);
void PAINT_OT_image_select_warp_undo_step(wmOperatorType *ot);
/** True while a selection transform gizmo is active in the current Image Editor. */
bool image_select_transform_is_floating(bContext *C);
/** True while a move-selection fragment is floating in the current Image Editor. */
bool image_select_move_is_floating(bContext *C);
/** Floating move in the given Image Editor space (not another window/space). */
bool image_select_move_is_floating_in_space(const SpaceImage *sima);
bool image_select_gradient_is_floating(bContext *C);
bool image_select_gradient_is_floating_in_space(const SpaceImage *sima);
/** True while a warp-selection fragment is floating in the current Image Editor. */
bool image_select_warp_is_floating(bContext *C);
bool image_select_warp_is_floating_in_space(const SpaceImage *sima);
/** True when any floating paint-select session owns the current Image Editor. */
bool image_select_session_is_floating(bContext *C);
/**
 * True when the brush must not start a stroke in the current Image Editor: either this editor
 * itself owns a live floating paint-select session, or its Image's canvas is currently borrowed
 * by a floating session in a *different* Image Editor (see
 * #bke::ImageRuntime::paint_selection_borrowed_by). Unlike #image_select_session_is_floating,
 * this is not meant to gate keymap activation -- an editor merely sharing a borrowed Image has no
 * session of its own to confirm/cancel.
 */
bool image_select_canvas_paint_blocked(bContext *C);
void image_paint_clipboard_ensure_atexit_handler();
/**
 * Modal keymap shared by the floating-selection operators (move / transform / warp).
 * Defined in mesh/paint_image_select_floating.cc; registered from #ED_keymap_paint.
 */
wmKeyMap *image_select_floating_modal_keymap(wmKeyConfig *keyconf);

/* paint_image_2d_curve_mask.cc */

/**
 * \brief Caching structure for curve mask.
 *
 * When 2d painting images the curve mask is used as an input.
 */
struct CurveMaskCache {
  /**
   * \brief Last #CurveMapping.changed_timestamp being read.
   *
   * When different the input cache needs to be recalculated.
   */
  int last_curve_timestamp;

  /**
   * \brief sampled version of the brush curve-mapping.
   */
  float *sampled_curve;

  /**
   * \brief Size in bytes of the curve_mask field.
   *
   * Used to determine if the curve_mask needs to be re-allocated.
   */
  size_t curve_mask_size;

  /**
   * \brief Curve mask that can be passed as curve_mask parameter when.
   */
  ushort *curve_mask;
};

void paint_curve_mask_cache_free_data(CurveMaskCache *curve_mask_cache);
void paint_curve_mask_cache_update(CurveMaskCache *curve_mask_cache,
                                   const Brush *brush,
                                   int diameter,
                                   float radius,
                                   const float cursor_position[2]);
/** Copy the rasterized mask pixels from \a src into \a dst, reallocating \a dst if needed. */
void paint_curve_mask_cache_copy(CurveMaskCache *dst, const CurveMaskCache *src);

/* `sculpt_uv.cc` */

void SCULPT_OT_uv_sculpt_grab(wmOperatorType *ot);
void SCULPT_OT_uv_sculpt_relax(wmOperatorType *ot);
void SCULPT_OT_uv_sculpt_pinch(wmOperatorType *ot);

/* paint_utils.cc */

/**
 * Convert the object-space axis-aligned bounding box (expressed as
 * its minimum and maximum corners) into a screen-space rectangle,
 * returns zero if the result is empty.
 */
bool paint_convert_bb_to_rect(rcti *rect,
                              const float bb_min[3],
                              const float bb_max[3],
                              const ARegion &region,
                              const RegionView3D &rv3d,
                              const Object &ob);

float paint_calc_object_space_radius(const ViewContext &vc,
                                     const float3 &center,
                                     float pixel_radius);

/**
 * Returns true when a color was sampled and false when a value was sampled.
 */
bool paint_get_tex_pixel(const MTex *mtex,
                         float u,
                         float v,
                         ImagePool *pool,
                         int thread,
                         float *r_intensity,
                         float r_rgba[4]);

void paint_stroke_operator_properties(wmOperatorType *ot);

void PAINT_OT_face_select_linked(wmOperatorType *ot);
void PAINT_OT_face_select_linked_pick(wmOperatorType *ot);
void PAINT_OT_face_select_all(wmOperatorType *ot);
void PAINT_OT_face_select_more(wmOperatorType *ot);
void PAINT_OT_face_select_less(wmOperatorType *ot);
void PAINT_OT_face_select_hide(wmOperatorType *ot);
void PAINT_OT_face_select_loop(wmOperatorType *ot);

void PAINT_OT_face_vert_reveal(wmOperatorType *ot);

void PAINT_OT_vert_select_all(wmOperatorType *ot);
void PAINT_OT_vert_select_ungrouped(wmOperatorType *ot);
void PAINT_OT_vert_select_hide(wmOperatorType *ot);
void PAINT_OT_vert_select_linked(wmOperatorType *ot);
void PAINT_OT_vert_select_linked_pick(wmOperatorType *ot);
void PAINT_OT_vert_select_more(wmOperatorType *ot);
void PAINT_OT_vert_select_less(wmOperatorType *ot);
void PAINT_OT_vert_select_loop(wmOperatorType *ot);

bool vert_paint_poll(bContext *C);
bool mask_paint_poll(bContext *C);
bool paint_curve_poll(bContext *C);

bool facemask_paint_poll(bContext *C);

namespace ed::sculpt_paint {

/**
 * Determines whether a given symmetry pass is valid.
 *
 * Uses the #ePaintSymmetryFlags enum.
 *
 * symm is a bit combination of XYZ.
 * 1 is X; 2 is Y; 3 is XY; 4 is Z; 5 is XZ; 6 is YZ; 7 is XYZ
 */
inline bool is_symmetry_iteration_valid(const char i, const char symm)
{
  return i == 0 || (symm & i && (symm != 5 || i != 3) && (symm != 6 || !ELEM(i, 3, 5)));
}

inline float3 symmetry_flip(const float3 &src, const ePaintSymmetryFlags symm)
{
  float3 dst;
  if (symm & PAINT_SYMM_X) {
    dst.x = -src.x;
  }
  else {
    dst.x = src.x;
  }
  if (symm & PAINT_SYMM_Y) {
    dst.y = -src.y;
  }
  else {
    dst.y = src.y;
  }
  if (symm & PAINT_SYMM_Z) {
    dst.z = -src.z;
  }
  else {
    dst.z = src.z;
  }
  return dst;
}

}  // namespace ed::sculpt_paint

/* image painting blur kernel */
struct BlurKernel {
  float *wdata;     /* actual kernel */
  int side;         /* kernel side */
  int side_squared; /* data side */
  int pixel_len;    /* pixels around center that kernel is wide */
};

/**
 * Paint blur kernels. Projective painting enforces use of a 2x2 kernel due to lagging.
 * Can be extended to other blur kernels later,
 */
BlurKernel *paint_new_blur_kernel(Brush *br, bool proj);
void paint_delete_blur_kernel(BlurKernel *);

/** Initialize viewport pivot from evaluated bounding box center of `ob`. */
void paint_init_pivot(Object *ob, Scene *scene, Paint *paint);

/* palette.cc */

void PALETTE_OT_new(wmOperatorType *ot);
void PALETTE_OT_color_add(wmOperatorType *ot);
void PALETTE_OT_color_delete(wmOperatorType *ot);

void PALETTE_OT_extract_from_image(wmOperatorType *ot);
void PALETTE_OT_sort(wmOperatorType *ot);
void PALETTE_OT_color_move(wmOperatorType *ot);
void PALETTE_OT_join(wmOperatorType *ot);

}  // namespace blender
