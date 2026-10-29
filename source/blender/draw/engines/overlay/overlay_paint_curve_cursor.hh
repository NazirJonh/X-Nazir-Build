/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 *
 * Draws paint-curve handles, silhouettes, and radius handles via the Overlay engine
 * so they persist when the mouse leaves the viewport (fixing the header-hover bug).
 */

#pragma once

#include "ED_curve_patch.hh"
#include "ED_paint_curve_draw.hh"
#include "ED_shape_session_overlay.hh"
#include "ED_view3d.hh"

#include "BKE_brush.hh"
#include "BKE_curves.hh"
#include "BKE_paint.hh"
#include "DEG_depsgraph_query.hh"
#include "DNA_brush_types.h"
#include "DNA_curve_types.h"
#include "DNA_curves_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"

#include "GPU_batch.hh"
#include "GPU_shader.hh"
#include "GPU_vertex_buffer.hh"
#include "GPU_vertex_format.hh"

#include "UI_resources.hh"

#include "BLI_hash_mm2a.hh"
#include "BLI_math_angle_types.hh"
#include "BLI_math_base.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_time.h"

#include "overlay_base.hh"

#include <cmath>
#include <optional>

/* Opt-in measurement for the per-frame batch churn. Handle/point/radius GPU batches are reused
 * across redraws while screen-space geometry is unchanged (hover/selection only retints them);
 * insert-preview and snap-marker batches still rebuild every sync. Scene-curve silhouettes are
 * keyed on #silhouette_batches_key_. Must stay 0 outside a measurement pass: the report writes to
 * stdout on every overlay redraw. Grep `DEBUG-pccursor` to find every touch point. */
#define PAINT_CURVE_CURSOR_PROFILING 0

#if PAINT_CURVE_CURSOR_PROFILING
#  include "BLI_time.h"
#  include <cstdio>
#endif

namespace blender::draw::overlay {

/**
 * Draws the paint-curve handles/silhouettes in the Overlay engine.
 * Replaces (and eventually removes) the WM paint-cursor path that failed
 * to draw when the mouse was outside the 3D viewport.
 */
class PaintCurveCursor : Overlay {
 private:
  PassSimple ps_ = {"PaintCurveCursor"};
  bool enabled_ = false;

  ed::sculpt_paint::PaintCurveScreenHandles handles_;
  ed::sculpt_paint::PaintCurveScreenSilhouettes silhouettes_;
  /** The live control-point session of this region, when it has one: a Vector shape session
   * (outline, origin and handles) or the modal Bézier outline input (handles only). It takes the
   * whole pass: the user is editing that outline, and the paint-curve overlay would draw a
   * second set of points over it. */
  ed::sculpt_paint::shape_overlay::SessionOverlay shape_overlay_;
  bool session_live_ = false;
  /** The animated dashed outline of a shape session, and its origin marker. Separate passes
   * because they use their own shaders; submitted around the shared handle pass to keep the
   * dash under the points. */
  PassSimple dash_ps_ = {"PaintShapeDash"};
  PassSimple marker_ps_ = {"PaintShapeOrigin"};
  /** Reused across syncs; #ED_curve_patch_overlay_data_get clears and refills it.
   * Cleared at the start of every #begin_sync so an early return cannot leave last-frame
   * spline pointers (dangling if the session died between frames). */
  CurvePatchOverlayData patch_overlay_;

  Vector<ed::sculpt_paint::PaintCurveCachedObjectSilhouette> silhouette_cache_;
  uint64_t silhouette_cache_key_ = 0;

  /** Line-only silhouettes of the material's Curve Patch effects (the Curve Edit tool in the
   * Paint display group), cached the way the scene-curve silhouettes above are: the polylines
   * per effect live in #stack_silhouette_cache_ and the persistent GPU batches parallel it, so
   * plain mouse-moves only re-resolve the hover. The faint and hover passes address both arrays
   * by index. */
  Vector<ed::sculpt_paint::PaintCurveStackSilhouette> stack_silhouette_cache_;
  uint64_t stack_silhouette_cache_key_ = 0;
  /* TODO: this mirrors the scene-curve cache + #CachedSilhouetteBatches pair above. They hold
   * different entry types (object vs effect) and are freed/indexed differently, so a shared
   * template would hide more than it saves until a third user appears. */
  Vector<Vector<gpu::Batch *>> stack_silhouette_batches_;
  uint64_t stack_silhouette_batches_key_ = 0;
  /** This sync's passes: the cache indices drawn faint, and the hovered effect (-1 when none). */
  Vector<int> stack_silhouette_faint_;
  int stack_hover_index_ = -1;

  /** Ephemeral GPU batches (insert preview, snap marker). Freed at the start of the next
   * begin_sync. Handle/segment/radius batches live in #handle_gpu_ instead. */
  Vector<gpu::Batch *> batches_;

  /** Persistent GPU batches for scene-curve silhouettes, keyed on #silhouette_cache_key_.
   * Rebuilt only when the projection cache changes (view/scene/geometry edit), not on every
   * mouse-move like #batches_. The faint and hover passes look these up by object pointer. */
  struct CachedSilhouetteBatches {
    const Object *object;
    Vector<gpu::Batch *> batches;
  };
  Vector<CachedSilhouetteBatches> silhouette_batches_;
  uint64_t silhouette_batches_key_ = 0;

  /** Persistent GPU batches of the shape session's dashed outline, keyed on the outline
   * geometry. The dash moves only when a point is edited; the phase advances in the shader. */
  Vector<gpu::Batch *> dash_batches_;
  uint64_t dash_batches_key_ = 0;
  bool dash_batches_valid_ = false;

  /**
   * Persistent GPU batches for handles/segments/radius, rebuilt only when screen-space geometry
   * changes. Hover and selection retint these batches; they do not move vertices. Insert preview
   * and the snap marker stay in #batches_ because they track the cursor.
   */
  struct HandleGpuCache {
    Vector<gpu::Batch *> segments;
    Vector<gpu::Batch *> point_underlay;
    Vector<gpu::Batch *> point_left;
    Vector<gpu::Batch *> point_right;
    Vector<gpu::Batch *> point_diamond;
    Vector<gpu::Batch *> point_end_left;
    Vector<gpu::Batch *> point_end_right;
    Vector<gpu::Batch *> radius_line;
    Vector<gpu::Batch *> radius_circle;
    /* Per-point strength handles (Curve Patch POINTS): the fixed track, the filled stretch from
     * the base to the knob, and the knob circle. */
    Vector<gpu::Batch *> strength_track;
    Vector<gpu::Batch *> strength_fill;
    Vector<gpu::Batch *> strength_circle;
    /* Per-point rotation handles (Curve Patch POINTS): the 180-degree arc, its arrowhead (two
     * line batches per handle) and the knob circle. */
    Vector<gpu::Batch *> angle_arc;
    Vector<gpu::Batch *> angle_arrow;
    Vector<gpu::Batch *> angle_circle;
  };
  HandleGpuCache handle_gpu_;
  /** Murmur2A of handle geometry. The field is 64-bit for storage, but #BLI_hash_mm2a_end is
   * 32-bit; a collision would keep stale batches until the next real geometry change. Same trade
   * as #silhouette_batches_key_. #handle_gpu_valid_ distinguishes "hashed to 0" from "empty". */
  uint64_t handle_gpu_key_ = 0;
  bool handle_gpu_valid_ = false;

  /** Region-space snap marker shown during a 3D paint-curve slide (set by the slide modal). */
  bool snap_marker_active_ = false;
  float2 snap_marker_pos_ = float2(0.0f);
  int snap_marker_type_ = 0;

 public:
  PaintCurveCursor() = default;
  ~PaintCurveCursor()
  {
    free_batches();
    free_silhouette_batches();
    free_stack_silhouette_batches();
    free_handle_gpu();
    free_dash_batches();
  }

  void begin_sync(Resources &res, const State &state) final
  {
#if PAINT_CURVE_CURSOR_PROFILING
    const double prof_t0 = BLI_time_now_seconds(); /* DEBUG-pccursor */
#endif
    free_batches();
    handles_ = {};
    silhouettes_ = {};
    stack_silhouette_faint_.clear();
    stack_hover_index_ = -1;
    enabled_ = false;
    snap_marker_active_ = false;
    /* Drop last-frame spline pointers before any early return. The only reader is later in
     * this function, after #ED_curve_patch_overlay_data_get refills the buffer. */
    patch_overlay_.splines.clear();
    patch_overlay_.active_index = -1;

    if (!state.is_space_v3d() && !state.is_space_image()) {
      free_handle_gpu();
      free_dash_batches();
      return;
    }
    if (state.hide_overlays || state.is_depth_only_drawing || state.is_material_select) {
      free_handle_gpu();
      free_dash_batches();
      return;
    }
    if (state.region == nullptr || state.depsgraph == nullptr || state.scene == nullptr ||
        state.view_layer == nullptr)
    {
      free_handle_gpu();
      free_dash_batches();
      return;
    }

    Scene *scene = const_cast<Scene *>(state.scene);
    ViewLayer *view_layer = const_cast<ViewLayer *>(state.view_layer);

    const int2 origin(state.region->winrct.xmin, state.region->winrct.ymin);
    const float2 mval_region = state.cursor_mval_valid ? float2(state.cursor_mval - origin) :
                                                         float2(-1.0e6f);

    /* A live control-point session is what the user is editing in this region: it takes the
     * whole pass, so the paint-curve overlay cannot draw a second set of points over it. It
     * uses the same control-point drawing and hit rules, so every system looks and behaves
     * identically. Which source answers (modal Bézier input, Image Editor or 3D Vector session)
     * is decided in one place, #session_overlay_get. */
    ed::sculpt_paint::shape_overlay::SessionQuery query;
    query.region = state.region;
    query.space_data = state.space_data;
    query.active_object = BKE_view_layer_active_object_get(view_layer);
    query.is_space_image = state.is_space_image();
    query.is_space_v3d = state.is_space_v3d();
    query.cursor = mval_region;
    query.cursor_valid = state.cursor_mval_valid;
    session_live_ = ed::sculpt_paint::shape_overlay::session_overlay_get(query, shape_overlay_);
    if (session_live_) {
      enabled_ = true;
      handles_ = std::move(shape_overlay_.handles);
      fill_pass_from_pod(res, state);
      return;
    }
    free_dash_batches();

    Paint *paint = ed::sculpt_paint::ED_paint_curve_resolve_active_paint(
        state.depsgraph, scene, view_layer, state.space_data, state.space_type);
    const Brush *brush = paint ? BKE_paint_brush_for_read(paint) : nullptr;
    if (!ed::sculpt_paint::ED_paint_curve_overlay_is_relevant(
            brush, state.active_tool_idname, state.is_space_v3d(), state.is_space_image()))
    {
      free_handle_gpu();
      return;
    }

    enabled_ = true;
    const bool is_curves_edit = ed::sculpt_paint::ED_paint_curve_is_curves_edit_tool(
        state.active_tool_idname);

    ViewContext vc = ed::sculpt_paint::ED_paint_curve_viewcontext_from_state(
        state.depsgraph,
        scene,
        view_layer,
        const_cast<ARegion *>(state.region),
        const_cast<View3D *>(state.v3d),
        const_cast<RegionView3D *>(state.rv3d));

    const Sculpt *sculpt = (scene->toolsettings) ? scene->toolsettings->sculpt : nullptr;
    /* Resolve the source object from the original scene's tool settings and map it to its
     * evaluated copy. `state.scene` is the evaluated scene, whose `paint_curve_source_object`
     * pointer does not match the evaluated objects iterated during silhouette building. Without
     * this the source-object exclusion fails and the curve being edited gets a hover highlight. */
    const Object *source_object = nullptr;
    {
      const Scene *scene_orig = DEG_get_original(state.scene);
      const Sculpt *sculpt_orig = (scene_orig && scene_orig->toolsettings) ?
                                      scene_orig->toolsettings->sculpt :
                                      nullptr;
      Object *src_orig = sculpt_orig ? sculpt_orig->paint_curve_source_object : nullptr;
      if (src_orig) {
        source_object = DEG_get_evaluated(state.depsgraph, src_orig);
      }
    }

    const bool effects_silhouettes = this->sync_stack_silhouettes(
        state, vc, mval_region, is_curves_edit);
    /* Non-empty overlay, not "session has an active item". A half-built session
     * (`active_patch` out of range with patches still present) still draws every spline;
     * hover and insert preview stay off because `active_index` is -1. The previous gate
     * (`has_active_item()`) hid the overlay entirely in that degenerate case. */
    const bool is_curve_patch_active = !patch_overlay_.splines.is_empty();

    const ed::sculpt_paint::PaintCurveOverlayModes modes =
        ed::sculpt_paint::ED_paint_curve_overlay_modes_get(brush,
                                                           state.active_tool_idname,
                                                           is_curve_patch_active,
                                                           state.cursor_mval_valid,
                                                           state.cursor_ctrl_pressed);
    const bool compute_hover = modes.compute_hover;
    const bool show_insert_preview = modes.show_insert_preview;

    /* Live 2D Curve Patch session in the Image Editor. Its canonical curve is stored in image UV,
     * so it is projected into region pixels first and then drawn through the regular screen-space
     * (`use_3d_space == false`) path. This branch takes priority: while a 2D session is alive it
     * is what the user is editing, whatever the brush's own paint curve says. */
    bool image_patch_drawn = false;
    if (state.is_space_image()) {
      bke::CurvesGeometry image_patch_geometry;
      if (ed::sculpt_paint::ED_image_curve_patch_overlay_geometry_get(state.region,
                                                                      image_patch_geometry))
      {
        /* Radius handles follow the same Sculpt-side preference the 3D editor uses
         * (`paint_curve_show_radius_handles` / `paint_curve_radius_display_mode`), so the two
         * editors stay one setting rather than two -- except in the POINTS stamp layout, where
         * the brush's own per-point toggles decide (see the visibility helper). Ctrl shows the
         * insert preview, matching Ctrl+RMB in the modal. */
        const ed::sculpt_paint::CurvePatchHandleVisibility patch_visibility =
            ed::sculpt_paint::ED_curve_patch_handle_visibility_get(brush);
        const bool show_radius_handles = patch_visibility.radius_regular ||
                                         patch_visibility.radius_all_points;
        ed::sculpt_paint::ED_paint_curve_screen_handles_build_from_geometry(
            vc,
            image_patch_geometry,
            /*use_3d_space=*/false,
            sculpt,
            /*show_radius_handles=*/show_radius_handles,
            mval_region,
            /*compute_segment_hover=*/state.cursor_mval_valid,
            /*show_insert_preview=*/state.cursor_mval_valid && state.cursor_ctrl_pressed,
            handles_,
            patch_visibility);
        image_patch_drawn = true;
      }
    }

    /* Which of the three branches below produced `handles_`: the Curve Patch ones get a wire color
     * of their own (see the override after the chain). */
    bool drawing_curve_patch = image_patch_drawn;

    if (image_patch_drawn) {
      /* Nothing further: the session curve is the only paint curve that matters right now. */
    }
    else if (effects_silhouettes) {
      /* Already built above, before the affordance gate: the effect silhouettes are this state's
       * whole overlay, and neither the handle pass nor the brush curve may run over them. */
    }
    else if (is_curve_patch_active && state.is_space_v3d()) {
      drawing_curve_patch = true;
      /* View3D only: these control curves are in the sculpt object's own space, and projecting
       * them needs the region's #RegionView3D. The Image Editor resolves the same active object
       * and would otherwise reach this branch whenever a 3D patch is live in another area, with
       * no `RegionView3D` to project through.
       *
       * Draw all patches. Only the active patch shows hover highlights and insert preview.
       * Non-active patches are drawn plain (no hover, no insert preview) so the user can
       * still see and click them. */
      handles_.points.clear();
      handles_.radius_handles.clear();
      handles_.strength_handles.clear();
      handles_.angle_handles.clear();
      handles_.segments.clear();
      handles_.insert_preview = {};

      const ed::sculpt_paint::CurvePatchHandleVisibility patch_visibility =
          ed::sculpt_paint::ED_curve_patch_handle_visibility_get(brush);
      const bool show_radius_handles = patch_visibility.radius_regular ||
                                       patch_visibility.radius_all_points;
      for (const int i : patch_overlay_.splines.index_range()) {
        const bke::CurvesGeometry *control_curve = patch_overlay_.splines[i];
        if (control_curve == nullptr) {
          continue;
        }
        const bool is_active = (i == patch_overlay_.active_index);
        ed::sculpt_paint::PaintCurveScreenHandles tmp_handles;
        ed::sculpt_paint::ED_paint_curve_screen_handles_build_from_geometry(
            vc,
            *control_curve,
            true,
            sculpt,
            show_radius_handles,
            mval_region,
            is_active ? compute_hover : false,
            is_active ? show_insert_preview : false,
            tmp_handles,
            patch_visibility);
        handles_.points.extend(tmp_handles.points);
        handles_.radius_handles.extend(tmp_handles.radius_handles);
        handles_.strength_handles.extend(tmp_handles.strength_handles);
        handles_.angle_handles.extend(tmp_handles.angle_handles);
        handles_.segments.extend(tmp_handles.segments);
        if (is_active) {
          handles_.insert_preview = tmp_handles.insert_preview;
        }
      }
    }
    else if (ed::sculpt_paint::ED_paint_curve_overlay_draws_brush_curve(
                 brush, state.active_tool_idname, state.is_space_v3d()))
    {
      /* Curve Edit uses the same paint-curve handle overlay as Stroke Method: Curve so Ctrl+RMB
       * point creation and handle drags are visible while editing (silhouettes alone are not
       * enough). */
      ed::sculpt_paint::ED_paint_curve_screen_handles_build(
          vc, *brush, sculpt, mval_region, compute_hover, show_insert_preview, handles_);
      /* Same near-black-canvas problem as Curve Patch below, and the same fix: only in the Image
       * Editor, where this is Stroke Method: Curve painting on arbitrary (often dark) image
       * content. In the 3D viewport this branch is Curve Edit or a 3D paint curve drawn over the
       * scene, which follows the theme like any other viewport overlay and was not asked to
       * change. */
      if (state.is_space_image()) {
        drawing_curve_patch = true;
      }
    }

    /* A Curve Patch is edited ON TOP of arbitrary image content -- a texture channel is very often
     * near-black -- where the builder's `TH_WIRE` (a dark grey) all but disappears. White reads on
     * anything the canvas can show, and the black half-alpha outline the builder already puts
     * behind every segment keeps it legible on white content too.
     *
     * Overridden here rather than in the builder because the builder also serves the ordinary
     * brush paint-curve overlay, which is drawn over the viewport, follows the theme, and was not
     * asked to change. Only the wire is touched: control points, handles and radius handles stay
     * on their theme colors, and the hover highlight is a separate pass drawn on top of this one.
     */
    if (drawing_curve_patch) {
      for (ed::sculpt_paint::PaintCurveSegmentDrawData &seg : handles_.segments) {
        seg.wire_color = float4(1.0f, 1.0f, 1.0f, seg.wire_color.w);
      }
    }

    if (is_curves_edit && !state.sculpt_tool_display_paint && state.is_space_v3d()) {
      /* Sculpt display group only: the scene's curve objects are what this tool picks into the
       * brush there. In the Paint display group the stack silhouettes above are the whole story,
       * and the scene curves would be noise around them. */
      const uint64_t key = ed::sculpt_paint::ED_paint_curve_silhouette_cache_key_hash(vc);
      ed::sculpt_paint::ED_paint_curve_screen_silhouettes_build_cached(vc,
                                                                       mval_region,
                                                                       source_object,
                                                                       compute_hover,
                                                                       key,
                                                                       silhouette_cache_,
                                                                       silhouette_cache_key_,
                                                                       silhouettes_);

      /* Rebuild the persistent silhouette GPU batches only when the projection cache changed
       * (view rotation, region resize, scene/geometry edit). On plain mouse-moves the key is
       * unchanged, so the batches are reused instead of being uploaded again every frame. */
      if (silhouette_cache_key_ != silhouette_batches_key_) {
        free_silhouette_batches();
        for (const ed::sculpt_paint::PaintCurveCachedObjectSilhouette &entry : silhouette_cache_) {
          CachedSilhouetteBatches cached;
          cached.object = entry.object;
          for (const Vector<float2> &polyline : entry.polylines) {
            make_line_strip_owned(cached.batches, polyline);
          }
          silhouette_batches_.append(std::move(cached));
        }
        silhouette_batches_key_ = silhouette_cache_key_;
      }
    }

    /* Snap marker: shown while a 3D slide or Curve Patch point drag is snapping to geometry.
     * World position is stored by the modal; project it into each viewport's region space here. */
    if (state.is_space_v3d()) {
      float world_pos[3];
      int type = 0;
      if (ed::sculpt_paint::ED_paint_curve_snap_marker_get(world_pos, &type) && vc.region) {
        float screen[2];
        ED_view3d_project_v2(vc.region, world_pos, screen);
        if (isfinite(screen[0]) && isfinite(screen[1])) {
          snap_marker_active_ = true;
          snap_marker_pos_ = float2(screen[0], screen[1]);
          snap_marker_type_ = type;
        }
      }
    }

#if PAINT_CURVE_CURSOR_PROFILING
    const double prof_t_build = BLI_time_now_seconds(); /* DEBUG-pccursor */
#endif

    fill_pass_from_pod(res, state);

#if PAINT_CURVE_CURSOR_PROFILING
    /* DEBUG-pccursor: `screen` is the CPU-side projection. `ephemeral` is insert-preview /
     * snap-marker batches rebuilt every sync; `cached` is the handle/segment/radius GPU cache,
     * which this measurement exists to judge. A cache only pays off if the GPU upload dominated.
     */
    auto owned_count = [](const Vector<gpu::Batch *> &v) {
      int n = 0;
      for (gpu::Batch *b : v) {
        if (b) {
          n++;
        }
      }
      return n;
    };
    const int cached_batches = owned_count(handle_gpu_.segments) +
                               owned_count(handle_gpu_.point_underlay) +
                               owned_count(handle_gpu_.point_left) +
                               owned_count(handle_gpu_.point_right) +
                               owned_count(handle_gpu_.point_diamond) +
                               owned_count(handle_gpu_.point_end_left) +
                               owned_count(handle_gpu_.point_end_right) +
                               owned_count(handle_gpu_.radius_line) +
                               owned_count(handle_gpu_.radius_circle);
    const double prof_t_end = BLI_time_now_seconds();
    printf(
        "[DEBUG-pccursor] total=%.3fms | screen=%.3f batches=%.3f | points=%d ephemeral=%d "
        "cached=%d\n",
        (prof_t_end - prof_t0) * 1000.0,
        (prof_t_build - prof_t0) * 1000.0,
        (prof_t_end - prof_t_build) * 1000.0,
        int(handles_.points.size()),
        int(batches_.size()),
        cached_batches);
    fflush(stdout);
#endif
  }

  void draw_output(Framebuffer &framebuffer, Manager &manager, View & /*view*/) final
  {
    if (!enabled_) {
      return;
    }
    GPU_framebuffer_bind(framebuffer);
    if (session_live_) {
      /* The dash goes under the points, the origin marker over them: the order the session's
       * own region callback drew in. */
      manager.submit(dash_ps_);
    }
    manager.submit(ps_);
    if (session_live_) {
      manager.submit(marker_ps_);
    }
  }

 private:
  void free_batches()
  {
    for (gpu::Batch *batch : batches_) {
      GPU_batch_discard(batch);
    }
    batches_.clear();
  }

  void free_silhouette_batches()
  {
    for (CachedSilhouetteBatches &entry : silhouette_batches_) {
      for (gpu::Batch *batch : entry.batches) {
        GPU_batch_discard(batch);
      }
    }
    silhouette_batches_.clear();
    silhouette_batches_key_ = 0;
  }

  /**
   * Build (or reuse) the Curve Patch effect silhouettes for the Curve Edit tool in the Paint
   * display group, and fill #patch_overlay_ with the passive stack preview when they are off.
   * Returns whether the effect silhouettes are this state's whole overlay.
   */
  bool sync_stack_silhouettes(const State &state,
                              const ViewContext &vc,
                              const float2 mval_region,
                              const bool is_curves_edit)
  {
    ED_curve_patch_overlay_data_get(vc.obact, patch_overlay_);
    bool effects_silhouettes = false;
    /* A live Curve Patch session does not gate this: with the Curve Edit tool in the Paint display
     * group the user is choosing among ALL effects, also while one of them is being edited. The
     * session ends (and commits) on the tool switch, so its container data is current by the time
     * the pick runs. */
    if (is_curves_edit && state.sculpt_tool_display_paint && state.is_space_v3d()) {
      /* The silhouettes ARE the overlay in this state -- lines only, drawn for every SELECTED
       * object's layered material (multi-select included) and never for the scene's curve
       * objects, which belong to the Sculpt display group's workflow below. Their being
       * non-empty is the gate: no handle pass runs then, and the brush-curve branch is blocked
       * in the chain below. Built here rather than in the chain, because the gate has to be
       * known before the affordance modes are asked. */
      const uint64_t key = ed::sculpt_paint::ED_paint_curve_stack_silhouette_cache_key_hash(vc);
      const std::optional<float2> hover_mval = state.cursor_mval_valid ?
                                                  std::optional<float2>(mval_region) :
                                                  std::nullopt;
      ed::sculpt_paint::ED_paint_curve_stack_silhouettes_build_cached(vc,
                                                                      hover_mval,
                                                                      key,
                                                                      stack_silhouette_cache_,
                                                                      stack_silhouette_cache_key_,
                                                                      stack_silhouette_faint_,
                                                                      stack_hover_index_);
      effects_silhouettes = !stack_silhouette_cache_.is_empty();
      if (effects_silhouettes) {
        /* The silhouettes are the whole overlay: no handle pass over the session's own curves. */
        patch_overlay_.splines.clear();
        patch_overlay_.active_index = -1;
      }

      /* Rebuild the persistent GPU batches only when the projection cache changed (view rotation,
       * region resize, selection or effect edit). On plain mouse-moves the key is unchanged, so
       * the batches are reused instead of being uploaded again every frame. */
      if (effects_silhouettes && stack_silhouette_cache_key_ != stack_silhouette_batches_key_) {
        free_stack_silhouette_batches();
        stack_silhouette_batches_.reserve(stack_silhouette_cache_.size());
        for (const ed::sculpt_paint::PaintCurveStackSilhouette &entry : stack_silhouette_cache_) {
          Vector<gpu::Batch *> batches;
          for (const Vector<float2> &polyline : entry.polylines) {
            make_line_strip_owned(batches, polyline);
          }
          stack_silhouette_batches_.append(std::move(batches));
        }
        stack_silhouette_batches_key_ = stack_silhouette_cache_key_;
      }
    }
    if (!effects_silhouettes && patch_overlay_.splines.is_empty() &&
        state.sculpt_tool_display_paint)
    {
      /* No live session on the object: the passive preview of the active Curve Patch stack row
       * (if the object's active material has one) is what the overlay shows. Paint display group
       * only: in the Sculpt group the effect's curves are neither this tool's subject nor
       * pickable, so drawing them there would invite interaction the tool cannot deliver. */
      ED_curve_patch_overlay_data_get_stack(vc.obact, patch_overlay_);
    }
    return effects_silhouettes;
  }

  void free_stack_silhouette_batches()
  {
    for (Vector<gpu::Batch *> &batches : stack_silhouette_batches_) {
      for (gpu::Batch *batch : batches) {
        if (batch != nullptr) {
          GPU_batch_discard(batch);
        }
      }
    }
    stack_silhouette_batches_.clear();
    stack_silhouette_batches_key_ = 0;
  }

  void free_dash_batches()
  {
    for (gpu::Batch *batch : dash_batches_) {
      if (batch != nullptr) {
        GPU_batch_discard(batch);
      }
    }
    dash_batches_.clear();
    dash_batches_key_ = 0;
    dash_batches_valid_ = false;
  }

  /** Geometry-only key of the shape session's dashed outline. The dash phase is a push
   * constant, so it does not invalidate the batches. */
  uint64_t hash_dash_geometry() const
  {
    BLI_HashMurmur2A mm2;
    BLI_hash_mm2a_init(&mm2, 0);
    BLI_hash_mm2a_add_int(&mm2, int(shape_overlay_.outlines.size()));
    for (const Vector<float2> &polyline : shape_overlay_.outlines) {
      BLI_hash_mm2a_add_int(&mm2, int(polyline.size()));
      if (!polyline.is_empty()) {
        BLI_hash_mm2a_add(&mm2,
                          reinterpret_cast<const unsigned char *>(polyline.data()),
                          sizeof(float2) * polyline.size());
      }
    }
    return uint64_t(BLI_hash_mm2a_end(&mm2));
  }

  void ensure_dash_batches()
  {
    const uint64_t key = hash_dash_geometry();
    if (dash_batches_valid_ && key == dash_batches_key_) {
      return;
    }
    free_dash_batches();
    dash_batches_.reserve(shape_overlay_.outlines.size());
    for (const int i : shape_overlay_.outlines.index_range()) {
      const bool cyclic = i < shape_overlay_.outline_cyclic.size() &&
                          shape_overlay_.outline_cyclic[i];
      make_dash_batch(shape_overlay_.outlines[i], cyclic);
    }
    dash_batches_key_ = key;
    dash_batches_valid_ = true;
  }

  /** A line strip in the float3 layout the dashed shader expects (`pos` is a vec3 there;
   * the polyline shader used for the handles takes a vec2). */
  gpu::Batch *make_dash_batch(Span<float2> verts, const bool cyclic)
  {
    if (verts.size() < 2) {
      dash_batches_.append(nullptr);
      return nullptr;
    }
    Vector<float3> verts3;
    verts3.reserve(verts.size() + (cyclic ? 1 : 0));
    for (const float2 &v : verts) {
      verts3.append(float3(v.x, v.y, 0.0f));
    }
    if (cyclic) {
      verts3.append(float3(verts[0].x, verts[0].y, 0.0f));
    }
    GPUVertFormat format = {};
    GPU_vertformat_attr_add(&format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
    gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, uint(verts3.size()));
    GPU_vertbuf_attr_fill(vbo, 0, verts3.data());
    gpu::Batch *batch = GPU_batch_create_ex(GPU_PRIM_LINE_STRIP, vbo, nullptr, GPU_BATCH_OWNS_VBO);
    dash_batches_.append(batch);
    return batch;
  }

  /** The animated dashed outline of the shape session: the same "marching ants" shader and
   * parameters the session's own region callback used, now drawn under the shared points. */
  void fill_dash_pass(const State &state)
  {
    dash_ps_.init();
    dash_ps_.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND_ALPHA);
    dash_ps_.shader_set(
        GPU_shader_get_builtin_shader(GPU_SHADER_3D_LINE_DASHED_UNIFORM_COLOR_ANIMATED));
    dash_ps_.push_constant("ModelViewProjectionMatrix", paint_curve_ortho_mvp(state));
    dash_ps_.push_constant("viewport_size", float2(state.region->winx, state.region->winy));
    dash_ps_.push_constant("colors_len", 2);
    dash_ps_.push_constant("color", ed::sculpt_paint::shape_overlay::dash_style::color_a());
    dash_ps_.push_constant("color2", ed::sculpt_paint::shape_overlay::dash_style::color_b());
    dash_ps_.push_constant("dash_width", ed::sculpt_paint::shape_overlay::dash_style::WIDTH);
    dash_ps_.push_constant("udash_factor",
                           ed::sculpt_paint::shape_overlay::dash_style::UDASH_FACTOR);
    dash_ps_.push_constant("dash_phase", float(fmod(BLI_time_now_seconds(), 1.0)));
    for (gpu::Batch *batch : dash_batches_) {
      if (batch != nullptr) {
        dash_ps_.draw(batch);
      }
    }
  }

  /** One filled diamond, owned by the ephemeral batch list. */
  gpu::Batch *make_diamond_batch(const float2 &center, const float radius)
  {
    GPUVertFormat format = {};
    GPU_vertformat_attr_add(&format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
    gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, 4);
    const float3 verts[4] = {{center.x, center.y - radius, 0.0f},
                             {center.x + radius, center.y, 0.0f},
                             {center.x, center.y + radius, 0.0f},
                             {center.x - radius, center.y, 0.0f}};
    GPU_vertbuf_attr_fill(vbo, 0, verts);
    gpu::Batch *batch = GPU_batch_create_ex(GPU_PRIM_TRI_FAN, vbo, nullptr, GPU_BATCH_OWNS_VBO);
    batches_.append(batch);
    return batch;
  }

  /** The origin marker of the active shape: the dark diamond with the orange core the region
   * callback used to draw. */
  void fill_marker_pass(const State &state)
  {
    marker_ps_.init();
    if (!shape_overlay_.has_origin) {
      return;
    }
    marker_ps_.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND_ALPHA);
    marker_ps_.shader_set(GPU_shader_get_builtin_shader(GPU_SHADER_3D_UNIFORM_COLOR));
    marker_ps_.push_constant("ModelViewProjectionMatrix", paint_curve_ortho_mvp(state));
    gpu::Batch *outer = this->make_diamond_batch(shape_overlay_.origin, 6.0f);
    marker_ps_.push_constant("color", float4(0.0f, 0.0f, 0.0f, 0.85f));
    marker_ps_.draw(outer);
    gpu::Batch *inner = this->make_diamond_batch(shape_overlay_.origin, 4.0f);
    marker_ps_.push_constant("color", float4(1.0f, 0.55f, 0.1f, 1.0f));
    marker_ps_.draw(inner);
  }

  void free_handle_gpu()
  {
    auto discard = [](Vector<gpu::Batch *> &v) {
      for (gpu::Batch *b : v) {
        if (b) {
          GPU_batch_discard(b);
        }
      }
      v.clear();
    };
    discard(handle_gpu_.segments);
    discard(handle_gpu_.point_underlay);
    discard(handle_gpu_.point_left);
    discard(handle_gpu_.point_right);
    discard(handle_gpu_.point_diamond);
    discard(handle_gpu_.point_end_left);
    discard(handle_gpu_.point_end_right);
    discard(handle_gpu_.radius_line);
    discard(handle_gpu_.radius_circle);
    discard(handle_gpu_.strength_track);
    discard(handle_gpu_.strength_fill);
    discard(handle_gpu_.strength_circle);
    discard(handle_gpu_.angle_arc);
    discard(handle_gpu_.angle_arrow);
    discard(handle_gpu_.angle_circle);
    handle_gpu_key_ = 0;
    handle_gpu_valid_ = false;
  }

  /** Geometry-only key: positions, handle types, polylines. Hover/selection/colors are push
   * constants and must not invalidate the cache. Digest is 32-bit Murmur2A, see #handle_gpu_key_.
   */
  uint64_t hash_handle_geometry() const
  {
    BLI_HashMurmur2A mm2;
    BLI_hash_mm2a_init(&mm2, 0);
    auto add_bytes = [&](const void *data, const size_t len) {
      BLI_hash_mm2a_add(&mm2, static_cast<const unsigned char *>(data), len);
    };
    auto add_float2 = [&](const float2 &v) { add_bytes(&v, sizeof(v)); };

    BLI_hash_mm2a_add_int(&mm2, int(handles_.points.size()));
    BLI_hash_mm2a_add_int(&mm2, int(handles_.segments.size()));
    BLI_hash_mm2a_add_int(&mm2, int(handles_.radius_handles.size()));
    BLI_hash_mm2a_add_int(&mm2, int(handles_.strength_handles.size()));
    BLI_hash_mm2a_add_int(&mm2, int(handles_.angle_handles.size()));
    for (const ed::sculpt_paint::PaintCurveHandleDrawData &hd : handles_.points) {
      add_float2(hd.position);
      add_float2(hd.handle_left);
      add_float2(hd.handle_right);
      BLI_hash_mm2a_add_int(&mm2, int(hd.h1));
      BLI_hash_mm2a_add_int(&mm2, int(hd.h2));
      BLI_hash_mm2a_add_int(&mm2, int(hd.show_handle_left));
      BLI_hash_mm2a_add_int(&mm2, int(hd.show_handle_right));
    }
    for (const ed::sculpt_paint::PaintCurveSegmentDrawData &seg : handles_.segments) {
      BLI_hash_mm2a_add_int(&mm2, int(seg.polyline.size()));
      if (!seg.polyline.is_empty()) {
        add_bytes(seg.polyline.data(), sizeof(float2) * seg.polyline.size());
      }
    }
    for (const ed::sculpt_paint::PaintCurveRadiusHandleDrawData &rd : handles_.radius_handles) {
      add_float2(rd.point);
      add_float2(rd.end);
    }
    for (const ed::sculpt_paint::PaintCurveStrengthHandleDrawData &sd :
         handles_.strength_handles)
    {
      add_float2(sd.point);
      add_float2(sd.knob);
    }
    for (const ed::sculpt_paint::PaintCurveAngleHandleDrawData &ad : handles_.angle_handles) {
      add_float2(ad.center);
      add_float2(ad.knob);
    }
    return uint64_t(BLI_hash_mm2a_end(&mm2));
  }

  /** Cached silhouette batches for `object`, or null when not cached. */
  const Vector<gpu::Batch *> *cached_silhouette_batches(const Object *object) const
  {
    for (const CachedSilhouetteBatches &entry : silhouette_batches_) {
      if (entry.object == object) {
        return &entry.batches;
      }
    }
    return nullptr;
  }

  /** Create a LINE_STRIP batch owned by `owner`. Vertices are float2 in an SSBO-compatible VBO. */
  gpu::Batch *make_line_strip_owned(Vector<gpu::Batch *> &owner, Span<float2> verts)
  {
    if (verts.size() < 2) {
      return nullptr;
    }
    GPUVertFormat format = {};
    GPU_vertformat_attr_add(&format, "pos", gpu::VertAttrType::SFLOAT_32_32);
    gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
    GPU_vertbuf_data_alloc(*vbo, uint(verts.size()));
    GPU_vertbuf_attr_fill(vbo, 0, verts.data());
    gpu::Batch *batch = GPU_batch_create_ex(GPU_PRIM_LINE_STRIP, vbo, nullptr, GPU_BATCH_OWNS_VBO);
    owner.append(batch);
    return batch;
  }

  /** Create a per-frame LINE_STRIP batch (discarded at the next begin_sync). */
  gpu::Batch *make_line_strip(Span<float2> verts)
  {
    return make_line_strip_owned(batches_, verts);
  }

  /** Create a closed LINE_STRIP batch by appending the first vertex at the end.
   * LINE_LOOP is not compatible with draw_expand, so we emulate it with a strip. */
  gpu::Batch *make_line_strip_closed(Span<float2> verts)
  {
    if (verts.size() < 2) {
      return nullptr;
    }
    /* Duplicate first vertex at the end to close the loop. */
    Vector<float2> closed;
    closed.reserve(verts.size() + 1);
    closed.extend(verts);
    closed.append(verts[0]);
    return make_line_strip(closed);
  }

  gpu::Batch *ensure_owned_strip(Vector<gpu::Batch *> &owner, Span<float2> verts)
  {
    if (verts.size() < 2) {
      owner.append(nullptr);
      return nullptr;
    }
    return make_line_strip_owned(owner, verts);
  }

  gpu::Batch *ensure_owned_strip_closed(Vector<gpu::Batch *> &owner, Span<float2> verts)
  {
    if (verts.size() < 2) {
      owner.append(nullptr);
      return nullptr;
    }
    Vector<float2> closed;
    closed.reserve(verts.size() + 1);
    closed.extend(verts);
    closed.append(verts[0]);
    return make_line_strip_owned(owner, closed);
  }

  void ensure_handle_gpu()
  {
    const uint64_t key = hash_handle_geometry();
    if (handle_gpu_valid_ && key == handle_gpu_key_) {
      return;
    }
    free_handle_gpu();

    handle_gpu_.segments.reserve(handles_.segments.size());
    for (const ed::sculpt_paint::PaintCurveSegmentDrawData &seg : handles_.segments) {
      ensure_owned_strip(handle_gpu_.segments, seg.polyline);
    }

    handle_gpu_.point_underlay.reserve(handles_.points.size());
    handle_gpu_.point_left.reserve(handles_.points.size());
    handle_gpu_.point_right.reserve(handles_.points.size());
    handle_gpu_.point_diamond.reserve(handles_.points.size());
    handle_gpu_.point_end_left.reserve(handles_.points.size());
    handle_gpu_.point_end_right.reserve(handles_.points.size());
    for (const ed::sculpt_paint::PaintCurveHandleDrawData &hd : handles_.points) {
      const bool has_left = hd.show_handle_left;
      const bool has_right = hd.show_handle_right;
      const float2 left2[2] = {hd.handle_left, hd.position};
      const float2 right2[2] = {hd.position, hd.handle_right};
      if (has_left && has_right) {
        const float2 strip3[3] = {hd.handle_left, hd.position, hd.handle_right};
        ensure_owned_strip(handle_gpu_.point_underlay, {strip3, 3});
      }
      else if (has_left) {
        ensure_owned_strip(handle_gpu_.point_underlay, {left2, 2});
      }
      else if (has_right) {
        ensure_owned_strip(handle_gpu_.point_underlay, {right2, 2});
      }
      else {
        handle_gpu_.point_underlay.append(nullptr);
      }
      if (has_left) {
        ensure_owned_strip(handle_gpu_.point_left, {left2, 2});
      }
      else {
        handle_gpu_.point_left.append(nullptr);
      }
      if (has_right) {
        ensure_owned_strip(handle_gpu_.point_right, {right2, 2});
      }
      else {
        handle_gpu_.point_right.append(nullptr);
      }

      const float w = 10.0f * 0.5f;
      const float2 diamond[4] = {{hd.position.x - w, hd.position.y},
                                 {hd.position.x, hd.position.y + w},
                                 {hd.position.x + w, hd.position.y},
                                 {hd.position.x, hd.position.y - w}};
      ensure_owned_strip_closed(handle_gpu_.point_diamond, {diamond, 4});

      auto make_endpoint = [&](const float2 &co, const int8_t htype, Vector<gpu::Batch *> &owner) {
        const float ew = 8.0f * 0.5f;
        if (htype == BEZIER_HANDLE_VECTOR) {
          const float2 tri[3] = {
              {co.x, co.y + ew}, {co.x - ew, co.y - ew}, {co.x + ew, co.y - ew}};
          ensure_owned_strip_closed(owner, {tri, 3});
        }
        else {
          const float2 box[4] = {{co.x - ew, co.y - ew},
                                 {co.x + ew, co.y - ew},
                                 {co.x + ew, co.y + ew},
                                 {co.x - ew, co.y + ew}};
          ensure_owned_strip_closed(owner, {box, 4});
        }
      };
      if (has_left) {
        make_endpoint(hd.handle_left, hd.h1, handle_gpu_.point_end_left);
      }
      else {
        handle_gpu_.point_end_left.append(nullptr);
      }
      if (has_right) {
        make_endpoint(hd.handle_right, hd.h2, handle_gpu_.point_end_right);
      }
      else {
        handle_gpu_.point_end_right.append(nullptr);
      }
    }

    constexpr int CIRCLE_SEGS = 16;
    const float r = ed::sculpt_paint::PAINT_CURVE_RADIUS_HANDLE_CIRCLE_RADIUS;
    handle_gpu_.radius_line.reserve(handles_.radius_handles.size());
    handle_gpu_.radius_circle.reserve(handles_.radius_handles.size());
    for (const ed::sculpt_paint::PaintCurveRadiusHandleDrawData &rd : handles_.radius_handles) {
      const float2 line2[2] = {rd.point, rd.end};
      ensure_owned_strip(handle_gpu_.radius_line, {line2, 2});
      float2 circle[CIRCLE_SEGS];
      for (int i = 0; i < CIRCLE_SEGS; i++) {
        const float angle = float(i) / float(CIRCLE_SEGS) * float(M_PI) * 2.0f;
        circle[i] = {rd.end.x + r * cosf(angle), rd.end.y + r * sinf(angle)};
      }
      ensure_owned_strip_closed(handle_gpu_.radius_circle, {circle, CIRCLE_SEGS});
    }

    handle_gpu_.strength_track.reserve(handles_.strength_handles.size());
    handle_gpu_.strength_fill.reserve(handles_.strength_handles.size());
    handle_gpu_.strength_circle.reserve(handles_.strength_handles.size());
    for (const ed::sculpt_paint::PaintCurveStrengthHandleDrawData &sd :
         handles_.strength_handles)
    {
      const float2 track2[2] = {sd.base, sd.end};
      ensure_owned_strip(handle_gpu_.strength_track, {track2, 2});
      const float2 fill2[2] = {sd.base, sd.knob};
      ensure_owned_strip(handle_gpu_.strength_fill, {fill2, 2});
      float2 circle[CIRCLE_SEGS];
      for (int i = 0; i < CIRCLE_SEGS; i++) {
        const float angle = float(i) / float(CIRCLE_SEGS) * float(M_PI) * 2.0f;
        circle[i] = {sd.knob.x + r * cosf(angle), sd.knob.y + r * sinf(angle)};
      }
      ensure_owned_strip_closed(handle_gpu_.strength_circle, {circle, CIRCLE_SEGS});
    }

    /* The rotation arc: a 180-degree half-circle centered on the stamp's current screen angle,
     * an arrowhead on the arc's leading end, and a knob circle on the arc. */
    constexpr int ARC_SEGS = 24;
    handle_gpu_.angle_arc.reserve(handles_.angle_handles.size());
    handle_gpu_.angle_circle.reserve(handles_.angle_handles.size());
    handle_gpu_.angle_arrow.reserve(handles_.angle_handles.size() * 2);
    for (const ed::sculpt_paint::PaintCurveAngleHandleDrawData &ad : handles_.angle_handles) {
      float2 arc[ARC_SEGS + 1];
      for (int i = 0; i <= ARC_SEGS; i++) {
        const float a = ad.angle + (float(i) / float(ARC_SEGS) - 0.5f) * float(M_PI);
        arc[i] = {ad.center.x + ad.radius * cosf(a), ad.center.y + ad.radius * sinf(a)};
      }
      ensure_owned_strip(handle_gpu_.angle_arc, {arc, ARC_SEGS + 1});
      /* Arrowhead on the arc's leading end (the arc runs counter-clockwise from
       * `angle - PI/2` to `angle + PI/2`): two wings folded back from the tip. */
      const float a_end = ad.angle + 0.5f * float(M_PI);
      const float2 tip = ad.center + float2(cosf(a_end), sinf(a_end)) * ad.radius;
      const float2 back_dir = -float2(-sinf(a_end), cosf(a_end)); /* Against the arc's run. */
      const float wing_len = std::max(ad.radius * 0.25f, 6.0f);
      const float wing_spread = 0.45f;
      const float2 side(-back_dir.y, back_dir.x);
      const float2 wing_a = tip -
                            (back_dir * cosf(wing_spread) + side * sinf(wing_spread)) * wing_len;
      const float2 wing_b = tip -
                            (back_dir * cosf(wing_spread) - side * sinf(wing_spread)) * wing_len;
      const float2 wing_line_a[2] = {tip, wing_a};
      const float2 wing_line_b[2] = {tip, wing_b};
      ensure_owned_strip(handle_gpu_.angle_arrow, {wing_line_a, 2});
      ensure_owned_strip(handle_gpu_.angle_arrow, {wing_line_b, 2});
      float2 circle[CIRCLE_SEGS];
      for (int i = 0; i < CIRCLE_SEGS; i++) {
        const float angle = float(i) / float(CIRCLE_SEGS) * float(M_PI) * 2.0f;
        circle[i] = {ad.knob.x + r * cosf(angle), ad.knob.y + r * sinf(angle)};
      }
      ensure_owned_strip_closed(handle_gpu_.angle_circle, {circle, CIRCLE_SEGS});
    }

    handle_gpu_key_ = key;
    handle_gpu_valid_ = true;

    BLI_assert(handle_gpu_.segments.size() == handles_.segments.size());
    BLI_assert(handle_gpu_.point_underlay.size() == handles_.points.size());
    BLI_assert(handle_gpu_.point_left.size() == handles_.points.size());
    BLI_assert(handle_gpu_.point_right.size() == handles_.points.size());
    BLI_assert(handle_gpu_.point_diamond.size() == handles_.points.size());
    BLI_assert(handle_gpu_.point_end_left.size() == handles_.points.size());
    BLI_assert(handle_gpu_.point_end_right.size() == handles_.points.size());
    BLI_assert(handle_gpu_.radius_line.size() == handles_.radius_handles.size());
    BLI_assert(handle_gpu_.radius_circle.size() == handles_.radius_handles.size());
  }

  /**
   * Draw a line-strip batch with the polyline shader.
   * Must be used instead of ps_.draw() because the polyline shader requires
   * draw_expand + gpu_vert_stride_count_offset to perform geometry expansion.
   */
  void draw_strip(gpu::Batch *batch)
  {
    const int vert_count = int(batch->vertex_count_get());
    const int3 stride_count = {1, vert_count, 0};
    ps_.push_constant("gpu_vert_stride_count_offset", stride_count);
    ps_.draw_expand(batch, GPU_PRIM_TRIS, 2, 1);
  }

  static float4x4 paint_curve_ortho_mvp(const State &state)
  {
    const float ofs = -0.01f;
    return math::projection::orthographic(ofs,
                                          float(state.region->winx) + ofs,
                                          ofs,
                                          float(state.region->winy) + ofs,
                                          -100.0f,
                                          100.0f);
  }

  static void polyline_workaround_push_constants(PassSimple &pass)
  {
    /* WORKAROUND: normally set by GPUBatch/IMM API. Must be set manually for PassSimple. */
    pass.push_constant("gpu_attr_0_fetch_int", false);
    pass.push_constant("gpu_attr_1_fetch_unorm8", false);
    pass.push_constant("gpu_attr_0_len", 3);
    pass.push_constant("gpu_attr_1_len", 3);
  }

  void fill_pass_from_pod(Resources & /*res*/, const State &state)
  {
    if (session_live_) {
      this->ensure_dash_batches();
      this->fill_dash_pass(state);
    }
    ps_.init();
    ps_.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND_ALPHA);
    ps_.shader_set(GPU_shader_get_builtin_shader(GPU_SHADER_3D_POLYLINE_UNIFORM_COLOR));
    ps_.push_constant("viewportSize", float2(state.region->winx, state.region->winy));
    ps_.push_constant("ModelViewProjectionMatrix", paint_curve_ortho_mvp(state));
    ps_.push_constant("lineSmooth", true);
    polyline_workaround_push_constants(ps_);

    ensure_handle_gpu();

    /* --- 1. Faint silhouettes --- */
    {
      ps_.push_constant("lineWidth", ed::sculpt_paint::PAINT_CURVE_SILHOUETTE_WIDTH);
      ps_.push_constant("color", ed::sculpt_paint::PAINT_CURVE_SILHOUETTE_COLOR);
      for (const ed::sculpt_paint::PaintCurveCachedObjectSilhouette &entry :
           silhouettes_.faint_objects)
      {
        if (const Vector<gpu::Batch *> *batches = cached_silhouette_batches(entry.object)) {
          for (gpu::Batch *b : *batches) {
            draw_strip(b);
          }
        }
      }
    }

    /* --- 2. Hover silhouette --- */
    if (silhouettes_.hover_object) {
      if (const Vector<gpu::Batch *> *batches = cached_silhouette_batches(
              silhouettes_.hover_object))
      {
        ps_.push_constant("lineWidth", ed::sculpt_paint::PAINT_CURVE_SILHOUETTE_HOVER_WIDTH);
        ps_.push_constant("color", ed::sculpt_paint::PAINT_CURVE_SILHOUETTE_HOVER_COLOR);
        for (gpu::Batch *b : *batches) {
          draw_strip(b);
        }
      }
    }

    /* --- 2b. Curve Patch effect silhouettes (Curve Edit tool, Paint display group): the same
     * blue and white-hover look as the scene-curve silhouettes above, one entry per effect of
     * the stack. */
    if (!stack_silhouette_faint_.is_empty() || stack_hover_index_ >= 0) {
      ps_.push_constant("lineWidth", ed::sculpt_paint::PAINT_CURVE_SILHOUETTE_WIDTH);
      ps_.push_constant("color", ed::sculpt_paint::PAINT_CURVE_SILHOUETTE_COLOR);
      const auto draw_stack_entry = [&](const int entry_i) {
        if (entry_i < 0 || entry_i >= stack_silhouette_batches_.size()) {
          return;
        }
        for (gpu::Batch *b : stack_silhouette_batches_[entry_i]) {
          if (b != nullptr) {
            draw_strip(b);
          }
        }
      };
      for (const int entry_i : stack_silhouette_faint_) {
        draw_stack_entry(entry_i);
      }
      if (stack_hover_index_ >= 0) {
        ps_.push_constant("lineWidth", ed::sculpt_paint::PAINT_CURVE_SILHOUETTE_HOVER_WIDTH);
        ps_.push_constant("color", ed::sculpt_paint::PAINT_CURVE_SILHOUETTE_HOVER_COLOR);
        draw_stack_entry(stack_hover_index_);
      }
    }

    /* --- 3 & 4. Bezier segment outlines then wires --- */
    {
      /* Segment outlines (black, width 3). */
      ps_.push_constant("lineWidth", 3.0f);
      ps_.push_constant("color", float4(0.0f, 0.0f, 0.0f, 0.5f));
      for (gpu::Batch *b : handle_gpu_.segments) {
        if (b) {
          draw_strip(b);
        }
      }

      /* Segment wires. All use the same wire_color (TH_WIRE) stored per-segment. */
      ps_.push_constant("lineWidth", 1.0f);
      for (const int i : IndexRange(handles_.segments.size())) {
        if (handle_gpu_.segments[i]) {
          const float4 &wc = handles_.segments[i].wire_color;
          ps_.push_constant("color", wc);
          draw_strip(handle_gpu_.segments[i]);
        }
      }

      /* Hovered segment highlight for segment-slide affordance.
       * Drawn last so it appears on top of outline and wire passes.
       * Uses TH_VERTEX_SELECT (matches hover silhouette) because TH_UV_SHADOW is not
       * defined for SPACE_VIEW3D and returns alpha=0 in that context. */
      float hover_col[4];
      ui::theme::get_color_type_4fv(TH_VERTEX_SELECT, SPACE_VIEW3D, hover_col);
      hover_col[3] = 1.0f;
      ps_.push_constant("lineWidth", 3.0f);
      ps_.push_constant("color", float4(hover_col[0], hover_col[1], hover_col[2], hover_col[3]));
      for (const int i : IndexRange(handles_.segments.size())) {
        if (handle_gpu_.segments[i] && handles_.segments[i].hovered) {
          draw_strip(handle_gpu_.segments[i]);
        }
      }
    }

    /* --- 4b. Segment insert preview (perpendicular line + inward arrows) --- */
    if (handles_.insert_preview.valid) {
      const ed::sculpt_paint::PaintCurveInsertPreviewDrawData &ip = handles_.insert_preview;
      const float2 &p = ip.point;
      const float2 &tan = ip.tangent;
      const float2 &perp = ip.perp;
      const float half_len = ed::sculpt_paint::PAINT_CURVE_INSERT_PREVIEW_HALF_LEN;
      const float arrow_len = ed::sculpt_paint::PAINT_CURVE_INSERT_PREVIEW_ARROW_LEN;
      const float arrow_wing = ed::sculpt_paint::PAINT_CURVE_INSERT_PREVIEW_ARROW_WING;
      const float arrow_inset = ed::sculpt_paint::PAINT_CURVE_INSERT_PREVIEW_ARROW_INSET;

      auto draw_line = [&](Span<float2> verts, float width, const float4 &col) {
        if (gpu::Batch *b = make_line_strip(verts)) {
          ps_.push_constant("lineWidth", width);
          ps_.push_constant("color", col);
          draw_strip(b);
        }
      };

      /* White perpendicular line with dark underlay. */
      {
        const float2 line[2] = {p - perp * half_len, p + perp * half_len};
        draw_line({line, 2}, 3.0f, float4(0.0f, 0.0f, 0.0f, 0.5f));
        draw_line({line, 2}, 1.0f, float4(1.0f, 1.0f, 1.0f, 0.9f));
      }

      /* Arrow chevrons on each side, tips pointing toward the curve (each other). */
      auto draw_arrow = [&](const float side_sign) {
        const float2 tip = p + perp * side_sign * arrow_inset;
        const float2 back = tip + perp * side_sign * arrow_len;
        const float2 wing_a = back + tan * arrow_wing;
        const float2 wing_b = back - tan * arrow_wing;
        const float2 left_wing[2] = {tip, wing_a};
        const float2 right_wing[2] = {tip, wing_b};
        draw_line({left_wing, 2}, 3.0f, float4(0.0f, 0.0f, 0.0f, 0.5f));
        draw_line({right_wing, 2}, 3.0f, float4(0.0f, 0.0f, 0.0f, 0.5f));
        draw_line({left_wing, 2}, 1.0f, float4(1.0f, 1.0f, 1.0f, 0.9f));
        draw_line({right_wing, 2}, 1.0f, float4(1.0f, 1.0f, 1.0f, 0.9f));
      };
      draw_arrow(-1.0f);
      draw_arrow(1.0f);
    }

    /* --- 5 & 6. Handle lines (black underlay then colored) --- */
    for (const int i : IndexRange(handles_.points.size())) {
      const ed::sculpt_paint::PaintCurveHandleDrawData &hd = handles_.points[i];
      if (gpu::Batch *b = handle_gpu_.point_underlay[i]) {
        ps_.push_constant("lineWidth", 3.0f);
        ps_.push_constant("color", float4(0.0f, 0.0f, 0.0f, 0.5f));
        draw_strip(b);
      }
      if (gpu::Batch *b = handle_gpu_.point_left[i]) {
        ps_.push_constant("lineWidth", 1.0f);
        ps_.push_constant("color", hd.color_left);
        draw_strip(b);
      }
      if (gpu::Batch *b = handle_gpu_.point_right[i]) {
        ps_.push_constant("lineWidth", 1.0f);
        ps_.push_constant("color", hd.color_right);
        draw_strip(b);
      }
    }

    /* --- 7. Control points + handle endpoints --- */
    {
      float selec_col[4], vert_col[4];
      ui::theme::get_color_type_4fv(TH_VERTEX_SELECT, SPACE_VIEW3D, selec_col);
      ui::theme::get_color_type_4fv(TH_VERTEX, SPACE_VIEW3D, vert_col);

      auto draw_cached_closed = [&](gpu::Batch *b, const float4 &outer, const float4 &inner) {
        if (!b) {
          return;
        }
        ps_.push_constant("lineWidth", 3.0f);
        ps_.push_constant("color", outer);
        draw_strip(b);
        ps_.push_constant("lineWidth", 1.0f);
        ps_.push_constant("color", inner);
        draw_strip(b);
      };

      for (const int i : IndexRange(handles_.points.size())) {
        const ed::sculpt_paint::PaintCurveHandleDrawData &hd = handles_.points[i];

        const bool hovered = hd.hovered_center;
        float4 cp_col = hd.selected_center ?
                            float4(selec_col[0], selec_col[1], selec_col[2], selec_col[3]) :
                            float4(vert_col[0], vert_col[1], vert_col[2], vert_col[3]);
        if (hovered) {
          const float hover_t = 0.65f;
          cp_col = float4(cp_col.x + (1.0f - cp_col.x) * hover_t,
                          cp_col.y + (1.0f - cp_col.y) * hover_t,
                          cp_col.z + (1.0f - cp_col.z) * hover_t,
                          1.0f);
        }
        const float4 inner_col = hovered ? float4(1.0f, 1.0f, 1.0f, 0.8f) :
                                           float4(1.0f, 1.0f, 1.0f, 0.5f);
        draw_cached_closed(handle_gpu_.point_diamond[i], cp_col, inner_col);

        draw_cached_closed(
            handle_gpu_.point_end_left[i], hd.color_left, float4(1.0f, 1.0f, 1.0f, 0.5f));
        draw_cached_closed(
            handle_gpu_.point_end_right[i], hd.color_right, float4(1.0f, 1.0f, 1.0f, 0.5f));
      }
    }

    /* --- 8. Radius handles --- */
    for (const int i : IndexRange(handles_.radius_handles.size())) {
      const ed::sculpt_paint::PaintCurveRadiusHandleDrawData &rd = handles_.radius_handles[i];
      if (gpu::Batch *b = handle_gpu_.radius_line[i]) {
        ps_.push_constant("lineWidth", 3.0f);
        ps_.push_constant("color", float4(0.0f, 0.0f, 0.0f, 0.5f));
        draw_strip(b);
        ps_.push_constant("lineWidth", 1.0f);
        ps_.push_constant("color", rd.color);
        draw_strip(b);
      }
      if (gpu::Batch *b = handle_gpu_.radius_circle[i]) {
        const float4 underlay_col = rd.hovered ? float4(1.0f, 1.0f, 1.0f, 0.8f) :
                                                 float4(1.0f, 1.0f, 1.0f, 0.5f);
        const float hover_t = 0.65f;
        const float4 circle_col = rd.hovered ? float4(rd.color.x + (1.0f - rd.color.x) * hover_t,
                                                      rd.color.y + (1.0f - rd.color.y) * hover_t,
                                                      rd.color.z + (1.0f - rd.color.z) * hover_t,
                                                      1.0f) :
                                               rd.color;
        ps_.push_constant("lineWidth", 1.0f);
        ps_.push_constant("color", underlay_col);
        draw_strip(b);
        ps_.push_constant("color", circle_col);
        draw_strip(b);
      }
    }

    /* --- 8b. Per-point strength handles (Curve Patch POINTS) --- */
    for (const int i : IndexRange(handles_.strength_handles.size())) {
      const ed::sculpt_paint::PaintCurveStrengthHandleDrawData &sd =
          handles_.strength_handles[i];
      if (gpu::Batch *b = handle_gpu_.strength_track[i]) {
        ps_.push_constant("lineWidth", 3.0f);
        ps_.push_constant("color", float4(0.0f, 0.0f, 0.0f, 0.5f));
        draw_strip(b);
        ps_.push_constant("lineWidth", 1.0f);
        ps_.push_constant("color", float4(sd.color.x, sd.color.y, sd.color.z, 0.5f));
        draw_strip(b);
      }
      if (gpu::Batch *b = handle_gpu_.strength_fill[i]) {
        /* The filled stretch from the track's base to the knob reads as a little gauge. */
        ps_.push_constant("lineWidth", 3.0f);
        ps_.push_constant("color", sd.color);
        draw_strip(b);
      }
      if (gpu::Batch *b = handle_gpu_.strength_circle[i]) {
        const float4 underlay_col = sd.hovered ? float4(1.0f, 1.0f, 1.0f, 0.8f) :
                                                 float4(1.0f, 1.0f, 1.0f, 0.5f);
        const float hover_t = 0.65f;
        const float4 circle_col = sd.hovered ? float4(sd.color.x + (1.0f - sd.color.x) * hover_t,
                                                      sd.color.y + (1.0f - sd.color.y) * hover_t,
                                                      sd.color.z + (1.0f - sd.color.z) * hover_t,
                                                      1.0f) :
                                               sd.color;
        ps_.push_constant("lineWidth", 1.0f);
        ps_.push_constant("color", underlay_col);
        draw_strip(b);
        ps_.push_constant("color", circle_col);
        draw_strip(b);
      }
    }

    /* --- 8c. Per-point rotation handles (Curve Patch POINTS) --- */
    for (const int i : IndexRange(handles_.angle_handles.size())) {
      const ed::sculpt_paint::PaintCurveAngleHandleDrawData &ad = handles_.angle_handles[i];
      if (gpu::Batch *b = handle_gpu_.angle_arc[i]) {
        ps_.push_constant("lineWidth", 3.0f);
        ps_.push_constant("color", float4(0.0f, 0.0f, 0.0f, 0.5f));
        draw_strip(b);
        ps_.push_constant("lineWidth", 1.0f);
        ps_.push_constant("color", ad.color);
        draw_strip(b);
      }
      /* Arrowhead, underlay then color, matching the arc. */
      for (int wing = 0; wing < 2; wing++) {
        const int index = i * 2 + wing;
        if (index >= handle_gpu_.angle_arrow.size()) {
          break;
        }
        if (gpu::Batch *b = handle_gpu_.angle_arrow[index]) {
          ps_.push_constant("lineWidth", 3.0f);
          ps_.push_constant("color", float4(0.0f, 0.0f, 0.0f, 0.5f));
          draw_strip(b);
          ps_.push_constant("lineWidth", 1.0f);
          ps_.push_constant("color", ad.color);
          draw_strip(b);
        }
      }
      if (gpu::Batch *b = handle_gpu_.angle_circle[i]) {
        const float4 underlay_col = ad.hovered ? float4(1.0f, 1.0f, 1.0f, 0.8f) :
                                                 float4(1.0f, 1.0f, 1.0f, 0.5f);
        const float hover_t = 0.65f;
        const float4 circle_col = ad.hovered ? float4(ad.color.x + (1.0f - ad.color.x) * hover_t,
                                                      ad.color.y + (1.0f - ad.color.y) * hover_t,
                                                      ad.color.z + (1.0f - ad.color.z) * hover_t,
                                                      1.0f) :
                                               ad.color;
        ps_.push_constant("lineWidth", 1.0f);
        ps_.push_constant("color", underlay_col);
        draw_strip(b);
        ps_.push_constant("color", circle_col);
        draw_strip(b);
      }
    }

    /* --- 9. Snap marker (3D slide or Curve Patch point drag snapping to geometry) --- */
    if (snap_marker_active_) {
      const float2 c = snap_marker_pos_;
      constexpr int SEGS = 24;
      const float r = 9.0f;
      float2 circle[SEGS];
      for (int i = 0; i < SEGS; i++) {
        const float a = float(i) / float(SEGS) * float(M_PI) * 2.0f;
        circle[i] = float2(c.x + r * cosf(a), c.y + r * sinf(a));
      }
      if (gpu::Batch *b = make_line_strip_closed({circle, SEGS})) {
        ps_.push_constant("lineWidth", 3.0f);
        ps_.push_constant("color", float4(0.0f, 0.0f, 0.0f, 0.6f));
        draw_strip(b);
        ps_.push_constant("lineWidth", 1.5f);
        ps_.push_constant("color", float4(1.0f, 1.0f, 1.0f, 1.0f));
        draw_strip(b);
      }
      /* Face Center target: add an inner dot at the face midpoint. */
      if (snap_marker_type_ & SCE_SNAP_TO_FACE_MIDPOINT) {
        constexpr int DOT_SEGS = 10;
        const float rr = 2.5f;
        float2 dot[DOT_SEGS];
        for (int i = 0; i < DOT_SEGS; i++) {
          const float a = float(i) / float(DOT_SEGS) * float(M_PI) * 2.0f;
          dot[i] = float2(c.x + rr * cosf(a), c.y + rr * sinf(a));
        }
        if (gpu::Batch *b = make_line_strip_closed({dot, DOT_SEGS})) {
          ps_.push_constant("lineWidth", 3.0f);
          ps_.push_constant("color", float4(1.0f, 1.0f, 1.0f, 1.0f));
          draw_strip(b);
        }
      }
    }

    if (session_live_) {
      this->fill_marker_pass(state);
    }
  }
};

}  // namespace blender::draw::overlay
