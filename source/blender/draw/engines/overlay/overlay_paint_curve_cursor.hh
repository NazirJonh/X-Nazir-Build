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

#include "ED_paint_curve_draw.hh"
#include "ED_view3d.hh"

#include "BKE_brush.hh"
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

#include "overlay_base.hh"

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

  Vector<ed::sculpt_paint::PaintCurveCachedObjectSilhouette> silhouette_cache_;
  uint64_t silhouette_cache_key_ = 0;

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
    free_handle_gpu();
  }

  void begin_sync(Resources &res, const State &state) final
  {
#if PAINT_CURVE_CURSOR_PROFILING
    const double prof_t0 = BLI_time_now_seconds(); /* DEBUG-pccursor */
#endif
    free_batches();
    handles_ = {};
    silhouettes_ = {};
    enabled_ = false;
    snap_marker_active_ = false;

    if (!state.is_space_v3d() && !state.is_space_image()) {
      free_handle_gpu();
      return;
    }
    if (state.hide_overlays || state.is_depth_only_drawing || state.is_material_select) {
      free_handle_gpu();
      return;
    }
    if (state.region == nullptr || state.depsgraph == nullptr || state.scene == nullptr ||
        state.view_layer == nullptr)
    {
      free_handle_gpu();
      return;
    }

    Scene *scene = const_cast<Scene *>(state.scene);
    ViewLayer *view_layer = const_cast<ViewLayer *>(state.view_layer);

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
    const bool is_curves_edit =
        ed::sculpt_paint::ED_paint_curve_is_curves_edit_tool(state.active_tool_idname);

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

    const int2 origin(state.region->winrct.xmin, state.region->winrct.ymin);
    const float2 mval_region = state.cursor_mval_valid ?
                                   float2(state.cursor_mval - origin) :
                                   float2(-1.0e6f);
    const bool is_curve_patch_active =
        ed::sculpt_paint::ED_paint_curve_patch_active_control_curve(vc.obact) != nullptr;

    /* An already-active session keeps its overlay regardless: the gate applies at session entry,
     * not inside a session that was legitimately started. */
    const bool is_curve_patch_stroke = brush->stroke_method == BRUSH_STROKE_CURVE_PATCH &&
                                       bke::brush::supports_curve_patch(*brush);
    const bool is_curve_stroke = brush->stroke_method == BRUSH_STROKE_CURVE ||
                                 is_curve_patch_stroke || is_curve_patch_active;
    const bool compute_hover = state.cursor_mval_valid && (is_curves_edit || is_curve_stroke) &&
                               state.is_space_v3d() &&
                               !ed::sculpt_paint::ED_paint_curve_slide_is_active();
    const bool show_insert_preview = state.cursor_mval_valid && state.cursor_ctrl_pressed &&
                                     state.is_space_v3d() &&
                                     !ed::sculpt_paint::ED_paint_curve_slide_is_active() &&
                                     (is_curves_edit || is_curve_stroke);

    if (is_curve_patch_active) {
      const int curves_num =
          ed::sculpt_paint::ED_paint_curve_patch_control_curves_num(vc.obact);

      /* Draw all patches. Only the active patch shows hover highlights and insert preview.
       * Non-active patches are drawn plain (no hover, no insert preview) so the user can
       * still see and click them. */
      handles_.points.clear();
      handles_.radius_handles.clear();
      handles_.segments.clear();
      handles_.insert_preview = {};

      const bke::CurvesGeometry *active_curve =
          ed::sculpt_paint::ED_paint_curve_patch_active_control_curve(vc.obact);

      for (int i = 0; i < curves_num; i++) {
        const bke::CurvesGeometry *control_curve =
            ed::sculpt_paint::ED_paint_curve_patch_control_curve_at(vc.obact, i);
        if (control_curve == nullptr) {
          continue;
        }
        const bool is_active = (control_curve == active_curve);
        ed::sculpt_paint::PaintCurveScreenHandles tmp_handles;
        ed::sculpt_paint::ED_paint_curve_screen_handles_build_from_geometry(
            vc,
            *control_curve,
            true,
            sculpt,
            true,
            mval_region,
            is_active ? compute_hover : false,
            is_active ? show_insert_preview : false,
            tmp_handles);
        /* Accumulate into the shared handles_ container. */
        handles_.points.extend(tmp_handles.points);
        handles_.radius_handles.extend(tmp_handles.radius_handles);
        handles_.segments.extend(tmp_handles.segments);
        /* Only keep insert_preview from the active patch. */
        if (is_active) {
          handles_.insert_preview = tmp_handles.insert_preview;
        }
      }
    }
    else if (brush->stroke_method == BRUSH_STROKE_CURVE || is_curve_patch_stroke || is_curves_edit) {
      /* Curve Edit uses the same paint-curve handle overlay as Stroke Method: Curve so Ctrl+RMB
       * point creation and handle drags are visible while editing (silhouettes alone are not enough). */
      ed::sculpt_paint::ED_paint_curve_screen_handles_build(
          vc, *brush, sculpt, mval_region, compute_hover, show_insert_preview, handles_);
    }

    if (is_curves_edit && state.is_space_v3d()) {
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

    /* Snap marker: shown only while a 3D slide is snapping to geometry. World position is stored
     * by the slide modal; project it into each viewport's region space here. */
    if (state.is_space_v3d() && ed::sculpt_paint::ED_paint_curve_slide_is_active()) {
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
     * which this measurement exists to judge. A cache only pays off if the GPU upload dominated. */
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
    printf("[DEBUG-pccursor] total=%.3fms | screen=%.3f batches=%.3f | points=%d ephemeral=%d "
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
    manager.submit(ps_);
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
    handle_gpu_key_ = 0;
    handle_gpu_valid_ = false;
  }

  /** Geometry-only key: positions, handle types, polylines. Hover/selection/colors are push
   * constants and must not invalidate the cache. Digest is 32-bit Murmur2A, see #handle_gpu_key_. */
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
    for (const ed::sculpt_paint::PaintCurveHandleDrawData &hd : handles_.points) {
      add_float2(hd.position);
      add_float2(hd.handle_left);
      add_float2(hd.handle_right);
      BLI_hash_mm2a_add_int(&mm2, int(hd.h1));
      BLI_hash_mm2a_add_int(&mm2, int(hd.h2));
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
    gpu::Batch *batch = GPU_batch_create_ex(
        GPU_PRIM_LINE_STRIP, vbo, nullptr, GPU_BATCH_OWNS_VBO);
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
      const float2 strip3[3] = {hd.handle_left, hd.position, hd.handle_right};
      ensure_owned_strip(handle_gpu_.point_underlay, {strip3, 3});
      const float2 left2[2] = {hd.handle_left, hd.position};
      ensure_owned_strip(handle_gpu_.point_left, {left2, 2});
      const float2 right2[2] = {hd.position, hd.handle_right};
      ensure_owned_strip(handle_gpu_.point_right, {right2, 2});

      const float w = 10.0f * 0.5f;
      const float2 diamond[4] = {
          {hd.position.x - w, hd.position.y},
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
          const float2 box[4] = {
              {co.x - ew, co.y - ew},
              {co.x + ew, co.y - ew},
              {co.x + ew, co.y + ew},
              {co.x - ew, co.y + ew}};
          ensure_owned_strip_closed(owner, {box, 4});
        }
      };
      make_endpoint(hd.handle_left, hd.h1, handle_gpu_.point_end_left);
      make_endpoint(hd.handle_right, hd.h2, handle_gpu_.point_end_right);
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
    return math::projection::orthographic(
        ofs, float(state.region->winx) + ofs, ofs, float(state.region->winy) + ofs, -100.0f, 100.0f);
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
      float faint_col[4];
      ui::theme::get_color_type_4fv(TH_WIRE, SPACE_VIEW3D, faint_col);
      faint_col[3] = 0.5f;
      ps_.push_constant("lineWidth", 1.0f);
      ps_.push_constant("color", float4(faint_col[0], faint_col[1], faint_col[2], faint_col[3]));
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
      if (const Vector<gpu::Batch *> *batches =
              cached_silhouette_batches(silhouettes_.hover_object))
      {
        float hover_col[4];
        ui::theme::get_color_type_4fv(TH_VERTEX_SELECT, SPACE_VIEW3D, hover_col);
        hover_col[3] = 1.0f;
        ps_.push_constant("lineWidth", 3.0f);
        ps_.push_constant("color", float4(hover_col[0], hover_col[1], hover_col[2], hover_col[3]));
        for (gpu::Batch *b : *batches) {
          draw_strip(b);
        }
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
      ps_.push_constant(
          "color", float4(hover_col[0], hover_col[1], hover_col[2], hover_col[3]));
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
        const float4 circle_col = rd.hovered ?
                                      float4(rd.color.x + (1.0f - rd.color.x) * hover_t,
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

    /* --- 9. Snap marker (shown during a 3D slide snapping to geometry) --- */
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
  }
};

}  // namespace blender::draw::overlay
