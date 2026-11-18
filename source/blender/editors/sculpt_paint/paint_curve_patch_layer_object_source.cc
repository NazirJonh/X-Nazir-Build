/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The conversion half of the Curve Patch object source: a Curves object's splines into the
 * document, and the publish the session entry and the refresh share. The write-back lives in
 * `paint_curve_patch_layer_object_writeback.cc`, the refresh/restamp entry points in
 * `paint_curve_patch_layer_object_restamp.cc`.
 */

#include "paint_curve_patch_layer.hh"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "MEM_guardedalloc.h"

#include "BLI_function_ref.hh"
#include "BLI_hash.h"
#include "BLI_index_mask.hh"
#include "BLI_index_range.hh"
#include "BLI_listbase_iterator.hh"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_string_ref.hh"
#include "BLI_utildefines.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "DNA_ID.h"
#include "DNA_brush_types.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_paint_vector_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_view3d_types.h"

#include "BKE_attribute.hh"
#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_curve_patch.hh"
#include "BKE_curves.hh"
#include "BKE_customdata.hh"
#include "BKE_global.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_curve_patch_object.hh"
#include "BKE_paint_layers_generate.hh"
#include "BKE_paint_layers_gradient.hh"
#include "BKE_paint_layers_target.hh"
#include "BKE_paint_material_sync.hh"
#include "BKE_paint_vector.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "ED_curve_patch.hh"
#include "ED_image.hh"
#include "ED_object.hh"
#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_undo.hh"
#include "ED_view3d.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "mesh/paint_material_source.hh"
#include "paint_image_curve_patch.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"

#include "mesh/sculpt_intern.hh"
#include "paint_curve_geometry.hh"
#include "paint_curve_patch_apply.hh"
#include "paint_curve_patch_document.hh"
#include "paint_curve_patch_edit_intern.hh"
#include "paint_curve_patch_effect.hh"
#include "paint_curve_patch_session.hh"
#include "paint_curve_patch_space_convert.hh"
#include "paint_curve_screen.hh"

#include "paint_curve_patch_layer_object_source_intern.hh"

namespace blender::ed::sculpt_paint {

/** Whether the object the session will run on has an active UV map: the renderer projects the
 * object-space curves through it, so without one the tool has no canvas to write. */
bool curve_patch_layer_object_has_uv(const Object &ob)
{
  const Mesh *mesh = BKE_object_get_original_mesh(&ob);
  /* UV maps are attributes of the mesh, not CustomData layers, so the legacy layer lookup always
   * came back empty. */
  return mesh != nullptr && !mesh->uv_map_names().is_empty();
}

const Object *curve_patch_object_evaluated(const Depsgraph *depsgraph, const Object &curve_object)
{
  if (depsgraph == nullptr) {
    /* No graph (a background or context-free read): the original data is all there is. */
    return &curve_object;
  }
  const Object *evaluated = DEG_get_evaluated(depsgraph, &curve_object);
  /* A graph that has not evaluated the object yet (or a swapped type) answers the original back;
   * only a curves object carries the splines the mode reads. */
  if (evaluated == nullptr || evaluated->type != OB_CURVES) {
    return &curve_object;
  }
  return evaluated;
}

float4x4 curve_patch_curve_to_painted_space(const Object &curve_object,
                                            const Object &painted_object)
{
  /* The 3D Space coordinates are the painted object's local ones (the space the owned container's
   * curves live in), so a curve object's splines are mapped from their own local space into the
   * painted object's -- the same transform an imported PaintCurve takes
   * (#ED_paintcurve_import_from_source_object). */
  return painted_object.world_to_object() * curve_object.object_to_world();
}

float4x4 curve_patch_painted_to_curve_space(const Object &curve_object,
                                            const Object &painted_object)
{
  return curve_object.world_to_object() * painted_object.object_to_world();
}

bool curve_patch_spline_from_object(const Object &curve_object,
                                    const Object &painted_object,
                                    const int spline_i,
                                    bke::CurvesGeometry &r_spline)
{
  if (curve_object.type != OB_CURVES || curve_object.data == nullptr ||
      GS(static_cast<ID *>(curve_object.data)->name) != ID_CV)
  {
    return false;
  }
  const bke::CurvesGeometry &src = id_cast<const Curves *>(curve_object.data)->geometry.wrap();
  if (spline_i < 0 || spline_i >= src.curves_num()) {
    return false;
  }
  /* The same per-spline copy the Separate Geometry node's curve branch makes: it carries the
   * whole attribute set over (radius, handle positions and types, resolution, cyclic, whatever
   * else), which a hand-written gather would have to be kept in sync with forever. */
  bke::CurvesGeometry curve = bke::curves_copy_curve_selection(
      src, IndexMask(IndexRange(spline_i, 1)), {});
  if (curve.points_num() == 0) {
    /* A degenerate spline contributes nothing. */
    return false;
  }
  const float4x4 source_from_curve = curve_patch_curve_to_painted_space(curve_object,
                                                                        painted_object);
  /* Order matters: the handle position attributes have to exist before the recomputes, which
   * otherwise return silently and leave a bezier collapsed at the origin (the bug documented at
   * #ED_paintcurve_control_curve_for_patch). */
  curve.handle_positions_left_for_write();
  curve.handle_positions_right_for_write();
  curve.calculate_bezier_auto_handles();
  curve.calculate_bezier_aligned_handles();
  curve_patch_transform_points(curve, source_from_curve);
  /* The patch reads `radius` with 1.0 = full brush size, but a hair-style object curve without an
   * explicit radius attribute would answer the generic 0.01 default and produce a microscopic
   * strip. */
  if (!curve.attributes().contains("radius")) {
    curve.radius_for_write().fill(1.0f);
  }
  r_spline = std::move(curve);
  return true;
}

bool curve_patch_item_object_curve(const PaintVectorItem &item,
                                   const Object &painted_object,
                                   const Depsgraph *depsgraph,
                                   bke::CurvesGeometry &r_curve)
{
  if (item.type != PAINT_VECTOR_ITEM_CURVE_PATCH || item.curve_object == nullptr) {
    return false;
  }
  /* The evaluated copy carries modifiers and animation: the stamp reads the curve as the
   * depsgraph resolves it, the same copy the source refresh hashes. */
  const Object *curve_object = curve_patch_object_evaluated(depsgraph, *item.curve_object);
  return curve_patch_spline_from_object(
      *curve_object, painted_object, item.curve_object_spline, r_curve);
}

void curve_patch_document_apply_objects(const PaintVector &pv,
                                        const Object &painted_object,
                                        const Depsgraph *depsgraph,
                                        CurvePatchDocument &r_doc)
{
  for (CurvePatchItem &patch : r_doc.patches) {
    const int item_i = patch.source_curve_index;
    if (item_i < 0 || item_i >= pv.items_num) {
      continue;
    }
    /* A curve whose object spline cannot be read (a missing spline, a degenerate one) keeps the
     * geometry of its own item: the fallback the item stores. */
    bke::CurvesGeometry curve;
    if (curve_patch_item_object_curve(pv.items[item_i], painted_object, depsgraph, curve)) {
      patch.control_curve = std::move(curve);
    }
  }
}

/* -------------------------------------------------------------------- */
/** \name Curve data of a Curve Patch effect
 *
 * What publishes the session's document and what a committed document is written back into. The
 * container's items carry every curve's parameters (and the geometry of a curve without an
 * external source). A curve that names an external Curves object reads its control curve from the
 * object's spline instead and writes its shape edits back into it, so the object is the single
 * source of truth for that curve while it is set.
 * \{ */

/** Publish the session document of \a payload: the container's items, with the control curve of
 * every item that names an external object replaced by the object's spline. */
void curve_patch_publish_document(MaterialPaintLayerCurvePatch &payload,
                                  Object &painted_object,
                                  const Depsgraph *depsgraph,
                                  CurvePatchDocument &r_doc)
{
  curve_patch_document_from_vector(*payload.vectors, r_doc);
  if (BKE_paint_layers_curve_patch_reads_object(&payload)) {
    curve_patch_document_apply_objects(*payload.vectors, painted_object, depsgraph, r_doc);
  }
  /* Curves too short to build are dropped the same way the anchor adoption skips them. */
  r_doc.patches.remove_if(
      [](const CurvePatchItem &item) { return item.control_curve.points_num() < 2; });
}

}  // namespace blender::ed::sculpt_paint
