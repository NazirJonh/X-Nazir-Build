/* SPDX-FileCopyrightText: 2020 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "MEM_guardedalloc.h"

#include "CLG_log.h"

#include "BLI_enumerable_thread_specific.hh"
#include "BLI_math_color.h"
#include "BLI_math_color_blend.h"
#include "BLI_math_vector.hh"
#include "BLI_time.h"

#include "BLT_translation.hh"

#include "BKE_attribute.hh"
#include "BKE_brush.hh"
#include "BKE_colortools.hh"
#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_layer.hh"
#include "BKE_mesh.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_paint_bvh_pixels.hh"
#include "BKE_paint_types.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "ED_image.hh"
#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "../paint_gradient_core.hh"
#include "../paint_gradient_session.hh"
#include "../paint_image_session_state.hh"
#include "../paint_intern.hh"

#include "mesh_brush_common.hh"
#include "sculpt_automask.hh"
#include "sculpt_color.hh"
#include "sculpt_filter.hh"
#include "sculpt_intern.hh"
#include "sculpt_smooth.hh"
#include "sculpt_undo.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>

namespace blender::ed::sculpt_paint::color {

static CLG_LogRef LOG = {"sculpt.gradient"};
static constexpr bool kEnableGradientDebugTelemetry = true;
static constexpr int64_t kGradientDebugPeriod = 16;

static void debug_log_gradient_step(const bool is_interactive,
                                    const bool use_image_backend,
                                    const int64_t tick_version,
                                    const bool did_apply,
                                    const bool did_flush)
{
  if constexpr (!kEnableGradientDebugTelemetry) {
    return;
  }

  const int64_t tick = std::max<int64_t>(tick_version, 1);
  if ((tick % kGradientDebugPeriod) != 0) {
    return;
  }

  CLOG_INFO(&LOG,
            "gradient_step tick=%lld interactive=%d image_backend=%d did_apply=%d did_flush=%d",
            static_cast<long long>(tick),
            int(is_interactive),
            int(use_image_backend),
            int(did_apply),
            int(did_flush));
  std::fprintf(stderr,
               "[sculpt.gradient] gradient_step tick=%lld interactive=%d image_backend=%d "
               "did_apply=%d did_flush=%d\n",
               static_cast<long long>(tick),
               int(is_interactive),
               int(use_image_backend),
               int(did_apply),
               int(did_flush));
  std::fflush(stderr);
}

enum class FilterType {
  Fill = 0,
  Hue,
  Saturation,
  Value,
  Brightness,
  Contrast,
  Red,
  Green,
  Blue,
  Smooth,
};

static const float fill_filter_default_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};

static float3 fill_color_resolve_from_paint(const bContext *C, const bool use_secondary_color)
{
  const ToolSettings *ts = CTX_data_tool_settings(C);
  const Sculpt *sd = ts->sculpt;
  const Paint *paint = &sd->paint;
  const Brush *brush = BKE_paint_brush_for_read(paint);

  /* Use brush colors if a brush tool is active, otherwise use unified paint colors. */
  if (WM_toolsystem_active_tool_is_brush(C) && brush) {
    const float3 color = use_secondary_color ? BKE_brush_secondary_color_get(paint, brush) :
                                               BKE_brush_color_get(paint, brush);
    return color;
  }

  /* Use unified paint colors when filter tool is active. */
  const float3 color = use_secondary_color ? paint->unified_paint_settings.secondary_color :
                                             paint->unified_paint_settings.color;
  return color;
}

static float3 fill_color_resolve(const bContext *C, wmOperator *op, const bool use_secondary_color)
{
  if (RNA_struct_property_is_set(op->ptr, "fill_color")) {
    float3 fill_color;
    RNA_float_get_array(op->ptr, "fill_color", fill_color);
    return fill_color;
  }

  return fill_color_resolve_from_paint(C, use_secondary_color);
}

/* Stores the used color as the fill color to ensure the redo panel works as expected. */
static void fill_color_store_current(const bContext *C, wmOperator *op)
{
  const bool use_secondary_color = RNA_boolean_get(op->ptr, "use_secondary_color");
  const float3 fill_color = fill_color_resolve_from_paint(C, use_secondary_color);
  RNA_float_set_array(op->ptr, "fill_color", fill_color);
}

static EnumPropertyItem prop_color_filter_types[] = {
    {int(FilterType::Fill), "FILL", 0, "Fill", "Fill with a specific color"},
    {int(FilterType::Hue), "HUE", 0, "Hue", "Change hue"},
    {int(FilterType::Saturation), "SATURATION", 0, "Saturation", "Change saturation"},
    {int(FilterType::Value), "VALUE", 0, "Value", "Change value"},
    {int(FilterType::Brightness), "BRIGHTNESS", 0, "Brightness", "Change brightness"},
    {int(FilterType::Contrast), "CONTRAST", 0, "Contrast", "Change contrast"},
    {int(FilterType::Smooth), "SMOOTH", 0, "Smooth", "Smooth colors"},
    {int(FilterType::Red), "RED", 0, "Red", "Change red channel"},
    {int(FilterType::Green), "GREEN", 0, "Green", "Change green channel"},
    {int(FilterType::Blue), "BLUE", 0, "Blue", "Change blue channel"},
    {0, nullptr, 0, nullptr, nullptr},
};

struct LocalData {
  Vector<float> factors;
  Vector<float4> colors;
  Vector<int> neighbor_offsets;
  Vector<int> neighbor_data;
  Vector<float4> average_colors;
  Vector<float4> new_colors;
};

BLI_NOINLINE static void clamp_factors(const MutableSpan<float> factors,
                                       const float min,
                                       const float max)
{
  for (float &factor : factors) {
    factor = std::clamp(factor, min, max);
  }
}

static void color_filter_task(const Depsgraph &depsgraph,
                              Object &ob,
                              const OffsetIndices<int> faces,
                              const Span<int> corner_verts,
                              const GroupedSpan<int> vert_to_face_map,
                              const MeshAttributeData &attribute_data,
                              const FilterType mode,
                              const float filter_strength,
                              const float3 &filter_fill_color,
                              const bke::pbvh::MeshNode &node,
                              LocalData &tls,
                              bke::GSpanAttributeWriter &color_attribute)
{
  SculptSession &ss = *ob.runtime->sculpt_session;

  const Span<float4> orig_colors = orig_color_data_get_mesh(ob, node);

  const Span<int> verts = node.verts();

  tls.factors.resize(verts.size());
  const MutableSpan<float> factors = tls.factors;
  fill_factor_from_hide_and_mask(attribute_data.hide_vert, attribute_data.mask, verts, factors);
  auto_mask::calc_vert_factors(
      depsgraph, ob, ss.filter_cache->automasking.get(), node, verts, factors);
  scale_factors(factors, filter_strength);

  tls.new_colors.resize(verts.size());
  const MutableSpan<float4> new_colors = tls.new_colors;

  /* Copy alpha. */
  for (const int i : verts.index_range()) {
    new_colors[i][3] = orig_colors[i][3];
  }

  switch (mode) {
    case FilterType::Fill: {
      clamp_factors(factors, 0.0f, 1.0f);
      for (const int i : verts.index_range()) {
        float fill_color_rgba[4];
        copy_v3_v3(fill_color_rgba, filter_fill_color);
        fill_color_rgba[3] = 1.0f;
        mul_v4_fl(fill_color_rgba, factors[i]);
        blend_color_mix_float(new_colors[i], orig_colors[i], fill_color_rgba);
      }
      break;
    }
    case FilterType::Hue: {
      for (const int i : verts.index_range()) {
        float3 hsv_color;
        rgb_to_hsv_v(orig_colors[i], hsv_color);
        const float hue = hsv_color[0];
        hsv_color[0] = fmod((hsv_color[0] + fabs(factors[i])) - hue, 1);
        hsv_to_rgb_v(hsv_color, new_colors[i]);
      }
      break;
    }
    case FilterType::Saturation: {
      for (const int i : verts.index_range()) {
        float3 hsv_color;
        rgb_to_hsv_v(orig_colors[i], hsv_color);

        if (hsv_color[1] > 0.001f) {
          hsv_color[1] = std::clamp(hsv_color[1] + factors[i] * hsv_color[1], 0.0f, 1.0f);
          hsv_to_rgb_v(hsv_color, new_colors[i]);
        }
        else {
          copy_v3_v3(new_colors[i], orig_colors[i]);
        }
      }
      break;
    }
    case FilterType::Value: {
      for (const int i : verts.index_range()) {
        float3 hsv_color;
        rgb_to_hsv_v(orig_colors[i], hsv_color);
        hsv_color[2] = std::clamp(hsv_color[2] + factors[i], 0.0f, 1.0f);
        hsv_to_rgb_v(hsv_color, new_colors[i]);
      }
      break;
    }
    case FilterType::Red: {
      for (const int i : verts.index_range()) {
        copy_v3_v3(new_colors[i], orig_colors[i]);
        new_colors[i][0] = std::clamp(orig_colors[i][0] + factors[i], 0.0f, 1.0f);
      }
      break;
    }
    case FilterType::Green: {
      for (const int i : verts.index_range()) {
        copy_v3_v3(new_colors[i], orig_colors[i]);
        new_colors[i][1] = std::clamp(orig_colors[i][1] + factors[i], 0.0f, 1.0f);
      }
      break;
    }
    case FilterType::Blue: {
      for (const int i : verts.index_range()) {
        copy_v3_v3(new_colors[i], orig_colors[i]);
        new_colors[i][2] = std::clamp(orig_colors[i][2] + factors[i], 0.0f, 1.0f);
      }
      break;
    }
    case FilterType::Brightness: {
      clamp_factors(factors, -1.0f, 1.0f);
      for (const int i : verts.index_range()) {
        const float brightness = factors[i];
        const float contrast = 0;
        float delta = contrast / 2.0f;
        const float gain = 1.0f - delta * 2.0f;
        delta *= -1;
        const float offset = gain * (brightness + delta);
        for (int component = 0; component < 3; component++) {
          new_colors[i][component] = std::clamp(
              gain * orig_colors[i][component] + offset, 0.0f, 1.0f);
        }
      }
      break;
    }
    case FilterType::Contrast: {
      clamp_factors(factors, -1.0f, 1.0f);
      for (const int i : verts.index_range()) {
        const float brightness = 0;
        const float contrast = factors[i];
        float delta = contrast / 2.0f;
        float gain = 1.0f - delta * 2.0f;

        float offset;
        if (contrast > 0) {
          gain = 1.0f / ((gain != 0.0f) ? gain : FLT_EPSILON);
          offset = gain * (brightness - delta);
        }
        else {
          delta *= -1;
          offset = gain * (brightness + delta);
        }
        for (int component = 0; component < 3; component++) {
          new_colors[i][component] = std::clamp(
              gain * orig_colors[i][component] + offset, 0.0f, 1.0f);
        }
      }
      break;
    }
    case FilterType::Smooth: {
      clamp_factors(factors, -1.0f, 1.0f);

      tls.colors.resize(verts.size());
      const MutableSpan<float4> colors = tls.colors;
      for (const int i : verts.index_range()) {
        colors[i] = color_vert_get(faces,
                                   corner_verts,
                                   vert_to_face_map,
                                   color_attribute.span,
                                   color_attribute.domain,
                                   verts[i]);
      }

      const GroupedSpan<int> neighbors = calc_vert_neighbors(faces,
                                                             corner_verts,
                                                             vert_to_face_map,
                                                             {},
                                                             verts,
                                                             tls.neighbor_offsets,
                                                             tls.neighbor_data);

      tls.average_colors.resize(verts.size());
      const MutableSpan<float4> average_colors = tls.average_colors;
      smooth::neighbor_color_average(faces,
                                     corner_verts,
                                     vert_to_face_map,
                                     color_attribute.span,
                                     color_attribute.domain,
                                     neighbors,
                                     average_colors);

      for (const int i : verts.index_range()) {
        const int vert = verts[i];

        if (factors[i] < 0.0f) {
          interp_v4_v4v4(average_colors[i], average_colors[i], colors[i], 0.5f);
        }

        bool copy_alpha = colors[i][3] == average_colors[i][3];

        if (factors[i] < 0.0f) {
          float4 delta_color;

          /* Unsharp mask. */
          copy_v4_v4(delta_color, ss.filter_cache->pre_smoothed_color[vert]);
          delta_color -= average_colors[i];

          copy_v4_v4(new_colors[i], colors[i]);
          madd_v4_v4fl(new_colors[i], delta_color, factors[i]);
        }
        else {
          blend_color_interpolate_float(new_colors[i], colors[i], average_colors[i], factors[i]);
        }

        new_colors[i] = math::clamp(new_colors[i], 0.0f, 1.0f);

        /* Prevent accumulated numeric error from corrupting alpha. */
        if (copy_alpha) {
          new_colors[i][3] = average_colors[i][3];
        }
      }
      break;
    }
  }

  for (const int i : verts.index_range()) {
    color_vert_set(faces,
                   corner_verts,
                   vert_to_face_map,
                   color_attribute.domain,
                   verts[i],
                   new_colors[i],
                   color_attribute.span);
  }
}

static void sculpt_color_presmooth_init(const Mesh &mesh, Object &object)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  const IndexMask &node_mask = ss.filter_cache->node_mask;
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
  const bke::GAttributeReader color_attribute = active_color_attribute(mesh);
  const GVArraySpan colors = *color_attribute;

  if (ss.filter_cache->pre_smoothed_color.is_empty()) {
    ss.filter_cache->pre_smoothed_color = Array<float4>(mesh.verts_num);
  }
  const MutableSpan<float4> pre_smoothed_color = ss.filter_cache->pre_smoothed_color;

  node_mask.foreach_index(
      [&](const int i) {
        for (const int vert : nodes[i].verts()) {
          pre_smoothed_color[vert] = color_vert_get(
              faces, corner_verts, vert_to_face_map, colors, color_attribute.domain, vert);
        }
      },
      exec_mode::grain_size(1));

  struct LocalData {
    Vector<int> neighbor_offsets;
    Vector<int> neighbor_data;
    Vector<float4> averaged_colors;
  };
  threading::EnumerableThreadSpecific<LocalData> all_tls;
  for ([[maybe_unused]] const int iteration : IndexRange(2)) {
    node_mask.foreach_index(
        [&](const int i) {
          LocalData &tls = all_tls.local();
          const Span<int> verts = nodes[i].verts();

          const GroupedSpan<int> neighbors = calc_vert_neighbors(faces,
                                                                 corner_verts,
                                                                 vert_to_face_map,
                                                                 {},
                                                                 verts,
                                                                 tls.neighbor_offsets,
                                                                 tls.neighbor_data);

          tls.averaged_colors.resize(verts.size());
          const MutableSpan<float4> averaged_colors = tls.averaged_colors;
          smooth::neighbor_data_average_mesh(
              pre_smoothed_color.as_span(), neighbors, averaged_colors);

          for (const int i : verts.index_range()) {
            pre_smoothed_color[verts[i]] = math::interpolate(
                pre_smoothed_color[verts[i]], averaged_colors[i], 0.5f);
          }
        },
        exec_mode::grain_size(1));
  }
}

static void sculpt_color_filter_apply(bContext *C, wmOperator *op, Object &ob)
{
  const Depsgraph &depsgraph = *CTX_data_depsgraph_pointer(C);
  const Sculpt &sd = *CTX_data_tool_settings(C)->sculpt;
  SculptSession &ss = *ob.runtime->sculpt_session;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();

  const FilterType mode = FilterType(RNA_enum_get(op->ptr, "type"));
  float filter_strength = RNA_float_get(op->ptr, "strength");
  const bool use_secondary_color = RNA_boolean_get(op->ptr, "use_secondary_color");
  const float3 fill_color = fill_color_resolve(C, op, use_secondary_color);

  Mesh &mesh = *id_cast<Mesh *>(ob.data);
  if (filter_strength < 0.0 && ss.filter_cache->pre_smoothed_color.is_empty()) {
    sculpt_color_presmooth_init(mesh, ob);
  }

  const IndexMask &node_mask = ss.filter_cache->node_mask;
  if (auto_mask::is_enabled(sd, ob, nullptr) && ss.filter_cache->automasking &&
      ss.filter_cache->automasking->settings.flags & BRUSH_AUTOMASKING_CAVITY_ALL)
  {
    ss.filter_cache->automasking->calc_cavity_factor(depsgraph, ob, node_mask);
  }

  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
  bke::GSpanAttributeWriter color_attribute = active_color_attribute_for_write(mesh);
  const MeshAttributeData attribute_data(mesh);

  threading::EnumerableThreadSpecific<LocalData> all_tls;
  node_mask.foreach_index(
      [&](const int i) {
        LocalData &tls = all_tls.local();
        color_filter_task(depsgraph,
                          ob,
                          faces,
                          corner_verts,
                          vert_to_face_map,
                          attribute_data,
                          mode,
                          filter_strength,
                          fill_color,
                          nodes[i],
                          tls,
                          color_attribute);
      },
      exec_mode::grain_size(1));
  pbvh.tag_attribute_changed(node_mask, mesh.active_color_attribute);
  color_attribute.finish();
  flush_update_step(C, UpdateType::Color);
}

static void sculpt_color_filter_end(bContext *C, wmOperator *op, Object &ob)
{
  SculptSession &ss = *ob.runtime->sculpt_session;

  if (FilterType(RNA_enum_get(op->ptr, "type")) == FilterType::Fill &&
      !RNA_struct_property_is_set(op->ptr, "fill_color"))
  {
    fill_color_store_current(C, op);
  }

  undo::push_end(ob);
  MEM_delete(ss.filter_cache);
  ss.filter_cache = nullptr;
  flush_update_done(C, ob, UpdateType::Color);
}

static wmOperatorStatus sculpt_color_filter_modal(bContext *C,
                                                  wmOperator *op,
                                                  const wmEvent *event)
{
  Object &ob = *CTX_data_active_object(C);
  SculptSession &ss = *ob.runtime->sculpt_session;

  if (event->type == LEFTMOUSE && event->val == KM_RELEASE) {
    /* For Fill: if this was a click (not a drag), apply once at the tool's configured strength */
    if (FilterType(RNA_enum_get(op->ptr, "type")) == FilterType::Fill &&
        !ss.filter_cache->has_dragged)
    {
      RNA_float_set(op->ptr, "strength", ss.filter_cache->start_filter_strength);
      sculpt_color_filter_apply(C, op, ob);
    }

    sculpt_color_filter_end(C, op, ob);
    return OPERATOR_FINISHED;
  }

  if (event->type != MOUSEMOVE) {
    return OPERATOR_RUNNING_MODAL;
  }

  /* Use a pixel threshold to distinguish a click from a drag */
  int2 start_mouse;
  RNA_int_get_array(op->ptr, "start_mouse", &start_mouse[0]);
  const int2 mouse_2d(event->mval[0], event->mval[1]);

  const int drag_threshold = WM_event_drag_threshold(event);
  ss.filter_cache->has_dragged |= math::distance_manhattan(start_mouse, mouse_2d) > drag_threshold;

  if (!ss.filter_cache->has_dragged) {
    return OPERATOR_RUNNING_MODAL;
  }

  const float len = (start_mouse[0] - event->mval[0]) * 0.001f;
  float filter_strength = ss.filter_cache->start_filter_strength * -len;

  RNA_float_set(op->ptr, "strength", filter_strength);

  sculpt_color_filter_apply(C, op, ob);

  return OPERATOR_RUNNING_MODAL;
}

static int sculpt_color_filter_init(bContext *C, wmOperator *op)
{
  const Scene &scene = *CTX_data_scene(C);
  Object &ob = *CTX_data_active_object(C);
  Sculpt &sd = *CTX_data_tool_settings(C)->sculpt;
  View3D *v3d = CTX_wm_view3d(C);

  const Base *base = CTX_data_active_base(C);
  if (!BKE_base_is_visible(v3d, base)) {
    return OPERATOR_CANCELLED;
  }

  int mval[2];
  RNA_int_get_array(op->ptr, "start_mouse", mval);
  float mval_fl[2] = {float(mval[0]), float(mval[1])};

  const bool use_automasking = auto_mask::is_enabled(sd, ob, nullptr);
  if (use_automasking) {
    if (v3d) {
      /* Update the active face set manually as the paint cursor is not enabled when using the Mesh
       * Filter Tool. */
      CursorGeometryInfo cgi;
      cursor_geometry_info_update(C, &cgi, mval_fl, false);
    }
  }

  /* Disable for multires and dyntopo for now */
  if (!color_supported_check(scene, ob, op->reports)) {
    return OPERATOR_CANCELLED;
  }

  /* Ensure that we have a PBVH to be able to push changes on only visible nodes. */
  bke::object::pbvh_ensure(*CTX_data_ensure_evaluated_depsgraph(C), ob);

  undo::push_begin(scene, ob, op);
  BKE_sculpt_color_layer_create_if_needed(&ob);

  /* CTX_data_ensure_evaluated_depsgraph should be used at the end to include the potential
   * creation of color layer data. */
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  BKE_sculpt_update_object_for_edit(depsgraph, &ob, true);

  filter::cache_init(C,
                     ob,
                     sd,
                     undo::Type::Color,
                     mval_fl,
                     RNA_float_get(op->ptr, "area_normal_radius"),
                     RNA_float_get(op->ptr, "strength"));
  const SculptSession &ss = *ob.runtime->sculpt_session;
  filter::Cache *filter_cache = ss.filter_cache;
  filter_cache->active_face_set = face_set_none_id;
  if (auto_mask::is_enabled(sd, ob, nullptr)) {
    auto_mask::filter_cache_ensure(*depsgraph, sd, ob);
  }

  return OPERATOR_PASS_THROUGH;
}

static wmOperatorStatus sculpt_color_filter_exec(bContext *C, wmOperator *op)
{
  Object &ob = *CTX_data_active_object(C);

  if (sculpt_color_filter_init(C, op) == OPERATOR_CANCELLED) {
    return OPERATOR_CANCELLED;
  }

  sculpt_color_filter_apply(C, op, ob);
  sculpt_color_filter_end(C, op, ob);

  return OPERATOR_FINISHED;
}

static wmOperatorStatus sculpt_color_filter_invoke(bContext *C,
                                                   wmOperator *op,
                                                   const wmEvent *event)
{
  Object &ob = *CTX_data_active_object(C);
  View3D *v3d = CTX_wm_view3d(C);
  if (v3d && v3d->shading.type == OB_SOLID) {
    v3d->shading.color_type = V3D_SHADING_VERTEX_COLOR;
  }

  RNA_int_set_array(op->ptr, "start_mouse", event->mval);

  /* Immediate execution path (used by key-bindings like `Ctrl+X`). */
  if (RNA_boolean_get(op->ptr, "use_immediate")) {
    RNA_float_set(op->ptr, "strength", 1.0f);
  }

  if (sculpt_color_filter_init(C, op) == OPERATOR_CANCELLED) {
    return OPERATOR_CANCELLED;
  }

  if (RNA_boolean_get(op->ptr, "use_immediate")) {
    sculpt_color_filter_apply(C, op, ob);
    sculpt_color_filter_end(C, op, ob);
    return OPERATOR_FINISHED;
  }

  ED_paint_brush_type_update_sticky_shading_color(C, &ob);

  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static std::string sculpt_color_filter_get_name(wmOperatorType * /*ot*/, PointerRNA *ptr)
{
  PropertyRNA *prop = RNA_struct_find_property(ptr, "type");
  const int value = RNA_property_enum_get(ptr, prop);
  const char *ui_name = nullptr;

  RNA_property_enum_name_gettexted(nullptr, ptr, prop, value, &ui_name);
  return ui_name;
}

static void sculpt_color_filter_ui(bContext * /*C*/, wmOperator *op)
{
  ui::Layout &layout = *op->layout;

  layout.prop(op->ptr, "strength", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  if (FilterType(RNA_enum_get(op->ptr, "type")) == FilterType::Fill) {
    layout.prop(op->ptr, "fill_color", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
}

namespace {

struct SculptColorGradientStaticContext {
  const Scene *scene = nullptr;
  wmOperator *op = nullptr;
  const Sculpt *sculpt = nullptr;
  Object *ob = nullptr;
  Depsgraph *depsgraph = nullptr;
  ARegion *region = nullptr;
  PaintModeSettings *paint_mode_settings = nullptr;
  Paint *paint = nullptr;
  Brush *brush = nullptr;
  bool interactive_preview = false;
};

struct SculptColorGradientSessionData {
  std::unique_ptr<gradient::session::Handle> handle;
  SculptColorGradientStaticContext static_context;
  UpdateType update_type = UpdateType::Color;
  int64_t tick_version = 0;
  bool has_last_gradient_params = false;
  gradient::Params last_gradient_params;
  double last_flush_time_seconds = -1.0;
};

static bool gradient_params_equal_with_screen_epsilon(const gradient::Params &a,
                                                      const gradient::Params &b)
{
  if (a.type != b.type || a.space != b.space || a.start_ws.x != b.start_ws.x ||
      a.start_ws.y != b.start_ws.y || a.start_ws.z != b.start_ws.z || a.end_ws.x != b.end_ws.x ||
      a.end_ws.y != b.end_ws.y || a.end_ws.z != b.end_ws.z || a.hardness != b.hardness ||
      a.clamp_to_range != b.clamp_to_range || a.curve != b.curve)
  {
    return false;
  }

  constexpr float screen_epsilon = 0.5f;
  constexpr float screen_epsilon_sq = screen_epsilon * screen_epsilon;
  const float start_delta_sq = math::length_squared(a.start_ss - b.start_ss);
  const float end_delta_sq = math::length_squared(a.end_ss - b.end_ss);
  return start_delta_sq <= screen_epsilon_sq && end_delta_sq <= screen_epsilon_sq;
}

class SculptImageGradientBackend : public gradient::session::Backend {
 public:
  bool begin_session(const gradient::session::StaticContext &static_context,
                     const gradient::session::DynamicState &dynamic_state) override
  {
    if (is_started_) {
      return true;
    }

    context_ = static_cast<const SculptColorGradientStaticContext *>(static_context.user_data);
    if (context_ == nullptr || context_->op == nullptr || context_->sculpt == nullptr ||
        context_->ob == nullptr || context_->depsgraph == nullptr || context_->region == nullptr ||
        context_->paint_mode_settings == nullptr)
    {
      return false;
    }

    if (!SCULPT_paint_image_canvas_get(
            *context_->paint_mode_settings, *context_->ob, &image_, &image_user_) ||
        image_ == nullptr || image_user_ == nullptr)
    {
      return false;
    }

    bke::pbvh::Tree *pbvh = bke::object::pbvh_get(*context_->ob);
    if (pbvh == nullptr || pbvh->type() != bke::pbvh::Type::Mesh) {
      return false;
    }

    if (!image::session::build_pixels_node_data(
            *context_->depsgraph, *context_->ob, *image_, *image_user_, pixels_node_data_))
    {
      return false;
    }

    ED_image_undo_push_begin(context_->op->type->name, PaintMode::Sculpt);
    undo_started_ = true;

    SCULPT_image_paint_push_undo_tiles(*context_->depsgraph,
                                       *context_->paint_mode_settings,
                                       *context_->ob,
                                       pixels_node_data_.mask);

    if (context_->interactive_preview) {
      MutableSpan<bke::pbvh::MeshNode> nodes = pbvh->nodes<bke::pbvh::MeshNode>();
      int64_t tile_slots_num = 0;
      for (const int node_index : pixels_node_data_.indices) {
        const bke::pbvh::pixels::NodeData &node_data = bke::pbvh::pixels::node_data_get(
            nodes[node_index]);
        tile_slots_num += node_data.tiles.size();
      }
      dirty_regions_scratch_.clear();
      dirty_regions_scratch_.reserve(
          int(std::min<int64_t>(tile_slots_num, std::numeric_limits<int>::max())));

      this->capture_baseline(*pbvh);
    }

    last_dynamic_state_ = dynamic_state;
    is_started_ = true;
    return true;
  }

  void restore_baseline(const gradient::session::DynamicState &dynamic_state) override
  {
    if (!is_started_ || image_ == nullptr || image_user_ == nullptr) {
      return;
    }
    if (baseline_rows_.is_empty()) {
      return;
    }

    last_dynamic_state_ = dynamic_state;

    if (context_->interactive_preview && !force_full_restore_ &&
        rollback_delta_prev_tick_.runs.is_empty())
    {
      /* Nothing from previous tick was applied to the image, so there is nothing to roll back. */
      return;
    }

    if (context_->interactive_preview && !force_full_restore_ &&
        rollback_delta_prev_tick_.source_tick_version >= dynamic_state.tick_version)
    {
      selective_restore_disabled_ = true;
      rollback_delta_prev_tick_.clear();
      force_full_restore_ = true;
      this->restore_baseline(dynamic_state);
      force_full_restore_ = false;
      return;
    }

    const bool use_selective_restore = context_->interactive_preview && !force_full_restore_ &&
                                       !selective_restore_disabled_ &&
                                       !rollback_delta_prev_tick_.runs.is_empty();
    bool selective_restore_anomaly = false;

    auto restore_snapshot_to_buffer =
        [&](const RowSnapshot &snapshot, ImBuf &image_buffer, const bool strict_storage_match) {
          const int pixel_offset = (int(snapshot.start_image_coordinate.y) * image_buffer.x +
                                    int(snapshot.start_image_coordinate.x)) *
                                   4;
          const int pixel_count = int(snapshot.num_pixels) * 4;

          if (image_buffer.float_buffer.data != nullptr) {
            if (snapshot.storage != RowStorage::Float4) {
              return !strict_storage_match;
            }
            std::memcpy(&image_buffer.float_buffer.data[pixel_offset],
                        snapshot.float_pixels.data(),
                        sizeof(float) * pixel_count);
            return true;
          }
          if (image_buffer.byte_buffer.data != nullptr) {
            if (snapshot.storage != RowStorage::Byte4) {
              return !strict_storage_match;
            }
            std::memcpy(&image_buffer.byte_buffer.data[pixel_offset],
                        snapshot.byte_pixels.data(),
                        sizeof(uchar) * pixel_count);
            return true;
          }
          return !strict_storage_match;
        };

    if (use_selective_restore) {
      short active_tile = std::numeric_limits<short>::min();
      ImageUser local_image_user = *image_user_;
      ImBuf *image_buffer = nullptr;

      for (const DirtyRowRun &row_run : rollback_delta_prev_tick_.runs) {
        if (row_run.rows_num <= 0 || row_run.rows_start < 0 ||
            row_run.rows_start + row_run.rows_num > baseline_rows_.size())
        {
          selective_restore_anomaly = true;
          break;
        }

        if (row_run.tile_number != active_tile) {
          image::session::release_image_buffer(*image_, image_buffer);
          active_tile = row_run.tile_number;
          if (!image::session::acquire_tile_image_buffer(
                  *image_, *image_user_, active_tile, local_image_user, image_buffer))
          {
            selective_restore_anomaly = true;
            break;
          }
        }

        const int64_t row_end = row_run.rows_start + row_run.rows_num;
        for (int64_t row_i = row_run.rows_start; row_i < row_end; row_i++) {
          const RowSnapshot &snapshot = baseline_rows_[row_i];
          if (snapshot.tile_number != row_run.tile_number ||
              !restore_snapshot_to_buffer(snapshot, *image_buffer, true))
          {
            selective_restore_anomaly = true;
            break;
          }
        }

        if (selective_restore_anomaly) {
          break;
        }
      }

      image::session::release_image_buffer(*image_, image_buffer);
    }
    else {
      short active_tile = std::numeric_limits<short>::min();
      ImageUser local_image_user = *image_user_;
      ImBuf *image_buffer = nullptr;

      for (const RowSnapshot &snapshot : baseline_rows_) {
        if (snapshot.tile_number != active_tile) {
          image::session::release_image_buffer(*image_, image_buffer);
          active_tile = snapshot.tile_number;
          if (!image::session::acquire_tile_image_buffer(
                  *image_, *image_user_, active_tile, local_image_user, image_buffer))
          {
            continue;
          }
        }
        if (image_buffer == nullptr) {
          continue;
        }

        restore_snapshot_to_buffer(snapshot, *image_buffer, false);
      }

      image::session::release_image_buffer(*image_, image_buffer);
    }

    if (selective_restore_anomaly && !force_full_restore_) {
      selective_restore_disabled_ = true;
      rollback_delta_prev_tick_.clear();
      force_full_restore_ = true;
      this->restore_baseline(dynamic_state);
      force_full_restore_ = false;
    }
  }

  void apply_preview(const gradient::session::DynamicState &dynamic_state) override
  {
    last_tick_had_updates_ = false;
    if (!is_started_ || pixels_node_data_.mask.is_empty()) {
      return;
    }

    const int gradient_type = (dynamic_state.gradient_params.type == gradient::Type::Linear) ?
                                  WPAINT_GRADIENT_TYPE_LINEAR :
                                  WPAINT_GRADIENT_TYPE_RADIAL;
    last_tick_had_updates_ = SCULPT_do_paint_brush_image_gradient(
        *context_->depsgraph,
        *context_->paint_mode_settings,
        *context_->sculpt,
        *context_->ob,
        pixels_node_data_.mask,
        context_->region,
        gradient_type,
        dynamic_state.gradient_params.start_ss,
        dynamic_state.gradient_params.end_ss,
        dynamic_state.gradient_params.hardness,
        dynamic_state.gradient_params.clamp_to_range,
        false,
        false,
        !context_->interactive_preview,
        !context_->interactive_preview,
        dynamic_state.gradient_params.clip_before_start);

    if (context_->interactive_preview) {
      if (image::session::should_mark_image_dirty_step(last_tick_had_updates_)) {
        if (!selective_restore_disabled_) {
          this->collect_dirty_regions_for_next_restore(dynamic_state.tick_version);
        }
        else {
          rollback_delta_prev_tick_.clear();
        }
        this->mark_nodes_image_dirty();
      }
      else {
        rollback_delta_prev_tick_.clear();
      }
    }
  }

  bool last_tick_had_updates() const override
  {
    return last_tick_had_updates_;
  }

  void commit() override
  {
    if (undo_started_) {
      ED_image_undo_push_end();
      undo_started_ = false;
    }
  }

  void cancel() override
  {
    force_full_restore_ = true;
    this->restore_baseline(last_dynamic_state_);
    force_full_restore_ = false;
    this->mark_nodes_image_dirty();
    if (undo_started_) {
      ED_image_undo_push_end();
      undo_started_ = false;
    }
  }

  void end_session() override
  {
    baseline_rows_.clear_and_shrink();
    rollback_delta_prev_tick_.clear_and_shrink();
    dirty_regions_scratch_.clear_and_shrink();
    tile_row_ranges_.clear_and_shrink();
    pixels_node_data_.clear_and_shrink();
    image_ = nullptr;
    image_user_ = nullptr;
    context_ = nullptr;
    is_started_ = false;
    selective_restore_disabled_ = false;
  }

 private:
  enum class RowStorage {
    Float4,
    Byte4,
  };

  struct RowSnapshot {
    short tile_number = 0;
    ushort2 start_image_coordinate = ushort2(0, 0);
    ushort num_pixels = 0;
    RowStorage storage = RowStorage::Byte4;
    Vector<float> float_pixels;
    Vector<uchar> byte_pixels;
  };

  struct DirtyRowRun {
    short tile_number = 0;
    int64_t rows_start = 0;
    int64_t rows_num = 0;
  };

  struct PreviewRollbackDelta {
    int64_t source_tick_version = std::numeric_limits<int64_t>::min();
    Vector<DirtyRowRun> runs;

    void clear()
    {
      source_tick_version = std::numeric_limits<int64_t>::min();
      runs.clear();
    }

    void clear_and_shrink()
    {
      source_tick_version = std::numeric_limits<int64_t>::min();
      runs.clear_and_shrink();
    }
  };

  struct TileRowRange {
    short tile_number = 0;
    int64_t rows_start = 0;
    int64_t rows_num = 0;
  };

  static bool row_intersects_region(const RowSnapshot &snapshot, const rcti &region)
  {
    const int row_xmin = int(snapshot.start_image_coordinate.x);
    const int row_xmax = row_xmin + int(snapshot.num_pixels) + 1;
    const int row_ymin = int(snapshot.start_image_coordinate.y);
    const int row_ymax = row_ymin + 1;

    return row_xmin < region.xmax && row_xmax > region.xmin && row_ymin < region.ymax &&
           row_ymax > region.ymin;
  }

  const TileRowRange *find_tile_row_range(const short tile_number) const
  {
    if (tile_row_ranges_.is_empty()) {
      return nullptr;
    }

    const auto it = std::lower_bound(
        tile_row_ranges_.begin(),
        tile_row_ranges_.end(),
        tile_number,
        [](const TileRowRange &item, const short value) { return item.tile_number < value; });
    if (it != tile_row_ranges_.end() && it->tile_number == tile_number) {
      return &*it;
    }
    return nullptr;
  }

  void capture_baseline(bke::pbvh::Tree &pbvh)
  {
    MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
    baseline_rows_.clear();

    int64_t baseline_rows_num = 0;
    for (const int node_index : pixels_node_data_.indices) {
      bke::pbvh::pixels::NodeData &node_data = bke::pbvh::pixels::node_data_get(nodes[node_index]);
      for (const bke::pbvh::pixels::UDIMTilePixels &tile_data : node_data.tiles) {
        baseline_rows_num += tile_data.pixel_rows.size();
      }
    }
    baseline_rows_.reserve(int(baseline_rows_num));

    for (const int node_index : pixels_node_data_.indices) {
      bke::pbvh::pixels::NodeData &node_data = bke::pbvh::pixels::node_data_get(nodes[node_index]);

      for (const bke::pbvh::pixels::UDIMTilePixels &tile_data : node_data.tiles) {
        ImageUser local_image_user = *image_user_;
        ImBuf *image_buffer = nullptr;
        if (!image::session::acquire_tile_image_buffer(
                *image_, *image_user_, tile_data.tile_number, local_image_user, image_buffer))
        {
          continue;
        }

        if (image_buffer->float_buffer.data != nullptr) {
          for (const bke::pbvh::pixels::PackedPixelRow &pixel_row : tile_data.pixel_rows) {
            RowSnapshot snapshot;
            snapshot.tile_number = tile_data.tile_number;
            snapshot.start_image_coordinate = pixel_row.start_image_coordinate;
            snapshot.num_pixels = pixel_row.num_pixels;
            snapshot.storage = RowStorage::Float4;

            const int pixel_offset = (int(pixel_row.start_image_coordinate.y) * image_buffer->x +
                                      int(pixel_row.start_image_coordinate.x)) *
                                     4;
            const int pixel_count = int(pixel_row.num_pixels) * 4;

            snapshot.float_pixels.resize(pixel_count);
            std::memcpy(snapshot.float_pixels.data(),
                        &image_buffer->float_buffer.data[pixel_offset],
                        sizeof(float) * pixel_count);

            baseline_rows_.append(std::move(snapshot));
          }
        }
        else if (image_buffer->byte_buffer.data != nullptr) {
          for (const bke::pbvh::pixels::PackedPixelRow &pixel_row : tile_data.pixel_rows) {
            RowSnapshot snapshot;
            snapshot.tile_number = tile_data.tile_number;
            snapshot.start_image_coordinate = pixel_row.start_image_coordinate;
            snapshot.num_pixels = pixel_row.num_pixels;
            snapshot.storage = RowStorage::Byte4;

            const int pixel_offset = (int(pixel_row.start_image_coordinate.y) * image_buffer->x +
                                      int(pixel_row.start_image_coordinate.x)) *
                                     4;
            const int pixel_count = int(pixel_row.num_pixels) * 4;

            snapshot.byte_pixels.resize(pixel_count);
            std::memcpy(snapshot.byte_pixels.data(),
                        &image_buffer->byte_buffer.data[pixel_offset],
                        sizeof(uchar) * pixel_count);

            baseline_rows_.append(std::move(snapshot));
          }
        }

        image::session::release_image_buffer(*image_, image_buffer);
      }
    }

    if (context_ != nullptr && context_->interactive_preview) {
      std::sort(baseline_rows_.begin(),
                baseline_rows_.end(),
                [](const RowSnapshot &a, const RowSnapshot &b) {
                  if (a.tile_number != b.tile_number) {
                    return a.tile_number < b.tile_number;
                  }
                  if (a.start_image_coordinate.y != b.start_image_coordinate.y) {
                    return a.start_image_coordinate.y < b.start_image_coordinate.y;
                  }
                  return a.start_image_coordinate.x < b.start_image_coordinate.x;
                });

      tile_row_ranges_.clear();
      if (!baseline_rows_.is_empty()) {
        short active_tile = baseline_rows_.first().tile_number;
        int64_t range_start = 0;
        for (const int64_t row_i : baseline_rows_.index_range()) {
          const short row_tile = baseline_rows_[row_i].tile_number;
          if (row_tile != active_tile) {
            TileRowRange range;
            range.tile_number = active_tile;
            range.rows_start = range_start;
            range.rows_num = row_i - range_start;
            tile_row_ranges_.append(range);

            active_tile = row_tile;
            range_start = row_i;
          }
        }

        TileRowRange range;
        range.tile_number = active_tile;
        range.rows_start = range_start;
        range.rows_num = baseline_rows_.size() - range_start;
        tile_row_ranges_.append(range);
      }
    }
  }

  void mark_nodes_image_dirty()
  {
    if (image_ == nullptr || image_user_ == nullptr || pixels_node_data_.indices.is_empty()) {
      return;
    }

    image::session::mark_pixels_node_image_dirty(
        pixels_node_data_, *context_->ob, *image_, *image_user_);
  }

  void collect_dirty_regions_for_next_restore(const int64_t source_tick_version)
  {
    rollback_delta_prev_tick_.clear();
    Vector<image::session::DirtyTileRegion> &dirty_regions = dirty_regions_scratch_;
    if (!image::session::collect_merged_dirty_tile_regions(
            pixels_node_data_, *context_->ob, dirty_regions))
    {
      return;
    }

    for (const image::session::DirtyTileRegion &dirty_tile_region : dirty_regions) {
      const TileRowRange *row_range = this->find_tile_row_range(dirty_tile_region.tile_number);
      if (row_range == nullptr) {
        selective_restore_disabled_ = true;
        rollback_delta_prev_tick_.clear();
        return;
      }

      const int64_t rows_begin = row_range->rows_start;
      const int64_t rows_end = row_range->rows_start + row_range->rows_num;
      const auto row_begin_it = baseline_rows_.begin() + rows_begin;
      const auto row_end_it = baseline_rows_.begin() + rows_end;

      const auto first_y_it = std::lower_bound(row_begin_it,
                                               row_end_it,
                                               dirty_tile_region.region.ymin,
                                               [](const RowSnapshot &snapshot, const int y) {
                                                 return int(snapshot.start_image_coordinate.y) < y;
                                               });
      const auto last_y_it = std::lower_bound(first_y_it,
                                              row_end_it,
                                              dirty_tile_region.region.ymax,
                                              [](const RowSnapshot &snapshot, const int y) {
                                                return int(snapshot.start_image_coordinate.y) < y;
                                              });

      bool has_open_run = false;
      int64_t run_start = 0;
      int64_t run_last = 0;
      for (auto row_it = first_y_it; row_it != last_y_it; ++row_it) {
        if (!row_intersects_region(*row_it, dirty_tile_region.region)) {
          if (has_open_run) {
            DirtyRowRun run;
            run.tile_number = dirty_tile_region.tile_number;
            run.rows_start = run_start;
            run.rows_num = run_last - run_start + 1;
            rollback_delta_prev_tick_.runs.append(run);
            has_open_run = false;
          }
          continue;
        }

        const int64_t row_i = int64_t(row_it - baseline_rows_.begin());
        if (!has_open_run) {
          run_start = row_i;
          run_last = row_i;
          has_open_run = true;
          continue;
        }

        if (row_i == run_last + 1) {
          run_last = row_i;
          continue;
        }

        DirtyRowRun run;
        run.tile_number = dirty_tile_region.tile_number;
        run.rows_start = run_start;
        run.rows_num = run_last - run_start + 1;
        rollback_delta_prev_tick_.runs.append(run);

        run_start = row_i;
        run_last = row_i;
      }

      if (has_open_run) {
        DirtyRowRun run;
        run.tile_number = dirty_tile_region.tile_number;
        run.rows_start = run_start;
        run.rows_num = run_last - run_start + 1;
        rollback_delta_prev_tick_.runs.append(run);
      }
    }

    if (!rollback_delta_prev_tick_.runs.is_empty()) {
      rollback_delta_prev_tick_.source_tick_version = source_tick_version;
    }
  }

  const SculptColorGradientStaticContext *context_ = nullptr;

  Image *image_ = nullptr;
  ImageUser *image_user_ = nullptr;

  bool undo_started_ = false;
  bool is_started_ = false;
  gradient::session::DynamicState last_dynamic_state_;
  image::session::PixelsNodeData pixels_node_data_;
  Vector<RowSnapshot> baseline_rows_;
  PreviewRollbackDelta rollback_delta_prev_tick_;
  Vector<image::session::DirtyTileRegion> dirty_regions_scratch_;
  Vector<TileRowRange> tile_row_ranges_;
  bool last_tick_had_updates_ = false;
  bool force_full_restore_ = false;
  bool selective_restore_disabled_ = false;
};

class SculptColorBackend : public gradient::session::Backend {
 public:
  bool begin_session(const gradient::session::StaticContext &static_context,
                     const gradient::session::DynamicState &dynamic_state) override
  {
    if (is_started_) {
      return true;
    }

    context_ = static_cast<const SculptColorGradientStaticContext *>(static_context.user_data);
    if (context_ == nullptr || context_->scene == nullptr || context_->op == nullptr ||
        context_->ob == nullptr || context_->depsgraph == nullptr || context_->region == nullptr ||
        context_->paint == nullptr || context_->brush == nullptr)
    {
      return false;
    }

    BKE_sculpt_color_layer_create_if_needed(context_->ob);
    BKE_sculpt_update_object_for_edit(context_->depsgraph, context_->ob, true);

    pbvh_ = bke::object::pbvh_get(*context_->ob);
    if (pbvh_ == nullptr || pbvh_->type() != bke::pbvh::Type::Mesh) {
      return false;
    }

    mesh_ = id_cast<Mesh *>(context_->ob->data);
    if (mesh_ == nullptr) {
      return false;
    }

    bke::GSpanAttributeWriter color_attribute = active_color_attribute_for_write(*mesh_);
    if (!color_attribute) {
      return false;
    }

    const MeshAttributeData attribute_data(*mesh_);
    const Span<float3> positions = bke::pbvh::vert_positions_eval(*context_->depsgraph,
                                                                  *context_->ob);
    const OffsetIndices<int> faces = mesh_->faces();
    const Span<int> corner_verts = mesh_->corner_verts();
    const GroupedSpan<int> vert_to_face_map = mesh_->vert_to_face_map();

    IndexMaskMemory memory;
    const IndexMask node_mask = bke::pbvh::all_leaf_nodes(*pbvh_, memory);

    undo::push_begin(*context_->scene, *context_->ob, context_->op);
    undo_started_ = true;
    undo::push_nodes(*context_->depsgraph, *context_->ob, node_mask, undo::Type::Color);

    MutableSpan<bke::pbvh::MeshNode> nodes = pbvh_->nodes<bke::pbvh::MeshNode>();
    Vector<float> factors;
    affected_node_indices_.clear();

    node_mask.foreach_index([&](const int node_index) {
      const Span<int> node_verts = nodes[node_index].verts();
      factors.resize(node_verts.size());
      fill_factor_from_hide_and_mask(
          attribute_data.hide_vert, attribute_data.mask, node_verts, factors);

      NodeSnapshot snapshot;
      snapshot.verts.reserve(node_verts.size());
      snapshot.visibility_factors.reserve(node_verts.size());
      snapshot.positions.reserve(node_verts.size());
      snapshot.baseline_colors.reserve(node_verts.size());

      for (const int i : node_verts.index_range()) {
        const float visibility_factor = factors[i];
        if (visibility_factor <= 0.0f) {
          continue;
        }

        const int vert = node_verts[i];
        snapshot.verts.append(vert);
        snapshot.visibility_factors.append(visibility_factor);
        snapshot.positions.append(positions[vert]);
        snapshot.baseline_colors.append(color_vert_get(faces,
                                                       corner_verts,
                                                       vert_to_face_map,
                                                       color_attribute.span,
                                                       color_attribute.domain,
                                                       vert));
      }

      if (!snapshot.verts.is_empty()) {
        affected_node_indices_.append(node_index);
        snapshots_.append(std::move(snapshot));
      }
    });

    color_attribute.finish();

    symmetry_ = int(SCULPT_mesh_symmetry_xyz_get(*context_->ob));
    radial_symmetry_[0] = mesh_->radial_symmetry[0];
    radial_symmetry_[1] = mesh_->radial_symmetry[1];
    radial_symmetry_[2] = mesh_->radial_symmetry[2];

    last_dynamic_state_ = dynamic_state;
    is_started_ = true;
    return true;
  }

  void restore_baseline(const gradient::session::DynamicState &dynamic_state) override
  {
    if (mesh_ == nullptr) {
      return;
    }

    last_dynamic_state_ = dynamic_state;

    bke::GSpanAttributeWriter color_attribute = active_color_attribute_for_write(*mesh_);
    if (!color_attribute) {
      return;
    }

    const OffsetIndices<int> faces = mesh_->faces();
    const Span<int> corner_verts = mesh_->corner_verts();
    const GroupedSpan<int> vert_to_face_map = mesh_->vert_to_face_map();

    for (const NodeSnapshot &snapshot : snapshots_) {
      for (const int i : snapshot.verts.index_range()) {
        color_vert_set(faces,
                       corner_verts,
                       vert_to_face_map,
                       color_attribute.domain,
                       snapshot.verts[i],
                       snapshot.baseline_colors[i],
                       color_attribute.span);
      }
    }

    this->tag_color_dirty();
    color_attribute.finish();
  }

  void apply_preview(const gradient::session::DynamicState &dynamic_state) override
  {
    if (mesh_ == nullptr) {
      return;
    }

    const std::unique_ptr<gradient::Calculator> calculator = gradient::create(
        dynamic_state.gradient_params);
    const float3 brush_color = BKE_brush_color_get(context_->paint, context_->brush);
    const float brush_alpha = context_->brush->alpha;
    const bool clamp_to_range = dynamic_state.gradient_params.clamp_to_range;

    bke::GSpanAttributeWriter color_attribute = active_color_attribute_for_write(*mesh_);
    if (!color_attribute) {
      return;
    }

    const OffsetIndices<int> faces = mesh_->faces();
    const Span<int> corner_verts = mesh_->corner_verts();
    const GroupedSpan<int> vert_to_face_map = mesh_->vert_to_face_map();

    for (const NodeSnapshot &snapshot : snapshots_) {
      for (const int i : snapshot.verts.index_range()) {
        float factor = paint_projected_gradient_factor_with_symmetry(
            context_->region, *calculator, snapshot.positions[i], symmetry_, radial_symmetry_);
        factor = paint_gradient_finalize_factor(
            *context_->brush, factor, clamp_to_range, brush_alpha, true);
        factor *= snapshot.visibility_factors[i];

        if (factor <= 0.0f) {
          continue;
        }

        float4 color = snapshot.baseline_colors[i];
        for (const int channel : IndexRange(3)) {
          color[channel] = interpf(brush_color[channel], color[channel], factor);
        }

        color_vert_set(faces,
                       corner_verts,
                       vert_to_face_map,
                       color_attribute.domain,
                       snapshot.verts[i],
                       color,
                       color_attribute.span);
      }
    }

    this->tag_color_dirty();
    color_attribute.finish();
  }

  void commit() override
  {
    if (undo_started_) {
      undo::push_end(*context_->ob);
      undo_started_ = false;
    }
  }

  void cancel() override
  {
    this->restore_baseline(last_dynamic_state_);

    if (undo_started_) {
      undo::push_end_ex(*context_->ob, true);
      undo_started_ = false;
    }
  }

  void end_session() override
  {
    snapshots_.clear_and_shrink();
    affected_node_indices_.clear_and_shrink();
    context_ = nullptr;
    is_started_ = false;
  }

 private:
  struct NodeSnapshot {
    Vector<int> verts;
    Vector<float> visibility_factors;
    Vector<float3> positions;
    Vector<float4> baseline_colors;
  };

  void tag_color_dirty()
  {
    if (mesh_ == nullptr || affected_node_indices_.is_empty()) {
      return;
    }

    bke::pbvh::Tree *pbvh = bke::object::pbvh_get(*context_->ob);
    if (pbvh == nullptr || pbvh->type() != bke::pbvh::Type::Mesh) {
      return;
    }

    IndexMaskMemory memory;
    const IndexMask node_mask = IndexMask::from_indices(affected_node_indices_.as_span(), memory);
    pbvh->tag_attribute_changed(node_mask, mesh_->active_color_attribute);
  }

  const SculptColorGradientStaticContext *context_ = nullptr;

  bke::pbvh::Tree *pbvh_ = nullptr;
  Mesh *mesh_ = nullptr;

  int symmetry_ = 0;
  int8_t radial_symmetry_[3] = {0, 0, 0};

  bool undo_started_ = false;
  bool is_started_ = false;
  gradient::session::DynamicState last_dynamic_state_;
  Vector<int> affected_node_indices_;
  Vector<NodeSnapshot> snapshots_;
};

static wmGesture *sculpt_color_gradient_gesture(wmOperator *op)
{
  return static_cast<wmGesture *>(op->customdata);
}

static SculptColorGradientSessionData *sculpt_color_gradient_session_data_ensure(
    const Scene &scene,
    wmOperator *op,
    Object &ob,
    Depsgraph &depsgraph,
    ARegion &region,
    const Sculpt &sculpt,
    PaintModeSettings &paint_mode_settings,
    Paint &paint,
    Brush &brush,
    const bool use_image_backend)
{
  wmGesture *gesture = sculpt_color_gradient_gesture(op);
  if (gesture == nullptr) {
    return nullptr;
  }

  SculptColorGradientSessionData *session_data = static_cast<SculptColorGradientSessionData *>(
      gesture->user_data.data);
  if (session_data != nullptr) {
    return session_data;
  }

  session_data = MEM_new<SculptColorGradientSessionData>(__func__);
  gesture->user_data.data = session_data;
  gesture->user_data.use_free = false;

  session_data->static_context.scene = &scene;
  session_data->static_context.op = op;
  session_data->static_context.sculpt = &sculpt;
  session_data->static_context.ob = &ob;
  session_data->static_context.depsgraph = &depsgraph;
  session_data->static_context.region = &region;
  session_data->static_context.paint_mode_settings = &paint_mode_settings;
  session_data->static_context.paint = &paint;
  session_data->static_context.brush = &brush;
  session_data->static_context.interactive_preview = true;

  std::unique_ptr<gradient::session::Backend> backend;
  if (use_image_backend) {
    backend = std::make_unique<SculptImageGradientBackend>();
    session_data->update_type = UpdateType::Image;
  }
  else {
    backend = std::make_unique<SculptColorBackend>();
    session_data->update_type = UpdateType::Color;
  }

  gradient::session::StaticContext static_context;
  static_context.user_data = &session_data->static_context;
  session_data->handle = std::make_unique<gradient::session::Handle>(std::move(backend),
                                                                     static_context);
  return session_data;
}

enum class SessionFinishType {
  Commit,
  Cancel,
};

static void sculpt_color_gradient_session_finish(bContext *C,
                                                 wmOperator *op,
                                                 const SessionFinishType finish_type)
{
  wmGesture *gesture = sculpt_color_gradient_gesture(op);
  if (gesture == nullptr) {
    return;
  }

  SculptColorGradientSessionData *session_data = static_cast<SculptColorGradientSessionData *>(
      gesture->user_data.data);
  if (session_data == nullptr) {
    return;
  }

  if (session_data->handle != nullptr) {
    if (finish_type == SessionFinishType::Commit) {
      session_data->handle->commit();
    }
    else {
      session_data->handle->cancel();
    }

    Object &ob = *CTX_data_active_object(C);
    flush_update_done(C, ob, session_data->update_type);
  }

  MEM_delete(session_data);
  gesture->user_data.data = nullptr;
  gesture->user_data.use_free = false;
}

}  // namespace

static wmOperatorStatus sculpt_color_gradient_exec(bContext *C, wmOperator *op)
{
  const Scene &scene = *CTX_data_scene(C);
  Object &ob = *CTX_data_active_object(C);
  ARegion *region = CTX_wm_region(C);
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  if (region == nullptr || region->regiondata == nullptr) {
    return OPERATOR_CANCELLED;
  }

  const View3D *v3d = CTX_wm_view3d(C);
  const Base *base = CTX_data_active_base(C);
  if (!BKE_base_is_visible(v3d, base)) {
    return OPERATOR_CANCELLED;
  }

  ToolSettings *ts = CTX_data_tool_settings(C);
  Brush *brush = BKE_paint_brush(&ts->sculpt->paint);
  if (brush == nullptr) {
    return OPERATOR_CANCELLED;
  }

  PaintModeSettings &paint_mode_settings = ts->paint_mode;

  BKE_curvemapping_init(brush->curve_distance_falloff);

  const int x_start = RNA_int_get(op->ptr, "xstart");
  const int y_start = RNA_int_get(op->ptr, "ystart");
  const int x_end = RNA_int_get(op->ptr, "xend");
  const int y_end = RNA_int_get(op->ptr, "yend");
  const int gradient_type_rna = RNA_enum_get(op->ptr, "type");
  const float hardness = RNA_float_get(op->ptr, "hardness");
  const bool clamp_to_range = RNA_boolean_get(op->ptr, "clamp_to_range");
  const bool clip_before_start = RNA_boolean_get(op->ptr, "clip_before_start");
  const bool is_interactive = sculpt_color_gradient_gesture(op) != nullptr;
  const bool use_image_backend = SCULPT_use_image_paint_brush(paint_mode_settings, ob);

  ed::sculpt_paint::gradient::Params gradient_params;
  gradient_params.type = (gradient_type_rna == WPAINT_GRADIENT_TYPE_LINEAR) ?
                             ed::sculpt_paint::gradient::Type::Linear :
                             ed::sculpt_paint::gradient::Type::Radial;
  /* This operator currently evaluates projected screen-space points.
   * Keep unsupported spaces as screen fallback for now. */
  gradient_params.space = ed::sculpt_paint::gradient::Space::Screen;
  gradient_params.start_ss = float2(float(x_start), float(y_start));
  gradient_params.end_ss = float2(float(x_end), float(y_end));
  gradient_params.hardness = hardness;
  gradient_params.clamp_to_range = clamp_to_range;
  gradient_params.curve = nullptr;
  gradient_params.clip_before_start = clip_before_start;
  ED_view3d_init_mats_rv3d(&ob, static_cast<RegionView3D *>(region->regiondata));

  gradient::session::DynamicState dynamic_state;
  dynamic_state.gradient_params = gradient_params;
  bool did_apply = false;
  SculptColorGradientSessionData *session_data = nullptr;

  if (is_interactive) {
    session_data = sculpt_color_gradient_session_data_ensure(scene,
                                                             op,
                                                             ob,
                                                             *depsgraph,
                                                             *region,
                                                             *ts->sculpt,
                                                             paint_mode_settings,
                                                             ts->sculpt->paint,
                                                             *brush,
                                                             use_image_backend);
    if (session_data == nullptr || session_data->handle == nullptr) {
      return OPERATOR_CANCELLED;
    }

    if (session_data->has_last_gradient_params &&
        gradient_params_equal_with_screen_epsilon(session_data->last_gradient_params,
                                                  gradient_params))
    {
      return OPERATOR_FINISHED;
    }

    dynamic_state.tick_version = ++session_data->tick_version;
    if (!session_data->handle->tick(dynamic_state)) {
      sculpt_color_gradient_session_finish(C, op, SessionFinishType::Cancel);
      return OPERATOR_CANCELLED;
    }

    session_data->has_last_gradient_params = true;
    session_data->last_gradient_params = gradient_params;
    did_apply = session_data->handle->last_tick_had_updates();
  }
  else {
    SculptColorGradientStaticContext static_context_payload;
    static_context_payload.scene = &scene;
    static_context_payload.op = op;
    static_context_payload.sculpt = ts->sculpt;
    static_context_payload.ob = &ob;
    static_context_payload.depsgraph = depsgraph;
    static_context_payload.region = region;
    static_context_payload.paint_mode_settings = &paint_mode_settings;
    static_context_payload.paint = &ts->sculpt->paint;
    static_context_payload.brush = brush;
    static_context_payload.interactive_preview = false;

    std::unique_ptr<gradient::session::Backend> backend;
    if (use_image_backend) {
      backend = std::make_unique<SculptImageGradientBackend>();
    }
    else {
      backend = std::make_unique<SculptColorBackend>();
    }
    gradient::session::StaticContext static_context;
    static_context.user_data = &static_context_payload;
    gradient::session::Handle session(std::move(backend), static_context);
    dynamic_state.tick_version = 1;
    if (!session.tick(dynamic_state)) {
      return OPERATOR_CANCELLED;
    }
    session.commit();
    did_apply = session.last_tick_had_updates();
  }

  if (!did_apply) {
    return OPERATOR_FINISHED;
  }

  const UpdateType update_type = use_image_backend ? UpdateType::Image : UpdateType::Color;
  bool did_flush = false;
  if (is_interactive) {
    BLI_assert(session_data != nullptr);
    if (!use_image_backend) {
      flush_update_step(C, update_type);
      did_flush = true;
    }
    else {
      constexpr double preview_flush_interval_seconds = 1.0 / 120.0;
      const double now_seconds = BLI_time_now_seconds();
      if (image::session::should_flush_preview_step(
              now_seconds, preview_flush_interval_seconds, session_data->last_flush_time_seconds))
      {
        flush_update_step(C, update_type);
        did_flush = true;
      }
    }
  }
  else {
    flush_update_step(C, update_type);
    flush_update_done(C, ob, update_type);
    did_flush = true;
  }

  const int64_t debug_tick_version = is_interactive ?
                                         (session_data != nullptr ? session_data->tick_version :
                                                                    1) :
                                         dynamic_state.tick_version;
  debug_log_gradient_step(
      is_interactive, use_image_backend, debug_tick_version, did_apply, did_flush);

  return OPERATOR_FINISHED;
}

static wmOperatorStatus sculpt_color_gradient_invoke(bContext *C,
                                                     wmOperator *op,
                                                     const wmEvent *event)
{
  wmOperatorStatus ret = WM_gesture_straightline_invoke(C, op, event);
  if (ret & OPERATOR_RUNNING_MODAL) {
    ARegion *region = CTX_wm_region(C);
    if (region->regiontype == RGN_TYPE_WINDOW) {
      /* TODO: hard-coded, extend `WM_gesture_straightline_*`. */
      if (event->type == LEFTMOUSE && event->val == KM_PRESS) {
        wmGesture *gesture = static_cast<wmGesture *>(op->customdata);
        gesture->is_active = true;
      }
    }
  }
  return ret;
}

static wmOperatorStatus sculpt_color_gradient_modal(bContext *C,
                                                    wmOperator *op,
                                                    const wmEvent *event)
{
  wmOperatorStatus ret = WM_gesture_straightline_modal(C, op, event);

  if (ret & OPERATOR_RUNNING_MODAL) {
    if (event->type == LEFTMOUSE && event->val == KM_RELEASE) { /* XXX, hardcoded */
      sculpt_color_gradient_session_finish(C, op, SessionFinishType::Commit);
      WM_gesture_straightline_cancel(C, op);
      ret &= ~OPERATOR_RUNNING_MODAL;
      ret |= OPERATOR_FINISHED;
    }
  }

  if (ret & OPERATOR_FINISHED) {
    sculpt_color_gradient_session_finish(C, op, SessionFinishType::Commit);
  }
  else if (ret & OPERATOR_CANCELLED) {
    sculpt_color_gradient_session_finish(C, op, SessionFinishType::Cancel);
  }

  return ret;
}

static void sculpt_color_gradient_cancel(bContext *C, wmOperator *op)
{
  sculpt_color_gradient_session_finish(C, op, SessionFinishType::Cancel);
  WM_gesture_straightline_cancel(C, op);
}

void SCULPT_OT_color_gradient(wmOperatorType *ot)
{
  ot->name = "Color Gradient";
  ot->idname = "SCULPT_OT_color_gradient";
  ot->description = "Draw a line to apply a color gradient in Sculpt Mode";

  ot->invoke = sculpt_color_gradient_invoke;
  ot->modal = sculpt_color_gradient_modal;
  ot->exec = sculpt_color_gradient_exec;
  ot->poll = SCULPT_mode_poll_view3d;
  ot->cancel = sculpt_color_gradient_cancel;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_DEPENDS_ON_CURSOR;

  paint_gradient_operator_properties(ot, WPAINT_GRADIENT_TYPE_LINEAR, PAINT_GRADIENT_SPACE_SCREEN);

  WM_operator_properties_gesture_straightline(ot, WM_CURSOR_EDIT);
}

void SCULPT_OT_color_filter(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Filter Color";
  ot->idname = "SCULPT_OT_color_filter";
  ot->description = "Applies a filter to modify the active color attribute";

  /* API callbacks. */
  ot->invoke = sculpt_color_filter_invoke;
  ot->exec = sculpt_color_filter_exec;
  ot->modal = sculpt_color_filter_modal;
  ot->poll = SCULPT_mode_poll;
  ot->ui = sculpt_color_filter_ui;
  ot->get_name = sculpt_color_filter_get_name;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* rna */
  filter::register_operator_props(ot);

  RNA_def_enum(
      ot->srna, "type", prop_color_filter_types, int(FilterType::Fill), "Filter Type", "");

  PropertyRNA *prop = RNA_def_float_color(ot->srna,
                                          "fill_color",
                                          3,
                                          fill_filter_default_color,
                                          0.0f,
                                          FLT_MAX,
                                          "Fill Color",
                                          "",
                                          0.0f,
                                          1.0f);
  RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_ID_MESH);
  RNA_def_property_subtype(prop, PROP_COLOR);

  prop = RNA_def_boolean(ot->srna,
                         "use_immediate",
                         false,
                         "Immediate Apply",
                         "Apply once without entering modal interaction");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);

  prop = RNA_def_boolean(ot->srna, "use_secondary_color", false, "Use Secondary Color", "");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}

}  // namespace blender::ed::sculpt_paint::color
