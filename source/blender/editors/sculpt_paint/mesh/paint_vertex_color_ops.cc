/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include <memory>

#include "BLI_array.hh"
#include "BLI_color.hh"
#include "BLI_function_ref.hh"
#include "BLI_listbase.h"
#include "BLI_math_base.h"
#include "BLI_math_color.h"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "BKE_attribute_math.hh"
#include "BKE_brush.hh"
#include "BKE_colortools.hh"
#include "BKE_context.hh"
#include "BKE_geometry_set.hh"
#include "BKE_mesh.hh"
#include "BKE_paint.hh"

#include "DEG_depsgraph.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_mesh.hh"
#include "ED_view3d.hh"

#include "../paint_gradient_core.hh"
#include "../paint_intern.hh" /* own include */
#include "sculpt_intern.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Internal Utility Functions
 * \{ */

static bool vertex_weight_paint_mode_poll(bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  Mesh *mesh = BKE_mesh_from_object(ob);
  return (ob && ELEM(ob->mode, OB_MODE_VERTEX_PAINT, OB_MODE_WEIGHT_PAINT)) &&
         (mesh && mesh->faces_num && !mesh->deform_verts().is_empty());
}

static void tag_object_after_update(Object &object)
{
  BLI_assert(object.type == OB_MESH);
  Mesh &mesh = *id_cast<Mesh *>(object.data);
  DEG_id_tag_update(&mesh.id, ID_RECALC_SYNC_TO_EVAL);
  /* NOTE: Original mesh is used for display, so tag it directly here. */
  BKE_mesh_batch_cache_dirty_tag(&mesh, BKE_MESH_BATCH_DIRTY_ALL);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vertex Color from Weight Operator
 * \{ */

static bool vertex_paint_from_weight(Object &ob)
{
  Mesh *mesh;
  if ((mesh = BKE_mesh_from_object(&ob)) == nullptr ||
      ED_mesh_color_ensure(mesh, nullptr) == false)
  {
    return false;
  }

  if (!mesh->attributes().contains(mesh->active_color_attribute)) {
    BLI_assert_unreachable();
    return false;
  }

  const int active_vertex_group_index = mesh->vertex_group_active_index - 1;
  const bDeformGroup *deform_group = static_cast<const bDeformGroup *>(
      BLI_findlink(&mesh->vertex_group_names, active_vertex_group_index));
  if (deform_group == nullptr) {
    BLI_assert_unreachable();
    return false;
  }

  bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();

  bke::GAttributeWriter color_attribute = attributes.lookup_for_write(
      mesh->active_color_attribute);
  if (!color_attribute) {
    BLI_assert_unreachable();
    return false;
  }

  /* Retrieve the vertex group with the domain and type of the existing color
   * attribute, in order to let the attribute API handle both conversions. */
  const GVArray vertex_group = *attributes.lookup(
      deform_group->name,
      bke::AttrDomain::Point,
      bke::cpp_type_to_attribute_type(color_attribute.varray.type()));
  if (!vertex_group) {
    BLI_assert_unreachable();
    return false;
  }

  GVArraySpan interpolated{
      attributes.adapt_domain(vertex_group, bke::AttrDomain::Point, color_attribute.domain)};

  color_attribute.varray.set_all(interpolated.data());
  color_attribute.finish();
  tag_object_after_update(ob);

  return true;
}

static wmOperatorStatus vertex_paint_from_weight_exec(bContext *C, wmOperator * /*op*/)
{
  Object *obact = CTX_data_active_object(C);
  if (vertex_paint_from_weight(*obact)) {
    WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, obact);
    return OPERATOR_FINISHED;
  }
  return OPERATOR_CANCELLED;
}

void PAINT_OT_vertex_color_from_weight(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Vertex Color from Weight";
  ot->idname = "PAINT_OT_vertex_color_from_weight";
  ot->description = "Convert active weight into gray scale vertex colors";

  /* API callbacks. */
  ot->exec = vertex_paint_from_weight_exec;
  ot->poll = vertex_weight_paint_mode_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* TODO: invert, alpha */
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Smooth Vertex Colors Operator
 * \{ */

static IndexMask get_selected_indices(const Mesh &mesh,
                                      const bke::AttrDomain domain,
                                      IndexMaskMemory &memory)
{
  const bke::AttributeAccessor attributes = mesh.attributes();

  if (mesh.editflag & ME_EDIT_PAINT_FACE_SEL) {
    const VArray<bool> selection = *attributes.lookup_or_default<bool>(
        ".select_poly", domain, false);
    return IndexMask::from_bools(selection, memory);
  }
  if (mesh.editflag & ME_EDIT_PAINT_VERT_SEL) {
    const VArray<bool> selection = *attributes.lookup_or_default<bool>(
        ".select_vert", domain, false);
    return IndexMask::from_bools(selection, memory);
  }
  return IndexMask(attributes.domain_size(domain));
}

static void face_corner_color_equalize_verts(Mesh &mesh, const IndexMask selection)
{
  const StringRef name = mesh.active_color_attribute;
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  bke::GSpanAttributeWriter attribute = attributes.lookup_for_write_span(name);
  if (!attribute) {
    BLI_assert_unreachable();
    return;
  }
  if (attribute.domain == bke::AttrDomain::Point) {
    return;
  }

  GVArray color_attribute_point = *attributes.lookup(name, bke::AttrDomain::Point);
  GVArray color_attribute_corner = attributes.adapt_domain(
      color_attribute_point, bke::AttrDomain::Point, bke::AttrDomain::Corner);
  color_attribute_corner.materialize(selection, attribute.span.data());
  attribute.finish();
}

static bool vertex_color_smooth(Object &ob)
{
  Mesh *mesh;
  if (((mesh = BKE_mesh_from_object(&ob)) == nullptr) ||
      (ED_mesh_color_ensure(mesh, nullptr) == false))
  {
    return false;
  }

  IndexMaskMemory memory;
  const IndexMask selection = get_selected_indices(*mesh, bke::AttrDomain::Corner, memory);

  face_corner_color_equalize_verts(*mesh, selection);

  tag_object_after_update(ob);

  return true;
}

static wmOperatorStatus vertex_color_smooth_exec(bContext *C, wmOperator * /*op*/)
{
  Object *obact = CTX_data_active_object(C);
  if (vertex_color_smooth(*obact)) {
    WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, obact);
    return OPERATOR_FINISHED;
  }
  return OPERATOR_CANCELLED;
}

void PAINT_OT_vertex_color_smooth(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Smooth Vertex Colors";
  ot->idname = "PAINT_OT_vertex_color_smooth";
  ot->description = "Smooth colors across vertices";

  /* API callbacks. */
  ot->exec = vertex_color_smooth_exec;
  ot->poll = vertex_paint_mode_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vertex Color Transformation Operators
 * \{ */

static void transform_active_color_data(
    Mesh &mesh, const FunctionRef<void(ColorGeometry4f &color)> transform_fn)
{
  const StringRef name = mesh.active_color_attribute;
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  if (!attributes.contains(name)) {
    BLI_assert_unreachable();
    return;
  }

  bke::GAttributeWriter color_attribute = attributes.lookup_for_write(name);
  if (!color_attribute) {
    BLI_assert_unreachable();
    return;
  }

  IndexMaskMemory memory;
  const IndexMask selection = get_selected_indices(mesh, color_attribute.domain, memory);

  selection.foreach_segment(
      [&](const IndexMaskSegment segment) {
        color_attribute.varray.type().to_static_type<ColorGeometry4f, ColorGeometry4b>(
            [&]<typename T>() {
              for ([[maybe_unused]] const int i : segment) {
                if constexpr (std::is_same_v<T, ColorGeometry4f>) {
                  ColorGeometry4f color = color_attribute.varray.get<ColorGeometry4f>(i);
                  transform_fn(color);
                  color_attribute.varray.set_by_copy(i, &color);
                }
                else if constexpr (std::is_same_v<T, ColorGeometry4b>) {
                  ColorGeometry4f color = color::decode(
                      color_attribute.varray.get<ColorGeometry4b>(i));
                  transform_fn(color);
                  ColorGeometry4b color_encoded = color::encode(color);
                  color_attribute.varray.set_by_copy(i, &color_encoded);
                }
              }
            });
      },
      exec_mode::grain_size(1024));

  color_attribute.finish();

  DEG_id_tag_update(&mesh.id, ID_RECALC_GEOMETRY);
}

static void transform_active_color(bContext *C,
                                   const FunctionRef<void(ColorGeometry4f &color)> transform_fn)
{
  using namespace blender::ed::sculpt_paint;
  Object &obact = *CTX_data_active_object(C);

  /* Ensure valid sculpt state. */
  BKE_sculpt_update_object_for_edit(CTX_data_ensure_evaluated_depsgraph(C), &obact, true);

  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(obact);

  IndexMaskMemory memory;
  const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);

  Mesh &mesh = *id_cast<Mesh *>(obact.data);
  transform_active_color_data(mesh, transform_fn);

  pbvh.tag_attribute_changed(node_mask, mesh.active_color_attribute);

  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, &obact);
}

static wmOperatorStatus vertex_color_brightness_contrast_exec(bContext *C, wmOperator *op)
{
  Object *obact = CTX_data_active_object(C);

  float gain, offset;
  {
    float brightness = RNA_float_get(op->ptr, "brightness");
    float contrast = RNA_float_get(op->ptr, "contrast");
    brightness /= 100.0f;
    float delta = contrast / 200.0f;
    /*
     * The algorithm is by Werner D. Streidt
     * (http://visca.com/ffactory/archives/5-99/msg00021.html)
     * Extracted of OpenCV `demhist.c`.
     */
    if (contrast > 0) {
      gain = 1.0f - delta * 2.0f;
      gain = 1.0f / max_ff(gain, FLT_EPSILON);
      offset = gain * (brightness - delta);
    }
    else {
      delta *= -1;
      gain = max_ff(1.0f - delta * 2.0f, 0.0f);
      offset = gain * brightness + delta;
    }
  }

  Mesh *mesh;
  if (((mesh = BKE_mesh_from_object(obact)) == nullptr) ||
      (ED_mesh_color_ensure(mesh, nullptr) == false))
  {
    return OPERATOR_CANCELLED;
  }

  transform_active_color(C, [&](ColorGeometry4f &color) {
    for (int i = 0; i < 3; i++) {
      color[i] = gain * color[i] + offset;
    }
  });

  return OPERATOR_FINISHED;
}

void PAINT_OT_vertex_color_brightness_contrast(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Vertex Paint Brightness/Contrast";
  ot->idname = "PAINT_OT_vertex_color_brightness_contrast";
  ot->description = "Adjust vertex color brightness/contrast";

  /* API callbacks. */
  ot->exec = vertex_color_brightness_contrast_exec;
  ot->poll = vertex_paint_mode_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* params */
  const float min = -100, max = +100;
  prop = RNA_def_float(ot->srna, "brightness", 0.0f, min, max, "Brightness", "", min, max);
  prop = RNA_def_float(ot->srna, "contrast", 0.0f, min, max, "Contrast", "", min, max);
  RNA_def_property_ui_range(prop, min, max, 1, 1);
}

static wmOperatorStatus vertex_color_hsv_exec(bContext *C, wmOperator *op)
{
  Object *obact = CTX_data_active_object(C);

  const float hue = RNA_float_get(op->ptr, "h");
  const float sat = RNA_float_get(op->ptr, "s");
  const float val = RNA_float_get(op->ptr, "v");

  Mesh *mesh;
  if (((mesh = BKE_mesh_from_object(obact)) == nullptr) ||
      (ED_mesh_color_ensure(mesh, nullptr) == false))
  {
    return OPERATOR_CANCELLED;
  }

  transform_active_color(C, [&](ColorGeometry4f &color) {
    float hsv[3];
    rgb_to_hsv_v(color, hsv);

    hsv[0] += (hue - 0.5f);
    if (hsv[0] > 1.0f) {
      hsv[0] -= 1.0f;
    }
    else if (hsv[0] < 0.0f) {
      hsv[0] += 1.0f;
    }
    hsv[1] *= sat;
    hsv[2] *= val;

    hsv_to_rgb_v(hsv, color);
  });

  return OPERATOR_FINISHED;
}

void PAINT_OT_vertex_color_hsv(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Vertex Paint Hue/Saturation/Value";
  ot->idname = "PAINT_OT_vertex_color_hsv";
  ot->description = "Adjust vertex color Hue/Saturation/Value";

  /* API callbacks. */
  ot->exec = vertex_color_hsv_exec;
  ot->poll = vertex_paint_mode_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* params */
  RNA_def_float(ot->srna, "h", 0.5f, 0.0f, 1.0f, "Hue", "", 0.0f, 1.0f);
  RNA_def_float(ot->srna, "s", 1.0f, 0.0f, 2.0f, "Saturation", "", 0.0f, 2.0f);

  ot->prop = RNA_def_float(ot->srna, "v", 1.0f, 0.0f, 2.0f, "Value", "", 0.0f, 2.0f);
  RNA_def_property_translation_context(ot->prop, BLT_I18NCONTEXT_COLOR);
}

static wmOperatorStatus vertex_color_invert_exec(bContext *C, wmOperator * /*op*/)
{
  Object *obact = CTX_data_active_object(C);

  Mesh *mesh;
  if (((mesh = BKE_mesh_from_object(obact)) == nullptr) ||
      (ED_mesh_color_ensure(mesh, nullptr) == false))
  {
    return OPERATOR_CANCELLED;
  }

  transform_active_color(C, [&](ColorGeometry4f &color) {
    for (int i = 0; i < 3; i++) {
      color[i] = 1.0f - color[i];
    }
  });

  return OPERATOR_FINISHED;
}

void PAINT_OT_vertex_color_invert(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Vertex Paint Invert";
  ot->idname = "PAINT_OT_vertex_color_invert";
  ot->description = "Invert RGB values";

  /* API callbacks. */
  ot->exec = vertex_color_invert_exec;
  ot->poll = vertex_paint_mode_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static wmOperatorStatus vertex_color_levels_exec(bContext *C, wmOperator *op)
{
  Object *obact = CTX_data_active_object(C);

  const float gain = RNA_float_get(op->ptr, "gain");
  const float offset = RNA_float_get(op->ptr, "offset");

  Mesh *mesh;
  if (((mesh = BKE_mesh_from_object(obact)) == nullptr) ||
      (ED_mesh_color_ensure(mesh, nullptr) == false))
  {
    return OPERATOR_CANCELLED;
  }

  transform_active_color(C, [&](ColorGeometry4f &color) {
    for (int i = 0; i < 3; i++) {
      color[i] = gain * (color[i] + offset);
    }
  });

  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, obact);

  return OPERATOR_FINISHED;
}

void PAINT_OT_vertex_color_levels(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Vertex Paint Levels";
  ot->idname = "PAINT_OT_vertex_color_levels";
  ot->description = "Adjust levels of vertex colors";

  /* API callbacks. */
  ot->exec = vertex_color_levels_exec;
  ot->poll = vertex_paint_mode_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* params */
  RNA_def_float(
      ot->srna, "offset", 0.0f, -1.0f, 1.0f, "Offset", "Value to add to colors", -1.0f, 1.0f);
  RNA_def_float(
      ot->srna, "gain", 1.0f, 0.0f, FLT_MAX, "Gain", "Value to multiply colors by", 0.0f, 10.0f);
}

/* -------------------------------------------------------------------- */
/** \name Vertex Color Gradient Operator
 * \{ */

static wmOperatorStatus vertex_color_gradient_exec(bContext *C, wmOperator *op)
{
  Object *obact = CTX_data_active_object(C);
  ARegion *region = CTX_wm_region(C);
  if (region == nullptr || region->regiondata == nullptr) {
    return OPERATOR_CANCELLED;
  }

  Mesh *mesh;
  if (((mesh = BKE_mesh_from_object(obact)) == nullptr) ||
      (ED_mesh_color_ensure(mesh, nullptr) == false))
  {
    return OPERATOR_CANCELLED;
  }

  ToolSettings *ts = CTX_data_tool_settings(C);
  Brush *brush = BKE_paint_brush(&ts->vpaint->paint);
  if (brush == nullptr) {
    return OPERATOR_CANCELLED;
  }

  BKE_curvemapping_init(brush->curve_distance_falloff);

  const int x_start = RNA_int_get(op->ptr, "xstart");
  const int y_start = RNA_int_get(op->ptr, "ystart");
  const int x_end = RNA_int_get(op->ptr, "xend");
  const int y_end = RNA_int_get(op->ptr, "yend");

  ed::sculpt_paint::gradient::Params gradient_params;
  gradient_params.type = (RNA_enum_get(op->ptr, "type") == WPAINT_GRADIENT_TYPE_LINEAR) ?
                             ed::sculpt_paint::gradient::Type::Linear :
                             ed::sculpt_paint::gradient::Type::Radial;
  /* This operator currently evaluates projected screen-space points.
   * Keep unsupported spaces as screen fallback for now. */
  gradient_params.space = ed::sculpt_paint::gradient::Space::Screen;
  gradient_params.start_ss = float2(float(x_start), float(y_start));
  gradient_params.end_ss = float2(float(x_end), float(y_end));
  gradient_params.hardness = RNA_float_get(op->ptr, "hardness");
  gradient_params.clamp_to_range = RNA_boolean_get(op->ptr, "clamp_to_range");
  gradient_params.curve = nullptr;
  gradient_params.clip_before_start = RNA_boolean_get(op->ptr, "clip_before_start");
  const std::unique_ptr<ed::sculpt_paint::gradient::Calculator> calculator =
      ed::sculpt_paint::gradient::create(gradient_params);

  const StringRef name = mesh->active_color_attribute;
  bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();
  const VArraySpan<bool> hide_vert = *attributes.lookup_or_default<bool>(
      ".hide_vert", bke::AttrDomain::Point, false);
  const VArraySpan<bool> hide_poly = *attributes.lookup_or_default<bool>(
      ".hide_poly", bke::AttrDomain::Face, false);
  if (!attributes.contains(name)) {
    BLI_assert_unreachable();
    return OPERATOR_CANCELLED;
  }

  bke::GAttributeWriter color_attribute = attributes.lookup_for_write(name);
  if (!color_attribute) {
    BLI_assert_unreachable();
    return OPERATOR_CANCELLED;
  }

  IndexMaskMemory memory;
  const IndexMask selection = get_selected_indices(*mesh, color_attribute.domain, memory);

  const Span<float3> positions = mesh->vert_positions();
  const Span<int> corner_verts = mesh->corner_verts();
  const Span<int> corner_to_face = mesh->corner_to_face_map();

  ED_view3d_init_mats_rv3d(obact, static_cast<RegionView3D *>(region->regiondata));

  const float3 brush_color = BKE_brush_color_get(&ts->vpaint->paint, brush);
  const float brush_alpha = brush->alpha;
  const bool clamp_to_range = gradient_params.clamp_to_range;
  const int symmetry = int(SCULPT_mesh_symmetry_xyz_get(*obact));

  selection.foreach_segment(
      [&](const IndexMaskSegment segment) {
        color_attribute.varray.type().to_static_type<ColorGeometry4f, ColorGeometry4b>(
            [&]<typename T>() {
              const auto evaluate_factor = [&](const float3 &position) {
                float factor = paint_projected_gradient_factor_with_symmetry(
                    region, *calculator, position, symmetry, mesh->radial_symmetry);
                return paint_gradient_finalize_factor(*brush, factor, clamp_to_range, brush_alpha);
              };

              for (const int i : segment) {
                const int vert = (color_attribute.domain == bke::AttrDomain::Point) ?
                                     i :
                                     corner_verts[i];

                if (hide_vert[vert]) {
                  continue;
                }
                if (color_attribute.domain == bke::AttrDomain::Corner &&
                    hide_poly[corner_to_face[i]]) {
                  continue;
                }

                const float factor = evaluate_factor(positions[vert]);

                if (factor == 0.0f) {
                  continue;
                }

                if constexpr (std::is_same_v<T, ColorGeometry4f>) {
                  ColorGeometry4f color = color_attribute.varray.get<ColorGeometry4f>(i);
                  for (const int channel : IndexRange(3)) {
                    color[channel] = interpf(brush_color[channel], color[channel], factor);
                  }
                  color_attribute.varray.set_by_copy(i, &color);
                }
                else if constexpr (std::is_same_v<T, ColorGeometry4b>) {
                  ColorGeometry4f color = color::decode(
                      color_attribute.varray.get<ColorGeometry4b>(i));
                  for (const int channel : IndexRange(3)) {
                    color[channel] = interpf(brush_color[channel], color[channel], factor);
                  }
                  const ColorGeometry4b encoded = color::encode(color);
                  color_attribute.varray.set_by_copy(i, &encoded);
                }
              }
            });
      },
      exec_mode::grain_size(1024));

  color_attribute.finish();
  tag_object_after_update(*obact);
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, obact);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus vertex_color_gradient_invoke(bContext *C,
                                                     wmOperator *op,
                                                     const wmEvent *event)
{
  return WM_gesture_straightline_invoke(C, op, event);
}

void PAINT_OT_vertex_color_gradient(wmOperatorType *ot)
{
  ot->name = "Vertex Color Gradient";
  ot->idname = "PAINT_OT_vertex_color_gradient";
  ot->description = "Apply a screen-space gradient to selected vertex colors";

  ot->invoke = vertex_color_gradient_invoke;
  ot->modal = WM_gesture_straightline_modal;
  ot->exec = vertex_color_gradient_exec;
  ot->poll = vertex_paint_poll_ignore_tool;
  ot->cancel = WM_gesture_straightline_cancel;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_DEPENDS_ON_CURSOR;

  paint_gradient_operator_properties(ot, WPAINT_GRADIENT_TYPE_LINEAR, PAINT_GRADIENT_SPACE_SCREEN);

  WM_operator_properties_gesture_straightline(ot, WM_CURSOR_EDIT);
}

/** \} */

/** \} */

}  // namespace blender
