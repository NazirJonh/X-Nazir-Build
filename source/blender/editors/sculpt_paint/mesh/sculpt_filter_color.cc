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
#include "BKE_colorband.hh"
#include "BKE_context.hh"
#include "BKE_image.hh"
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

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "../paint_intern.hh"

#include "paint_image_select_gradient.hh"

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
 * #GradientGeometry). Painting honors the face selection mask.
 * `SCULPT_use_image_paint_brush` decides, per object, which of the two applies (see
 * #sculpt_color_gradient_apply and #sculpt_color_gradient_apply_image_object).
 *
 * Lifetime (driven by `WM_gesture_straightline_*`):
 * - `invoke`: opens exactly one undo step for the whole gesture -- sculpt-undo
 *   (`push_begin_multi_object`) for a color-attribute selection, or Image-undo
 *   (`ED_image_undo_push_begin`) when any object uses an image/Material canvas; the two cannot
 *   both be open at once (see #sculpt_color_gradient_uses_image_undo) -- then hands control to
 *   `WM_gesture_straightline_invoke`.
 * - `exec`: called by the gesture on every mouse move (live preview) and once more
 *   on release (final commit). The color-attribute path always blends from the undo snapshot
 *   (`orig_color_data_lookup_mesh`), so repeated previews are idempotent; `push_nodes(...,
 *   Color)`/`do_push_undo_tile` only store a node/tile the first time, preserving the
 *   pre-stroke original for the whole drag.
 * - `modal`: delegates to `WM_gesture_straightline_modal`; on FINISHED closes the
 *   undo step, on CANCELLED restores originals and discards the step.
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
 * brush), with the radius reaching the point of that plane under the cursor.
 */
struct GradientGeometry {
  eSculptGradientType type = SCULPT_GRADIENT_LINEAR;

  float2 start_ss = float2(0.0f);
  float2 axis_ss = float2(0.0f);
  float inv_axis_len_sq = 0.0f;

  float3 center = float3(0.0f);
  float3 normal = float3(0.0f, 0.0f, 1.0f);
  float radius = 0.0f;

  /** False for a zero-length drag, which paints nothing. */
  bool is_valid() const
  {
    return (type == SCULPT_GRADIENT_LINEAR) ? inv_axis_len_sq > 0.0f : radius > 1e-6f;
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
 */
class GradientEvaluator {
  const GradientGeometry &geometry_;
  const ARegion *region_;
  float4x4 object_to_world_;
  Vector<ePaintSymmetryFlags, 8> passes_;

 public:
  GradientEvaluator(const GradientGeometry &geometry, Object &ob, const ARegion *region)
      : geometry_(geometry), region_(region), object_to_world_(ob.object_to_world())
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

  const GradientEvaluator evaluator(geometry, ob, region);
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
            const float t = evaluator.t_at(vert_positions[vert]);
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
                                          byte_storage);

                for (const int px : range.index_range()) {
                  const float t = row_t[px];
                  if (std::isnan(t)) {
                    continue;
                  }
                  float4 &pixel = scene_linear[px];
                  pixel = paints_colors ?
                              gradient_blend_color(coloring, pixel, t) :
                              gradient_blend_value(coloring, pixel, channel_value, t);
                }

                if (!float_buffer.is_empty()) {
                  write_image_pixels(
                      scene_linear, float_buffer, *processors, pixel_row, range, image_buffer->x);
                }
                else {
                  write_image_pixels(
                      scene_linear, byte_buffer, *processors, pixel_row, range, image_buffer->x);
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

  return WM_gesture_straightline_invoke(C, op, event);
}

static wmOperatorStatus sculpt_color_gradient_modal(bContext *C,
                                                    wmOperator *op,
                                                    const wmEvent *event)
{
  if (ELEM(event->type, WHEELUPMOUSE, WHEELDOWNMOUSE)) {
    const wmGesture *gesture = static_cast<const wmGesture *>(op->customdata);
    if (gesture != nullptr && gesture->is_active) {
      /* Shifting the halfway color along the drag gives a long, soft falloff without having to
       * drag far past the start. */
      const float step = (event->type == WHEELUPMOUSE) ? 0.05f : -0.05f;
      const float midpoint = math::clamp(
          RNA_float_get(op->ptr, "midpoint") + step, 0.05f, 0.95f);
      RNA_float_set(op->ptr, "midpoint", midpoint);
      sculpt_color_gradient_exec(C, op);
      sculpt_color_gradient_status_update(C, op);
      return OPERATOR_RUNNING_MODAL;
    }
  }

  const wmOperatorStatus ret = WM_gesture_straightline_modal(C, op, event);

  if (ret & OPERATOR_FINISHED) {
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
                       0.05f,
                       0.95f);

  WM_operator_properties_gesture_straightline(ot, WM_CURSOR_EDIT);
}

/** \} */

}  // namespace blender::ed::sculpt_paint::color
