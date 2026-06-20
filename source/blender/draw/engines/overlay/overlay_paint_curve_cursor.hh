/* SPDX-FileCopyrightText: 2026 Blender Authors
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

#include "BKE_paint.hh"
#include "DEG_depsgraph_query.hh"
#include "DNA_brush_types.h"
#include "DNA_curve_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"

#include "GPU_batch.hh"
#include "GPU_shader.hh"
#include "GPU_vertex_buffer.hh"
#include "GPU_vertex_format.hh"

#include "UI_resources.hh"

#include "BLI_math_angle_types.hh"
#include "BLI_math_base.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector_types.hh"

#include "overlay_base.hh"

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

  /** GPU batches created during begin_sync. Freed at the start of the next begin_sync. */
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

 public:
  PaintCurveCursor() = default;
  ~PaintCurveCursor()
  {
    free_batches();
    free_silhouette_batches();
  }

  void begin_sync(Resources &res, const State &state) final
  {
    free_batches();
    handles_ = {};
    silhouettes_ = {};
    enabled_ = false;

    if (!state.is_space_v3d() && !state.is_space_image()) {
      return;
    }
    if (state.hide_overlays || state.is_depth_only_drawing || state.is_material_select) {
      return;
    }
    if (state.region == nullptr || state.depsgraph == nullptr || state.scene == nullptr ||
        state.view_layer == nullptr)
    {
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
    const bool is_curve_stroke = brush->stroke_method == BRUSH_STROKE_CURVE;
    const bool compute_hover = state.cursor_mval_valid && (is_curves_edit || is_curve_stroke) &&
                               state.is_space_v3d() &&
                               !ed::sculpt_paint::ED_paint_curve_slide_is_active();
    const bool show_insert_preview = state.cursor_mval_valid && state.cursor_ctrl_pressed &&
                                     state.is_space_v3d() &&
                                     !ed::sculpt_paint::ED_paint_curve_slide_is_active() &&
                                     (is_curves_edit || is_curve_stroke);

    ed::sculpt_paint::ED_paint_curve_screen_handles_build(
        vc, *brush, sculpt, mval_region, compute_hover, show_insert_preview, handles_);

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

    fill_pass_from_pod(res, state);
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
    /* Build batches once, draw in order: outline → wire → hover (on top). */
    {
      Vector<gpu::Batch *> seg_batches;
      seg_batches.reserve(handles_.segments.size());
      for (const ed::sculpt_paint::PaintCurveSegmentDrawData &seg : handles_.segments) {
        seg_batches.append(make_line_strip(seg.polyline));
      }

      /* Segment outlines (black, width 3). */
      ps_.push_constant("lineWidth", 3.0f);
      ps_.push_constant("color", float4(0.0f, 0.0f, 0.0f, 0.5f));
      for (gpu::Batch *b : seg_batches) {
        if (b) {
          draw_strip(b);
        }
      }

      /* Segment wires. All use the same wire_color (TH_WIRE) stored per-segment. */
      ps_.push_constant("lineWidth", 1.0f);
      for (const int i : IndexRange(handles_.segments.size())) {
        if (seg_batches[i]) {
          const float4 &wc = handles_.segments[i].wire_color;
          ps_.push_constant("color", wc);
          draw_strip(seg_batches[i]);
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
        if (seg_batches[i] && handles_.segments[i].hovered) {
          draw_strip(seg_batches[i]);
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
    for (const ed::sculpt_paint::PaintCurveHandleDrawData &hd : handles_.points) {
      /* Black underlay: 3-vert strip (left → center → right). */
      float2 strip3[3] = {hd.handle_left, hd.position, hd.handle_right};
      if (gpu::Batch *b = make_line_strip({strip3, 3})) {
        ps_.push_constant("lineWidth", 3.0f);
        ps_.push_constant("color", float4(0.0f, 0.0f, 0.0f, 0.5f));
        draw_strip(b);
      }
      /* Colored left segment. */
      {
        float2 left2[2] = {hd.handle_left, hd.position};
        if (gpu::Batch *b = make_line_strip({left2, 2})) {
          ps_.push_constant("lineWidth", 1.0f);
          ps_.push_constant("color", hd.color_left);
          draw_strip(b);
        }
      }
      /* Colored right segment. */
      {
        float2 right2[2] = {hd.position, hd.handle_right};
        if (gpu::Batch *b = make_line_strip({right2, 2})) {
          ps_.push_constant("lineWidth", 1.0f);
          ps_.push_constant("color", hd.color_right);
          draw_strip(b);
        }
      }
    }

    /* --- 7. Control points + handle endpoints --- */
    {
      float selec_col[4], vert_col[4];
      ui::theme::get_color_type_4fv(TH_VERTEX_SELECT, SPACE_VIEW3D, selec_col);
      ui::theme::get_color_type_4fv(TH_VERTEX, SPACE_VIEW3D, vert_col);

      auto draw_endpoint = [&](const float2 &co, float width, int8_t htype, const float4 &col) {
        const float w = width * 0.5f;
        if (htype == BEZIER_HANDLE_VECTOR) {
          const float2 tri[3] = {{co.x, co.y + w},
                                  {co.x - w, co.y - w},
                                  {co.x + w, co.y - w}};
          if (gpu::Batch *b = make_line_strip_closed({tri, 3})) {
            ps_.push_constant("lineWidth", 3.0f);
            ps_.push_constant("color", col);
            draw_strip(b);
            ps_.push_constant("lineWidth", 1.0f);
            ps_.push_constant("color", float4(1.0f, 1.0f, 1.0f, 0.5f));
            draw_strip(b);
          }
        }
        else {
          const float2 box[4] = {{co.x - w, co.y - w},
                                  {co.x + w, co.y - w},
                                  {co.x + w, co.y + w},
                                  {co.x - w, co.y + w}};
          if (gpu::Batch *b = make_line_strip_closed({box, 4})) {
            ps_.push_constant("lineWidth", 3.0f);
            ps_.push_constant("color", col);
            draw_strip(b);
            ps_.push_constant("lineWidth", 1.0f);
            ps_.push_constant("color", float4(1.0f, 1.0f, 1.0f, 0.5f));
            draw_strip(b);
          }
        }
      };

      for (const ed::sculpt_paint::PaintCurveHandleDrawData &hd : handles_.points) {
        const float w = 10.0f * 0.5f;
        const float2 &co = hd.position;

        /* Control point diamond. */
        {
          const float4 cp_col = hd.selected_center ?
                                    float4(selec_col[0], selec_col[1], selec_col[2], selec_col[3]) :
                                    float4(vert_col[0], vert_col[1], vert_col[2], vert_col[3]);
          const float2 diamond[4] = {
              {co.x - w, co.y}, {co.x, co.y + w}, {co.x + w, co.y}, {co.x, co.y - w}};
          if (gpu::Batch *b = make_line_strip_closed({diamond, 4})) {
            ps_.push_constant("lineWidth", 3.0f);
            ps_.push_constant("color", cp_col);
            draw_strip(b);
            ps_.push_constant("lineWidth", 1.0f);
            ps_.push_constant("color", float4(1.0f, 1.0f, 1.0f, 0.5f));
            draw_strip(b);
          }
        }

        /* Handle endpoints. */
        draw_endpoint(hd.handle_left, 8.0f, hd.h1, hd.color_left);
        draw_endpoint(hd.handle_right, 8.0f, hd.h2, hd.color_right);
      }
    }

    /* --- 8. Radius handles --- */
    for (const ed::sculpt_paint::PaintCurveRadiusHandleDrawData &rd : handles_.radius_handles) {
      /* Line from pivot to end. */
      {
        float2 line2[2] = {rd.point, rd.end};
        if (gpu::Batch *b = make_line_strip({line2, 2})) {
          ps_.push_constant("lineWidth", 3.0f);
          ps_.push_constant("color", float4(0.0f, 0.0f, 0.0f, 0.5f));
          draw_strip(b);
          ps_.push_constant("lineWidth", 1.0f);
          ps_.push_constant("color", rd.color);
          draw_strip(b);
        }
      }
      /* Circle at endpoint. */
      {
        constexpr int CIRCLE_SEGS = 16;
        const float r = ed::sculpt_paint::PAINT_CURVE_RADIUS_HANDLE_CIRCLE_RADIUS;
        float2 circle[CIRCLE_SEGS];
        for (int i = 0; i < CIRCLE_SEGS; i++) {
          const float angle = float(i) / float(CIRCLE_SEGS) * float(M_PI) * 2.0f;
          circle[i] = {rd.end.x + r * cosf(angle), rd.end.y + r * sinf(angle)};
        }
        if (gpu::Batch *b = make_line_strip_closed({circle, CIRCLE_SEGS})) {
          ps_.push_constant("lineWidth", 1.0f);
          ps_.push_constant("color", float4(1.0f, 1.0f, 1.0f, 0.5f));
          draw_strip(b);
          ps_.push_constant("color", rd.color);
          draw_strip(b);
        }
      }
    }
  }
};

}  // namespace blender::draw::overlay
