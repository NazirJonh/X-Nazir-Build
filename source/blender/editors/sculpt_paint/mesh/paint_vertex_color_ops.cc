/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "MEM_guardedalloc.h"

#include "DNA_brush_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BLI_array.hh"
#include "BLI_bitmap.h"
#include "BLI_color.hh"
#include "BLI_function_ref.hh"
#include "BLI_listbase.h"
#include "BLI_math_base.h"
#include "BLI_math_color.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "BKE_attribute_math.hh"
#include "BKE_brush.hh"
#include "BKE_colortools.hh"
#include "BKE_context.hh"
#include "BKE_geometry_set.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_iterators.hh"
#include "BKE_object.hh"
#include "BKE_paint.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_mesh.hh"
#include "ED_view3d.hh"

#include "../paint_intern.hh" /* own include */
#include "../paint_gradient_core.hh"
#include "sculpt_intern.hh"

#include <algorithm>
#include <memory>

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

  /* Hidden should never count as selected. */
  IndexMask visible = IndexMask::from_bools_inverse(
      *attributes.lookup_or_default<bool>(".hide_poly", domain, false), memory);

  if (mesh.editflag & ME_EDIT_PAINT_FACE_SEL) {
    const VArray<bool> selection = *attributes.lookup_or_default<bool>(
        ".select_poly", domain, false);
    return IndexMask::from_bools(visible, selection, memory);
  }
  if (mesh.editflag & ME_EDIT_PAINT_VERT_SEL) {
    const VArray<bool> selection = *attributes.lookup_or_default<bool>(
        ".select_vert", domain, false);
    return IndexMask::from_bools(visible, selection, memory);
  }

  return visible;
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
  if (attribute.domain != bke::AttrDomain::Point) {
    GVArray color_attribute_point = *attributes.lookup(name, bke::AttrDomain::Point);
    GVArray color_attribute_corner = attributes.adapt_domain(
        color_attribute_point, bke::AttrDomain::Point, bke::AttrDomain::Corner);
    color_attribute_corner.materialize(selection, attribute.span.data());
  }
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

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vertex Color Gradient Operator
 *
 * Straight-line gesture that blends the active brush color over the active color
 * attribute, built on the shared `gradient::Calculator` engine
 * (`paint_gradient_core`). Structure and UX mirror `PAINT_OT_weight_gradient`:
 * - vertex screen positions are cached once on init, so later previews cannot feed
 *   back on already-modified geometry,
 * - every preview blends from the init snapshot, making repeated previews idempotent,
 * - cancelling restores the snapshot (regular undo covers the finished stroke).
 *
 * Direction and brush-curve semantics match the weight gradient: the brush color is
 * strongest at the drag start and falls off toward the original color at the end
 * (`BKE_brush_curve_strength_clamped` maps the raw factor as a distance).
 * \{ */

enum {
  VC_GRADIENT_TYPE_LINEAR = 0,
  VC_GRADIENT_TYPE_RADIAL = 1,
};

/* Gesture-owned snapshot (plain MEM allocations: `wmGesture.user_data` bypasses
 * constructors/destructors, so no `Array`, `VArray` or `std::string` members here). */
struct VCGradient_State {
  float *vert_sco;         /* [verts_num * 2] cached screen positions, FLT_MAX == skip. */
  float *orig_colors;      /* [totelem * 4] decoded snapshot (RGBA floats). */
  BLI_bitmap *elem_selected; /* [totelem] paintable domain elements. */
  int verts_num;
  int totelem;
  bke::AttrDomain domain;
};

struct VCGradient_InitData {
  ARegion *region;
  VCGradient_State *state;
  BLI_bitmap *vert_visit;
};

static void vcgradient_init_mapfunc(void *user_data,
                                    int index,
                                    const float co[3],
                                    const float /*no*/[3])
{
  VCGradient_InitData *data = static_cast<VCGradient_InitData *>(user_data);
  /* Generative modifiers may map several evaluated verts onto one original index: the first
   * position wins, mirroring the weight gradient. */
  if (BLI_BITMAP_TEST(data->vert_visit, index)) {
    return;
  }
  float *sco = &data->state->vert_sco[index * 2];
  if (ED_view3d_project_float_object(
          data->region, co, sco, V3D_PROJ_TEST_CLIP_BB | V3D_PROJ_TEST_CLIP_NEAR) !=
      V3D_PROJ_RET_OK)
  {
    sco[0] = FLT_MAX;
    sco[1] = FLT_MAX;
  }
  BLI_BITMAP_ENABLE(data->vert_visit, index);
}

static VCGradient_State *vcgradient_state_create(bContext *C,
                                                 Object &ob,
                                                 Mesh &mesh,
                                                 ARegion *region)
{
  if (!ED_mesh_color_ensure(&mesh, nullptr)) {
    return nullptr;
  }
  if (mesh.active_color_attribute == nullptr) {
    return nullptr;
  }

  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  bke::GAttributeWriter color_attribute = attributes.lookup_for_write(mesh.active_color_attribute);
  if (!color_attribute) {
    return nullptr;
  }
  const bke::AttrDomain domain = color_attribute.domain;
  const int totelem = int(color_attribute.varray.size());
  if (totelem <= 0 || mesh.verts_num <= 0) {
    return nullptr;
  }

  VCGradient_State *state = MEM_new<VCGradient_State>(__func__);
  state->domain = domain;
  state->verts_num = mesh.verts_num;
  state->totelem = totelem;
  state->vert_sco = MEM_new_array<float>(size_t(mesh.verts_num) * 2, "VCGradient sco");
  state->orig_colors = MEM_new_array<float>(size_t(totelem) * 4, "VCGradient orig");
  state->elem_selected = BLI_BITMAP_NEW(totelem, "VCGradient selected");

  /* Snapshot originals decoded to float. */
  color_attribute.varray.type().to_static_type<ColorGeometry4f, ColorGeometry4b>(
      [&]<typename T>() {
        for (int i = 0; i < totelem; i++) {
          ColorGeometry4f c;
          if constexpr (std::is_same_v<T, ColorGeometry4f>) {
            c = color_attribute.varray.get<ColorGeometry4f>(i);
          }
          else {
            c = color::decode(color_attribute.varray.get<ColorGeometry4b>(i));
          }
          for (int ch = 0; ch < 4; ch++) {
            state->orig_colors[i * 4 + ch] = c[ch];
          }
        }
      });
  color_attribute.finish();

  /* Paintable elements: face/vertex selection, minus hidden verts. */
  IndexMaskMemory sel_memory;
  const IndexMask selection = get_selected_indices(mesh, domain, sel_memory);
  selection.foreach_index([&](const int i) { BLI_BITMAP_ENABLE(state->elem_selected, i); });
  const VArray<bool> hide_vert = *attributes.lookup_or_default<bool>(
      ".hide_vert", bke::AttrDomain::Point, false);
  if (domain == bke::AttrDomain::Point) {
    for (int i = 0; i < totelem; i++) {
      if (hide_vert[i]) {
        BLI_BITMAP_DISABLE(state->elem_selected, i);
      }
    }
  }
  else {
    const Span<int> corner_verts = mesh.corner_verts();
    for (int i = 0; i < totelem; i++) {
      if (hide_vert[corner_verts[i]]) {
        BLI_BITMAP_DISABLE(state->elem_selected, i);
      }
    }
  }

  /* Cache screen positions from the evaluated mesh (correct under modifiers). */
  for (int i = 0; i < mesh.verts_num; i++) {
    state->vert_sco[i * 2] = FLT_MAX;
    state->vert_sco[i * 2 + 1] = FLT_MAX;
  }
  ED_view3d_init_mats_rv3d(&ob, static_cast<RegionView3D *>(region->regiondata));
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  const Object *ob_eval = DEG_get_evaluated(depsgraph, &ob);
  const Mesh *mesh_eval = BKE_object_get_evaluated_mesh(ob_eval);
  BLI_bitmap *vert_visit = BLI_BITMAP_NEW(mesh.verts_num, __func__);
  VCGradient_InitData init_data{region, state, vert_visit};
  BKE_mesh_foreach_mapped_vert(mesh_eval, vcgradient_init_mapfunc, &init_data, MESH_FOREACH_NOP);
  MEM_delete(vert_visit);
  return state;
}

/* Paint (`vert_weight != nullptr`) or restore (`vert_weight == nullptr`) the snapshot. */
static bool vcgradient_write_colors(Mesh &mesh,
                                    VCGradient_State *state,
                                    const float *vert_weight,
                                    const float3 &brush_color)
{
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  if (mesh.active_color_attribute == nullptr) {
    return false;
  }
  bke::GAttributeWriter color_attribute = attributes.lookup_for_write(mesh.active_color_attribute);
  if (!color_attribute || color_attribute.domain != state->domain ||
      int(color_attribute.varray.size()) != state->totelem)
  {
    return false;
  }
  const int totelem = state->totelem;
  const Span<int> corner_verts = (state->domain == bke::AttrDomain::Corner) ?
                                     mesh.corner_verts() :
                                     Span<int>();
  const float *orig = state->orig_colors;
  color_attribute.varray.type().to_static_type<ColorGeometry4f, ColorGeometry4b>(
      [&]<typename T>() {
        for (int i = 0; i < totelem; i++) {
          ColorGeometry4f mixed;
          if (vert_weight == nullptr) {
            for (int ch = 0; ch < 4; ch++) {
              mixed[ch] = orig[i * 4 + ch];
            }
          }
          else {
            if (!BLI_BITMAP_TEST(state->elem_selected, i)) {
              continue;
            }
            const int vert = (state->domain == bke::AttrDomain::Point) ? i : corner_verts[i];
            const float w = vert_weight[vert];
            for (int ch = 0; ch < 3; ch++) {
              mixed[ch] = orig[i * 4 + ch] + (brush_color[ch] - orig[i * 4 + ch]) * w;
            }
            mixed[3] = orig[i * 4 + 3];
          }
          if constexpr (std::is_same_v<T, ColorGeometry4f>) {
            color_attribute.varray.set_by_copy(i, &mixed);
          }
          else {
            const ColorGeometry4b encoded = color::encode(mixed);
            color_attribute.varray.set_by_copy(i, &encoded);
          }
        }
      });
  color_attribute.finish();
  return true;
}

static void vcgradient_state_update(Object &ob,
                                    Mesh &mesh,
                                    VCGradient_State *state,
                                    const ed::sculpt_paint::gradient::Calculator &calculator,
                                    const Brush &brush,
                                    const float3 &brush_color,
                                    const float brush_alpha)
{
  /* Positions are cached, so no mesh mapping is needed on update. */
  Array<float> vert_weight(state->verts_num, 0.0f);
  const float *sco = state->vert_sco;
  for (int i = 0; i < state->verts_num; i++) {
    const float x = sco[i * 2];
    if (x == FLT_MAX) {
      continue;
    }
    const float t = calculator.evaluate(float3(x, sco[i * 2 + 1], 0.0f));
    vert_weight[i] = BKE_brush_curve_strength_clamped(&brush, std::max(0.0f, t), 1.0f) *
                     brush_alpha;
  }
  if (vcgradient_write_colors(mesh, state, vert_weight.data(), brush_color)) {
    tag_object_after_update(ob);
  }
}

static void vcgradient_state_restore(Object &ob, Mesh &mesh, VCGradient_State *state)
{
  const float3 unused(0.0f);
  if (vcgradient_write_colors(mesh, state, nullptr, unused)) {
    tag_object_after_update(ob);
  }
}

static void vcgradient_state_free(VCGradient_State *state)
{
  if (state == nullptr) {
    return;
  }
  MEM_delete(state->vert_sco);
  MEM_delete(state->orig_colors);
  MEM_delete(state->elem_selected);
  MEM_delete(state);
}

static wmOperatorStatus paint_vertex_color_gradient_exec(bContext *C, wmOperator *op)
{
  wmGesture *gesture = static_cast<wmGesture *>(op->customdata);
  ARegion *region = CTX_wm_region(C);
  Object *ob = CTX_data_active_object(C);
  Mesh *mesh = (ob != nullptr) ? BKE_mesh_from_object(ob) : nullptr;
  if (mesh == nullptr) {
    return OPERATOR_CANCELLED;
  }
  const bool is_interactive = (gesture != nullptr);

  bool created_here = false;
  VCGradient_State *state = nullptr;
  if (is_interactive) {
    state = static_cast<VCGradient_State *>(gesture->user_data.data);
    if (state == nullptr) {
      state = vcgradient_state_create(C, *ob, *mesh, region);
      if (state == nullptr) {
        return OPERATOR_CANCELLED;
      }
      gesture->user_data.data = state;
      gesture->user_data.use_free = false;
      created_here = true;
    }
  }
  else {
    state = vcgradient_state_create(C, *ob, *mesh, region);
    if (state == nullptr) {
      return OPERATOR_CANCELLED;
    }
    created_here = true;
  }

  ToolSettings *ts = CTX_data_tool_settings(C);
  VPaint *vp = (ts != nullptr) ? ts->vpaint : nullptr;
  Brush *brush = (vp != nullptr) ? BKE_paint_brush(&vp->paint) : nullptr;
  if (brush == nullptr) {
    if (created_here) {
      vcgradient_state_free(state);
      if (is_interactive) {
        gesture->user_data.data = nullptr;
      }
    }
    return OPERATOR_CANCELLED;
  }

  BKE_curvemapping_init(brush->curve_distance_falloff);

  /* Raw (unclamped) factor: the brush falloff curve below performs the clamping, matching the
   * weight gradient pipeline. */
  ed::sculpt_paint::gradient::Params params;
  params.type = (RNA_enum_get(op->ptr, "type") == VC_GRADIENT_TYPE_RADIAL) ?
                    ed::sculpt_paint::gradient::Type::Radial :
                    ed::sculpt_paint::gradient::Type::Linear;
  params.space = ed::sculpt_paint::gradient::Space::Screen;
  params.start_ss = float2(float(RNA_int_get(op->ptr, "xstart")),
                           float(RNA_int_get(op->ptr, "ystart")));
  params.end_ss = float2(float(RNA_int_get(op->ptr, "xend")),
                         float(RNA_int_get(op->ptr, "yend")));
  params.clamp_to_range = false;
  const std::unique_ptr<ed::sculpt_paint::gradient::Calculator> calculator =
      ed::sculpt_paint::gradient::create(params);

  const float3 brush_color = BKE_brush_color_get(&vp->paint, brush);
  vcgradient_state_update(*ob, *mesh, state, *calculator, *brush, brush_color, brush->alpha);

  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
  if (!is_interactive) {
    vcgradient_state_free(state);
  }
  return OPERATOR_FINISHED;
}

static wmOperatorStatus paint_vertex_color_gradient_invoke(bContext *C,
                                                           wmOperator *op,
                                                           const wmEvent *event)
{
  Object *ob = CTX_data_active_object(C);
  Mesh *mesh = (ob != nullptr) ? BKE_mesh_from_object(ob) : nullptr;
  if (mesh == nullptr || !ED_mesh_color_ensure(mesh, nullptr)) {
    return OPERATOR_CANCELLED;
  }
  return WM_gesture_straightline_invoke(C, op, event);
}

static wmOperatorStatus paint_vertex_color_gradient_modal(bContext *C,
                                                          wmOperator *op,
                                                          const wmEvent *event)
{
  wmGesture *gesture = static_cast<wmGesture *>(op->customdata);
  VCGradient_State *state = static_cast<VCGradient_State *>(gesture->user_data.data);
  Object *ob = CTX_data_active_object(C);

  const wmOperatorStatus ret = WM_gesture_straightline_modal(C, op, event);
  if (ret & OPERATOR_FINISHED) {
    /* The gesture already ran the final exec (live preview is the final state). */
    if (state != nullptr) {
      vcgradient_state_free(state);
      gesture->user_data.data = nullptr;
    }
    if (ob != nullptr) {
      WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
    }
    return OPERATOR_FINISHED;
  }
  if (ret & OPERATOR_CANCELLED) {
    if (state != nullptr && ob != nullptr) {
      Mesh *mesh = BKE_mesh_from_object(ob);
      if (mesh != nullptr) {
        vcgradient_state_restore(*ob, *mesh, state);
      }
      vcgradient_state_free(state);
      gesture->user_data.data = nullptr;
      WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
    }
    return OPERATOR_CANCELLED;
  }
  return OPERATOR_RUNNING_MODAL;
}

static void paint_vertex_color_gradient_cancel(bContext *C, wmOperator *op)
{
  wmGesture *gesture = static_cast<wmGesture *>(op->customdata);
  VCGradient_State *state = (gesture != nullptr) ?
                                static_cast<VCGradient_State *>(gesture->user_data.data) :
                                nullptr;
  Object *ob = CTX_data_active_object(C);
  if (state != nullptr && ob != nullptr) {
    Mesh *mesh = BKE_mesh_from_object(ob);
    if (mesh != nullptr) {
      vcgradient_state_restore(*ob, *mesh, state);
    }
    vcgradient_state_free(state);
    if (gesture != nullptr) {
      gesture->user_data.data = nullptr;
    }
    WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
  }
  WM_gesture_straightline_cancel(C, op);
}

void PAINT_OT_vertex_color_gradient(wmOperatorType *ot)
{
  static const EnumPropertyItem gradient_types[] = {
      {VC_GRADIENT_TYPE_LINEAR, "LINEAR", 0, "Linear", ""},
      {VC_GRADIENT_TYPE_RADIAL, "RADIAL", 0, "Radial", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Vertex Color Gradient";
  ot->idname = "PAINT_OT_vertex_color_gradient";
  ot->description = "Draw a line to apply a color gradient to the active color attribute";

  /* API callbacks. */
  ot->invoke = paint_vertex_color_gradient_invoke;
  ot->modal = paint_vertex_color_gradient_modal;
  ot->exec = paint_vertex_color_gradient_exec;
  ot->poll = vertex_paint_poll_ignore_tool;
  ot->cancel = paint_vertex_color_gradient_cancel;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_DEPENDS_ON_CURSOR;

  prop = RNA_def_enum(ot->srna, "type", gradient_types, 0, "Type", "");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);

  WM_operator_properties_gesture_straightline(ot, WM_CURSOR_EDIT);
}

/** \} */

}  // namespace blender
