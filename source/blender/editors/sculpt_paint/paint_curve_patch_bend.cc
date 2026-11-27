/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The Text Decal half of the Curve Patch bridge: the bend curve a Text Decal binds to, and the
 * live/restore passes of the two bend sessions. A bend row is a container, never a paint canvas,
 * so it stamps nothing and only re-tags the decal that follows its curve.
 */

#include "paint_curve_patch_layer.hh"
#include "paint_curve_patch_layer_intern.hh"

#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.h"
#include "BLI_span.hh"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "DNA_brush_types.h"
#include "DNA_material_types.h"
#include "DNA_object_types.h"
#include "DNA_paint_vector_types.h"

#include "BKE_context.hh"
#include "BKE_curves.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_vector.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "paint_curve_patch_document.hh"
#include "paint_curve_patch_session.hh"
#include "paint_image_curve_patch.hh"

namespace blender::ed::sculpt_paint {

/**
 * Append the Text Decal's bend curve: a two-point Bezier segment across the raster's middle, in the
 * container's own 2D UV (0,0 lower-left, 1,1 upper-right), off the edges so both ends can be
 * grabbed. The frozen stamp parameters still come from the brush, so the curve reads like any
 * other Curve Patch curve.
 */
PaintVectorItem &curve_patch_layer_bend_curve_item_add(PaintVector &pv, const Brush *brush)
{
  PaintVectorItem &item = curve_patch_layer_curve_item_default(pv, brush);
  float radius;
  float radius_per_size;
  curve_patch_layer_brush_radius_for_space(brush, true, radius, radius_per_size);
  if (brush != nullptr) {
    curve_patch_layer_item_params_refresh(
        item, *brush, radius, radius_per_size, float3(0.0f, 0.0f, 1.0f));
  }
  bke::CurvesGeometry &geom = item.curve_patch_geometry.wrap();
  if (geom.points_num() == 2) {
    const MutableSpan<float3> positions = geom.positions_for_write();
    positions[0] = float3(0.1f, 0.5f, 0.0f);
    positions[1] = float3(0.9f, 0.5f, 0.0f);
    geom.handle_positions_left_for_write();
    geom.handle_positions_right_for_write();
    geom.calculate_bezier_auto_handles();
    geom.tag_positions_changed();
  }
  item.flag &= ~PAINT_VECTOR_ITEM_CURVE_SEED;
  BKE_paint_vector_tag_changed(pv);
  return item;
}

MaterialPaintLayer *ED_paint_layers_curve_patch_bend_row_add(bContext &C,
                                                            Object &painted,
                                                            Material &ma,
                                                            MaterialPaintLayer &text_row)
{
  MaterialPaintLayer *owner = text_row.role == MA_PAINT_LAYER_ROLE_MASK_ITEM ?
                                  BKE_paint_layers_mask_item_owner(ma, text_row) :
                                  BKE_paint_layers_correction_owner(ma, text_row);
  if (owner == nullptr) {
    return nullptr;
  }
  /* "New Curve" replaces the curve it already has instead of leaving the old effect behind as an
   * orphan. The row is a container, so resetting it is the whole of the operation. */
  MaterialPaintLayer *effect = nullptr;
  bool created = false;
  if (const MaterialPaintLayer *existing = BKE_paint_layers_text_decal_bend_row_get(ma, text_row);
      existing != nullptr && existing->curve_patch != nullptr)
  {
    effect = const_cast<MaterialPaintLayer *>(existing);
    BKE_paint_vector_items_clear(*effect->curve_patch->vectors);
  }
  else {
    effect = BKE_paint_layers_curve_patch_effect_add(
        ma, owner, "Text Bend Curve", *CTX_data_main(&C));
    created = true;
  }
  if (effect == nullptr || effect->curve_patch == nullptr) {
    return nullptr;
  }
  MaterialPaintLayerCurvePatch &payload = *effect->curve_patch;
  /* A new bend starts in 3D Space: the curve is an object-space line across the decal frame and
   * the text is read as Projected through that same frame, so New Curve shows the bend at once in
   * the 3D Viewport. The Space switch can move it to 2D (material UV) later. */
  payload.space = MA_CURVE_PATCH_SPACE_3D;
  payload.stored_space = MA_CURVE_PATCH_SPACE_3D;
  BKE_paint_layers_text_decal_bend_space_sync(ma, text_row, false);
  /* The effect exists only to carry the curve: it lays nothing over the layer (opacity zero), and
   * its session never stamps (see #CurvePatchSession::is_bend_curve_only). Its channels stay on --
   * a Curve Patch session needs an enabled one for its canvas -- but their maps are emptied when
   * the tool opens and are never written, so no stamp trace is left behind. */
  BKE_paint_layers_set_opacity(ma, effect, 0.0f);
  for (const MaterialPaintLayerChannel &record :
       Span<const MaterialPaintLayerChannel>(effect->channels, effect->channels_num))
  {
    BKE_paint_layers_channel_set_enabled(ma, effect, eMaterialPaintChannel(record.channel), true);
  }

  {
    PaintVector &pv = *payload.vectors;
    PaintVectorItem &item = curve_patch_layer_curve_item_default(pv, payload.brush);
    float radius;
    float radius_per_size;
    curve_patch_layer_brush_radius_for_space(payload.brush, false, radius, radius_per_size);
    if (payload.brush != nullptr) {
      curve_patch_layer_item_params_refresh(
          item, *payload.brush, radius, radius_per_size, float3(0.0f, 0.0f, 1.0f));
    }

    /* A segment across the decal frame's middle, off its edges so both ends can be grabbed,
     * written in the painted object's space. The frame's own values live in the controller's space
     * when one is attached, so both the centre and the axes are carried into the painted object
     * first. */
    const MaterialPaintLayerProjection *projection = text_row.projection;
    const float default_normal[3] = {0.0f, 0.0f, 1.0f};
    const float *normal = (projection != nullptr) ? projection->normal : default_normal;
    const float roll = (projection != nullptr) ? projection->roll : 0.0f;
    float axes[3][3];
    BKE_paint_layers_projection_frame_matrix(normal, roll, axes);
    float center[3] = {0.0f, 0.0f, 0.0f};
    if (projection != nullptr) {
      copy_v3_v3(center, projection->center);
    }
    float size[3];
    BKE_paint_layers_projection_size_effective(text_row, size);

    if (text_row.projection_object != nullptr) {
      /* Controller space -> world -> the painted object's space, for the centre and the axes. */
      float center_world[3];
      mul_v3_m4v3(center_world, text_row.projection_object->object_to_world().ptr(), center);
      mul_v3_m4v3(center, painted.world_to_object().ptr(), center_world);
      float ctrl_to_world[3][3];
      float world_to_painted[3][3];
      copy_m3_m4(ctrl_to_world, text_row.projection_object->object_to_world().ptr());
      copy_m3_m4(world_to_painted, painted.world_to_object().ptr());
      float conversion[3][3];
      mul_m3_m3m3(conversion, world_to_painted, ctrl_to_world);
      for (float (&axis)[3] : axes) {
        mul_m3_v3(conversion, axis);
      }
    }

    /* The two footprint axes of the projection direction: the first is the one the raster's U
     * reads, so the seed runs along it. */
    const int8_t axis =
        (projection != nullptr) ? BKE_paint_layers_projection_axis_get(text_row) :
                                  int8_t(MA_PAINT_LAYER_PROJECTION_AXIS_Z);
    const int u_index = (axis == MA_PAINT_LAYER_PROJECTION_AXIS_X) ? 1 : 0;
    const float half_extent = BKE_paint_layers_projection_size_normalize(size[u_index]) * 0.5f;
    const float along[3] = {axes[u_index][0] * half_extent * 0.8f,
                            axes[u_index][1] * half_extent * 0.8f,
                            axes[u_index][2] * half_extent * 0.8f};

    bke::CurvesGeometry &geom = item.curve_patch_geometry.wrap();
    if (geom.points_num() == 2) {
      const MutableSpan<float3> positions = geom.positions_for_write();
      positions[0] = float3(center[0] - along[0], center[1] - along[1], center[2] - along[2]);
      positions[1] = float3(center[0] + along[0], center[1] + along[1], center[2] + along[2]);
      geom.handle_positions_left_for_write();
      geom.handle_positions_right_for_write();
      geom.calculate_bezier_auto_handles();
      geom.tag_positions_changed();
    }
    item.flag &= ~PAINT_VECTOR_ITEM_CURVE_SEED;
    BKE_paint_vector_tag_changed(pv);
  }

  if (!BKE_paint_layers_text_decal_bend_row_set(ma, text_row, effect->marker)) {
    /* A refused bend (a linked material) must not leave a curve effect behind that nothing uses. */
    if (created) {
      BKE_paint_layers_remove(ma, effect);
    }
    return nullptr;
  }
  /* Pin the object the operator is working on: the curve was just written in its space, so a later
   * refresh with no editor context must not guess "the first mesh that uses the material". */
  BKE_paint_layers_text_decal_bend_painted_object_set(ma, text_row, &painted);
  /* Switch the bend on so the new curve shows at once; it may already be on when the user is only
   * replacing the row, and the setter then reports that nothing changed. */
  BKE_paint_layers_text_decal_bend_enabled_set(ma, text_row, true);
  /* A bend curve that leaves the frame must clip there, not fold back through the raster and
   * scramble the glyph order: the decal's wrap is Clip whenever a bend is authored. The depth slab
   * takes the Decal preset's default at the same time. */
  text_row.mapping.wrap = MA_PAINT_LAYER_MAPPING_WRAP_CLIP;
  if (text_row.text_decal != nullptr &&
      text_row.text_decal->space == MA_PAINT_LAYER_TEXT_SPACE_CANVAS)
  {
    /* The canvas text is authored and read at raw UV; a mapping left from the projected decal
     * would shift the coverage under the curve. The projection payload stays for a later trip
     * back to Projected. */
    BKE_paint_layers_mapping_reset_raw_uv(ma, &text_row);
  }
  BKE_paint_layers_projection_set_depth_size(
      ma, &text_row, PAINT_LAYER_PROJECTION_DEPTH_DECAL_DEFAULT);
  BKE_paint_layers_tag_edited(ma);
  return effect;
}

void curve_patch_layer_bend_text_space_sync(Material &ma,
                                            MaterialPaintLayer &text_row,
                                            const bool to_2d)
{
  BKE_paint_layers_text_decal_bend_space_sync(ma, text_row, to_2d);
}

void curve_patch_layer_live_update(bContext &C, CurvePatchSession &session)
{
  if (!session.is_bend_curve_only || session.layer_material == nullptr ||
      session.doc.patches.is_empty())
  {
    return;
  }
  MaterialPaintLayer *row = curve_patch_layer_document_to_row(session, false);
  if (row == nullptr) {
    return;
  }
  Material &ma = *session.layer_material;
  /* No map is rendered from a bend row: tag the material and let the Text Decal rasterize again
   * from the container the edit just wrote. The refresh runs on the following scene update, not
   * inside this modal event: re-entering the BLF rasterizer from a live Sculpt session is what a
   * modal must not do. */
  BKE_paint_layers_tag_edited(ma);
  WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &ma.id);
}

void curve_patch_layer_live_update_2d(bContext &C, ImageCurvePatchSession &session)
{
  if (!session.is_bend_curve_only || session.layer_material == nullptr) {
    return;
  }
  MaterialPaintLayer *row = curve_patch_layer_session_row_2d(session);
  if (row == nullptr || row->curve_patch == nullptr || row->curve_patch->vectors == nullptr) {
    return;
  }
  PaintVector &pv = *row->curve_patch->vectors;
  curve_patch_document_write_items(
      session.doc,
      pv,
      [&](const int patch_i) -> const bke::CurvesGeometry * {
        return session.uv_curves.index_range().contains(patch_i) ? &session.uv_curves[patch_i] :
                                                                   nullptr;
      },
      true);
  BKE_paint_vector_tag_changed(pv);
  Material &ma = *session.layer_material;
  /* The same rule as the 3D live path: tag the material and let the next scene update run the
   * Text Decal refresh. Rasterizing inside a modal handler would stall the UI thread on a 2048²
   * bake and re-enter the BLF rasterizer from a live session. */
  BKE_paint_layers_tag_edited(ma);
  WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &ma.id);
}

void curve_patch_layer_restore_session_2d(bContext &C, ImageCurvePatchSession &session)
{
  if (!session.is_bend_curve_only || session.layer_material == nullptr ||
      session.initial_uv_curves.size() != session.doc.patches.size())
  {
    return;
  }
  MaterialPaintLayer *row = curve_patch_layer_session_row_2d(session);
  if (row == nullptr || row->curve_patch == nullptr || row->curve_patch->vectors == nullptr) {
    return;
  }
  PaintVector &pv = *row->curve_patch->vectors;
  curve_patch_document_write_items(
      session.doc,
      pv,
      [&](const int patch_i) -> const bke::CurvesGeometry * {
        return session.initial_uv_curves.index_range().contains(patch_i) ?
                   &session.initial_uv_curves[patch_i] :
                   nullptr;
      },
      true);
  BKE_paint_vector_tag_changed(pv);
  Material &ma = *session.layer_material;
  /* As in the live path, the refresh waits for the scene update; a modal event never rasterizes. */
  BKE_paint_layers_tag_edited(ma);
  WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &ma.id);
}

}  // namespace blender::ed::sculpt_paint
