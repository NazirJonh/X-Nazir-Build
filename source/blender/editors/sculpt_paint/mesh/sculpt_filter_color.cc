/* SPDX-FileCopyrightText: 2020 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "MEM_guardedalloc.h"

#include "BLI_bounds.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_map.hh"
#include "BLI_math_base.h"
#include "BLI_math_color.h"
#include "BLI_math_color_blend.h"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_task.hh"
#include "BLI_time.h"

#include "BLT_translation.hh"

#include "BKE_attribute.hh"
#include "BKE_brush.hh"
#include "BKE_bvhutils.hh"
#include "BKE_colorband.hh"
#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_image_paint_selection.hh"
#include "BKE_layer.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_paint_bvh_pixels.hh"
#include "BKE_paint_types.hh"
#include "BKE_report.hh"
#include "BKE_unit.hh"

#include "GPU_immediate.hh"
#include "GPU_immediate_util.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_space_api.hh"
#include "ED_view3d.hh"

#include "BKE_mesh_mapping.hh"
#include "BKE_screen.hh"

#include "../paint_intern.hh"
#include "../paint_gradient_curve.hh"

#include "../selection/paint_image_select_gradient.hh"
#include "../selection/paint_image_select_intern.hh"

#include "mesh_brush_common.hh"
#include "sculpt_automask.hh"
#include "sculpt_color.hh"
#include "sculpt_filter.hh"
#include "sculpt_geodesic.hh"
#include "sculpt_intern.hh"
#include "sculpt_smooth.hh"
#include "sculpt_undo.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>

#include <fmt/format.h>

namespace blender::ed::sculpt_paint::color {

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
                              const FaceSelectionMask &face_selection_mask,
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
  filter_factors_with_face_selection(face_selection_mask, verts, factors);
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
                                   verts[i],
                                   face_selection_mask.select_poly);
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
      smooth::neighbor_color_average(face_selection_mask.vert_paintable.as_span(),
                                     face_selection_mask.select_poly,
                                     faces,
                                     corner_verts,
                                     vert_to_face_map,
                                     color_attribute.span,
                                     color_attribute.domain,
                                     colors,
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
                   color_attribute.span,
                   face_selection_mask.select_poly);
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
  const MeshAttributeData attribute_data(mesh);
  const FaceSelectionMask &face_selection_mask = face_selection_mask_ensure(object);
  const bke::GAttributeReader color_attribute = active_color_attribute(mesh);
  const GVArraySpan colors = *color_attribute;

  if (ss.filter_cache->pre_smoothed_color.is_empty()) {
    ss.filter_cache->pre_smoothed_color = Array<float4>(mesh.verts_num);
  }
  const MutableSpan<float4> pre_smoothed_color = ss.filter_cache->pre_smoothed_color;

  node_mask.foreach_index(
      [&](const int i) {
        for (const int vert : nodes[i].verts()) {
          pre_smoothed_color[vert] = color_vert_get(faces,
                                                    corner_verts,
                                                    vert_to_face_map,
                                                    colors,
                                                    color_attribute.domain,
                                                    vert,
                                                    face_selection_mask.select_poly);
        }
      },
      exec_mode::grain_size(1));

  struct LocalData {
    Vector<int> neighbor_offsets;
    Vector<int> neighbor_data;
    Vector<float4> averaged_colors;
  };
  threading::EnumerableThreadSpecific<LocalData> all_tls;
  /* The pre-smoothing averages over the vertex 1-ring topology. With the face selection masking
   * active, neighbors without a selected face are skipped (and the average renormalized): their
   * masked colors read as zero and would darken the selection boundary. Without the masking the
   * plain topology average is used. Only the negative-strength unsharp path consumes this data. */
  const bool filter = face_selection_mask.state == FaceSelectionState::Active;
  const Span<bool> vert_paintable = filter ? face_selection_mask.vert_paintable.as_span() :
                                             Span<bool>();
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
          if (filter) {
            for (const int i : verts.index_range()) {
              float4 sum(0);
              int valid = 0;
              for (const int neighbor : neighbors[i]) {
                if (!vert_paintable[neighbor]) {
                  continue;
                }
                sum += pre_smoothed_color[neighbor];
                valid++;
              }
              averaged_colors[i] = valid > 0 ? sum / float(valid) :
                                               pre_smoothed_color[verts[i]];
            }
          }
          else {
            smooth::neighbor_data_average_mesh(
                pre_smoothed_color.as_span(), neighbors, averaged_colors);
          }

          for (const int i : verts.index_range()) {
            pre_smoothed_color[verts[i]] = math::interpolate(
                pre_smoothed_color[verts[i]], averaged_colors[i], 0.5f);
          }
        },
        exec_mode::grain_size(1));
  }
}

static void sculpt_color_filter_apply_object(bContext *C, wmOperator *op, Object &ob)
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
  bke::GSpanAttributeWriter color_attribute = active_color_attribute_for_write(mesh);
  if (!color_attribute) {
    /* Objects that could not be synchronized to the shared channel have no valid active color
     * attribute; skip them entirely (mirrors the brush path's early return). */
    return;
  }
  const FaceSelectionMask &face_selection_mask = face_selection_mask_ensure(ob);
  if (filter_strength < 0.0 && ss.filter_cache->pre_smoothed_color.is_empty()) {
    sculpt_color_presmooth_init(mesh, ob);
  }

  const IndexMask &node_mask = ss.filter_cache->node_mask;
  if (auto_mask::is_enabled(sd.paint, ob, nullptr) && ss.filter_cache->automasking &&
      ss.filter_cache->automasking->settings.flags & BRUSH_AUTOMASKING_CAVITY_ALL)
  {
    ss.filter_cache->automasking->calc_cavity_factor(depsgraph, ob, node_mask);
  }

  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
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
                          face_selection_mask,
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
}

static void sculpt_color_filter_apply(bContext *C, wmOperator *op)
{
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
  const Vector<Object *> objects = sculpt_mode_objects(vc);
  for (Object *ob : objects) {
    /* Objects that could not be synchronized to the shared channel have no valid active color
     * attribute; their per-object body is a cheap no-op (empty attribute writer). */
    sculpt_color_filter_apply_object(C, op, *ob);
    flush_update_step(vc, *ob, UpdateType::Color);
  }
}

static void sculpt_color_filter_end(bContext *C, wmOperator *op)
{
  if (FilterType(RNA_enum_get(op->ptr, "type")) == FilterType::Fill &&
      !RNA_struct_property_is_set(op->ptr, "fill_color"))
  {
    fill_color_store_current(C, op);
  }

  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
  const Vector<Object *> objects = sculpt_mode_objects(vc);

  undo::push_end_all_ex(false, true);

  for (Object *ob : objects) {
    SculptSession &ss = *ob->runtime->sculpt_session;
    if (ss.filter_cache) {
      MEM_delete(ss.filter_cache);
      ss.filter_cache = nullptr;
    }
    flush_update_done(C, *ob, UpdateType::Color);
    WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
  }
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
      sculpt_color_filter_apply(C, op);
    }

    sculpt_color_filter_end(C, op);
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

  sculpt_color_filter_apply(C, op);

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

  const bool use_automasking = auto_mask::is_enabled(sd.paint, ob, nullptr);
  if (use_automasking) {
    if (v3d) {
      /* Update the active face set manually as the paint cursor is not enabled when using the Mesh
       * Filter Tool. */
      cursor_geometry_info_update(C, mval_fl, false);
    }
  }

  /* Disable for multires and dyntopo for now */
  if (!color_supported_check(scene, ob, op->reports)) {
    return OPERATOR_CANCELLED;
  }

  ViewContext vc = ED_view3d_viewcontext_init(C, CTX_data_depsgraph_pointer(C));
  const Vector<Object *> objects = sculpt_mode_objects(vc);

  /* Ensure that we have a PBVH to be able to push changes on only visible nodes. */
  for (Object *object : objects) {
    bke::object::pbvh_ensure(*CTX_data_ensure_evaluated_depsgraph(C), *object);
  }

  undo::push_begin_multi_object(scene, op, objects);
  ensure_shared_color_attributes(ob, objects);

  /* CTX_data_ensure_evaluated_depsgraph should be used at the end to include the potential
   * creation of color layer data. */
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  for (Object *object : objects) {
    BKE_sculpt_update_object_for_edit(depsgraph, object, true);

    /* NOTE: The filter cache is built for every object: the modal handler reads the active
     * object's cache unconditionally, and the apply pass reads the cached #FaceSelectionMask
     * (only #FaceSelectionState::Active filters any vertex). */
    filter::cache_init(C,
                       *object,
                       sd,
                       undo::NodeDataFlag::Color,
                       mval_fl,
                       RNA_float_get(op->ptr, "area_normal_radius"),
                       RNA_float_get(op->ptr, "strength"));
    /* Build the face selection masking state once here, so the per-step apply pass reads it from
     * the filter cache instead of re-deriving it on every step. */
    face_selection_mask_ensure(*object);
    const SculptSession &ss = *object->runtime->sculpt_session;
    filter::Cache *filter_cache = ss.filter_cache;
    filter_cache->active_face_set = face_set_none_id;
    if (auto_mask::is_enabled(sd.paint, *object, nullptr)) {
      auto_mask::filter_cache_ensure(*depsgraph, sd.paint, *object);
    }
  }

  return OPERATOR_PASS_THROUGH;
}

static wmOperatorStatus sculpt_color_filter_exec(bContext *C, wmOperator *op)
{
  if (sculpt_color_filter_init(C, op) == OPERATOR_CANCELLED) {
    return OPERATOR_CANCELLED;
  }

  sculpt_color_filter_apply(C, op);
  sculpt_color_filter_end(C, op);

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
    sculpt_color_filter_apply(C, op);
    sculpt_color_filter_end(C, op);
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
  ot->poll = sculpt_mode_poll;
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

/* -------------------------------------------------------------------- */
/** \name Sculpt Color Gradient Operator (Phase 2)
 *
 * `SCULPT_OT_color_gradient`: gesture that blends a gradient (primary -> secondary color, or the
 * tool's color ramp, see #Sculpt::gradient_color_source) over the active color attribute, or
 * over every enabled Image/Material paint canvas target. Linear gradients follow the screen-space
 * drag line; radial ones lie on the surface around the point under the drag start (see
 * #GradientGeometry); curve gradients follow a hand-drawn polyline raycast onto the surface
 * (see #gradient_curve::GradientCurve3D and the Curve gesture section below). Painting honors
 * the face selection mask.
 * `SCULPT_use_image_paint_brush` decides, per object, which of the two applies (see
 * #sculpt_color_gradient_apply and #sculpt_color_gradient_apply_image_object).
 *
 * Lifetime (driven by `WM_gesture_straightline_*`, or by the Curve gesture's own modal):
 * - `invoke`: opens exactly one undo step for the whole gesture -- sculpt-undo
 *   (`push_begin_multi_object`) for a color-attribute selection, or Image-undo
 *   (`ED_image_undo_push_begin`) when any object uses an image/Material canvas; the two cannot
 *   both be open at once (see #sculpt_color_gradient_uses_image_undo) -- then hands control to
 *   `WM_gesture_straightline_invoke` or the Curve modal.
 * - `exec`: called by the gesture on every mouse move (live preview) and once more
 *   on release (final commit). The color-attribute path always blends from the undo snapshot
 *   (`orig_color_data_lookup_mesh`), so repeated previews are idempotent; `push_nodes(...,
 *   Color)`/`do_push_undo_tile` only store a node/tile the first time, preserving the
 *   pre-stroke original for the whole drag.
 * - `modal`: delegates to `WM_gesture_straightline_modal` (or the Curve modal); on FINISHED
 *   closes the undo step, on CANCELLED restores originals and discards the step.
 * - `cancel`: same restore + discard path for external cancellation.
 *
 * A selection that mixes both canvas types in one gesture is not supported: whichever objects
 * do not match the gesture's chosen undo system are skipped and reported, rather than risking a
 * push into a system with no active step for them (see #sculpt_color_gradient_apply).
 * \{ */

/** Everything that maps a gradient parameter to a color, resolved once per apply call. */
struct GradientColoring {
  eSculptGradientColorSource source = SCULPT_GRADIENT_COLOR_SOURCE_COLORS;
  /** #Sculpt::gradient_colorband, for #SCULPT_GRADIENT_COLOR_SOURCE_RAMP. */
  const ColorBand *colorband = nullptr;
  /**
   * Two-stop ramp built from the start / end colors for #SCULPT_GRADIENT_COLOR_SOURCE_COLORS, so
   * that source gets the same Color Mode and Interpolation settings as the ramp.
   */
  ColorBand colors_band;
  eImagePaint_GradientRepeat repeat = IMAGE_PAINT_GRADIENT_REPEAT_NONE;
  IMB_BlendMode blend_mode = IMB_BLEND_MIX;
  float opacity = 1.0f;
  /** Where along the gradient the halfway color lands, adjusted with the mouse wheel. */
  float midpoint = 0.5f;
};

static GradientColoring sculpt_color_gradient_coloring_get(const bContext *C, wmOperator *op)
{
  const Sculpt &sd = *CTX_data_tool_settings(C)->sculpt;
  GradientColoring coloring;
  coloring.source = eSculptGradientColorSource(sd.gradient_color_source);
  coloring.colorband = &sd.gradient_colorband;
  if (coloring.colorband->tot == 0) {
    /* Never evaluate a ramp without stops (not initialized yet); fall back to the colors. */
    coloring.source = SCULPT_GRADIENT_COLOR_SOURCE_COLORS;
  }

  ColorBand &band = coloring.colors_band;
  band.tot = 2;
  band.color_mode = sd.gradient_colorband.color_mode;
  band.ipotype = sd.gradient_colorband.ipotype;
  band.ipotype_hue = sd.gradient_colorband.ipotype_hue;
  const float *stop_colors[2] = {sd.gradient_color, sd.gradient_secondary_color};
  for (const int i : IndexRange(2)) {
    CBData &stop = band.data[i];
    stop.r = stop_colors[i][0];
    stop.g = stop_colors[i][1];
    stop.b = stop_colors[i][2];
    stop.a = stop_colors[i][3];
    stop.pos = float(i);
  }
  coloring.repeat = eImagePaint_GradientRepeat(sd.gradient_repeat);
  coloring.blend_mode = IMB_BlendMode(sd.gradient_blend_mode);
  coloring.opacity = sd.gradient_opacity;
  coloring.midpoint = RNA_float_get(op->ptr, "midpoint");
  return coloring;
}

/** Map the unbounded gradient parameter into `[0, 1]` according to the repeat mode. */
static float gradient_repeat_apply_raw(const eImagePaint_GradientRepeat repeat, const float t)
{
  switch (repeat) {
    case IMAGE_PAINT_GRADIENT_REPEAT_REPEAT:
      return t - std::floor(t);
    case IMAGE_PAINT_GRADIENT_REPEAT_REFLECT: {
      const float m = std::fmod(std::abs(t), 2.0f);
      return (m > 1.0f) ? 2.0f - m : m;
    }
    case IMAGE_PAINT_GRADIENT_REPEAT_NONE:
      break;
  }
  return math::clamp(t, 0.0f, 1.0f);
}

/** The ramp position of the gradient parameter: repeat first, then the midpoint bend (the same
 * order as the Image Editor gradient). */
static float gradient_ramp_position(const GradientColoring &coloring, const float t)
{
  return image_paint_gradient_remap_midpoint(gradient_repeat_apply_raw(coloring.repeat, t),
                                             coloring.midpoint);
}

/** Straight-alpha gradient color at parameter \a t, alpha already scaled by the opacity. */
static float4 gradient_color_at(const GradientColoring &coloring, const float t)
{
  const float t_repeat = gradient_ramp_position(coloring, t);
  float4 color;
  const ColorBand *band = (coloring.source == SCULPT_GRADIENT_COLOR_SOURCE_RAMP) ?
                              coloring.colorband :
                              &coloring.colors_band;
  BKE_colorband_evaluate(band, t_repeat, color);
  color.w *= coloring.opacity;
  return color;
}

/** Composite the gradient color at \a t over \a orig, keeping the original alpha. */
static float4 gradient_blend_color(const GradientColoring &coloring,
                                   const float4 &orig,
                                   const float t)
{
  float4 color = gradient_color_at(coloring, t);
  /* The blend functions expect a premultiplied source: with a straight color the full color would
   * be added at any non-zero alpha, turning a fade to transparent into a hard step. */
  color.x *= color.w;
  color.y *= color.w;
  color.z *= color.w;
  float4 result;
  IMB_blend_color_float(result, orig, color, coloring.blend_mode);
  result.w = orig.w;
  return result;
}

/**
 * Scalar and normal material channels have no meaningful mapping from a color ramp: they get the
 * channel's own value at the gradient start, fading back to the original at the end.
 */
static float4 gradient_blend_value(const GradientColoring &coloring,
                                   const float4 &orig,
                                   const float4 &value,
                                   const float t)
{
  const float weight = (1.0f - gradient_ramp_position(coloring, t)) * coloring.opacity;
  float4 result = orig;
  result.x = orig.x + (value.x - orig.x) * weight;
  result.y = orig.y + (value.y - orig.y) * weight;
  result.z = orig.z + (value.z - orig.z) * weight;
  return result;
}

/**
 * The gesture's shape. Linear is a screen-space line; Radial is a world-space disc on the surface:
 * centered on the point under the drag start, lying in the plane of its sampled normal (like a
 * brush), with the radius reaching the point of that plane under the cursor. Curve is a
 * world-space polyline projected onto the surface while it is drawn (see
 * #GradientCurve3D), reused by both apply paths through the shared pointer.
 */
struct GradientGeometry {
  eSculptGradientType type = SCULPT_GRADIENT_LINEAR;

  float2 start_ss = float2(0.0f);
  float2 axis_ss = float2(0.0f);
  float inv_axis_len_sq = 0.0f;

  float3 center = float3(0.0f);
  float3 normal = float3(0.0f, 0.0f, 1.0f);
  float radius = 0.0f;

  /* Curve gradient (`SCULPT_GRADIENT_CURVE`). */
  std::shared_ptr<const gradient_curve::GradientCurve3D> curve;
  /** #eGradientCurveMode */
  eGradientCurveMode curve_mode = GRADIENT_CURVE_MODE_ALONG;
  /** Influence limit around the curve in world units (0: unlimited for Along). */
  float curve_width = 0.0f;
  /** #eSculptGradientCurveDistance */
  eSculptGradientCurveDistance curve_distance = SCULPT_GRADIENT_CURVE_EUCLIDEAN;

  /** False for a zero-length drag or a degenerate curve, which paint nothing. */
  bool is_valid() const
  {
    switch (type) {
      case SCULPT_GRADIENT_LINEAR:
        return inv_axis_len_sq > 0.0f;
      case SCULPT_GRADIENT_RADIAL:
        return radius > 1e-6f;
      case SCULPT_GRADIENT_CURVE:
        return curve != nullptr && curve->is_valid();
    }
    return false;
  }
};

/** World-space distance from the radial center to the point of its plane under \a mval. */
static float gradient_radial_radius(const ARegion *region,
                                    const View3D *v3d,
                                    const float3 &center,
                                    const float3 &normal,
                                    const float2 &mval)
{
  float4 plane;
  plane_from_point_normal_v3(plane, center, normal);
  float3 on_plane;
  if (!ED_view3d_win_to_3d_on_plane(region, plane, mval, false, on_plane)) {
    /* The plane is seen edge-on: fall back to the view-aligned plane through the center. */
    ED_view3d_win_to_3d(v3d, region, center, mval, on_plane);
  }
  float3 offset = on_plane - center;
  offset -= normal * math::dot(offset, normal);
  return math::length(offset);
}

/** Minimum cursor travel in pixels before a new surface sample is accepted. */
static constexpr float GRADIENT_CURVE_MIN_DIST_PX = 2.5f;

/** Wheel step of the gradient's halfway color. */
static constexpr float GRADIENT_MIDPOINT_STEP = 0.05f;
/** Clamp range of the halfway color, keeping both gradient stops visible. */
static constexpr float GRADIENT_MIDPOINT_MIN = 0.05f;
static constexpr float GRADIENT_MIDPOINT_MAX = 0.95f;

/**
 * Live state of a Curve gesture: the raw accepted samples and the curve built from them. The
 * modal keeps this in sync with the operator's RNA collection (same point count), so the
 * evaluation can reuse the already-built curve instead of rebuilding it from RNA on every
 * preview pass -- and the overlay and the apply passes are then guaranteed to share one curve.
 */
struct ColorGradientCurveData {
  /** Raw surface samples in world space (mirrored into RNA for the live preview and redo). */
  Vector<float3> raw_points;
  /** Resampled / smoothed curve rebuilt after every accepted sample; drives the overlay. */
  std::shared_ptr<const gradient_curve::GradientCurve3D> curve;
  /** Screen position of the last accepted sample, for the minimum-distance threshold. */
  float2 last_mval = float2(0.0f);
  /** Region type the draw callback is registered with; static, stays valid if the area closes. */
  ARegionType *region_type = nullptr;
  void *draw_handle = nullptr;
  /** Live-preview throttling (mirrors the Image Editor's select gradient): the apply pass walks
   * every PBVH node, so it is rate-limited to ~60 Hz and the remainder is flushed by #timer. */
  double last_preview_time = 0.0;
  bool preview_pending = false;
  wmTimer *timer = nullptr;
  /** Set right before the commit-time #sculpt_color_gradient_exec, so the geodesic distance mode
   * (and its warnings) only run once, on release, and the live gesture stays Euclidean. */
  bool is_final = false;
};

static ColorGradientCurveData *curve_gesture_data_get(wmOperator *op)
{
  if (op->customdata == nullptr || !RNA_boolean_get(op->ptr, "is_curve_gesture")) {
    return nullptr;
  }
  return static_cast<ColorGradientCurveData *>(op->customdata);
}

/**
 * Build the world-space curve from the raw drawn points: resampled at an even arc-length step and
 * smoothed per the tool settings. The spacing (auto: ~1/64 of the stroke extent, capped at
 * #gradient_curve::max_points) and the smoothing iterations come from the shared builder. The
 * returned curve may be invalid for degenerate input.
 */
static std::shared_ptr<gradient_curve::GradientCurve3D> sculpt_color_gradient_curve_build(
    const Sculpt &sd, const Span<float3> raw_points)
{
  const gradient_curve::GradientCurveBuildParams params = {
      /*spacing=*/0.0f,
      /*smooth=*/sd.gradient_curve_smooth,
      gradient_curve::default_smooth_iterations,
  };
  auto curve = std::make_shared<gradient_curve::GradientCurve3D>();
  curve->build(raw_points, params);
  return curve;
}

/**
 * The curve built by the live Curve gesture, when the operator is running its modal and the RNA
 * collection is in sync with the gesture's samples (null otherwise: standalone execution and the
 * redo panel always rebuild from RNA).
 */
static std::shared_ptr<const gradient_curve::GradientCurve3D>
sculpt_color_gradient_curve_cached_get(wmOperator *op)
{
  const ColorGradientCurveData *data = curve_gesture_data_get(op);
  if (data == nullptr) {
    return nullptr;
  }
  PropertyRNA *prop = RNA_struct_find_property(op->ptr, "curve_points");
  const int rna_len = (prop != nullptr) ? RNA_property_collection_length(op->ptr, prop) : 0;
  if (data->curve == nullptr || int(data->raw_points.size()) != rna_len) {
    return nullptr;
  }
  return data->curve;
}

/**
 * Build the world-space curve gradient from the operator's stored RNA points. The raw input is
 * resampled at an even arc-length step and smoothed per the tool settings; the spacing keeps the
 * resampled count within #gradient_curve::max_points while tracking the stroke's extent. During
 * the live gesture the curve built by the modal is reused, so the overlay and the apply passes
 * evaluate one shared curve instead of rebuilding it on every preview pass.
 */
static void gradient_geometry_curve_init(GradientGeometry &geometry,
                                         const Sculpt &sd,
                                         wmOperator *op,
                                         const Vector<float3> &raw_points)
{
  geometry.curve_mode = eGradientCurveMode(sd.gradient_curve_mode);
  geometry.curve_width = sd.gradient_curve_width;
  geometry.curve_distance = eSculptGradientCurveDistance(sd.gradient_curve_distance);
  /* While the curve gesture is live, the geodesic field would be rebuilt (a full Dijkstra sweep)
   * on every accepted sample, which is far too heavy to keep up with the cursor. The live preview
   * stays Euclidean; the geodesic distance is only resolved on the final commit (see
   * #sculpt_color_gradient_curve_session_end). */
  const ColorGradientCurveData *data = curve_gesture_data_get(op);
  if (data != nullptr && !data->is_final) {
    geometry.curve_distance = SCULPT_GRADIENT_CURVE_EUCLIDEAN;
  }
  geometry.curve = sculpt_color_gradient_curve_cached_get(op);
  if (geometry.curve == nullptr) {
    geometry.curve = sculpt_color_gradient_curve_build(sd, raw_points);
  }
}

static GradientGeometry sculpt_color_gradient_geometry_get(const bContext *C, wmOperator *op)
{
  const Sculpt &sd = *CTX_data_tool_settings(C)->sculpt;
  const float2 start(float(RNA_int_get(op->ptr, "xstart")), float(RNA_int_get(op->ptr, "ystart")));
  const float2 end(float(RNA_int_get(op->ptr, "xend")), float(RNA_int_get(op->ptr, "yend")));

  GradientGeometry geometry;
  geometry.type = eSculptGradientType(sd.gradient_type);
  if (geometry.type == SCULPT_GRADIENT_LINEAR) {
    geometry.start_ss = start;
    geometry.axis_ss = end - start;
    const float len_sq = math::length_squared(geometry.axis_ss);
    /* Shorter than a pixel: degenerate. */
    geometry.inv_axis_len_sq = (len_sq >= 1.0f) ? 1.0f / len_sq : 0.0f;
    return geometry;
  }
  if (geometry.type == SCULPT_GRADIENT_CURVE) {
    Vector<float3> raw_points;
    PropertyRNA *curve_prop = RNA_struct_find_property(op->ptr, "curve_points");
    if (curve_prop != nullptr && RNA_property_type(curve_prop) == PROP_COLLECTION) {
      RNA_PROP_BEGIN (op->ptr, itemptr, curve_prop) {
        float co[3];
        RNA_float_get_array(&itemptr, "location", co);
        raw_points.append(float3(co));
      }
      RNA_PROP_END;
    }
    gradient_geometry_curve_init(geometry, sd, op, raw_points);
    return geometry;
  }

  RNA_float_get_array(op->ptr, "center", geometry.center);
  RNA_float_get_array(op->ptr, "normal", geometry.normal);
  geometry.normal = math::normalize(geometry.normal);
  const ARegion *region = CTX_wm_region(C);
  const View3D *v3d = CTX_wm_view3d(C);
  if (region != nullptr && region->regiondata != nullptr && v3d != nullptr) {
    geometry.radius = gradient_radial_radius(
        region, v3d, geometry.center, geometry.normal, end);
  }
  return geometry;
}

/**
 * Evaluates the gradient parameter (0 at the start, 1 at the end, unbounded outside) for the
 * object-space positions of one object, honoring its mirror symmetry: every symmetry pass
 * mirrors the position and the pass nearest to the gradient start wins, so a mirrored half gets
 * the mirrored gradient.
 *
 * The Curve type deviates from that rule on purpose: the pass whose mirrored position lies
 * closest *to the curve* wins (minimal distance, not minimal t), otherwise a mirrored half would
 * pick up whichever curve section happens to produce the smaller parameter. In geodesic distance
 * mode the mirrored seeds are merged into the per-object field up front (mirroring preserves arc
 * length), so #t_at_vert reads the winning (distance, parameter) pair directly.
 */
class GradientEvaluator {
  const GradientGeometry &geometry_;
  const ARegion *region_;
  float4x4 object_to_world_;
  Vector<ePaintSymmetryFlags, 8> passes_;

  /** Geodesic distance mode (`SCULPT_GRADIENT_CURVE_GEODESIC`): per-vertex distance and
   * curve-parameter field over this object's surface. Built by #sculpt_color_gradient_apply_object
   * and owned by it for the duration of the evaluator. */
  const geodesic::GeodesicCurveField *geodesic_ = nullptr;

 public:
  GradientEvaluator(const GradientGeometry &geometry,
                    Object &ob,
                    const ARegion *region,
                    const geodesic::GeodesicCurveField *geodesic = nullptr)
      : geometry_(geometry), region_(region), object_to_world_(ob.object_to_world()),
        geodesic_(geodesic)
  {
    const int symmetry_flags = int(mesh_symmetry_xyz_get(ob));
    for (int i = 0; i <= symmetry_flags; i++) {
      if (is_symmetry_iteration_valid(i, symmetry_flags)) {
        passes_.append(ePaintSymmetryFlags(i));
      }
    }
    if (geometry_.type == SCULPT_GRADIENT_LINEAR && region_ != nullptr &&
        region_->regiondata != nullptr)
    {
      /* #ED_view3d_project_float_object projects object-space positions of this object. */
      ED_view3d_init_mats_rv3d(&ob, static_cast<RegionView3D *>(region_->regiondata));
    }
  }

  bool is_valid() const
  {
    if (!geometry_.is_valid()) {
      return false;
    }
    return geometry_.type != SCULPT_GRADIENT_LINEAR ||
           (region_ != nullptr && region_->regiondata != nullptr);
  }

  /** NaN when no symmetry pass can see \a position (outside the view for Linear). */
  float t_at(const float3 &position) const
  {
    if (geometry_.type == SCULPT_GRADIENT_CURVE) {
      /* The geodesic field is indexed by vertex (see #t_at_vert); a position-only query cannot
       * resolve it. The vertex paths never reach here in geodesic mode. */
      return (geodesic_ == nullptr) ? t_at_curve_euclidean(position) :
                                      std::numeric_limits<float>::quiet_NaN();
    }
    float best = std::numeric_limits<float>::quiet_NaN();
    for (const ePaintSymmetryFlags pass : passes_) {
      const float3 flipped = symmetry_flip(position, pass);
      float t;
      if (geometry_.type == SCULPT_GRADIENT_LINEAR) {
        float2 co_ss;
        if (ED_view3d_project_float_object(region_,
                                           flipped,
                                           co_ss,
                                           V3D_PROJ_TEST_CLIP_BB | V3D_PROJ_TEST_CLIP_NEAR) !=
            V3D_PROJ_RET_OK)
        {
          continue;
        }
        t = math::dot(co_ss - geometry_.start_ss, geometry_.axis_ss) * geometry_.inv_axis_len_sq;
      }
      else {
        float3 offset = math::transform_point(object_to_world_, flipped) - geometry_.center;
        offset -= geometry_.normal * math::dot(offset, geometry_.normal);
        t = math::length(offset) / geometry_.radius;
      }
      if (!(t >= best)) {
        best = t;
      }
    }
    return best;
  }

  /**
   * Geodesic-mode Curve evaluation for a vertex of the object the field was built for: the field
   * already merged every symmetry pass, so there is nothing left to mirror here. Unreachable
   * vertices (disconnected or fully hidden) read as an infinite distance and are left untouched.
   */
  float t_at_vert(const int vert) const
  {
    BLI_assert(geometry_.type == SCULPT_GRADIENT_CURVE && geodesic_ != nullptr);
    if (!geometry_.curve->is_valid()) {
      return std::numeric_limits<float>::quiet_NaN();
    }
    const float dist = geodesic_->dist[vert];
    if (!(dist < std::numeric_limits<float>::max())) {
      return std::numeric_limits<float>::quiet_NaN();
    }
    return curve_eval_t(geodesic_->param[vert], dist);
  }

 private:
  /**
   * Euclidean Curve evaluation: the pass whose mirrored position lies closest to the curve wins
   * (minimal distance, not minimal t), so a mirrored half picks up the mirrored curve section,
   * and the winning (parameter, distance) pair is mapped to a parameter with the same Along /
   * Across rules as #GradientCurve3D::eval_t.
   */
  float t_at_curve_euclidean(const float3 &position) const
  {
    float best_dist = std::numeric_limits<float>::max();
    float best_s = 0.0f;
    bool found = false;
    for (const ePaintSymmetryFlags pass : passes_) {
      const float3 flipped = symmetry_flip(position, pass);
      const float3 world = math::transform_point(object_to_world_, flipped);
      const gradient_curve::CurveProjection proj = geometry_.curve->project(world);
      if (proj.distance < best_dist) {
        best_dist = proj.distance;
        best_s = proj.s;
        found = true;
      }
    }
    if (!found) {
      return std::numeric_limits<float>::quiet_NaN();
    }
    return curve_eval_t(best_s, best_dist);
  }

  /** Same parameter rules as #GradientCurve3D::eval_t, from a precomputed (s, distance) pair. */
  float curve_eval_t(const float s, const float dist) const
  {
    const gradient_curve::CurveProjection proj = {s, dist};
    return gradient_curve::eval_t_from_projection(
        geometry_.curve_mode, geometry_.curve_width, geometry_.curve->length(), proj);
  }
};

/** Restore every node captured in the in-progress Color undo step back to its snapshot. */
static void sculpt_color_gradient_restore_object(Object &ob)
{
  bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob);
  if (pbvh == nullptr || pbvh->type() != bke::pbvh::Type::Mesh) {
    return;
  }
  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh->nodes<bke::pbvh::MeshNode>();
  IndexMaskMemory memory;
  const IndexMask node_mask = IndexMask::from_predicate(
      nodes.index_range(),
      memory,
      [&](const int i) { return orig_color_data_lookup_mesh(ob, nodes[i]).has_value(); });
  if (node_mask.is_empty()) {
    return;
  }
  Mesh &mesh = *id_cast<Mesh *>(ob.data);
  bke::GSpanAttributeWriter color_attribute = active_color_attribute_for_write(mesh);
  if (!color_attribute) {
    return;
  }
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
  /* Only the corners of selected faces were painted; writing the per-vertex original into the
   * others would flatten their distinct corner colors. */
  const SculptSession &ss = *ob.runtime->sculpt_session;
  const Span<bool> select_poly = (ss.filter_cache != nullptr) ?
                                     Span<bool>(face_selection_mask_ensure(ob).select_poly) :
                                     Span<bool>();
  node_mask.foreach_index(
      [&](const int i) {
        const Span<float4> orig_data = *orig_color_data_lookup_mesh(ob, nodes[i]);
        const Span<int> verts = nodes[i].verts();
        for (const int k : verts.index_range()) {
          color_vert_set(faces,
                         corner_verts,
                         vert_to_face_map,
                         color_attribute.domain,
                         verts[k],
                         orig_data[k],
                         color_attribute.span,
                         select_poly);
        }
      },
      exec_mode::grain_size(1));
  pbvh->tag_attribute_changed(node_mask, mesh.active_color_attribute);
  color_attribute.finish();
}

/** Free the minimal filter cache allocated in #sculpt_color_gradient_init, if still present. */
static void sculpt_color_gradient_cache_free(Object &ob)
{
  SculptSession &ss = *ob.runtime->sculpt_session;
  if (ss.filter_cache) {
    MEM_delete(ss.filter_cache);
    ss.filter_cache = nullptr;
  }
}

static void sculpt_color_gradient_restore(bContext *C)
{
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
  const Vector<Object *> objects = sculpt_mode_objects(vc);
  for (Object *ob : objects) {
    sculpt_color_gradient_restore_object(*ob);
    sculpt_color_gradient_cache_free(*ob);
    flush_update_step(vc, *ob, UpdateType::Color);
  }
}

/**
 * Build the geodesic (distance, curve-parameter) field for one object in
 * #SCULPT_GRADIENT_CURVE_GEODESIC distance mode: nearby mesh vertices and their symmetry mirrors
 * seed a propagation of the arc-length parameter over the surface
 * (see #geodesic::curve_geodesic_field_create). Returns false when no seed
 * vertex could be resolved; the caller falls back to the Euclidean projection then.
 */
static bool sculpt_color_gradient_geodesic_field_build(
    Object &ob,
    const Span<float3> vert_positions,
    const GradientGeometry &geometry,
    bke::BVHTreeFromMesh &r_vert_bvh,
    geodesic::GeodesicCurveField &r_field)
{
  const Mesh &mesh = *id_cast<const Mesh *>(ob.data);

  r_vert_bvh = bke::bvhtree_from_mesh_verts_init(mesh, IndexMask(mesh.verts_num));
  if (r_vert_bvh.tree == nullptr) {
    return false;
  }

  const float4x4 world_to_object = ob.world_to_object();
  const float4x4 object_to_world = ob.object_to_world();
  const int samples = geometry.curve->points_num();
  const float curve_length = geometry.curve->length();
  /* The vertex positions and BVH are in object space, matching Dijkstra's edge lengths. Keep the
   * seed neighborhood and projected distances in that same space. */
  const Span<int2> edges = mesh.edges();
  float average_edge_length = 0.0f;
  for (const int2 &edge : edges) {
    average_edge_length += math::distance(vert_positions[edge[0]], vert_positions[edge[1]]);
  }
  average_edge_length = edges.is_empty() ? 0.0f : average_edge_length / float(edges.size());
  const float seed_radius = std::max(average_edge_length, 1e-6f);
  Map<int, float2> seed_map;
  const int symmetry_flags = int(mesh_symmetry_xyz_get(ob));
  for (int i = 0; i <= symmetry_flags; i++) {
    if (!is_symmetry_iteration_valid(i, symmetry_flags)) {
      continue;
    }
    const ePaintSymmetryFlags pass = ePaintSymmetryFlags(i);
    for (const int k : IndexRange(samples)) {
      const float s = (samples > 1) ? curve_length * (float(k) / float(samples - 1)) : 0.0f;
      const float3 obj_co = symmetry_flip(
          math::transform_point(world_to_object, geometry.curve->position_at_s(s)), pass);
      BLI_bvhtree_range_query_cpp(
          *r_vert_bvh.tree,
          obj_co,
          seed_radius,
          [&](const int vert, const float3 & /*co*/, const float /*dist_sq*/) {
            const float3 vert_obj = symmetry_flip(vert_positions[vert], pass);
            const float3 vert_world = math::transform_point(object_to_world, vert_obj);
            const gradient_curve::CurveProjection projection =
                geometry.curve->project(vert_world);
            const float3 projected_obj = math::transform_point(
                world_to_object, geometry.curve->position_at_s(projection.s));
            const float distance = math::distance(vert_obj, projected_obj);
            const float2 candidate(projection.s, distance);
            if (!seed_map.contains(vert) || distance < seed_map.lookup(vert).y) {
              seed_map.add_overwrite(vert, candidate);
            }
          });
    }
  }
  if (seed_map.is_empty()) {
    return false;
  }

  Vector<int> seed_verts;
  Vector<float> seed_params;
  Vector<float> seed_distances;
  seed_verts.reserve(seed_map.size());
  seed_params.reserve(seed_map.size());
  seed_distances.reserve(seed_map.size());
  for (const auto &[vert, values] : seed_map.items()) {
    seed_verts.append(vert);
    seed_params.append(values.x);
    seed_distances.append(values.y);
  }

  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<int> corner_edges = mesh.corner_edges();
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArray<bool> hide_poly_varray = *attributes.lookup_or_default<bool>(
      ".hide_poly", bke::AttrDomain::Face, false);
  Array<bool> hide_poly_array(mesh.faces_num, false);
  hide_poly_varray.materialize(hide_poly_array.as_mutable_span());

  /* Same lazily-built adjacency caches as the Expand tool's geodesic falloff. */
  SculptSession &ss = *ob.runtime->sculpt_session;
  if (ss.edge_to_face_map.is_empty()) {
    ss.edge_to_face_map = blender::bke::mesh::build_edge_to_face_map(
        faces, corner_edges, edges.size(), ss.edge_to_face_offsets, ss.edge_to_face_indices);
  }
  if (ss.vert_to_edge_map.is_empty()) {
    ss.vert_to_edge_map = blender::bke::mesh::build_vert_to_edge_map(
        edges, mesh.verts_num, ss.vert_to_edge_offsets, ss.vert_to_edge_indices);
  }

  r_field = geodesic::curve_geodesic_field_create(vert_positions,
                                                 edges,
                                                 faces,
                                                 corner_verts,
                                                 ss.vert_to_edge_map,
                                                 ss.edge_to_face_map,
                                                 hide_poly_array,
                                                 seed_verts,
                                                 seed_params,
                                                 seed_distances);
  return true;
}

static bool sculpt_color_gradient_apply_object(bContext *C,
                                               Object &ob,
                                               const ARegion *region,
                                               const GradientGeometry &geometry,
                                               const GradientColoring &coloring)
{
  const Depsgraph &depsgraph = *CTX_data_depsgraph_pointer(C);
  bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob);
  if (pbvh == nullptr || pbvh->type() != bke::pbvh::Type::Mesh) {
    return false;
  }
  Mesh &mesh = *id_cast<Mesh *>(ob.data);

  bke::GSpanAttributeWriter color_attribute = active_color_attribute_for_write(mesh);
  if (!color_attribute) {
    return false;
  }

  IndexMaskMemory memory;
  const IndexMask node_mask = bke::pbvh::all_leaf_nodes(*pbvh, memory);
  if (node_mask.is_empty()) {
    color_attribute.finish();
    return false;
  }

  /* First push wins: later pushes of the same step keep the pre-stroke snapshot. */
  undo::push_nodes(depsgraph, ob, node_mask, undo::NodeDataFlag::Color);

  const Span<float3> vert_positions = bke::pbvh::vert_positions_eval(depsgraph, ob);
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
  const FaceSelectionMask &face_selection_mask = face_selection_mask_ensure(ob);
  const bool use_face_selection = face_selection_mask.state == FaceSelectionState::Active;

  /* Geodesic distance mode: one (distance, parameter) field per object, seeded from the curve and
   * its symmetry mirrors. Built here rather than per vertex because the Dijkstra sweep is far too
   * heavy to repeat per query. The vertex BVH is owned by the struct and freed with it. */
  geodesic::GeodesicCurveField geodesic_field;
  bke::BVHTreeFromMesh geodesic_vert_bvh;
  bool use_geodesic = false;
  if (geometry.type == SCULPT_GRADIENT_CURVE &&
      geometry.curve_distance == SCULPT_GRADIENT_CURVE_GEODESIC && geometry.curve != nullptr &&
      geometry.curve->is_valid())
  {
    use_geodesic = sculpt_color_gradient_geodesic_field_build(
        ob, vert_positions, geometry, geodesic_vert_bvh, geodesic_field);
  }

  const GradientEvaluator evaluator(
      geometry, ob, region, use_geodesic ? &geodesic_field : nullptr);
  const bool evaluator_valid = evaluator.is_valid();

  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh->nodes<bke::pbvh::MeshNode>();
  node_mask.foreach_index(
      [&](const int i) {
        const std::optional<Span<float4>> orig_opt = orig_color_data_lookup_mesh(ob, nodes[i]);
        const Span<int> verts = nodes[i].verts();
        for (const int k : verts.index_range()) {
          const int vert = verts[k];
          const float4 orig_color = orig_opt.has_value() ?
                                        (*orig_opt)[k] :
                                        color_vert_get(faces,
                                                       corner_verts,
                                                       vert_to_face_map,
                                                       color_attribute.span,
                                                       color_attribute.domain,
                                                       vert,
                                                       face_selection_mask.select_poly);

          /* Unaffected vertices are still written (with their original color): a previous move of
           * the same gesture may have painted them. */
          float4 new_color = orig_color;
          if (evaluator_valid &&
              !(use_face_selection && !face_selection_mask.vert_paintable[vert]))
          {
            const float t = use_geodesic ? evaluator.t_at_vert(vert) :
                                           evaluator.t_at(vert_positions[vert]);
            if (!std::isnan(t)) {
              new_color = math::clamp(gradient_blend_color(coloring, orig_color, t), 0.0f, 1.0f);
              new_color.w = orig_color.w;
            }
          }

          color_vert_set(faces,
                         corner_verts,
                         vert_to_face_map,
                         color_attribute.domain,
                         vert,
                         new_color,
                         color_attribute.span,
                         face_selection_mask.select_poly);
        }
      },
      exec_mode::grain_size(1));

  pbvh->tag_attribute_changed(node_mask, mesh.active_color_attribute);
  color_attribute.finish();
  return evaluator_valid;
}

/**
 * Image / Material (PBR) paint canvas path: blends the gradient into every enabled image target's
 * pixels. The color channel (and a plain image canvas) gets the gradient colors, every other
 * material channel its own value (see #gradient_blend_value).
 *
 * Deliberately does not reuse the per-dab machinery in `sculpt_paint_image.cc` (texture
 * sampling, multi-channel pairing, stroke accumulators): only the already-exported building
 * blocks are used (#paint::image::init_image_paint_targets, #fetch_image_buffers,
 * #do_push_undo_tile, #calc_pixel_row_positions, #read_image_pixels / #write_image_pixels,
 * #bke::pbvh::pixels::mark_image_dirty) so this stays independent of that file's internal,
 * actively-changing per-dab representation.
 */
static bool sculpt_color_gradient_apply_image_object(bContext *C,
                                                      Object &ob,
                                                      const ARegion *region,
                                                      const Sculpt &sd,
                                                      PaintModeSettings &paint_mode_settings,
                                                      const Brush *brush,
                                                      const GradientGeometry &geometry,
                                                      const GradientColoring &coloring)
{
  using namespace paint::image;
  using bke::pbvh::pixels::PackedPixelRow;
  using bke::pbvh::pixels::PixelData;
  using bke::pbvh::pixels::PixelNode;
  using bke::pbvh::pixels::UDIMTilePixels;

  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob);
  if (pbvh == nullptr || pbvh->type() != bke::pbvh::Type::Mesh) {
    return false;
  }

  const GradientEvaluator evaluator(geometry, ob, region);
  if (!evaluator.is_valid()) {
    return false;
  }

  Vector<ImagePaintTarget> targets = init_image_paint_targets(
      ob, paint_mode_settings, brush, sd.paint.visible_material_channels);
  if (targets.is_empty()) {
    return false;
  }

  const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
  const StringRef uv_map_name = BKE_paint_canvas_uvmap_name_get(&paint_mode_settings, &ob)
                                    .value_or("");
  const Span<float3> vert_positions = bke::pbvh::vert_positions_eval(*depsgraph, ob);

  /* No stroke or filter cache holds it on this path, so it is built for this call. */
  FaceSelectionMask face_selection_mask;
  face_selection_mask_build(mesh, face_selection_mask);
  const bool use_face_selection = face_selection_mask.state == FaceSelectionState::Active;
  const Span<int> corner_tri_faces = use_face_selection ? mesh.corner_tri_faces() : Span<int>();
  const Span<bool> select_poly = face_selection_mask.select_poly;

  IndexMaskMemory memory;
  const IndexMask node_mask = bke::pbvh::all_leaf_nodes(*pbvh, memory);
  if (node_mask.is_empty()) {
    return false;
  }
  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh->nodes<bke::pbvh::MeshNode>();

  /* Written from every node's task below (grain_size(1)); a plain bool would race. */
  std::atomic<bool> any_painted = false;

  /* Per-pixel gradient parameter (NaN: left untouched), keyed by pixel layout. Material channels
   * whose images share a layout (same UV map and resolution, see
   * #BKE_paint_pixels_layout_key_get) share one #PixelData, so the projection + symmetry
   * evaluation -- by far the dominant per-pixel cost -- runs once per layout instead of once per
   * channel. Indexed by node, then flattened over that node's tiles and rows in iteration order. */
  Map<const PixelData *, Array<Array<float>>> t_by_layout;

  for (ImagePaintTarget &target : targets) {
    ImageData &image_data = *target.data;
    if (!bke::pbvh::build_pixels(
            *depsgraph, ob, *image_data.image, *image_data.image_user, uv_map_name))
    {
      continue;
    }

    PixelData &pixel_data = bke::pbvh::pixels::data_get(*pbvh);
    MutableSpan<PixelNode> pixel_nodes = pixel_data.nodes;
    const bool paints_colors = !target.is_material_channel || target.is_color_channel ||
                               !target.color_override.has_value();
    const float4 channel_value = target.color_override.value_or(float4(1.0f));

    /* Sequential (default #exec_mode::serial): #fetch_image_buffers inserts into
     * #ImageData::buffers / #processors, which are plain, non-thread-safe maps -- exactly the
     * same reason `sculpt_paint_image.cc`'s own per-dab pass fetches every node's buffers in a
     * dedicated pass before its parallel node loop, rather than from inside it. */
    node_mask.foreach_index([&](const int i) {
      PixelNode &pixel_node = pixel_nodes[i];
      if (!pixel_node.tiles.is_empty()) {
        fetch_image_buffers(image_data, nodes[i], pixel_node);
      }
    });

    bool compute_t = false;
    Array<Array<float>> &node_t = t_by_layout.lookup_or_add_cb(&pixel_data, [&]() {
      compute_t = true;
      return Array<Array<float>>(pixel_nodes.size());
    });

    node_mask.foreach_index(
        [&](const int i) {
          PixelNode &pixel_node = pixel_nodes[i];
          if (pixel_node.tiles.is_empty()) {
            return;
          }
          do_push_undo_tile(image_data, nodes[i], pixel_node);

          if (compute_t) {
            int64_t node_pixel_num = 0;
            for (const UDIMTilePixels &tile : pixel_node.tiles) {
              for (const PackedPixelRow &pixel_row : tile.pixel_rows) {
                node_pixel_num += pixel_row.num_pixels;
              }
            }
            node_t[i].reinitialize(node_pixel_num);
          }
          const MutableSpan<float> pixel_t = node_t[i];

          bool node_touched = false;
          int64_t tile_pixel_offset = 0;
          for (UDIMTilePixels &tile : pixel_node.tiles) {
            const Span<PackedPixelRow> pixel_rows = tile.pixel_rows;
            /* Offset of each row into `pixel_t`, so rows can be processed independently. */
            Array<int64_t> row_offsets(pixel_rows.size());
            for (const int r : pixel_rows.index_range()) {
              row_offsets[r] = tile_pixel_offset;
              tile_pixel_offset += pixel_rows[r].num_pixels;
            }

            /* The parameter does not depend on the image buffer, so it is filled even when this
             * channel's buffer is missing: another channel sharing the layout may still use it.
             * A single low-poly node can own the whole canvas, hence parallel over rows rather
             * than relying on the node loop alone. */
            if (compute_t) {
              threading::parallel_for(pixel_rows.index_range(), 256, [&](const IndexRange rows) {
                Vector<float3> positions;
                for (const int r : rows) {
                  const PackedPixelRow &pixel_row = pixel_rows[r];
                  const IndexRange range(0, pixel_row.num_pixels);
                  MutableSpan<float> row_t = pixel_t.slice(row_offsets[r], range.size());
                  if (use_face_selection) {
                    const int tri = pixel_node.uv_primitives.tri_indices[pixel_row.uv_primitive_index];
                    if (!select_poly[corner_tri_faces[tri]]) {
                      row_t.fill(std::numeric_limits<float>::quiet_NaN());
                      continue;
                    }
                  }
                  positions.resize(range.size());
                  calc_pixel_row_positions(vert_positions,
                                           pixel_data.vert_tris,
                                           pixel_node.uv_primitives.tri_indices,
                                           pixel_node.uv_primitives.delta_barycentric_coords,
                                           pixel_row,
                                           range,
                                           positions);
                  for (const int px : range.index_range()) {
                    row_t[px] = evaluator.t_at(positions[px]);
                  }
                }
              });
            }

            ImBuf *image_buffer = image_data.buffers.lookup_default(tile.tile_number, nullptr);
            const TileColorspaceProcessor *processors = image_data.processors.lookup_ptr(
                tile.tile_number);
            if (image_buffer == nullptr || processors == nullptr) {
              continue;
            }
            const bool premul_storage = image_data.image->alpha_mode == IMA_ALPHA_PREMUL;

            MutableSpan<float4> float_buffer;
            MutableSpan<uchar4> byte_buffer;
            if (image_buffer->float_data()) {
              float_buffer = MutableSpan(
                  reinterpret_cast<float4 *>(image_buffer->float_data_for_write()),
                  image_buffer->x * image_buffer->y);
            }
            else {
              byte_buffer = MutableSpan(
                  reinterpret_cast<uchar4 *>(image_buffer->byte_data_for_write()),
                  image_buffer->x * image_buffer->y);
            }

            /* Rows never overlap in the image, so they can be blended concurrently; the tile's
             * dirty region is not thread-safe, so it is accumulated afterwards. */
            Array<bool> rows_changed(pixel_rows.size(), false);
            /* Resolved once per tile, outside the parallel loop: the sampler is lock-free, while
             * the per-pixel BKE blend-sample call would serialize the rows on a mutex. The mask is
             * applied here, not folded into the shared `t`, because it depends on this image. */
            const ImagePaintSelectionTileSampler selection_sampler =
                BKE_image_paint_selection_tile_sampler_get(image_data.image, tile.tile_number);
            const bool use_selection_mask = !selection_sampler.is_unrestricted();
            threading::parallel_for(pixel_rows.index_range(), 256, [&](const IndexRange rows) {
              Vector<float4> byte_storage;
              for (const int r : rows) {
                const PackedPixelRow &pixel_row = pixel_rows[r];
                const IndexRange range(0, pixel_row.num_pixels);
                const Span<float> row_t = pixel_t.slice(row_offsets[r], range.size());
                if (std::all_of(row_t.begin(), row_t.end(), [](const float t) {
                      return std::isnan(t);
                    }))
                {
                  continue;
                }

                const MutableSpan<float4> scene_linear =
                    !float_buffer.is_empty() ?
                        read_image_pixels(
                            float_buffer, *processors, pixel_row, range, image_buffer->x) :
                        read_image_pixels(byte_buffer,
                                          *processors,
                                          pixel_row,
                                          range,
                                          image_buffer->x,
                                          byte_storage,
                                          premul_storage);

                bool row_touched = false;
                for (const int px : range.index_range()) {
                  const float t = row_t[px];
                  if (std::isnan(t)) {
                    continue;
                  }
                  float weight = 1.0f;
                  if (use_selection_mask) {
                    weight = selection_sampler.sample(
                        pixel_row.start_image_coordinate.x + px,
                        pixel_row.start_image_coordinate.y);
                    if (weight <= 0.0f) {
                      continue;
                    }
                  }
                  float4 &pixel = scene_linear[px];
                  const float4 blended = paints_colors ?
                                             gradient_blend_color(coloring, pixel, t) :
                                             gradient_blend_value(
                                                 coloring, pixel, channel_value, t);
                  /* Linear mix by weight (not a threshold) so a feathered selection edge stays
                   * smooth. */
                  pixel = weight >= 1.0f ? blended : math::interpolate(pixel, blended, weight);
                  row_touched = true;
                }
                if (!row_touched) {
                  continue;
                }

                if (!float_buffer.is_empty()) {
                  write_image_pixels(
                      scene_linear, float_buffer, *processors, pixel_row, range, image_buffer->x);
                }
                else {
                  write_image_pixels(scene_linear,
                                     byte_buffer,
                                     *processors,
                                     pixel_row,
                                     range,
                                     image_buffer->x,
                                     premul_storage);
                }
                rows_changed[r] = true;
              }
            });

            for (const int r : pixel_rows.index_range()) {
              if (!rows_changed[r]) {
                continue;
              }
              const PackedPixelRow &pixel_row = pixel_rows[r];
              const int2 start(pixel_row.start_image_coordinate.x,
                               pixel_row.start_image_coordinate.y);
              const int2 end = start + int2(pixel_row.num_pixels + 1, 1);
              tile.mark_dirty(Bounds<int2>(start, end));
              any_painted = true;
            }

            if (tile.flags.dirty) {
              node_touched = true;
              /* Immediate (non-delayed) partial GPU upload: unlike a brush stroke, this runs once
               * per move, so there is no run of dabs for the delayed variant to coalesce. */
              ImageUser tile_user = *image_data.image_user;
              tile_user.tile = tile.tile_number;
              BKE_image_update_gputexture(image_data.image,
                                          &tile_user,
                                          tile.dirty_region.xmin,
                                          tile.dirty_region.ymin,
                                          BLI_rcti_size_x(&tile.dirty_region),
                                          BLI_rcti_size_y(&tile.dirty_region));
            }
          }

          /* #mark_image_dirty only acts when this flag is set (mirroring how the per-dab pass in
           * `sculpt_paint_image.cc` ORs it in from each tile's dirty flag before calling it). */
          pixel_node.flags.dirty |= node_touched;
          bke::pbvh::pixels::mark_image_dirty(
              nodes[i], pixel_node, *image_data.image, image_data.buffers);
        },
        exec_mode::grain_size(1));
  }

  return any_painted;
}

/**
 * Whether this gesture's selection should use the Image-undo system instead of the sculpt-undo
 * one. Exactly one of the two may be open at a time (#BKE_undosys_step_push_init frees a live
 * step of the other system), so the choice is made once, for every object at once, the same way
 * #stroke_undo_begin in sculpt.cc does it for an ordinary brush stroke: image wins whenever any
 * object in the selection uses an image/Material canvas.
 */
static bool sculpt_color_gradient_uses_image_undo(bContext *C, const Span<Object *> objects)
{
  const Sculpt *sd = CTX_data_tool_settings(C)->sculpt;
  const Scene *scene = CTX_data_scene(C);
  PaintModeSettings *paint_mode_settings = (scene != nullptr && scene->toolsettings) ?
                                               &scene->toolsettings->paint_mode :
                                               nullptr;
  if (sd == nullptr || paint_mode_settings == nullptr) {
    return false;
  }
  const Brush *brush = BKE_paint_brush_for_read(&sd->paint);
  for (Object *ob : objects) {
    if (ob != nullptr && ob->type == OB_MESH &&
        SCULPT_use_image_paint_brush(
            *paint_mode_settings, *ob, brush, sd->paint.visible_material_channels))
    {
      return true;
    }
  }
  return false;
}

/** Close the undo transaction #sculpt_color_gradient_init opened, in the system it chose. */
static void sculpt_color_gradient_undo_end(const bool use_image_undo)
{
  if (use_image_undo) {
    /* Exactly one live image step for the whole gesture. */
    if (ED_image_undo_is_step_active()) {
      ED_image_undo_push_end();
    }
    return;
  }
  undo::push_end_all_ex(false, true);
}

/** Throw away the transaction #sculpt_color_gradient_init opened, for a cancelled gesture. */
static void sculpt_color_gradient_undo_cancel(const bool use_image_undo)
{
  if (use_image_undo) {
    /* Image undo does not roll anything back on its own; write the captured tiles back first,
     * mirroring #stroke_undo_cancel in sculpt.cc. */
    if (ED_image_undo_is_step_active()) {
      ED_image_paint_tile_map_restore(ED_image_paint_tile_map_get());
      ED_image_undo_push_end();
    }
    return;
  }
  undo::discard_init_step();
}

/**
 * Apply the current gesture state to every color-ATTRIBUTE sculpt object. Cheap (touches
 * vertices, not pixels), so this is safe to call on every live-preview mouse-move as well as the
 * final commit. Image/Material canvas objects are skipped here and handled by
 * #sculpt_color_gradient_apply_image_canvases. Returns true if any object was painted.
 */
static bool sculpt_color_gradient_apply(bContext *C, wmOperator *op)
{
  ARegion *region = CTX_wm_region(C);
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
  const Vector<Object *> objects = sculpt_mode_objects(vc);
  if (objects.is_empty()) {
    return false;
  }

  const GradientGeometry geometry = sculpt_color_gradient_geometry_get(C, op);
  const GradientColoring coloring = sculpt_color_gradient_coloring_get(C, op);

  const Sculpt *sd = CTX_data_tool_settings(C)->sculpt;
  const Brush *brush = (sd != nullptr) ? BKE_paint_brush_for_read(&sd->paint) : nullptr;
  const Scene *scene = CTX_data_scene(C);
  PaintModeSettings *paint_mode_settings = (scene != nullptr && scene->toolsettings) ?
                                               &scene->toolsettings->paint_mode :
                                               nullptr;
  /* Decided once for the whole gesture in #sculpt_color_gradient_init; recomputed identically
   * here since canvas/material configuration does not change mid-drag. */
  const bool use_image_undo = sculpt_color_gradient_uses_image_undo(C, objects);

  bool any_painted = false;
  bool mismatched_canvas_skipped = false;
  for (Object *ob : objects) {
    if (ob == nullptr || ob->type != OB_MESH) {
      continue;
    }
    const bool object_is_image_canvas = paint_mode_settings != nullptr && sd != nullptr &&
                                        SCULPT_use_image_paint_brush(
                                            *paint_mode_settings,
                                            *ob,
                                            brush,
                                            sd->paint.visible_material_channels);
    if (object_is_image_canvas) {
      continue;
    }
    if (use_image_undo) {
      /* This gesture opened Image-undo instead of sculpt-undo (see #sculpt_color_gradient_init):
       * painting this color-attribute object here would push a sculpt-undo node with no active
       * step for it. Skip it -- mixed canvases within one selection are not supported. */
      mismatched_canvas_skipped = true;
      continue;
    }
    if (sculpt_color_gradient_apply_object(C, *ob, region, geometry, coloring)) {
      any_painted = true;
    }
    flush_update_step(vc, *ob, UpdateType::Color);
  }

  if (!any_painted && mismatched_canvas_skipped) {
    BKE_report(op->reports,
               RPT_WARNING,
               "Color Gradient: objects using a different paint canvas than the rest of the "
               "selection were skipped (mixed canvases are not supported in one gesture)");
  }
  return any_painted;
}

/**
 * Live-preview + final pass for Image/Material canvas objects: blends the gradient into every
 * enabled image target of every image-canvas object. Called on every mouse-move as well as the
 * final commit, mirroring #sculpt_color_gradient_apply's contract for color-attribute objects.
 *
 * Idempotency: unlike the color-attribute path (which always blends from the sculpt-undo
 * snapshot), pixels here are blended in place in the live `ImBuf`. Repeated calls would compound
 * on top of the previous move's result without the explicit rollback below --
 * #ED_image_paint_tile_map_restore writes every tile captured so far back to its pristine,
 * pre-gesture state (a no-op on the first call, since nothing has been captured yet) and
 * invalidates them, so the capture inside #do_push_undo_tile that follows re-captures that same
 * pristine data as the "original" for this move's blend. See its doc-comment in ED_paint.hh.
 * Returns true if any object was painted.
 */
static bool sculpt_color_gradient_apply_image_canvases(bContext *C, wmOperator *op)
{
  ARegion *region = CTX_wm_region(C);
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
  const Vector<Object *> objects = sculpt_mode_objects(vc);
  if (objects.is_empty()) {
    return false;
  }

  /* Roll every tile touched by a previous move (if any) back to its pristine state before
   * re-blending from scratch this move. Guarded the same way #sculpt_color_gradient_undo_end /
   * #undo_cancel are: a selection with no image-canvas object never opened Image-undo in
   * #sculpt_color_gradient_init, and touching some unrelated, foreign image-paint session's tile
   * map here would be collateral damage. */
  if (ED_image_undo_is_step_active()) {
    ED_image_paint_tile_map_restore(ED_image_paint_tile_map_get());
  }

  const Sculpt *sd = CTX_data_tool_settings(C)->sculpt;
  const Scene *scene = CTX_data_scene(C);
  PaintModeSettings *paint_mode_settings = (scene != nullptr && scene->toolsettings) ?
                                               &scene->toolsettings->paint_mode :
                                               nullptr;
  if (paint_mode_settings == nullptr || sd == nullptr) {
    return false;
  }
  const Brush *brush = BKE_paint_brush_for_read(&sd->paint);

  const GradientGeometry geometry = sculpt_color_gradient_geometry_get(C, op);
  const GradientColoring coloring = sculpt_color_gradient_coloring_get(C, op);

  /* Geodesic distances only exist per vertex of the color-attribute path; pixel positions of an
   * image canvas cannot be resolved to a vertex, so they always take the Euclidean projection.
   * Reported on the final commit only, not on every live-preview move: the gesture's final apply
   * is marked by #ColorGradientCurveData::is_final before the session tears its data down, and a
   * standalone / redo run has no gesture data at all. The live gesture deliberately resets its
   * distance mode to Euclidean (see #gradient_geometry_curve_init), so this is the only place the
   * user is told the image canvases are painted with the Euclidean fallback. */
  const ColorGradientCurveData *curve_data = curve_gesture_data_get(op);
  const bool is_final_apply = curve_data == nullptr || curve_data->is_final;
  if (geometry.type == SCULPT_GRADIENT_CURVE &&
      geometry.curve_distance == SCULPT_GRADIENT_CURVE_GEODESIC && is_final_apply)
  {
    for (Object *ob : objects) {
      if (ob != nullptr && ob->type == OB_MESH &&
          SCULPT_use_image_paint_brush(
              *paint_mode_settings, *ob, brush, sd->paint.visible_material_channels))
      {
        BKE_report(op->reports,
                   RPT_WARNING,
                   "Geodesic curve distance is not supported for image canvases; using Euclidean "
                   "distance there");
        break;
      }
    }
  }

  bool any_painted = false;
  for (Object *ob : objects) {
    if (ob == nullptr || ob->type != OB_MESH) {
      continue;
    }
    if (!SCULPT_use_image_paint_brush(
            *paint_mode_settings, *ob, brush, sd->paint.visible_material_channels))
    {
      continue;
    }
    if (sculpt_color_gradient_apply_image_object(
            C, *ob, region, *sd, *paint_mode_settings, brush, geometry, coloring))
    {
      any_painted = true;
    }
    flush_update_step(vc, *ob, UpdateType::Color);
  }
  return any_painted;
}

/** Show the midpoint (and the radial gradient's radius) in the area header while dragging. */
static void sculpt_color_gradient_status_update(bContext *C, wmOperator *op)
{
  ScrArea *area = CTX_wm_area(C);
  const Sculpt *sd = CTX_data_tool_settings(C)->sculpt;
  if (area == nullptr || sd == nullptr) {
    return;
  }
  std::string text = fmt::format(
      "{}: {:.2f}", IFACE_("Midpoint"), RNA_float_get(op->ptr, "midpoint"));
  if (sd->gradient_type == SCULPT_GRADIENT_RADIAL) {
    const GradientGeometry geometry = sculpt_color_gradient_geometry_get(C, op);
    const Scene &scene = *CTX_data_scene(C);
    char radius_str[64];
    BKE_unit_value_as_string_scaled(radius_str,
                                    int(sizeof(radius_str)),
                                    geometry.radius,
                                    4,
                                    B_UNIT_LENGTH,
                                    scene.unit,
                                    false,
                                    true);
    text = fmt::format("{}: {}    {}", IFACE_("Radius"), radius_str, text);
  }
  text += fmt::format("    ({})", IFACE_("Wheel: move midpoint"));
  ED_area_status_text(area, text.c_str());
}

static void sculpt_color_gradient_status_clear(bContext *C)
{
  if (ScrArea *area = CTX_wm_area(C)) {
    ED_area_status_text(area, nullptr);
  }
}

/**
 * Prepare the selection mask of every image canvas the gradient will paint. Returns false (after
 * reporting) when a canvas is borrowed by a floating selection session: its lifted fragments leave
 * holes that the gradient would fill. Otherwise refreshes the face-selection-derived masks the same
 * way the brush does at stroke start (#sculpt_brush_stroke_invoke), so the sampler never reads a
 * stale mask. Runs before any undo step is opened so a refusal leaves nothing behind.
 */
static bool sculpt_color_gradient_canvases_prepare(bContext *C,
                                                   wmOperator *op,
                                                   const Span<Object *> objects)
{
  Scene &scene = *CTX_data_scene(C);
  const Sculpt *sd = CTX_data_tool_settings(C)->sculpt;
  if (sd == nullptr || scene.toolsettings == nullptr) {
    return true;
  }
  PaintModeSettings &paint_mode_settings = scene.toolsettings->paint_mode;
  const Brush *brush = BKE_paint_brush_for_read(&sd->paint);

  Vector<Image *> images;
  for (Object *ob : objects) {
    if (ob == nullptr || ob->type != OB_MESH ||
        !SCULPT_use_image_paint_brush(
            paint_mode_settings, *ob, brush, sd->paint.visible_material_channels))
    {
      continue;
    }
    for (paint::image::ImagePaintTarget &target : paint::image::init_image_paint_targets(
             *ob, paint_mode_settings, brush, sd->paint.visible_material_channels))
    {
      Image *image = target.data->image;
      if (image == nullptr) {
        continue;
      }
      if (image->runtime != nullptr && image->runtime->paint_selection_borrowed_by != nullptr) {
        BKE_report(op->reports,
                   RPT_WARNING,
                   "Image is being edited by a floating selection, finish it before using the "
                   "gradient");
        return false;
      }
      images.append_non_duplicates(image);
    }
  }

  for (Image *image : images) {
    image_paint_selection_mask_from_face_selection(C, &scene, image);
  }
  return true;
}

static int sculpt_color_gradient_init(bContext *C, wmOperator *op)
{
  const Scene &scene = *CTX_data_scene(C);
  Object &ob = *CTX_data_active_object(C);
  View3D *v3d = CTX_wm_view3d(C);

  const Base *base = CTX_data_active_base(C);
  if (!BKE_base_is_visible(v3d, base)) {
    return OPERATOR_CANCELLED;
  }

  /* Same domain restriction as the color filter: no dyntopo / multires support yet. Also
   * required by the image/pixel-node path below, whose PBVH must be Mesh-typed. */
  if (!color_supported_check(scene, ob, op->reports)) {
    return OPERATOR_CANCELLED;
  }

  ViewContext vc = ED_view3d_viewcontext_init(C, CTX_data_depsgraph_pointer(C));
  const Vector<Object *> objects = sculpt_mode_objects(vc);
  if (objects.is_empty()) {
    return OPERATOR_CANCELLED;
  }

  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  for (Object *object : objects) {
    bke::object::pbvh_ensure(*depsgraph, *object);
    BKE_sculpt_update_object_for_edit(depsgraph, object, true);
  }

  if (!sculpt_color_gradient_canvases_prepare(C, op, objects)) {
    return OPERATOR_CANCELLED;
  }

  if (sculpt_color_gradient_uses_image_undo(C, objects)) {
    /* Image/Material canvas: exactly one undo system may be open per gesture (see
     * #sculpt_color_gradient_uses_image_undo). Any color-attribute object mixed into this
     * selection is skipped for the gesture instead (reported from #sculpt_color_gradient_apply),
     * so the color-attribute setup below (shared attributes, filter cache) is not needed here. */
    ED_image_undo_push_begin(op->type->name, PaintMode::Sculpt);
    return OPERATOR_PASS_THROUGH;
  }

  undo::push_begin_multi_object(scene, op, objects);
  ensure_shared_color_attributes(ob, objects);

  for (Object *object : objects) {
    /* #face_selection_mask_ensure stores its result in the stroke or filter cache; the gradient
     * gesture has neither, so a minimal filter cache is allocated to hold it (freed in
     * #sculpt_color_gradient_restore and at every finish path below). */
    SculptSession &ss = *object->runtime->sculpt_session;
    if (!ss.filter_cache) {
      ss.filter_cache = MEM_new<filter::Cache>(__func__);
    }
    face_selection_mask_ensure(*object);
  }

  return OPERATOR_PASS_THROUGH;
}

static void sculpt_color_gradient_finish(bContext *C, const bool use_image_undo)
{
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
  const Vector<Object *> objects = sculpt_mode_objects(vc);
  sculpt_color_gradient_undo_end(use_image_undo);
  for (Object *ob : objects) {
    sculpt_color_gradient_cache_free(*ob);
    flush_update_done(C, *ob, UpdateType::Color);
    WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
  }
}

static void sculpt_color_gradient_abort(bContext *C)
{
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
  const Vector<Object *> objects = sculpt_mode_objects(vc);
  sculpt_color_gradient_restore(C);
  sculpt_color_gradient_undo_cancel(sculpt_color_gradient_uses_image_undo(C, objects));
  for (Object *ob : objects) {
    flush_update_done(C, *ob, UpdateType::Color);
    WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
  }
}

static wmOperatorStatus sculpt_color_gradient_exec(bContext *C, wmOperator *op)
{
  const bool is_interactive = (op->customdata != nullptr);
  if (!is_interactive) {
    /* Standalone execution (redo panel, Python): own the whole undo step. */
    if (sculpt_color_gradient_init(C, op) == OPERATOR_CANCELLED) {
      return OPERATOR_CANCELLED;
    }
    Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
    ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
    const bool use_image_undo = sculpt_color_gradient_uses_image_undo(
        C, sculpt_mode_objects(vc));
    const bool attributes_painted = sculpt_color_gradient_apply(C, op);
    const bool images_painted = sculpt_color_gradient_apply_image_canvases(C, op);
    if (!attributes_painted && !images_painted) {
      sculpt_color_gradient_abort(C);
      return OPERATOR_CANCELLED;
    }
    sculpt_color_gradient_finish(C, use_image_undo);
    return OPERATOR_FINISHED;
  }

  /* Interactive preview: the gesture already opened the undo step in invoke. Both canvas types
   * are re-applied on every move (see #sculpt_color_gradient_apply_image_canvases for how the
   * image path stays idempotent across repeated moves). */
  sculpt_color_gradient_apply(C, op);
  sculpt_color_gradient_apply_image_canvases(C, op);
  return OPERATOR_FINISHED;
}

/* -------------------------------------------------------------------- */
/** \name Color Gradient Curve gesture
 *
 * The Curve type cannot reuse the straight-line gesture: every cursor position along the drag is
 * raycast onto the surface and kept as a world-space curve sample. The samples live in the
 * operator's RNA (`curve_points`, world space) the moment they are accepted, so the live preview
 * (#sculpt_color_gradient_exec) and the redo panel read the exact same geometry no matter where
 * the viewport was when the curve was drawn. #ColorGradientCurveData (defined with the gradient
 * geometry above) keeps the built curve so the overlay and the apply passes share it.
 * \{ */

static void sculpt_color_gradient_curve_draw(const bContext * /*C*/,
                                             ARegion * /*region*/,
                                             void *arg)
{
  ColorGradientCurveData *data = static_cast<ColorGradientCurveData *>(arg);
  const Span<float3> polyline = data->curve != nullptr ? data->curve->points() :
                                                        Span<float3>(data->raw_points);
  if (polyline.size() < 2) {
    return;
  }

  GPU_blend(GPU_BLEND_ALPHA);
  GPU_line_smooth(true);
  /* The curve hugs the surface; a depth test would clip most of it against the mesh it follows. */
  const GPUDepthTest depth_prev = GPU_depth_test_get();
  GPU_depth_test(GPU_DEPTH_NONE);

  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  /* Dark under-stroke then light core, matching the Image Editor's gradient overlay. */
  for (const int pass : IndexRange(2)) {
    GPU_line_width((pass == 0) ? 3.0f : 1.5f);
    if (pass == 0) {
      immUniformColor4f(0.0f, 0.0f, 0.0f, 0.55f);
    }
    else {
      immUniformColor4f(1.0f, 1.0f, 1.0f, 0.9f);
    }
    immBegin(GPU_PRIM_LINE_STRIP, polyline.size());
    for (const float3 &co : polyline) {
      immVertex3f(pos, co.x, co.y, co.z);
    }
    immEnd();
  }

  /* Start / end markers: axis-aligned crosses scaled with the curve, big enough to spot. */
  const float marker = 0.03f * math::length(polyline.last() - polyline.first());
  immBegin(GPU_PRIM_LINES, 12);
  for (const float3 &co : {polyline.first(), polyline.last()}) {
    for (const int axis : IndexRange(3)) {
      float3 offset(0.0f);
      offset[axis] = marker;
      immVertex3f(pos, co.x - offset.x, co.y - offset.y, co.z - offset.z);
      immVertex3f(pos, co.x + offset.x, co.y + offset.y, co.z + offset.z);
    }
  }
  immEnd();

  immUnbindProgram();
  GPU_depth_test(depth_prev);
  GPU_blend(GPU_BLEND_NONE);
  GPU_line_smooth(false);
}

/** Raycast the cursor onto the front-most sculpt surface, in world space. */
static bool sculpt_color_gradient_curve_surface_point(bContext *C,
                                                      const float2 &mval,
                                                      float3 &r_world)
{
  Object *hit_ob = nullptr;
  const std::optional<CursorGeometryInfo> hit = cursor_geometry_info_update(C, mval, true, &hit_ob);
  if (!hit || hit_ob == nullptr) {
    return false;
  }
  r_world = math::transform_point(hit_ob->object_to_world(), hit->location);
  return true;
}

/** Store one accepted sample in the operator's RNA (live preview and redo read it from there). */
static void sculpt_color_gradient_curve_point_store(wmOperator *op, const float3 &world)
{
  PointerRNA itemptr;
  RNA_collection_add(op->ptr, "curve_points", &itemptr);
  RNA_float_set_array(&itemptr, "location", world);
}

/**
 * Thin the accepted samples to a small, editable set of control points before committing, so the
 * stored curve, the redo panel and any future per-point editing work on a handful of points
 * instead of one per few pixels. RDP runs on the screen-space projection (the metric matches what
 * the user sees), but the kept 3D world points are preserved verbatim. The raw points and the RNA
 * collection are rewritten together so they stay in sync.
 */
static void sculpt_color_gradient_curve_simplify(bContext *C,
                                                 wmOperator *op,
                                                 ColorGradientCurveData *data)
{
  const ARegion *region = CTX_wm_region(C);
  if (data->raw_points.size() <= 2 || region == nullptr || region->regiondata == nullptr) {
    return;
  }
  Array<float2> screen(data->raw_points.size());
  for (const int i : data->raw_points.index_range()) {
    float co[2];
    if (ED_view3d_project_float_global(region, data->raw_points[i], co, V3D_PROJ_TEST_NOP) !=
        V3D_PROJ_RET_OK)
    {
      /* A sample could not be projected (degenerate view): keep the raw points rather than
       * dropping a point and silently changing the shape. */
      return;
    }
    screen[i] = float2(co);
  }
  const Vector<int> kept = gradient_curve::simplify_control_points<float2>(
      screen, gradient_curve::simplify_tolerance_px, gradient_curve::max_control_points);
  if (kept.size() >= data->raw_points.size()) {
    return;
  }
  Vector<float3> reduced;
  reduced.reserve(kept.size());
  for (const int index : kept) {
    reduced.append(data->raw_points[index]);
  }
  data->raw_points = std::move(reduced);
  data->curve = sculpt_color_gradient_curve_build(*CTX_data_tool_settings(C)->sculpt,
                                                  data->raw_points);
  RNA_collection_clear(op->ptr, "curve_points");
  for (const float3 &co : data->raw_points) {
    sculpt_color_gradient_curve_point_store(op, co);
  }
}

/**
 * Live preview during the curve gesture. The apply pass walks every PBVH node of every object, so
 * it is throttled to ~60 Hz; a skipped update sets #ColorGradientCurveData::preview_pending and is
 * flushed by the gesture's timer tick.
 */
static void sculpt_color_gradient_curve_preview(bContext *C,
                                                wmOperator *op,
                                                ColorGradientCurveData *data,
                                                const bool force)
{
  if (!force) {
    const double now = BLI_time_now_seconds();
    if (now - data->last_preview_time < (1.0 / 60.0)) {
      data->preview_pending = true;
      return;
    }
    data->last_preview_time = now;
  }
  else {
    data->last_preview_time = BLI_time_now_seconds();
  }
  data->preview_pending = false;
  sculpt_color_gradient_exec(C, op);
}

static void sculpt_color_gradient_curve_status_update(bContext *C, wmOperator *op)
{
  ScrArea *area = CTX_wm_area(C);
  if (area == nullptr) {
    return;
  }
  const Sculpt *sd = CTX_data_tool_settings(C)->sculpt;
  std::string text = fmt::format("{}: {}    ({})",
                                 IFACE_("Curve points"),
                                 RNA_collection_length(op->ptr, "curve_points"),
                                 IFACE_("Wheel: move midpoint, Backspace: remove last point"));
  if (sd != nullptr &&
      eSculptGradientCurveDistance(sd->gradient_curve_distance) == SCULPT_GRADIENT_CURVE_GEODESIC)
  {
    text += fmt::format("    ({})", IFACE_("Geodesic distances are computed on release"));
  }
  ED_area_status_text(area, text.c_str());
}

/** Release the gesture's resources, then either commit or roll back the whole undo step. */
static void sculpt_color_gradient_curve_session_end(bContext *C,
                                                    wmOperator *op,
                                                    const bool commit)
{
  ColorGradientCurveData *data = static_cast<ColorGradientCurveData *>(op->customdata);
  if (data == nullptr) {
    return;
  }
  if (data->timer != nullptr) {
    WM_event_timer_remove(CTX_wm_manager(C), CTX_wm_window(C), data->timer);
    data->timer = nullptr;
  }
  if (data->region_type != nullptr && data->draw_handle != nullptr) {
    ED_region_draw_cb_exit(data->region_type, data->draw_handle);
  }
  sculpt_color_gradient_status_clear(C);
  if (commit) {
    if (data->raw_points.size() >= 2) {
      /* Settle the curve to its editable control points, then mark this as the final apply so
       * the geodesic distance mode (too heavy for the live gesture) runs exactly once. */
      sculpt_color_gradient_curve_simplify(C, op, data);
      data->is_final = true;
      sculpt_color_gradient_exec(C, op);
    }
    Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
    ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
    sculpt_color_gradient_finish(
        C, sculpt_color_gradient_uses_image_undo(C, sculpt_mode_objects(vc)));
  }
  else {
    sculpt_color_gradient_abort(C);
  }
  WM_cursor_modal_restore(CTX_wm_window(C));
  if (ARegion *draw_region = CTX_wm_region(C)) {
    ED_region_tag_redraw(draw_region);
  }
  MEM_delete(data);
  op->customdata = nullptr;
}

static wmOperatorStatus sculpt_color_gradient_curve_modal(bContext *C,
                                                          wmOperator *op,
                                                          const wmEvent *event)
{
  ColorGradientCurveData *data = static_cast<ColorGradientCurveData *>(op->customdata);
  ARegion *region = CTX_wm_region(C);
  const Sculpt &sd = *CTX_data_tool_settings(C)->sculpt;

  if (event->type == TIMER) {
    if (data->timer == nullptr || event->customdata != data->timer) {
      return OPERATOR_PASS_THROUGH;
    }
    /* Flush a preview the mouse-move throttling skipped. */
    if (data->preview_pending) {
      sculpt_color_gradient_curve_preview(C, op, data, true);
    }
    return OPERATOR_RUNNING_MODAL;
  }

  if (event->type == EVT_MODAL_MAP) {
    /* Because "Gesture Straight Line" is assigned to this operator, the window manager rewrites
     * the matching events before the modal sees them: LMB release arrives as SELECT, Esc / RMB as
     * CANCEL and LMB press as BEGIN. The raw checks further down stay as a fallback for a user-
     * rebound keymap, which is also why unmapped events (Backspace, wheel, mouse-move) still come
     * through raw. */
    switch (event->val) {
      case GESTURE_MODAL_SELECT:
        sculpt_color_gradient_curve_session_end(C, op, true);
        return OPERATOR_FINISHED;
      case GESTURE_MODAL_CANCEL:
        sculpt_color_gradient_curve_session_end(C, op, false);
        return OPERATOR_CANCELLED;
      default:
        /* BEGIN (and Move / Snap / Flip, which this gesture does not use) keeps running. */
        return OPERATOR_RUNNING_MODAL;
    }
  }

  if (ELEM(event->type, WHEELUPMOUSE, WHEELDOWNMOUSE)) {
    /* Shifting the halfway color along the curve, like the straight-line gestures. */
    const float step = (event->type == WHEELUPMOUSE) ? GRADIENT_MIDPOINT_STEP :
                                                       -GRADIENT_MIDPOINT_STEP;
    const float midpoint = math::clamp(RNA_float_get(op->ptr, "midpoint") + step,
                                       GRADIENT_MIDPOINT_MIN,
                                       GRADIENT_MIDPOINT_MAX);
    RNA_float_set(op->ptr, "midpoint", midpoint);
    sculpt_color_gradient_curve_preview(C, op, data, true);
    sculpt_color_gradient_curve_status_update(C, op);
    return OPERATOR_RUNNING_MODAL;
  }

  if ((event->type == EVT_ESCKEY || event->type == RIGHTMOUSE) && event->val == KM_PRESS) {
    sculpt_color_gradient_curve_session_end(C, op, false);
    return OPERATOR_CANCELLED;
  }

  if (event->type == EVT_BACKSPACEKEY && event->val == KM_PRESS) {
    if (PropertyRNA *prop = RNA_struct_find_property(op->ptr, "curve_points")) {
      const int len = RNA_property_collection_length(op->ptr, prop);
      if (len > 0) {
        RNA_property_collection_remove(op->ptr, prop, len - 1);
        data->raw_points.resize(len - 1);
        data->curve = (len - 1 >= 2) ? sculpt_color_gradient_curve_build(sd, data->raw_points) :
                                       nullptr;
        sculpt_color_gradient_curve_preview(C, op, data, true);
        sculpt_color_gradient_curve_status_update(C, op);
        ED_region_tag_redraw(region);
      }
    }
    return OPERATOR_RUNNING_MODAL;
  }

  if (event->type == MOUSEMOVE) {
    const float2 mval(float(event->mval[0]), float(event->mval[1]));
    if (data->raw_points.is_empty() ||
        math::distance(mval, data->last_mval) >= GRADIENT_CURVE_MIN_DIST_PX)
    {
      float3 world;
      /* Misses (cursor off the mesh) are simply skipped: the next hit continues the curve. */
      if (sculpt_color_gradient_curve_surface_point(C, mval, world)) {
        data->raw_points.append(world);
        data->last_mval = mval;
        data->curve = sculpt_color_gradient_curve_build(sd, data->raw_points);
        sculpt_color_gradient_curve_point_store(op, world);
        sculpt_color_gradient_curve_preview(C, op, data, false);
        sculpt_color_gradient_curve_status_update(C, op);
      }
    }
    ED_region_tag_redraw(region);
    return OPERATOR_RUNNING_MODAL;
  }

  if (event->type == LEFTMOUSE && event->val == KM_RELEASE) {
    sculpt_color_gradient_curve_session_end(C, op, true);
    return OPERATOR_FINISHED;
  }

  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus sculpt_color_gradient_curve_invoke(bContext *C,
                                                           wmOperator *op,
                                                           const wmEvent *event)
{
  ARegion *region = CTX_wm_region(C);

  int start_mval[2];
  WM_event_drag_start_mval(event, region, start_mval);
  float3 first_point;
  if (!sculpt_color_gradient_curve_surface_point(
          C, float2(float(start_mval[0]), float(start_mval[1])), first_point))
  {
    /* The gesture's undo step is already open at this point: roll it back like a cancel. */
    BKE_report(op->reports, RPT_WARNING, "Curve gradient must start on the surface");
    sculpt_color_gradient_abort(C);
    return OPERATOR_CANCELLED;
  }

  RNA_collection_clear(op->ptr, "curve_points");
  RNA_boolean_set(op->ptr, "is_curve_gesture", true);

  ColorGradientCurveData *data = MEM_new<ColorGradientCurveData>(__func__);
  data->region_type = region->runtime->type;
  data->raw_points.append(first_point);
  data->last_mval = float2(float(start_mval[0]), float(start_mval[1]));
  data->curve = sculpt_color_gradient_curve_build(
      *CTX_data_tool_settings(C)->sculpt, data->raw_points);
  sculpt_color_gradient_curve_point_store(op, first_point);
  if (data->region_type != nullptr) {
    data->draw_handle = ED_region_draw_cb_activate(
        data->region_type, sculpt_color_gradient_curve_draw, data, REGION_DRAW_POST_VIEW);
  }
  /* Flushes a live preview the mouse-move throttling skipped, so a slow stroke still catches up
   * between events without blocking on the full PBVH apply pass. */
  data->timer = WM_event_timer_add(CTX_wm_manager(C), CTX_wm_window(C), TIMER, 1.0 / 30.0);
  op->customdata = data;

  WM_event_add_modal_handler(C, op);
  WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_CROSS);
  ED_region_tag_redraw(region);
  return OPERATOR_RUNNING_MODAL;
}

/** \} */

/**
 * For the Radial type, store the surface point and sampled normal under the cursor as the
 * gradient center and plane. Returns false when the cursor is not over a sculpt object.
 */
static bool sculpt_color_gradient_radial_center_set(bContext *C,
                                                    wmOperator *op,
                                                    const wmEvent *event)
{
  /* The drag start, which the straight-line gesture also starts from. */
  int mval[2];
  WM_event_drag_start_mval(event, CTX_wm_region(C), mval);
  Object *hit_ob = nullptr;
  const std::optional<CursorGeometryInfo> hit = cursor_geometry_info_update(
      C, float2(mval[0], mval[1]), true, &hit_ob);
  if (!hit || hit_ob == nullptr) {
    return false;
  }
  const float4x4 &object_to_world = hit_ob->object_to_world();
  const float3 center = math::transform_point(object_to_world, hit->location);
  const float3x3 normal_matrix = math::transpose(float3x3(hit_ob->world_to_object()));
  const float3 normal = math::normalize(normal_matrix * hit->normal);
  RNA_float_set_array(op->ptr, "center", center);
  RNA_float_set_array(op->ptr, "normal", normal);
  return true;
}

/* -------------------------------------------------------------------- */
/** \name Color Gradient interactive editing
 *
 * With the `interactive` property enabled, a Linear or Radial gradient does not end when the drag
 * does: the operator stays modal and the gradient stays live. The start (Radial: center) and end
 * (Radial: radius) handles, and the midpoint marker, stay draggable; Enter applies, Esc (or RMB)
 * rolls the whole step back. This reuses the drag gesture's machinery — the geometry is derived
 * from the same RNA coordinates the gesture writes — so one undo step covers the whole session.
 * \{ */

/** Which handle the mouse drag moves (see #ColorGradientInteractiveData::dragging). */
enum GradientInteractiveHandle {
  GRADIENT_HANDLE_NONE = 0,
  GRADIENT_HANDLE_START = 1,
  GRADIENT_HANDLE_END = 2,
  GRADIENT_HANDLE_MID = 3,
};

/**
 * Sentinel whose address tags #ColorGradientInteractiveData inside #wmOperator::customdata: the
 * drag phase stores a #wmGesture there and the Curve phase a #ColorGradientCurveData, so the
 * pointer alone cannot tell the phases apart. The compare below only rules the cast in or out,
 * it never dereferences through a foreign type.
 */
static constexpr int GRADIENT_INTERACTIVE_DATA_TAG = 0;

struct ColorGradientInteractiveData {
  /** First member, see #GRADIENT_INTERACTIVE_DATA_TAG. */
  const int *tag = &GRADIENT_INTERACTIVE_DATA_TAG;
  /** #GradientInteractiveHandle currently under the mouse drag. */
  int dragging = GRADIENT_HANDLE_NONE;
  /** Region type the draw callback is registered with; static, stays valid if the area closes. */
  ARegionType *region_type = nullptr;
  void *draw_handle = nullptr;
  /** Last accepted handle positions (region pixels), mirrored from the RNA by the modal so the
   * draw callback never has to touch the operator. */
  float2 start_px = float2(0.0f);
  float2 end_px = float2(0.0f);
  float midpoint = 0.5f;
};

static ColorGradientInteractiveData *interactive_data_get(wmOperator *op)
{
  if (op->customdata == nullptr) {
    return nullptr;
  }
  auto *data = static_cast<ColorGradientInteractiveData *>(op->customdata);
  if (data->tag != &GRADIENT_INTERACTIVE_DATA_TAG) {
    /* The drag gesture's #wmGesture or the Curve phase's data, not this struct. */
    return nullptr;
  }
  return data;
}

/** The gradient line's screen-space start (Radial: projected center) and end, region pixels. */
static void interactive_handles_screen(const bContext *C,
                                       wmOperator *op,
                                       float2 &r_start,
                                       float2 &r_end)
{
  const Sculpt &sd = *CTX_data_tool_settings(C)->sculpt;
  r_end = float2(float(RNA_int_get(op->ptr, "xend")), float(RNA_int_get(op->ptr, "yend")));
  if (eSculptGradientType(sd.gradient_type) == SCULPT_GRADIENT_RADIAL) {
    const ARegion *region = CTX_wm_region(C);
    float center_co[3];
    RNA_float_get_array(op->ptr, "center", center_co);
    float projected[3];
    if (region != nullptr && region->regiondata != nullptr &&
        ED_view3d_project_float_global(
            region, center_co, projected, V3D_PROJ_TEST_CLIP_DEFAULT) == V3D_PROJ_RET_OK)
    {
      r_start = float2(projected[0], projected[1]);
      return;
    }
    /* Fall back to the drag start if the center is off-screen. */
    r_start = float2(float(RNA_int_get(op->ptr, "xstart")), float(RNA_int_get(op->ptr, "ystart")));
    return;
  }
  r_start = float2(float(RNA_int_get(op->ptr, "xstart")), float(RNA_int_get(op->ptr, "ystart")));
}

/** Screen position of the midpoint marker: along the handle line, at the halfway color point. */
static float2 interactive_mid_point(const ColorGradientInteractiveData &data)
{
  return math::interpolate(data.start_px, data.end_px, data.midpoint);
}

/** Screen-space hit test radius of the handles. */
static constexpr float GRADIENT_INTERACTIVE_HANDLE_RADIUS = 10.0f;

static int interactive_hit_test(const ColorGradientInteractiveData &data, const float2 &mval)
{
  const float2 mid = interactive_mid_point(data);
  if (math::distance(mid, mval) <= GRADIENT_INTERACTIVE_HANDLE_RADIUS) {
    return GRADIENT_HANDLE_MID;
  }
  if (math::distance(data.start_px, mval) <= GRADIENT_INTERACTIVE_HANDLE_RADIUS) {
    return GRADIENT_HANDLE_START;
  }
  if (math::distance(data.end_px, mval) <= GRADIENT_INTERACTIVE_HANDLE_RADIUS) {
    return GRADIENT_HANDLE_END;
  }
  return GRADIENT_HANDLE_NONE;
}

/** Segment count of the interactive gradient's handle discs. */
static constexpr int GRADIENT_INTERACTIVE_HANDLE_SEGMENTS = 16;

static void sculpt_color_gradient_interactive_draw(const bContext * /*C*/,
                                                   ARegion * /*region*/,
                                                   void *arg)
{
  const ColorGradientInteractiveData *data = static_cast<ColorGradientInteractiveData *>(arg);

  GPU_blend(GPU_BLEND_ALPHA);
  GPU_line_smooth(true);
  /* The handles float over the 3D scene; a depth test would be meaningless in POST_PIXEL. */
  const GPUDepthTest depth_prev = GPU_depth_test_get();
  GPU_depth_test(GPU_DEPTH_NONE);

  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  /* This fork has no 2D uniform-color builtin; the 3D one with z = 0 in POST_PIXEL is the
   * same pattern the curve gradient's overlay uses. */
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  /* Dark under-stroke then light core, matching the curve gradient's overlay. */
  const float2 mid = interactive_mid_point(*data);
  for (const int pass : IndexRange(2)) {
    GPU_line_width((pass == 0) ? 3.0f : 1.5f);
    immUniformColor4f(0.0f, 0.0f, 0.0f, 0.55f);
    if (pass == 1) {
      immUniformColor4f(1.0f, 1.0f, 1.0f, 0.9f);
    }
    immBegin(GPU_PRIM_LINE_STRIP, 3);
    immVertex3f(pos, data->start_px.x, data->start_px.y, 0.0f);
    immVertex3f(pos, mid.x, mid.y, 0.0f);
    immVertex3f(pos, data->end_px.x, data->end_px.y, 0.0f);
    immEnd();
  }

  /* Handles: filled discs with a contrasting outline; the active one is slightly larger. */
  const float radius = GRADIENT_INTERACTIVE_HANDLE_RADIUS;
  immUniformColor4f(0.9f, 0.9f, 0.9f, 0.9f);
  imm_draw_circle_fill_3d(
      pos, data->start_px.x, data->start_px.y, radius * 0.6f, GRADIENT_INTERACTIVE_HANDLE_SEGMENTS);
  imm_draw_circle_fill_3d(
      pos, data->end_px.x, data->end_px.y, radius * 0.6f, GRADIENT_INTERACTIVE_HANDLE_SEGMENTS);
  imm_draw_circle_fill_3d(
      pos, mid.x, mid.y, radius * 0.45f, GRADIENT_INTERACTIVE_HANDLE_SEGMENTS);
  immUniformColor4f(0.0f, 0.0f, 0.0f, 0.8f);
  GPU_line_width(1.0f);
  for (const float2 &center : {data->start_px, data->end_px, mid}) {
    imm_draw_circle_wire_3d(
        pos, center.x, center.y, radius * 0.6f, GRADIENT_INTERACTIVE_HANDLE_SEGMENTS);
  }

  immUnbindProgram();
  GPU_depth_test(depth_prev);
  GPU_blend(GPU_BLEND_NONE);
  GPU_line_smooth(false);
}

/** Sync the draw overlay's mirrored state and tag the region. */
static void interactive_overlay_refresh(bContext *C,
                                        wmOperator *op,
                                        ColorGradientInteractiveData &data)
{
  interactive_handles_screen(C, op, data.start_px, data.end_px);
  data.midpoint = RNA_float_get(op->ptr, "midpoint");
  if (ARegion *region = CTX_wm_region(C)) {
    ED_region_tag_redraw(region);
  }
}

static void sculpt_color_gradient_interactive_status(bContext *C)
{
  if (ScrArea *area = CTX_wm_area(C)) {
    ED_area_status_text(area, IFACE_("Drag handles: move gradient  Wheel: midpoint  Enter: "
                                    "apply  Esc: cancel"));
  }
}

/** Tear down the interactive session: confirm applies (closes the undo step), cancel restores. */
static void sculpt_color_gradient_interactive_end(bContext *C,
                                                  wmOperator *op,
                                                  const bool confirmed)
{
  ColorGradientInteractiveData *data = static_cast<ColorGradientInteractiveData *>(op->customdata);
  if (data == nullptr) {
    return;
  }
  BLI_assert(data->tag == &GRADIENT_INTERACTIVE_DATA_TAG);
  if (data->region_type != nullptr && data->draw_handle != nullptr) {
    ED_region_draw_cb_exit(data->region_type, data->draw_handle);
  }
  MEM_delete(data);
  op->customdata = nullptr;
  WM_cursor_modal_restore(CTX_wm_window(C));

  sculpt_color_gradient_status_clear(C);
  if (confirmed) {
    Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
    ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
    sculpt_color_gradient_finish(C, sculpt_color_gradient_uses_image_undo(C,
        sculpt_mode_objects(vc)));
  }
  else {
    sculpt_color_gradient_abort(C);
  }
  if (ARegion *region = CTX_wm_region(C)) {
    ED_region_tag_redraw(region);
  }
}

/** Switch from the finished drag gesture to the persistent editing phase. */
static wmOperatorStatus sculpt_color_gradient_interactive_enter(bContext *C, wmOperator *op)
{
  ARegion *region = CTX_wm_region(C);
  ColorGradientInteractiveData *data = MEM_new<ColorGradientInteractiveData>(__func__);
  data->region_type = region->runtime->type;
  if (data->region_type != nullptr) {
    data->draw_handle = ED_region_draw_cb_activate(data->region_type,
                                                   sculpt_color_gradient_interactive_draw,
                                                   data,
                                                   REGION_DRAW_POST_PIXEL);
  }
  op->customdata = data;
  interactive_overlay_refresh(C, op, *data);
  WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_CROSS);
  sculpt_color_gradient_interactive_status(C);
  return OPERATOR_RUNNING_MODAL;
}

/** Persistent editing phase after the drag gesture finished (see #sculpt_color_gradient_modal). */
static wmOperatorStatus sculpt_color_gradient_interactive_modal(bContext *C,
                                                                wmOperator *op,
                                                                const wmEvent *event)
{
  /* The caller routed through #interactive_data_get, so the cast is type-safe. */
  ColorGradientInteractiveData &data = *static_cast<ColorGradientInteractiveData *>(
      op->customdata);
  BLI_assert(data.tag == &GRADIENT_INTERACTIVE_DATA_TAG);
  ARegion *region = CTX_wm_region(C);
  const float2 mval = region != nullptr ?
                          float2(float(event->xy[0] - region->winrct.xmin),
                                 float(event->xy[1] - region->winrct.ymin)) :
                          float2(0.0f);

  if (event->type == EVT_MODAL_MAP) {
    switch (event->val) {
      case GESTURE_MODAL_BEGIN: {
        /* LMB press: grab a handle; on the Radial type, clicking empty space re-centers the
         * gradient on the surface point under the cursor. */
        data.dragging = interactive_hit_test(data, mval);
        if (data.dragging == GRADIENT_HANDLE_NONE &&
            eSculptGradientType(CTX_data_tool_settings(C)->sculpt->gradient_type) ==
                SCULPT_GRADIENT_RADIAL)
        {
          if (sculpt_color_gradient_radial_center_set(C, op, event)) {
            sculpt_color_gradient_exec(C, op);
            interactive_overlay_refresh(C, op, data);
          }
        }
        ED_region_tag_redraw(region);
        return OPERATOR_RUNNING_MODAL;
      }
      case GESTURE_MODAL_SELECT: {
        /* LMB release: drop the handle. */
        data.dragging = GRADIENT_HANDLE_NONE;
        ED_region_tag_redraw(region);
        return OPERATOR_RUNNING_MODAL;
      }
      case GESTURE_MODAL_CANCEL: {
        sculpt_color_gradient_interactive_end(C, op, false);
        return OPERATOR_CANCELLED;
      }
      default:
        return OPERATOR_RUNNING_MODAL;
    }
  }

  switch (event->type) {
    case MOUSEMOVE: {
      if (data.dragging == GRADIENT_HANDLE_NONE) {
        return OPERATOR_PASS_THROUGH | OPERATOR_RUNNING_MODAL;
      }
      switch (data.dragging) {
        case GRADIENT_HANDLE_START:
          RNA_int_set(op->ptr, "xstart", int(mval.x));
          RNA_int_set(op->ptr, "ystart", int(mval.y));
          break;
        case GRADIENT_HANDLE_END:
          RNA_int_set(op->ptr, "xend", int(mval.x));
          RNA_int_set(op->ptr, "yend", int(mval.y));
          break;
        case GRADIENT_HANDLE_MID: {
          const float2 start = data.start_px;
          const float2 axis = data.end_px - start;
          const float len_sq = math::length_squared(axis);
          if (len_sq >= 1.0f) {
            const float t = math::clamp(math::dot(mval - start, axis) / len_sq,
                                        GRADIENT_MIDPOINT_MIN,
                                        GRADIENT_MIDPOINT_MAX);
            RNA_float_set(op->ptr, "midpoint", t);
          }
          break;
        }
        default:
          break;
      }
      sculpt_color_gradient_exec(C, op);
      interactive_overlay_refresh(C, op, data);
      return OPERATOR_RUNNING_MODAL;
    }
    case WHEELUPMOUSE:
    case WHEELDOWNMOUSE: {
      const float step = (event->type == WHEELUPMOUSE) ? GRADIENT_MIDPOINT_STEP :
                                                         -GRADIENT_MIDPOINT_STEP;
      const float midpoint = math::clamp(RNA_float_get(op->ptr, "midpoint") + step,
                                         GRADIENT_MIDPOINT_MIN,
                                         GRADIENT_MIDPOINT_MAX);
      RNA_float_set(op->ptr, "midpoint", midpoint);
      sculpt_color_gradient_exec(C, op);
      interactive_overlay_refresh(C, op, data);
      return OPERATOR_RUNNING_MODAL;
    }
    case EVT_RETKEY:
    case EVT_PADENTER: {
      if (event->val == KM_PRESS) {
        sculpt_color_gradient_interactive_end(C, op, true);
        return OPERATOR_FINISHED;
      }
      return OPERATOR_RUNNING_MODAL;
    }
    case INBETWEEN_MOUSEMOVE: {
      /* Coalesced intermediate moves: no pass-through, the modal handler must keep them. */
      return OPERATOR_RUNNING_MODAL;
    }
    default:
      /* Navigation (orbit, pan, zoom) and other unhandled events pass through, so the session
       * survives camera moves. */
      return OPERATOR_PASS_THROUGH | OPERATOR_RUNNING_MODAL;
  }
}

/** \} */

static wmOperatorStatus sculpt_color_gradient_invoke(bContext *C,
                                                     wmOperator *op,
                                                     const wmEvent *event)
{
  Object &ob = *CTX_data_active_object(C);
  View3D *v3d = CTX_wm_view3d(C);
  if (v3d && v3d->shading.type == OB_SOLID) {
    v3d->shading.color_type = V3D_SHADING_VERTEX_COLOR;
  }

  const Sculpt &sd = *CTX_data_tool_settings(C)->sculpt;
  if (sd.gradient_type == SCULPT_GRADIENT_RADIAL &&
      !sculpt_color_gradient_radial_center_set(C, op, event))
  {
    BKE_report(op->reports, RPT_WARNING, "Radial gradient must start on the surface");
    return OPERATOR_CANCELLED;
  }

  if (sculpt_color_gradient_init(C, op) == OPERATOR_CANCELLED) {
    return OPERATOR_CANCELLED;
  }

  ED_paint_brush_type_update_sticky_shading_color(C, &ob);

  /* A leftover flag from a previous Curve run must not route the straight-line gesture into the
   * curve modal. */
  RNA_boolean_set(op->ptr, "is_curve_gesture", false);
  if (sd.gradient_type == SCULPT_GRADIENT_CURVE) {
    return sculpt_color_gradient_curve_invoke(C, op, event);
  }
  return WM_gesture_straightline_invoke(C, op, event);
}

static wmOperatorStatus sculpt_color_gradient_modal(bContext *C,
                                                    wmOperator *op,
                                                    const wmEvent *event)
{
  if (RNA_boolean_get(op->ptr, "is_curve_gesture") && op->customdata != nullptr) {
    return sculpt_color_gradient_curve_modal(C, op, event);
  }

  if (interactive_data_get(op) != nullptr) {
    return sculpt_color_gradient_interactive_modal(C, op, event);
  }

  if (ELEM(event->type, WHEELUPMOUSE, WHEELDOWNMOUSE)) {
    const wmGesture *gesture = static_cast<const wmGesture *>(op->customdata);
    if (gesture != nullptr && gesture->is_active) {
      /* Shifting the halfway color along the drag gives a long, soft falloff without having to
       * drag far past the start. */
      const float step = (event->type == WHEELUPMOUSE) ? GRADIENT_MIDPOINT_STEP :
                                                         -GRADIENT_MIDPOINT_STEP;
      const float midpoint = math::clamp(RNA_float_get(op->ptr, "midpoint") + step,
                                         GRADIENT_MIDPOINT_MIN,
                                         GRADIENT_MIDPOINT_MAX);
      RNA_float_set(op->ptr, "midpoint", midpoint);
      sculpt_color_gradient_exec(C, op);
      sculpt_color_gradient_status_update(C, op);
      return OPERATOR_RUNNING_MODAL;
    }
  }

  const wmOperatorStatus ret = WM_gesture_straightline_modal(C, op, event);

  if (ret & OPERATOR_FINISHED) {
    if (RNA_boolean_get(op->ptr, "interactive")) {
      /* Interactive editing: the gradient stays live with draggable handles instead of
       * committing on release (Enter applies, Esc rolls the step back). */
      sculpt_color_gradient_status_clear(C);
      return sculpt_color_gradient_interactive_enter(C, op);
    }
    /* The gesture already ran the final exec for both canvas types. */
    sculpt_color_gradient_status_clear(C);
    Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
    ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
    sculpt_color_gradient_finish(
        C, sculpt_color_gradient_uses_image_undo(C, sculpt_mode_objects(vc)));
    return OPERATOR_FINISHED;
  }
  if (ret & OPERATOR_CANCELLED) {
    sculpt_color_gradient_status_clear(C);
    sculpt_color_gradient_abort(C);
    return OPERATOR_CANCELLED;
  }
  sculpt_color_gradient_status_update(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static void sculpt_color_gradient_cancel(bContext *C, wmOperator *op)
{
  if (RNA_boolean_get(op->ptr, "is_curve_gesture") && op->customdata != nullptr) {
    /* External teardown of a Curve gesture: #sculpt_color_gradient_curve_session_end rolls the
     * whole undo step back and frees the draw handler and sample data. */
    sculpt_color_gradient_curve_session_end(C, op, false);
    return;
  }
  if (interactive_data_get(op) != nullptr) {
    /* External teardown of the interactive editing phase (window close, file load): roll the
     * whole undo step back and free the draw handler. */
    sculpt_color_gradient_interactive_end(C, op, false);
    return;
  }
  sculpt_color_gradient_status_clear(C);
  sculpt_color_gradient_abort(C);
  WM_gesture_straightline_cancel(C, op);
}

static wmOperatorStatus sculpt_color_gradient_colors_flip_exec(bContext *C, wmOperator * /*op*/)
{
  Sculpt *sd = CTX_data_tool_settings(C)->sculpt;
  if (sd == nullptr) {
    return OPERATOR_CANCELLED;
  }
  if (sd->gradient_color_source == SCULPT_GRADIENT_COLOR_SOURCE_RAMP) {
    /* Mirror the stops so the ramp runs the other way, like the colors swap below. */
    ColorBand &coba = sd->gradient_colorband;
    for (int i = 0; i < coba.tot; i++) {
      coba.data[i].pos = 1.0f - coba.data[i].pos;
    }
    BKE_colorband_update_sort(&coba);
  }
  else {
    std::swap(sd->gradient_color, sd->gradient_secondary_color);
  }
  WM_event_add_notifier(C, NC_SCENE | ND_TOOLSETTINGS, nullptr);
  return OPERATOR_FINISHED;
}

void SCULPT_OT_color_gradient_colors_flip(wmOperatorType *ot)
{
  ot->name = "Swap Gradient Colors";
  ot->idname = "SCULPT_OT_color_gradient_colors_flip";
  ot->description =
      "Swap the start and end colors of the Color Gradient tool, or reverse its color ramp";

  ot->exec = sculpt_color_gradient_colors_flip_exec;
  ot->poll = sculpt_mode_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

void SCULPT_OT_color_gradient(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Color Gradient";
  ot->idname = "SCULPT_OT_color_gradient";
  ot->description = "Draw a color gradient across the active color attribute or paint canvas";

  /* API callbacks. */
  ot->invoke = sculpt_color_gradient_invoke;
  ot->exec = sculpt_color_gradient_exec;
  ot->modal = sculpt_color_gradient_modal;
  ot->cancel = sculpt_color_gradient_cancel;
  ot->poll = sculpt_mode_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_DEPENDS_ON_CURSOR;

  /* rna */
  PropertyRNA *prop = RNA_def_float_vector_xyz(ot->srna,
                                               "center",
                                               3,
                                               nullptr,
                                               -FLT_MAX,
                                               FLT_MAX,
                                               "Center",
                                               "World-space center of a radial gradient",
                                               -FLT_MAX,
                                               FLT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
  prop = RNA_def_float_vector_xyz(ot->srna,
                                  "normal",
                                  3,
                                  nullptr,
                                  -1.0f,
                                  1.0f,
                                  "Normal",
                                  "World-space plane normal of a radial gradient",
                                  -1.0f,
                                  1.0f);
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
  RNA_def_float_factor(ot->srna,
                       "midpoint",
                       0.5f,
                       0.0f,
                       1.0f,
                       "Midpoint",
                       "Position along the gradient where the halfway color lands (changed with "
                       "the mouse wheel while dragging)",
                       GRADIENT_MIDPOINT_MIN,
                       GRADIENT_MIDPOINT_MAX);

  /* World-space samples of the Curve gradient's drag; stored the moment each point is accepted so
   * the live preview and the redo panel share one source of truth (kept over redo, unlike the
   * gesture coordinates, since a Curve drag cannot be replayed from the view alone). */
  prop = RNA_def_collection_runtime(
      ot->srna, "curve_points", RNA_OperatorStrokeElement, "Curve Points", "");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
  RNA_def_property_ui_text(prop,
                           "Curve Points",
                           "World-space points of the drawn gradient curve, in stroke order");
  prop = RNA_def_boolean(ot->srna, "is_curve_gesture", false, "Curve Gesture", "");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);

  /* Interactive editing (Linear and Radial): keep the gradient live with draggable handles after
   * the drag ends, confirm with Enter, cancel with Esc. */
  prop = RNA_def_boolean(ot->srna,
                         "interactive",
                         false,
                         "Interactive",
                         "After releasing the mouse, keep editing the gradient with its handles: "
                         "confirm with Enter, cancel with Esc");

  WM_operator_properties_gesture_straightline(ot, WM_CURSOR_EDIT);
}

/** \} */

}  // namespace blender::ed::sculpt_paint::color
