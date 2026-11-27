/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Implementation of the Stack Layers <-> Curve Patch bridge; see #paint_curve_patch_layer.hh.
 */

#include "paint_curve_patch_layer.hh"
#include "paint_curve_patch_layer_intern.hh"
#include "paint_curve_patch_layer_object_source_intern.hh"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "MEM_guardedalloc.h"

#include "BLT_translation.hh"

#include "BLI_assert.h"
#include "BLI_function_ref.hh"
#include "BLI_index_mask.hh"
#include "BLI_index_range.hh"
#include "BLI_hash.h"
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
#include "DNA_node_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_paint_vector_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_userdef_types.h"
#include "DNA_view3d_types.h"
#include "DNA_windowmanager_types.h"

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
#include "BKE_paint_layers_asset.hh"
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
#include "paint_curve_screen.hh"
#include "paint_curve_patch_effect.hh"
#include "paint_curve_patch_session.hh"
#include "paint_curve_patch_space_convert.hh"

namespace blender::ed::sculpt_paint {

/**
 * Run \a fn with the context's window area and region pointed at \a area / \a region, then restore
 * them. A modal operator's region stays the one it registered with, so a tool invoked from the
 * Outliner or a panel must be launched in the editor that owns its hit testing. A null area (the
 * editor is not on screen) runs \a fn in the current context.
 */
static void curve_patch_layer_with_area_context(bContext &C,
                                                ScrArea *area,
                                                ARegion *region,
                                                const FunctionRef<void()> &fn)
{
  if (area == nullptr || region == nullptr) {
    fn();
    return;
  }
  ScrArea *area_prev = CTX_wm_area(&C);
  ARegion *region_prev = CTX_wm_region(&C);
  const bool already_there = (area_prev == area && region_prev == region);
  if (!already_there) {
    CTX_wm_area_set(&C, area);
    CTX_wm_region_set(&C, region);
  }
  fn();
  if (!already_there) {
    CTX_wm_area_set(&C, area_prev);
    CTX_wm_region_set(&C, region_prev);
  }
}

/* -------------------------------------------------------------------- */
/** \name Brush restore
 * \{ */

PrevBrushRestore::PrevBrushRestore(Scene &scene, Paint &paint)
    : scene_(&scene),
      paint_(&paint),
      prev_brush_(BKE_paint_brush(&paint)),
      bindings_backup_(ToolBrushBindingsBackup::capture(paint))
{
  /* With Material canvas brush sync on, the entry's #WM_toolsystem_activate_brush_and_tool pushes
   * the row's snapshot into the paired Paint and re-points its main binding. Capture it before that
   * happens, so the session cannot leave its private brush in the other editor. */
  this->paired_paint_ = BKE_paint_material_sync_target_get(this->scene_, this->paint_);
  if (this->paired_paint_ != nullptr) {
    this->prev_paired_brush_ = BKE_paint_brush(this->paired_paint_);
    this->paired_bindings_backup_ = ToolBrushBindingsBackup::capture(*this->paired_paint_);
  }
}

PrevBrushRestore::~PrevBrushRestore()
{
  if (this->released_ || this->scene_ == nullptr || this->paint_ == nullptr) {
    return;
  }
  /* The same user-facing switch the entry used, so the outgoing PBR state (here: the snapshot's)
   * is snapshotted before the previous brush comes back with its own. */
  BKE_paint_brush_set_synced(*this->scene_, *this->paint_, this->prev_brush_);
  /* The entry went through the tool system, which recorded the snapshot in the brush binding of
   * the snapshot's type -- a per-type one for the tools that are limited to a brush type (Sculpt
   * Mode's Paint tool is), the main one otherwise. A pointer-only restore would leave that binding
   * behind, and the next tool refresh (an undo step, a tool round-trip, a mode re-entry) would
   * silently put the snapshot back in the hand after the restore had already run -- the "the wrong
   * brush stays active" reports. Restoring the whole bindings snapshot also keeps one effect's
   * snapshot out of the next effect's restored hand when the user switches between them. */
  this->bindings_backup_->restore(*this->paint_);
  /* The restored brush IS the active one now, so the main binding follows the hand again even
   * when the paint had no main binding before the session. Only the main binding is written here:
   * the per-type ones came back from the snapshot above. */
  WM_toolsystem_brush_binding_main_update_from_active(this->paint_);

  /* Material canvas brush sync also moved the paired Paint (Sculpt <-> Image) to the snapshot. Put
   * its brush and bindings back the same way, or its next tool refresh would restore the session's
   * brush in the other editor. This is a no-op when sync did not run (mask target, non-material
   * brush, sync off): the paired brush then equals the captured one. */
  if (this->paired_paint_ != nullptr) {
    BKE_paint_brush_set_synced(*this->scene_, *this->paired_paint_, this->prev_paired_brush_);
    this->paired_bindings_backup_->restore(*this->paired_paint_);
    WM_toolsystem_brush_binding_main_update_from_active(this->paired_paint_);
  }
}

bool PrevBrushRestore::activate(Brush &brush)
{
  if (this->scene_ == nullptr || this->paint_ == nullptr) {
    return false;
  }
  return BKE_paint_brush_set_synced(*this->scene_, *this->paint_, &brush);
}

void PrevBrushRestore::release()
{
  this->released_ = true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name PaintVector <-> CurvePatchDocument converter
 * \{ */

/** Field-wise reset to an empty document, shared by both publishers: the document holds an
 * `Array<MTex>` texture binding, which is neither copy- nor move-assignable (DNA types delete
 * both). */
static void curve_patch_document_reset(CurvePatchDocument &r_doc)
{
  r_doc.patches.clear();
  r_doc.active_patch = 0;
  r_doc.active_point = -1;
  r_doc.texture_pick_point = -1;
  r_doc.pending_point_texture_restamp = false;
  curve_patch_texture_binding_clear(r_doc.texture);
  r_doc.last_synced = CurvePatchLiveInputs{};
  r_doc.last_synced_params = bke::CurvePatchParams{};
  r_doc.undo_steps.clear();
  r_doc.undo_step_current = -1;
  r_doc.undo_anchor_trimmed = false;
}

void curve_patch_document_from_vector(const PaintVector &pv, CurvePatchDocument &r_doc)
{
  curve_patch_document_reset(r_doc);

  int patch_index = 0;
  int active_patch = 0;
  for (const int item_i : IndexRange(pv.items_num)) {
    const PaintVectorItem &pvi = pv.items[item_i];
    if (pvi.type != PAINT_VECTOR_ITEM_CURVE_PATCH) {
      /* A Shape item is not a curve: skipped, its index never renumbered. */
      continue;
    }
    if (!BKE_paint_vector_item_curve_geometry_is_valid(pvi)) {
      /* Never read a geometry that has no runtime: a corrupt item must not crash the tool (and is
       * repaired on the next save, see `paint_vector_blend_write`). */
      continue;
    }
    r_doc.patches.append_as();
    CurvePatchItem &item = r_doc.patches.last();
    /* The item index is what keeps the patch and its persisted row in step for the active-curve
     * sync and for the write-back below. */
    item.source_curve_index = item_i;
    item.name = pvi.curve_name;
    item.curve_id = pvi.curve_id;
    item.control_curve = pvi.curve_patch_geometry.wrap();
    item.params = BKE_paint_vector_curve_params_decode(pvi.curve_patch_params);
    if (item_i == pv.active_item) {
      active_patch = patch_index;
    }
    patch_index++;
  }
  /* The active item counts only curve items: a Shape item can be active without being a patch.
   * #PaintVector::items is a DNA array, not a #Vector, so bounds-check by hand. */
  r_doc.active_patch =
      (pv.active_item >= 0 && pv.active_item < pv.items_num) ? active_patch : 0;
  r_doc.active_point = -1;
}

/* The brush radius helpers the document converters use; defined in the Add path section below. */
static void curve_patch_layer_radius_attribute_ensure(bke::CurvesGeometry &geom);
static void curve_patch_layer_brush_radius(const Brush *brush,
                                           float &r_radius,
                                           float &r_radius_per_size);
static uint32_t curve_patch_layer_brush_stamp_seed(const Brush &brush);

/** The write-back both spaces share: the document's patches go into the container's items, and
 * the active patch maps back to its item. Only the geometry differs (3D stores the control curve,
 * 2D the canonical UV curve), so \a geometry_of names it per patch; null keeps the item's stored
 * geometry. */
void curve_patch_document_write_items(
    const CurvePatchDocument &doc,
    PaintVector &r_pv,
    const FunctionRef<const bke::CurvesGeometry *(int)> geometry_of,
    const bool ensure_radius)
{
  const auto store_geometry = [&](PaintVectorItem &pvi, const int patch_i) {
    if (const bke::CurvesGeometry *geometry = geometry_of(patch_i)) {
      pvi.curve_patch_geometry.wrap() = *geometry;
      if (ensure_radius) {
        curve_patch_layer_radius_attribute_ensure(pvi.curve_patch_geometry.wrap());
      }
    }
  };

  /* First pass: refresh the items that already exist, through each patch's remembered item index.
   * A stale index (the item was deleted under a live session) falls through to the append pass. */
  Vector<int> written(doc.patches.size(), -1);
  for (const int patch_i : doc.patches.index_range()) {
    const CurvePatchItem &item = doc.patches[patch_i];
    const int item_i = item.source_curve_index;
    if (item_i < 0 || item_i >= r_pv.items_num ||
        r_pv.items[item_i].type != PAINT_VECTOR_ITEM_CURVE_PATCH)
    {
      continue;
    }
    PaintVectorItem &pvi = r_pv.items[item_i];
    store_geometry(pvi, patch_i);
    pvi.curve_patch_params = BKE_paint_vector_curve_params_encode(item.params);
    if (!item.name.empty()) {
      BLI_strncpy(pvi.curve_name, item.name.c_str(), sizeof(pvi.curve_name));
    }
    written[patch_i] = item_i;
  }

  /* Second pass: patches created inside the session append as new items, after every existing one
   * (Shape items included, in their original order). */
  for (const int patch_i : doc.patches.index_range()) {
    if (written[patch_i] >= 0) {
      continue;
    }
    const CurvePatchItem &item = doc.patches[patch_i];
    PaintVectorItem &pvi = BKE_paint_vector_item_add(r_pv);
    pvi.type = PAINT_VECTOR_ITEM_CURVE_PATCH;
    store_geometry(pvi, patch_i);
    pvi.curve_patch_params = BKE_paint_vector_curve_params_encode(item.params);
    if (item.name.empty()) {
      SNPRINTF(pvi.curve_name, "Curve %d", r_pv.items_num);
    }
    else {
      BLI_strncpy(pvi.curve_name, item.name.c_str(), sizeof(pvi.curve_name));
    }
    written[patch_i] = r_pv.items_num - 1;
  }

  /* The active patch maps back to its item; a document without patches leaves the stored index. */
  if (doc.has_active_item()) {
    const int item_i = written[doc.active_patch];
    r_pv.active_item = (item_i >= 0) ? item_i : r_pv.active_item;
  }
}

void curve_patch_document_to_vector(const CurvePatchDocument &doc, PaintVector &r_pv)
{
  curve_patch_document_write_items(
      doc,
      r_pv,
      [&](const int patch_i) -> const bke::CurvesGeometry * {
        /* A curve that names an external object keeps its own geometry as the fallback: the
         * object is the source of truth, and its shape is written there (see
         * #curve_patch_document_to_objects), not into the item. */
        const int item_i = doc.patches[patch_i].source_curve_index;
        if (item_i >= 0 && item_i < r_pv.items_num &&
            r_pv.items[item_i].type == PAINT_VECTOR_ITEM_CURVE_PATCH &&
            r_pv.items[item_i].curve_object != nullptr)
        {
          return nullptr;
        }
        return &doc.patches[patch_i].control_curve;
      },
      false);
}

/** Write a committed document back into the row's container, and the curves that name an external
 * object into their objects. A \a for_save write carries the container's curves but touches no
 * staleness mark and no object (a save writes the objects' own data already, and a file write
 * changes nothing). */
static void curve_patch_write_back_document(CurvePatchSession &session,
                                            MaterialPaintLayer &row,
                                            const bool for_save)
{
  if (row.curve_patch->vectors == nullptr) {
    return;
  }
  /* A look edit in the list is only picked up by the modal's poll; the last one may not have
   * been. */
  curve_patch_layer_active_item_follow(session);
  PaintVector &pv = *row.curve_patch->vectors;
  curve_patch_document_to_vector(session.doc, pv);
  if (for_save) {
    return;
  }
  curve_patch_document_to_objects(session, row);
  /* The container changed: the row's content follows it. The save half tags nothing (a file write
   * changes nothing, and a loaded file re-bakes anyway). */
  BKE_paint_vector_tag_changed(pv);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add path (seed + wrapper)
 * \{ */

/** Whether the container already holds a curve item a session can build a patch from. Broken or
 * too-short items do not count: they are skipped by the converter, so a row carrying only those
 * still needs its seed (otherwise the entry would refuse with "no usable control curve"). */
static bool curve_patch_layer_row_has_curves(const PaintVector &pv)
{
  for (const int i : IndexRange(pv.items_num)) {
    const PaintVectorItem &item = pv.items[i];
    if (item.type == PAINT_VECTOR_ITEM_CURVE_PATCH &&
        BKE_paint_vector_item_curve_geometry_is_valid(item) &&
        item.curve_patch_geometry.wrap().points_num() >= 2)
    {
      return true;
    }
  }
  return false;
}

/**
 * Give \a geom the per-point `radius` attribute a patch expects, as the multiplier 1 of the frozen
 * #bke::CurvePatchParams::radius that an anchor stroke writes (`anchor_radii` in
 * `paint_curve_patch_session.cc`). Without the attribute a curve's points read the generic curve
 * default of 0.01, which made the whole patch a hundredth of the brush radius: stamped, but too small
 * to see. A curve that already carries the attribute is the user's and is left alone.
 */
static void curve_patch_layer_radius_attribute_ensure(bke::CurvesGeometry &geom)
{
  if (geom.points_num() > 0 && !geom.attributes().contains("radius")) {
    geom.radius_for_write().fill(1.0f);
  }
}

/** The id the next curve item of \a pv gets: one above the largest in use. */
static int curve_patch_layer_next_curve_id(const PaintVector &pv)
{
  int next = 1;
  for (const int i : IndexRange(pv.items_num)) {
    next = std::max(next, pv.items[i].curve_id + 1);
  }
  return next;
}

/** Give every curve item that has no id yet (a file from before the id existed) one. */
static void curve_patch_layer_curve_ids_ensure(PaintVector &pv)
{
  for (const int i : IndexRange(pv.items_num)) {
    if (pv.items[i].type == PAINT_VECTOR_ITEM_CURVE_PATCH && pv.items[i].curve_id <= 0) {
      pv.items[i].curve_id = curve_patch_layer_next_curve_id(pv);
    }
  }
}

/** Object-space radius of a curve made from a brush that has no usable Unprojected Size. */
static constexpr float curve_patch_layer_default_radius = 0.1f;

/** The radius (half the brush's Unprojected Size, the DIAMETER) and its ratio to the snapshot's own
 * Size slider that a curve made from \a brush freezes. */
static void curve_patch_layer_brush_radius(const Brush *brush,
                                           float &r_radius,
                                           float &r_radius_per_size)
{
  const float diameter = (brush != nullptr) ? brush->unprojected_size : 0.0f;
  r_radius = diameter > 0.0f ? diameter * 0.5f : curve_patch_layer_default_radius;
  const int brush_size = (brush != nullptr && brush->size > 0) ? brush->size : 1;
  r_radius_per_size = r_radius / float(brush_size);
}

/** The same pair for a curve of the given Space: object-space units in 3D, image pixels in 2D (the
 * unit a 2D curve's frozen parameters carry, scaled by the rasterizer through the tile size). */
void curve_patch_layer_brush_radius_for_space(const Brush *brush,
                                              const bool is_2d,
                                              float &r_radius,
                                              float &r_radius_per_size)
{
  if (!is_2d) {
    curve_patch_layer_brush_radius(brush, r_radius, r_radius_per_size);
    return;
  }
  const int brush_size = (brush != nullptr && brush->size > 0) ? brush->size : 1;
  r_radius = float(brush_size) * 0.5f;
  r_radius_per_size = r_radius / float(brush_size);
}

/** The stamp seed of a curve made (or renewed) from \a brush. One function for both, so a curve
 * keeps the same texture distribution whichever path built it; it follows the snapshot's name,
 * which stays unique per snapshot (`.CP <source>`, `.CP <source>.001`, ...). */
static uint32_t curve_patch_layer_brush_stamp_seed(const Brush &brush)
{
  return BLI_hash_string(brush.id.name + 2);
}

/** Rebuild the frozen stamp parameters of \a item from \a brush as it is NOW with the given radius
 * and plane normal. Only the stamp seed is kept, so the texture distribution of the curve stays what
 * it was; the three placement paths (3D seed, 2D seed, Space conversion) share this. */
void curve_patch_layer_item_params_refresh(PaintVectorItem &item,
                                           const Brush &brush,
                                           const float radius,
                                           const float radius_per_size,
                                           const float3 &plane_normal)
{
  const bke::CurvePatchParams previous = BKE_paint_vector_curve_params_decode(
      item.curve_patch_params);
  const bke::CurvePatchParams params = curve_patch_params_from_brush(
      brush,
      radius,
      radius_per_size,
      plane_normal,
      previous.stamp_seed,
      brush.curve_patch.swap_axis != 0);
  item.curve_patch_params = BKE_paint_vector_curve_params_encode(params);
}

/** Append one default curve item to \a pv: a two-point segment sized from \a brush, with the
 * frozen parameters the brush would stamp with. Used by the row seed and the curve-add operator. */
PaintVectorItem &curve_patch_layer_curve_item_default(PaintVector &pv, const Brush *brush)
{
  /* The brush's Unprojected Size is the DIAMETER in Blender units; the patch works with the
   * radius (the same value a stroke freezes as #StrokeCache::initial_radius). */
  /* The snapshot's own size slider, not the unified setting: at Add time there is no Paint the
   * unified state could be read from, and the frozen ratio belongs to the snapshot anyway. */
  float radius;
  float radius_per_size;
  curve_patch_layer_brush_radius(brush, radius, radius_per_size);

  const int curve_id = curve_patch_layer_next_curve_id(pv);
  PaintVectorItem &item = BKE_paint_vector_item_add(pv);
  item.type = PAINT_VECTOR_ITEM_CURVE_PATCH;
  item.curve_id = curve_id;
  item.flag |= PAINT_VECTOR_ITEM_CURVE_SEED;
  SNPRINTF(item.curve_name, "Curve %d", pv.items_num);

  /* From the brush, like every other patch: the stamp mode, layout, projection, spacing, caps and
   * end falloff the brush's Curve Patch settings choose. Setting only the radius left them all at
   * the struct's defaults (spacing 0 among them), so the sampler built no stamps and answered a
   * weight of zero for every texel -- a "painted" texel with no coverage. */
  bke::CurvePatchParams params;
  if (brush != nullptr) {
    params = curve_patch_params_from_brush(*brush,
                                           radius,
                                           radius_per_size,
                                           float3(0.0f, 0.0f, 1.0f),
                                           curve_patch_layer_brush_stamp_seed(*brush),
                                           brush->curve_patch.swap_axis != 0);
  }
  else {
    params.radius = radius;
    params.radius_per_size = radius_per_size;
    params.plane_normal = float3(0.0f, 0.0f, 1.0f);
  }
  item.curve_patch_params = BKE_paint_vector_curve_params_encode(params);

  /* One straight segment centered on the object origin, one brush diameter long: the passive
   * overlay shows it right away, and the first edit moves it onto the surface. The auto handles
   * follow the endpoints. */
  bke::CurvesGeometry &geom = item.curve_patch_geometry.wrap();
  paintcurve_geometry_init_bezier(geom, 2);
  curve_patch_layer_radius_attribute_ensure(geom);
  if (geom.points_num() == 2) {
    const MutableSpan<float3> positions = geom.positions_for_write();
    positions[0] = float3(-radius, 0.0f, 0.0f);
    positions[1] = float3(radius, 0.0f, 0.0f);
    geom.tag_positions_changed();
  }
  return item;
}

/** Re-derive every curve's frozen stamp parameters from the effect's (new) brush snapshot, the way
 * a curve made from it would get them. What the user shaped on the curve itself stays: its points,
 * Width (radius and its ratio to the brush size), Opacity, visibility, mirror axes and axis swap. */
static void curve_patch_layer_curves_reset_from_brush(MaterialPaintLayerCurvePatch &payload)
{
  if (payload.vectors == nullptr || payload.brush == nullptr) {
    return;
  }
  PaintVector &pv = *payload.vectors;
  float default_radius;
  float default_radius_per_size;
  curve_patch_layer_brush_radius(payload.brush, default_radius, default_radius_per_size);
  for (const int i : IndexRange(pv.items_num)) {
    PaintVectorItem &item = pv.items[i];
    if (item.type != PAINT_VECTOR_ITEM_CURVE_PATCH) {
      continue;
    }
    const bke::CurvePatchParams old_params = BKE_paint_vector_curve_params_decode(
        item.curve_patch_params);
    /* A curve with no usable Width (data from before the ratio existed) takes the brush's. */
    const float radius = old_params.radius > 0.0f ? old_params.radius : default_radius;
    const float radius_per_size = old_params.radius_per_size > 0.0f ? old_params.radius_per_size :
                                                                      default_radius_per_size;
    bke::CurvePatchParams params = curve_patch_params_from_brush(
        *payload.brush,
        radius,
        radius_per_size,
        old_params.plane_normal,
        curve_patch_layer_brush_stamp_seed(*payload.brush),
        old_params.swap_axis);
    params.opacity = old_params.opacity;
    params.hidden = old_params.hidden;
    params.symmetry_flags = old_params.symmetry_flags;
    item.curve_patch_params = BKE_paint_vector_curve_params_encode(params);
  }
  BKE_paint_vector_tag_changed(pv);
}

void ED_paint_layers_curve_patch_row_seed_curve(MaterialPaintLayerCurvePatch &payload)
{
  if (payload.vectors == nullptr) {
    return;
  }
  PaintVector &pv = *payload.vectors;
  if (curve_patch_layer_row_has_curves(pv)) {
    return;
  }
  curve_patch_layer_curve_item_default(pv, payload.brush);
}

bool ED_paint_layers_curve_patch_row_maps_ensure(bContext &C,
                                                Material &ma,
                                                MaterialPaintLayer &row)
{
  if (row.source != MA_PAINT_LAYER_SOURCE_CURVE_PATCH || row.curve_patch == nullptr) {
    return false;
  }
  const Scene *scene = CTX_data_scene(&C);
  if (scene == nullptr || scene->toolsettings == nullptr) {
    return false;
  }
  Main &bmain = *CTX_data_main(&C);
  const PaintModeSettings &paint_mode = scene->toolsettings->paint_mode;
  const int image_size = paint_mode.new_channel_image_size > 0 ?
                             paint_mode.new_channel_image_size :
                             PAINT_NEW_CHANNEL_IMAGE_SIZE_1K;

  /* The interactive Image effect resolves its targets from the enabled channel maps, and the
   * bake renders every enabled record in the material's set. Creating the maps for that same set
   * is what keeps the session's preview and the committed bake showing the same channels; the
   * bake would create whatever is missing right after the commit anyway. */
  const uint32_t channel_set = BKE_paint_layers_channel_set_mask_get(ma);
  int created_num = 0;
  auto ensure_channel = [&](const int channel) -> bool {
    if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM ||
        (channel_set & (uint32_t(1) << channel)) == 0)
    {
      return false;
    }
    PaintLayersTarget target;
    target.material = &ma;
    target.layer = &row;
    target.channel = channel;
    target.mode = PaintLayersTargetMode::Content;
    /* Read from the row's own records: the target resolver falls back to a shared image before the
     * record exists, which made every entry count as a creation and regenerate the graph. */
    const bool existed = std::any_of(row.channels,
                                     row.channels + row.channels_num,
                                     [&](const MaterialPaintLayerChannel &record) {
                                       return int(record.channel) == channel &&
                                              record.image != nullptr;
                                     });
    const bool ready = BKE_paint_layers_target_ensure_writable(bmain, target, image_size) !=
                       nullptr;
    created_num += int(ready && !existed);
    return ready;
  };

  bool any_ready = false;
  for (const MaterialPaintLayerChannel &record : Span<MaterialPaintLayerChannel>(row.channels,
                                                                                row.channels_num))
  {
    if (record.state != MA_PAINT_LAYER_CHANNEL_ENABLED) {
      continue;
    }
    any_ready |= ensure_channel(int(record.channel));
  }
  if (!any_ready) {
    /* A row from before the records existed: fall back to the maps a Paint row starts with. */
    any_ready = ensure_channel(int(PAINT_MATERIAL_CHANNEL_BASE_COLOR));
    if (!any_ready) {
      any_ready = ensure_channel(BKE_paint_layers_texpaint_slot_channel(ma.paint_active_slot));
    }
  }
  if (created_num > 0) {
    /* A new map has no image node in the generated material yet, so the stamped pixels would reach
     * an image nothing samples -- and show only after some unrelated stroke regenerated the graph.
     * Regenerate here, before the session captures the evaluated material, the way a brush stroke
     * does when it creates a map (`paint_image_ops_paint.cc`). */
    WM_file_tag_modified();
    BKE_paint_layers_regenerate_tagged(bmain);
    CTX_data_ensure_evaluated_depsgraph(&C);
  }
  if (Object *ob = CTX_data_active_object(&C)) {
    /* The paint slots mirror the active row's maps, which just came into being or moved. */
    BKE_texpaint_slots_refresh_object(CTX_data_scene(&C), ob);
  }
  return any_ready;
}

bool curve_patch_view3d_find(wmWindowManager *wm, ViewContext &r_vc)
{
  if (wm == nullptr) {
    return false;
  }
  for (wmWindow &win : wm->windows) {
    const bScreen *screen = WM_window_get_active_screen(&win);
    if (screen == nullptr) {
      continue;
    }
    for (ScrArea &area : screen->areabase) {
      if (area.spacetype != SPACE_VIEW3D) {
        continue;
      }
      ARegion *region = BKE_area_find_region_type(&area, RGN_TYPE_WINDOW);
      if (region == nullptr || region->regiondata == nullptr) {
        continue;
      }
      r_vc.win = &win;
      r_vc.region = region;
      r_vc.rv3d = static_cast<RegionView3D *>(region->regiondata);
      r_vc.v3d = static_cast<View3D *>(area.spacedata.first);
      return true;
    }
  }
  return false;
}

/**
 * A ViewContext on a 3D Viewport of the current screen. The entry can be triggered from the
 * Outliner or the Properties panel, where the context has no RegionView3D; the re-stamp and the
 * symmetry passes still need one, so the first View3D window is used. Nullopt when the screen has
 * no 3D Viewport at all.
 */
static std::optional<ViewContext> curve_patch_layer_view3d_context(bContext &C, Object &ob)
{
  bScreen *screen = CTX_wm_screen(&C);
  if (screen == nullptr) {
    return std::nullopt;
  }
  /* The panel's own area when the button lives in a View3D sidebar; otherwise the largest 3D
   * Viewport of the screen (the Outliner/Properties entry). */
  ScrArea *area = CTX_wm_area(&C);
  if (area == nullptr || area->spacetype != SPACE_VIEW3D) {
    area = BKE_screen_find_big_area(screen, SPACE_VIEW3D, 0);
  }
  if (area == nullptr) {
    return std::nullopt;
  }
  View3D *v3d = reinterpret_cast<View3D *>(area->spacedata.first);
  ARegion *region = BKE_area_find_region_type(area, RGN_TYPE_WINDOW);
  if (v3d == nullptr || region == nullptr || region->regiondata == nullptr) {
    return std::nullopt;
  }
  ViewContext vc = {};
  vc.C = &C;
  vc.region = region;
  vc.bmain = CTX_data_main(&C);
  vc.depsgraph = CTX_data_ensure_evaluated_depsgraph(&C);
  vc.scene = CTX_data_scene(&C);
  vc.view_layer = CTX_data_view_layer(&C);
  vc.v3d = v3d;
  vc.win = CTX_wm_window(&C);
  vc.rv3d = reinterpret_cast<RegionView3D *>(region->regiondata);
  vc.obact = &ob;
  if (vc.bmain == nullptr || vc.depsgraph == nullptr || vc.scene == nullptr ||
      vc.view_layer == nullptr || vc.win == nullptr)
  {
    return std::nullopt;
  }
  return vc;
}

/**
 * Lay the not yet placed (seed) curves on the surface under the view. The Add-time default is a
 * segment on the object origin, which is usually inside the mesh, so nothing would be projected
 * onto the texture until the user dragged both points out. The segment is put around the object's
 * origin as seen from the viewport (the view centre when the origin is off screen), one hit per end.
 */
static void curve_patch_layer_seeds_place(bContext &C,
                                          const Object &ob,
                                          const ViewContext &vc,
                                          const Paint &paint,
                                          const Brush &brush,
                                          PaintVector &pv)
{
  constexpr float half_length_px = 60.0f;
  for (const int item_i : IndexRange(pv.items_num)) {
    PaintVectorItem &item = pv.items[item_i];
    if (item.type != PAINT_VECTOR_ITEM_CURVE_PATCH) {
      continue;
    }
    /* Every curve of the container, not only a seed: a curve made before this attribute was
     * written reads a hundredth of its radius. */
    if (BKE_paint_vector_item_curve_geometry_is_valid(item)) {
      curve_patch_layer_radius_attribute_ensure(item.curve_patch_geometry.wrap());
    }
    if ((item.flag & PAINT_VECTOR_ITEM_CURVE_SEED) == 0) {
      continue;
    }
    if (!BKE_paint_vector_item_curve_geometry_is_valid(item)) {
      continue;
    }
    bke::CurvesGeometry &geom = item.curve_patch_geometry.wrap();
    if (geom.points_num() != 2) {
      continue;
    }
    const float2 region_center(float(vc.region->winx) * 0.5f, float(vc.region->winy) * 0.5f);
    float2 origin_px;
    const bool origin_visible = ED_view3d_project_float_global(vc.region,
                                                               ob.object_to_world().location(),
                                                               origin_px,
                                                               V3D_PROJ_TEST_NOP) ==
                                V3D_PROJ_RET_OK;
    /* The origin first, then the view centre: the latter always lies inside the region. */
    const float2 candidates[2] = {origin_visible ? origin_px : region_center, region_center};

    float3 hit[2];
    float3 normal[2];
    bool placed = false;
    for (const float2 &center : candidates) {
      placed = true;
      for (const int end : IndexRange(2)) {
        const float mval[2] = {center.x + (end == 0 ? -half_length_px : half_length_px), center.y};
        float co[3];
        float no[3];
        /* False means only the object-origin plane answered, not the surface. */
        if (!paintcurve_surface_place(&C, nullptr, vc, mval, nullptr, true, co, no)) {
          placed = false;
          break;
        }
        hit[end] = float3(co);
        normal[end] = float3(no);
      }
      if (placed) {
        break;
      }
    }
    if (!placed) {
      continue;
    }
    /* A fresh two-point curve has no handle attributes yet: they come into being with the first
     * handle recalculation. The handles of such a curve sit on their point, so the point stands in
     * for them, and the snap below rebuilds them from the new position. */
    const std::optional<Span<float3>> handles_left = geom.handle_positions_left();
    const std::optional<Span<float3>> handles_right = geom.handle_positions_right();
    const Span<float3> points = geom.positions();
    /* Copied before the first snap moves anything: each end is translated from these, in the order
     * #paintcurve_apply_point_surface_snap reads them -- left handle, point, right handle. */
    const float3 initial[2][3] = {
        {handles_left ? (*handles_left)[0] : points[0],
         points[0],
         handles_right ? (*handles_right)[0] : points[0]},
        {handles_left ? (*handles_left)[1] : points[1],
         points[1],
         handles_right ? (*handles_right)[1] : points[1]},
    };
    for (const int end : IndexRange(2)) {
      /* The same snap a dragged point goes through: it also recomputes the auto handles. */
      paintcurve_apply_point_surface_snap(geom, end, initial[end], hit[end], normal[end]);
    }
    geom.tag_positions_changed();

    /* The radius a brush stroke of the same Size would measure here: its pixel radius projected at
     * the curve, which is what the Add-time default (a world size read off the snapshot, stale on a
     * copy of a brush) could not know -- it came out near zero. The ratio to the Size slider keeps
     * the live-size sync working. */
    const float3 center = (hit[0] + hit[1]) * 0.5f;
    const float radius = object_space_radius_get(vc, paint, brush, center, 1.0f);
    const int brush_size = BKE_brush_size_get(&paint, &brush);
    if (radius > 0.0f) {
      /* Rebuilt from the brush as it is NOW, not as it was at Add: the user may have changed the
       * Curve Patch settings of the snapshot (or assigned another brush) in between. Only the
       * stamp seed is kept, so the texture distribution of a curve stays what it was. */
      curve_patch_layer_item_params_refresh(item,
                                            brush,
                                            radius,
                                            brush_size > 0 ? radius / float(brush_size) : 0.0f,
                                            math::normalize(normal[0] + normal[1]));
    }
    item.flag &= ~PAINT_VECTOR_ITEM_CURVE_SEED;
  }
}

/**
 * The 2D Space counterpart of #curve_patch_layer_seeds_place: lay the not yet placed (seed)
 * curves in the Image Editor's visible canvas, as a UV segment across the view center.
 *
 * Everything is UV, and the radius is in IMAGE PIXELS -- the units a 2D curve's frozen parameters
 * carry (the rasterizer scales them through the reference tile's resolution), so the segment
 * keeps the size the interactive 2D path would measure from the same brush.
 */
static void curve_patch_layer_seeds_place_2d(PaintVector &pv,
                                             const Paint &paint,
                                             const Brush &brush,
                                             SpaceImage *sima,
                                             const ARegion *region)
{
  if (sima == nullptr || region == nullptr) {
    return;
  }
  const int center[2] = {region->winx / 2, region->winy / 2};
  /* A fifth of the visible width to each side: a segment the user can grab either end of at any
   * zoom level, the same spirit the 3D placement's 60-pixel half length has. */
  const int half_span = region->winx / 5;
  const int end_a[2] = {center[0] - half_span, center[1]};
  const int end_b[2] = {center[0] + half_span, center[1]};
  float2 uv_a(0.0f, 0.0f);
  float2 uv_b(0.0f, 0.0f);
  ED_image_mouse_pos(sima, region, end_a, uv_a);
  ED_image_mouse_pos(sima, region, end_b, uv_b);

  const float pixel_radius = BKE_brush_radius_get(&paint, &brush);
  const int brush_size = BKE_brush_size_get(&paint, &brush);
  for (const int item_i : IndexRange(pv.items_num)) {
    PaintVectorItem &item = pv.items[item_i];
    if (item.type != PAINT_VECTOR_ITEM_CURVE_PATCH) {
      continue;
    }
    /* Every curve of the container, not only a seed: a curve made before this attribute was
     * written reads a hundredth of its radius (the same repair the 3D placement does). */
    if (BKE_paint_vector_item_curve_geometry_is_valid(item)) {
      curve_patch_layer_radius_attribute_ensure(item.curve_patch_geometry.wrap());
    }
    if ((item.flag & PAINT_VECTOR_ITEM_CURVE_SEED) == 0 ||
        !BKE_paint_vector_item_curve_geometry_is_valid(item))
    {
      continue;
    }
    bke::CurvesGeometry &geom = item.curve_patch_geometry.wrap();
    if (geom.points_num() != 2) {
      continue;
    }
    MutableSpan<float3> positions = geom.positions_for_write();
    positions[0] = float3(uv_a.x, uv_a.y, 0.0f);
    positions[1] = float3(uv_b.x, uv_b.y, 0.0f);
    /* A fresh two-point curve has no handle attributes yet; AUTO handles stand in for them until
     * the first edit, exactly like the 3D seed. */
    geom.handle_positions_left_for_write();
    geom.handle_positions_right_for_write();
    geom.calculate_bezier_auto_handles();
    geom.tag_positions_changed();

    if (pixel_radius > 0.0f) {
      /* The 2D stamp projects in the +Z plane of the UV canvas by definition. */
      curve_patch_layer_item_params_refresh(
          item,
          brush,
          pixel_radius,
          brush_size > 0 ? pixel_radius / float(brush_size) : 0.0f,
          float3(0.0f, 0.0f, 1.0f));
    }
    item.flag &= ~PAINT_VECTOR_ITEM_CURVE_SEED;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Session entry / commit
 * \{ */

/** Remember the stack row a session was entered from, for the commit. The row is stored by marker:
 * the Outliner may delete it while the modal runs. */
static void curve_patch_layer_attach_row(CurvePatchSession &session,
                                         Material &ma,
                                         MaterialPaintLayer &row)
{
  session.layer_material = &ma;
  session.layer_row_marker = row.marker;
}

bool curve_patch_layer_material_alive(const Material *ma)
{
  const Main *bmain = G_MAIN;
  if (ma == nullptr || bmain == nullptr) {
    return false;
  }
  for (const Material &candidate : bmain->materials) {
    if (&candidate == ma) {
      return true;
    }
  }
  return false;
}

/** The row a live layer session edits, or null when it was deleted (or is not a Curve Patch row). */
static MaterialPaintLayer *curve_patch_layer_session_row(const CurvePatchSession &session)
{
  if (!curve_patch_layer_material_alive(session.layer_material) ||
      BLI_uuid_is_nil(session.layer_row_marker))
  {
    return nullptr;
  }
  MaterialPaintLayer *row = BKE_paint_layers_find(*session.layer_material,
                                                  session.layer_row_marker);
  if (row == nullptr || row->source != MA_PAINT_LAYER_SOURCE_CURVE_PATCH ||
      row->curve_patch == nullptr)
  {
    return nullptr;
  }
  return row;
}

/* -------------------------------------------------------------------- */
/** \name Entry steps shared by the 3D and 2D activation
 *
 * The two entries differ in the editor, the paint and the session they publish; everything below is
 * the same in both, so a fix to one cannot silently miss the other. The ORDER of the calls inside
 * each entry is load-bearing (gate before the mode change, map reset before the session begins,
 * bake hash before the first restamp) and stays with the entries.
 * \{ */

/** The row's brush snapshot when it can run the tool, else null with the reason reported. */
static Brush *curve_patch_layer_entry_brush_get(ReportList *reports,
                                                const MaterialPaintLayer &row)
{
  Brush *brush = row.curve_patch->brush;
  if (brush == nullptr) {
    BKE_report(reports,
               RPT_WARNING,
               "This Curve Patch effect has no brush; assign one from the Curve Patch panel");
    return nullptr;
  }
  if (!bke::brush::supports_curve_patch(*brush)) {
    BKE_report(reports,
               RPT_WARNING,
               "This brush type cannot stamp a Curve Patch; assign another brush to the effect");
    return nullptr;
  }
  if (ID_IS_LINKED(&brush->id)) {
    BKE_report(reports,
               RPT_WARNING,
               "This Curve Patch effect's brush is linked; assign a local brush first");
    return nullptr;
  }
  if (brush->material_paint == nullptr) {
    BKE_brush_material_paint_ensure(brush);
  }
  return brush;
}

/** Give the row at least one curve, with ids and a placement in the visible space (\a seeds_place),
 * so the first session has something to grab. */
static void curve_patch_layer_entry_curves_prepare(MaterialPaintLayer &row,
                                                   FunctionRef<void(PaintVector &)> seeds_place)
{
  PaintVector &pv = *row.curve_patch->vectors;
  if (!curve_patch_layer_row_has_curves(pv)) {
    ED_paint_layers_curve_patch_row_seed_curve(*row.curve_patch);
    BKE_paint_layers_bake_request(row);
  }
  curve_patch_layer_curve_ids_ensure(pv);
  seeds_place(pv);
}

/** The interactive Image effect resolves its targets from the row's channel maps and refuses a
 * canvas without one; the same maps are what the bake renders into. Create them all before the
 * session resolves its targets. */
static bool curve_patch_layer_entry_maps_prepare(bContext &C,
                                                 Material &ma,
                                                 MaterialPaintLayer &row,
                                                 ReportList *reports)
{
  if (!ED_paint_layers_curve_patch_row_maps_ensure(C, ma, row)) {
    BKE_report(reports, RPT_WARNING, "Could not create a channel map for this Curve Patch effect");
    return false;
  }
  /* The new maps and the moved active row are visible in the properties/Outliner before the
   * modal takes over the events. */
  WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &ma.id);
  WM_event_add_notifier(&C, NC_SPACE | ND_SPACE_PROPERTIES, nullptr);
  return true;
}

/** Switch \a paint to the snapshot, then jump to its tool: switching the tool restores that tool's
 * last brush, which would replace the snapshot, so the snapshot is activated again afterwards. The
 * returned restore captured the user's outgoing brush and puts it back on every exit; null (with
 * the reason reported) when the mode cannot hold the brush. */
static std::unique_ptr<PrevBrushRestore> curve_patch_layer_entry_brush_switch(
    bContext &C,
    Scene &scene,
    Paint &paint,
    Brush &brush,
    const char *mode_name,
    ReportList *reports)
{
  auto restore = std::make_unique<PrevBrushRestore>(scene, paint);
  if (!restore->activate(brush)) {
    BKE_reportf(reports, RPT_WARNING, "This brush cannot be activated in %s", mode_name);
    return nullptr;
  }
  WM_toolsystem_activate_brush_and_tool(&C, &paint, &brush);
  /* The tool jump may have restored the tool's own brush; the row's snapshot is the session's. */
  if (!restore->activate(brush)) {
    BKE_reportf(reports, RPT_WARNING, "This brush cannot be activated in %s", mode_name);
    return nullptr;
  }
  return restore;
}

/** Stamp the bake hash of the document the session started from, so a planner tick during the
 * session does not re-render the maps from the (still unedited) document over the live preview.
 * The commit invalidates it again; a cancel keeps it, because the effect's undo restores exactly
 * the pixels that hash describes. The graph is rebuilt before the modal captures the evaluated
 * material: the bake state decides which images the generated graph samples for this effect, and
 * without the rebuild the first stamps are invisible. */
static void curve_patch_layer_entry_finish(bContext &C, Material &ma, MaterialPaintLayer &row)
{
  /* A bend row is a container, never a canvas: its maps were emptied for the session and must not
   * be rendered from the curve, or the stamp trace the user is not supposed to see comes back. */
  if (BKE_paint_layers_text_decal_bend_owner_get(ma, row) == nullptr) {
    BKE_paint_layers_bake_request(row);
    BKE_paint_layers_bake_finalize(ma, row);
  }
  BKE_paint_layers_tag_edited(ma);
  BKE_paint_layers_regenerate_tagged(*CTX_data_main(&C));
  CTX_data_ensure_evaluated_depsgraph(&C);
}

/** \} */

/**
 * The 2D Space entry: open the row's tool in the Image Editor instead of the 3D Viewport.
 *
 * The curves of the container are UV coordinates (Space = 2D is the row's own choice), so the
 * editing session is the Image Editor's (#ImageCurvePatchSession), and the stamp rasterizes
 * directly into the UV space of the row's channel maps -- no mesh projection anywhere. The
 * material-slot, UV-map, brush-snapshot and canvas checks mirror `curve_patch_layer_activate`
 * one to one; what differs is the editor the tool runs in and the session it publishes.
 */
bool curve_patch_layer_activate_2d(bContext &C, Material &ma, MaterialPaintLayer &row)
{
  Scene *scene = CTX_data_scene(&C);
  ReportList *reports = CTX_wm_reports(&C);
  BLI_assert(scene != nullptr && scene->toolsettings != nullptr);
  BLI_assert(row.source == MA_PAINT_LAYER_SOURCE_CURVE_PATCH && row.curve_patch != nullptr &&
             row.curve_patch->vectors != nullptr);
  /* The asserts vanish in a release build: a row that lost its container (a failed payload
   * create, an undo of the add) must refuse here rather than dereference null below. */
  if (scene == nullptr || scene->toolsettings == nullptr ||
      row.source != MA_PAINT_LAYER_SOURCE_CURVE_PATCH || row.curve_patch == nullptr ||
      row.curve_patch->vectors == nullptr)
  {
    BKE_report(reports, RPT_WARNING, "That row is not a Curve Patch effect");
    return false;
  }

  /* The tool edits in an Image Editor: the one the click came from when there is one, otherwise
   * the largest of the screen. There is no cross-screen search here -- the 3D path requires
   * "a 3D Viewport open", and the 2D path requires "an Image Editor open" the same way. */
  ScrArea *image_area = CTX_wm_area(&C);
  if (image_area == nullptr || image_area->spacetype != SPACE_IMAGE) {
    image_area = BKE_screen_find_big_area(CTX_wm_screen(&C), SPACE_IMAGE, 0);
  }
  SpaceImage *sima = image_area != nullptr ?
                         reinterpret_cast<SpaceImage *>(image_area->spacedata.first) :
                         nullptr;
  ARegion *image_region = image_area != nullptr ?
                              BKE_area_find_region_type(image_area, RGN_TYPE_WINDOW) :
                              nullptr;
  if (sima == nullptr || image_region == nullptr) {
    BKE_report(reports, RPT_WARNING, "Open an Image Editor to edit a 2D-space Curve Patch effect");
    return false;
  }

  Object *ob = CTX_data_active_object(&C);
  if (ob == nullptr || ob->type != OB_MESH) {
    BKE_report(reports, RPT_WARNING, "Select a mesh object to edit a Curve Patch effect");
    return false;
  }
  const short material_slot = BKE_object_material_slot_find_index(ob, &ma);
  if (material_slot == 0) {
    BKE_report(reports, RPT_WARNING, "This effect's material is not assigned to the active object");
    return false;
  }
  /* A session already editing THIS row is a re-entry (the activation ran before the explicit
   * entry); one editing ANOTHER row must finish first, in either space. */
  if (ed::sculpt_paint::image_curve_patch_session_edits_row(ma, row.marker)) {
    return true;
  }
  if (image_curve_patch_session_active() ||
      (ob->runtime != nullptr && ob->runtime->sculpt_session != nullptr &&
       ob->runtime->sculpt_session->curve_patch_session != nullptr))
  {
    BKE_report(reports, RPT_WARNING, "Finish the live Curve Patch first");
    return false;
  }

  /* The session resolves its paint targets through the object's ACTIVE material slot; a stack
   * focused on another slot would otherwise paint the wrong material's row. */
  if (ob->actcol != material_slot) {
    ob->actcol = material_slot;
  }
  /* The row becomes active before the stroke gate below reads it: the gate decides on the ACTIVE
   * row's lock state, and this is the row the session is about to edit. A Text Decal's bend row is
   * the exception: it is a container, not a paint canvas, and making it active would take the Text
   * Decal panel (and its own operators) away for as long as the curve is edited. */
  const MaterialPaintLayer *bend_text_row = BKE_paint_layers_text_decal_bend_owner_get(ma, row);
  if (bend_text_row == nullptr && !BLI_uuid_equal(ma.active_layer_marker, row.marker)) {
    BKE_paint_layers_active_set(ma, row.marker);
  }

  /* One gate per session: a locked or hidden row takes no session, and the user hears why before
   * anything is switched (the same gate the interactive 2D entry runs on every stroke). A bend
   * row lays no pixels, so the paint gate has nothing to decide for it. */
  if (bend_text_row == nullptr && scene->toolsettings != nullptr &&
      material::paint_layers_stroke_gate_report(C, *ob, scene->toolsettings->paint_mode, reports))
  {
    return false;
  }

  /* The tool runs in Texture Paint mode: the editor must be in it (the 2D modal validates
   * `sima->mode == SI_MODE_PAINT` on every event and COMMITS when it is lost), and the object
   * carries the mode for the tool system's brush activation. */
  if (sima->mode != SI_MODE_PAINT) {
    sima->mode = SI_MODE_PAINT;
    WM_event_add_notifier(&C, NC_SPACE | ND_SPACE_IMAGE, nullptr);
  }
  if ((ob->mode & OB_MODE_TEXTURE_PAINT) == 0) {
    /* Leave the mode the object is in first (a Sculpt Mode 3D Viewport next to the Image Editor
     * is the usual case): entering Texture Paint on top of it left both mode bits set, a value
     * `Object.mode` has no enum item for, which broke the header and the toolset switch. */
    if (!ed::object::mode_compat_set(&C, ob, OB_MODE_TEXTURE_PAINT, reports)) {
      return false;
    }
    Main *bmain = CTX_data_main(&C);
    Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(&C);
    ED_object_texture_paint_mode_enter_ex(*bmain, *scene, *depsgraph, *ob);
  }

  Paint &paint_2d = scene->toolsettings->imapaint.paint;
  PaintVector &pv = *row.curve_patch->vectors;

  /* A Text Decal's bend curve is a container, not a paint canvas: no channel, no stamp, no map.
   * The session edits the curve alone and the commit writes it back; the decal's own coverage map
   * is only the image the user sees while drawing, never a target. */
  if (bend_text_row != nullptr) {
    /* The bend session paints nothing, but the editor's composite/canvas resolution and the
     * session validation read the material canvas source. */
    scene->toolsettings->paint_mode.canvas_source = PAINT_CANVAS_SOURCE_MATERIAL;
    const bool canvas = bend_text_row->text_decal != nullptr &&
                        bend_text_row->text_decal->space == MA_PAINT_LAYER_TEXT_SPACE_CANVAS;
    Image *ref_image = (bend_text_row->text_decal != nullptr) ?
                           bend_text_row->text_decal->coverage :
                           nullptr;
    /* A canvas bend is edited over whatever the editor already shows (the material composite), so
     * the curve's canvas UV matches the picture; only the tiled coverage answers for pixel
     * resolution. A projected bend has no such shared UV, so its own raster is shown. */
    if (ref_image == nullptr && canvas) {
      ref_image = sima->image;
    }
    if (ref_image == nullptr) {
      BKE_report(reports,
                 RPT_WARNING,
                 "The Text Decal has no raster yet: wait for its coverage map, then edit the curve");
      return false;
    }
    if (!curve_patch_layer_row_has_curves(pv)) {
      curve_patch_layer_bend_curve_item_add(pv, row.curve_patch->brush);
      BKE_paint_layers_tag_edited(ma);
    }
    /* The modal reads the active brush for its live overlay; the row's own snapshot serves when it
     * has one, the mode's current brush otherwise. Nothing is stamped, so any Paint Brush works. */
    Brush *brush = row.curve_patch->brush;
    if (brush != nullptr && !bke::brush::supports_curve_patch(*brush)) {
      brush = nullptr;
    }
    if (brush == nullptr) {
      brush = BKE_paint_brush(&paint_2d);
    }
    if (brush == nullptr) {
      BKE_report(reports,
                 RPT_WARNING,
                 "Assign a Paint Brush to edit the bend curve: the curve tool needs one for its "
                 "overlay");
      return false;
    }
    /* Show the decal's own raster for a projected bend; a canvas bend stays over the composite. */
    if (!canvas && sima->image != ref_image) {
      ED_space_image_set(CTX_data_main(&C), sima, ref_image, false);
      WM_event_add_notifier(&C, NC_SPACE | ND_SPACE_IMAGE, nullptr);
    }
    ImageCurvePatchSession *session = image_curve_patch_session_begin_layer(
        &C, reports, pv, brush, &paint_2d, ref_image, ma, row.marker);
    if (session == nullptr) {
      return false;
    }
    session->is_bend_curve_only = true;
    session->editor_uv_origin = image_curve_patch_editor_uv_origin(sima);
    session->initial_uv_curves = session->uv_curves;
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &ma.id);

    /* The modal runs from the Image Editor: a modal handler's region stays the one it registered
     * with, and every hit test is region-pixel math through that region. */
    wmOperatorStatus status = OPERATOR_CANCELLED;
    curve_patch_layer_with_area_context(C, image_area, image_region, [&]() {
      status = WM_operator_name_call(&C,
                                     "PAINT_OT_image_curve_patch_edit",
                                     wm::OpCallContext::InvokeDefault,
                                     nullptr,
                                     nullptr);
    });
    if ((status & OPERATOR_RUNNING_MODAL) == 0) {
      BKE_report(reports, RPT_WARNING, "The Curve Patch editor could not start");
      image_curve_patch_session_cancel(&C, session);
      return false;
    }
    return true;
  }

  /* The row's brush snapshot is the tool this session runs (the same preparations the 3D entry
   * makes; a snapshot that cannot paint in the 2D mode has no meaning for the effect). */
  Brush *brush = curve_patch_layer_entry_brush_get(reports, row);
  if (brush == nullptr) {
    return false;
  }
  /* The 2D tool's Paint is the texture paint one, not the sculpt one: the snapshot is switched
   * into THAT paint, so the modal's live-brush reads (`BKE_paint_get_active_from_context` in
   * Texture Paint mode) resolve to it. */
  if (!BKE_paint_can_use_brush(&paint_2d, brush)) {
    brush->ob_mode |= OB_MODE_TEXTURE_PAINT;
  }

  /* A Curve Patch effect stores material channel maps: the session must paint the Material
   * canvas, not the editor-image one, or the stamps would land where the row never reads. The
   * Material canvas is also what makes the 2D rasterizer resolve the row's multi-channel targets
   * (`paint_image_curve_patch_raster.cc` resolves them through this source). */
  PaintModeSettings &paint_mode = scene->toolsettings->paint_mode;
  paint_mode.canvas_source = PAINT_CANVAS_SOURCE_MATERIAL;
  BKE_paint_material_layer_target_mode_set(
      *CTX_data_main(&C), *scene, paint_2d, paint_mode, PAINT_LAYER_TARGET_CONTENT);

  /* The 2D seed is laid in the Image Editor's visible canvas (UV), not on the mesh. */
  curve_patch_layer_entry_curves_prepare(row, [&](PaintVector &seed_pv) {
    curve_patch_layer_seeds_place_2d(seed_pv, paint_2d, *brush, sima, image_region);
  });
  if (!curve_patch_layer_entry_maps_prepare(C, ma, row, reports)) {
    return false;
  }
  std::unique_ptr<PrevBrushRestore> restore = curve_patch_layer_entry_brush_switch(
      C, *scene, paint_2d, *brush, "Texture Paint mode", reports);
  if (restore == nullptr) {
    return false;
  }

  /* The maps hold the previous render of these very curves: stamping over it would leave the old
   * result under a moved or deleted curve, so the maps the session writes start empty and it
   * renders all curves. The targets resolve through the same call the session's rasterizer makes
   * (`resolve_targets`), so a channel that joins or leaves between the reset and the restamp
   * still captures into the session's tile map. */
  const Vector<PaintMaterialImageTarget> targets = BKE_paint_material_image_targets_get(
      *ob, paint_mode, brush->material_paint, paint_2d.visible_material_channels);
  bool maps_reset = false;
  for (const PaintMaterialImageTarget &target : targets) {
    if (target.image == nullptr) {
      continue;
    }
    PaintLayersTarget reset_target;
    reset_target.material = &ma;
    reset_target.layer = &row;
    reset_target.channel = int(target.channel);
    reset_target.mode = PaintLayersTargetMode::Content;
    maps_reset |= BKE_paint_layers_target_content_reset(reset_target);
  }

  /* The reference image is the row's Base Color map when it has one, else its first map: the
   * patch's pixel scale is built against it, and the Material canvas targets write every enabled
   * channel regardless of which one the editor displays. */
  Image *ref_image = nullptr;
  for (const PaintMaterialImageTarget &target : targets) {
    if (target.image == nullptr) {
      continue;
    }
    if (target.channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
      ref_image = target.image;
      break;
    }
    if (ref_image == nullptr) {
      ref_image = target.image;
    }
  }
  if (ref_image == nullptr) {
    BKE_report(reports, RPT_WARNING, curve_patch_refusal_message(CurvePatchRefusal::NoImageCanvas));
    return false;
  }

  ImageCurvePatchSession *session = image_curve_patch_session_begin_layer(
      &C, reports, pv, brush, &paint_2d, ref_image, ma, row.marker);
  if (session == nullptr) {
    /* Nothing was published; the destructor restores the previous brush. A session that failed
     * after the maps were reset leaves them empty, so the row must not look up to date. */
    if (maps_reset) {
      BKE_paint_layers_bake_request(row);
    }
    return false;
  }
  /* Owned by the session: its destructor is the single path that puts the user's brush back, on
   * commit and on cancel alike (see #ImageCurvePatchSession::layer_brush_restore). */
  session->layer_brush_restore = std::move(restore);
  session->editor_uv_origin = image_curve_patch_editor_uv_origin(sima);

  curve_patch_layer_entry_finish(C, ma, row);

  /* The first restamp must run in the Image Editor's context: the rebuild reads the brush
   * through `BKE_paint_get_active_from_context`, and the canvas the rasterizer writes is
   * resolved there too. The modal runs from there as well: its region stays the one it registered
   * with, so invoking from the Outliner would bind the editor to the wrong rectangle. */
  wmOperatorStatus status = OPERATOR_CANCELLED;
  curve_patch_layer_with_area_context(C, image_area, image_region, [&]() {
    image_curve_patch_session_restore_and_restamp(&C, session);
    status = WM_operator_name_call(&C,
                                   "PAINT_OT_image_curve_patch_edit",
                                   wm::OpCallContext::InvokeDefault,
                                   nullptr,
                                   nullptr);
  });
  if ((status & OPERATOR_RUNNING_MODAL) == 0) {
    BKE_report(reports, RPT_WARNING, "The Curve Patch editor could not start");
    image_curve_patch_session_cancel(&C, session);
    return false;
  }
  return true;
}

bool curve_patch_layer_activate(bContext &C, Material &ma, MaterialPaintLayer &row)
{
  Scene *scene = CTX_data_scene(&C);
  if (scene == nullptr || scene->toolsettings == nullptr) {
    return false;
  }
  ReportList *reports = CTX_wm_reports(&C);
  if (row.source != MA_PAINT_LAYER_SOURCE_CURVE_PATCH || row.curve_patch == nullptr ||
      row.curve_patch->vectors == nullptr)
  {
    BKE_report(reports, RPT_WARNING, "That row is not a Curve Patch effect");
    return false;
  }
  Object *ob = CTX_data_active_object(&C);
  if (ob == nullptr || ob->type != OB_MESH) {
    BKE_report(reports, RPT_WARNING, "Select a mesh object to edit a Curve Patch effect");
    return false;
  }
  /* Everything that can refuse the entry is checked BEFORE the mode switch below, so a refused
   * click leaves the user in the mode they were in. */
  const short material_slot = BKE_object_material_slot_find_index(ob, &ma);
  if (material_slot == 0) {
    BKE_report(reports, RPT_WARNING, "This effect's material is not assigned to the active object");
    return false;
  }
  const MaterialPaintLayer *bend_text_row = BKE_paint_layers_text_decal_bend_owner_get(ma, row);
  if (bend_text_row == nullptr && !curve_patch_layer_object_has_uv(*ob)) {
    BKE_report(reports,
               RPT_WARNING,
               "A Curve Patch effect needs a UV map on the object to project its curves onto");
    return false;
  }
  /* A row whose records are all switched off has no canvas for the session to stamp into: the
   * session would refuse at its very end, after the entry had already created a fallback map and
   * switched the brush. Stay a plain selection instead; the effect's curves still show as the
   * passive overlay and stay pickable. A Text Decal's bend row is exempt: it never stamps, so it
   * only needs one channel for the session's canvas and that one is re-enabled below. */
  if (bend_text_row != nullptr) {
    const bool any_enabled =
        std::any_of(row.channels,
                    row.channels + row.channels_num,
                    [](const MaterialPaintLayerChannel &record) {
                      return record.state == MA_PAINT_LAYER_CHANNEL_ENABLED;
                    });
    if (!any_enabled) {
      BKE_paint_layers_channel_set_enabled(
          ma, &row, eMaterialPaintChannel(PAINT_MATERIAL_CHANNEL_BASE_COLOR), true);
    }
  }
  else if (row.channels_num > 0 &&
           std::none_of(row.channels,
                        row.channels + row.channels_num,
                        [](const MaterialPaintLayerChannel &record) {
                          return record.state == MA_PAINT_LAYER_CHANNEL_ENABLED;
                        }))
  {
    BKE_report(reports, RPT_INFO, "Enable a channel on this Curve Patch effect to edit its curves");
    return false;
  }
  /* The space decides which editor hosts the tool: a 2D Space effect edits its UV curves in the
   * Image Editor and never needs a View3D; everything below stays the historical 3D path. */
  if (curve_patch_layer_row_space(row) == MA_CURVE_PATCH_SPACE_2D) {
    return curve_patch_layer_activate_2d(C, ma, row);
  }
  /* The entry may come from the Outliner or a panel; the session needs a real View3D context for
   * its re-stamps and symmetry passes. */
  const std::optional<ViewContext> view_context = curve_patch_layer_view3d_context(C, *ob);
  if (!view_context) {
    BKE_report(reports, RPT_WARNING, "Open a 3D Viewport to edit a Curve Patch effect");
    return false;
  }
  if (bend_text_row != nullptr) {
    /* Opening the bend tool in a 3D Viewport is the moment the user says which object's space the
     * curve lives in; pin it so a context-free refresh never falls back to an arbitrary mesh. */
    BKE_paint_layers_text_decal_bend_painted_object_set(
        ma, const_cast<MaterialPaintLayer &>(*bend_text_row), ob);
  }

  /* Activation IS the entry into the row's tool, so the mode the tool runs in is entered here,
   * through the same toggle the mode dropdown uses, instead of refusing with "enter Sculpt Mode"
   * for a switch this click can perform. The toggle also handles the other modes being active and
   * creates the Sculpt session the session below requires. */
  if (!sculpt_mode_poll(&C)) {
    PointerRNA props = WM_operator_properties_create("SCULPT_OT_sculptmode_toggle");
    WM_operator_name_call(
        &C, "SCULPT_OT_sculptmode_toggle", wm::OpCallContext::ExecDefault, &props, nullptr);
    WM_operator_properties_free(&props);
    ob = CTX_data_active_object(&C);
    /* The toggle tags the object for re-evaluation; the session below needs the PBVH that
     * evaluation builds, so force it now rather than relying on the next event. */
    CTX_data_ensure_evaluated_depsgraph(&C);
  }
  Sculpt *sd = scene->toolsettings->sculpt;
  if (sd == nullptr || !sculpt_mode_poll(&C) || ob == nullptr) {
    BKE_report(reports, RPT_WARNING, "Enter Sculpt Mode to edit a Curve Patch effect");
    return false;
  }
  if (ob->runtime == nullptr || ob->runtime->sculpt_session == nullptr) {
    BKE_report(reports, RPT_WARNING, "The object has no Sculpt session");
    return false;
  }
  if (const CurvePatchSession *live = ob->runtime->sculpt_session->curve_patch_session) {
    if (live->layer_material == &ma && BLI_uuid_equal(live->layer_row_marker, row.marker)) {
      /* Already inside this very effect's tool (the activation entered it before the explicit
       * entry ran): nothing to start twice. */
      return true;
    }
    BKE_report(reports, RPT_WARNING, "Finish the live Curve Patch first");
    return false;
  }
  /* The session resolves its paint targets through the object's ACTIVE material slot; a stack
   * focused on another slot would otherwise paint the wrong material's row. */
  if (ob->actcol != material_slot) {
    ob->actcol = material_slot;
  }

  /* The row's brush snapshot is the tool this session runs: without a supported one there is
   * nothing to stamp, and the user has to assign another brush first. Prepare the snapshot on the
   * way (older rows may carry a brush from before Curve Patch was its stroke method). A Text
   * Decal's bend row stamps nothing, so it may run on the mode's active brush instead: it is a
   * container created from the decal panel, not a paint effect with a brush of its own. */
  const bool borrowed_brush = bend_text_row != nullptr && row.curve_patch->brush == nullptr;
  Brush *brush = nullptr;
  if (borrowed_brush) {
    brush = BKE_paint_brush(&sd->paint);
    if (brush == nullptr || !bke::brush::supports_curve_patch(*brush)) {
      BKE_report(reports,
                 RPT_WARNING,
                 "Assign a Paint Brush to edit the bend curve: the curve tool needs one for its "
                 "overlay");
      return false;
    }
  }
  else {
    brush = curve_patch_layer_entry_brush_get(reports, row);
  }
  if (brush == nullptr) {
    return false;
  }
  /* The borrowed brush belongs to the mode, not to the row: it must come back unchanged, and the
   * bend session never stamps, so it does not need the Curve Patch stroke method. */
  if (!borrowed_brush) {
    if (brush->stroke_method != BRUSH_STROKE_CURVE_PATCH) {
      brush->stroke_method = BRUSH_STROKE_CURVE_PATCH;
    }
    if (!BKE_paint_can_use_brush(&sd->paint, brush)) {
      brush->ob_mode |= OB_MODE_SCULPT;
    }
  }

  /* A Curve Patch effect stores material channel maps: the session must paint the Material canvas,
   * not the vertex-color or Poly Paint one, or the strokes would land where the row never reads. */
  PaintModeSettings &paint_mode = scene->toolsettings->paint_mode;
  paint_mode.canvas_source = PAINT_CANVAS_SOURCE_MATERIAL;
  /* Through the BKE switch, not a plain write: leaving a Mask target mode restores the saved
   * content brush and clears #mask_saved_brush, so the restore below captures the brush the user
   * actually holds on content -- and a later Mask entry does not remember the snapshot. */
  BKE_paint_material_layer_target_mode_set(
      *CTX_data_main(&C), *scene, sd->paint, paint_mode, PAINT_LAYER_TARGET_CONTENT);

  /* The row must be the active one: the image effect resolves its targets through it, and the
   * operator also activates the object mask editing target. */
  if (!BLI_uuid_equal(ma.active_layer_marker, row.marker)) {
    BKE_paint_layers_active_set(ma, row.marker);
  }

  curve_patch_layer_entry_curves_prepare(row, [&](PaintVector &seed_pv) {
    curve_patch_layer_seeds_place(C, *ob, *view_context, sd->paint, *brush, seed_pv);
  });
  if (!curve_patch_layer_entry_maps_prepare(C, ma, row, reports)) {
    return false;
  }
  std::unique_ptr<PrevBrushRestore> restore = curve_patch_layer_entry_brush_switch(
      C, *scene, sd->paint, *brush, "Sculpt Mode", reports);
  if (restore == nullptr) {
    return false;
  }

  /* The maps hold the previous render of these very curves: stamping over it would leave the old
   * result under a moved or deleted curve, so the maps the session writes start empty and it
   * renders all curves. */
  bool maps_reset = false;
  const auto reset_maps = [&](const Span<int> channels) {
    for (const int channel : channels) {
      PaintLayersTarget target;
      target.material = &ma;
      target.layer = &row;
      target.channel = channel;
      target.mode = PaintLayersTargetMode::Content;
      maps_reset |= BKE_paint_layers_target_content_reset(target);
    }
  };
  const auto publish_document = [&](CurvePatchDocument &doc) {
    curve_patch_publish_document(*row.curve_patch, *ob, view_context->depsgraph, doc);
  };
  CurvePatchSession *session = curve_patch_start_from_layer(
      C, *ob, *sd, publish_document, *view_context, reset_maps, bend_text_row != nullptr);
  if (session == nullptr) {
    /* Nothing was published; the destructor restores the previous brush. A session that failed
     * after the maps were reset leaves them empty, so the row must not look up to date. */
    if (maps_reset) {
      BKE_paint_layers_bake_request(row);
    }
    return false;
  }
  session->layer_brush_restore = std::move(restore);
  curve_patch_layer_attach_row(*session, ma, row);

  /* The interactive effect now owns the row's maps. */
  curve_patch_layer_entry_finish(C, ma, row);

  /* The modal runs with the snapshot as the active brush, so #CurvePatchEditOpData's invoke-time
   * identity checks see the row's own brush and the edit stays inside it. */
  const wmOperatorStatus status = WM_operator_name_call(
      &C, "SCULPT_OT_curve_patch_edit", wm::OpCallContext::InvokeDefault, nullptr, nullptr);
  if ((status & OPERATOR_RUNNING_MODAL) == 0) {
    /* The invoke refused: end the session the same way a refused anchor stroke would, which also
     * restores the previous brush through the session's own restore. */
    BKE_report(reports, RPT_WARNING, "The Curve Patch editor could not start");
    curve_patch_discard_on_session_end(*ob);
    return false;
  }
  return true;
}

/** The active Curve Patch effect of the active object's active material, or false. */
static bool curve_patch_layer_active_effect_get(bContext &C,
                                                Material *&r_ma,
                                                MaterialPaintLayer *&r_row)
{
  Object *ob = CTX_data_active_object(&C);
  if (ob == nullptr || ob->type != OB_MESH) {
    return false;
  }
  r_ma = BKE_object_material_get(ob, ob->actcol);
  if (r_ma == nullptr) {
    return false;
  }
  r_row = BKE_paint_layers_active_layer_get(*r_ma);
  return r_row != nullptr && r_row->source == MA_PAINT_LAYER_SOURCE_CURVE_PATCH &&
         r_row->curve_patch != nullptr;
}

bool curve_patch_layer_enter_active_row(bContext &C)
{
  Material *ma = nullptr;
  MaterialPaintLayer *row = nullptr;
  if (!curve_patch_layer_active_effect_get(C, ma, row)) {
    return false;
  }
  return curve_patch_layer_enter_if_sculpting(C, *ma, *row);
}

/* The mode switch that resumes an edit runs before the tool system has synced the area's tool to
 * the new mode, and entering the tool right there trips its consistency check. The entry therefore
 * waits for the first timer tick of this modal, which is a later pass of the event loop. */
static wmOperatorStatus curve_patch_layer_resume_invoke(bContext *C,
                                                        wmOperator *op,
                                                        const wmEvent * /*event*/)
{
  /* Background runs have no window to host a timer, and most mode entries have no effect to
   * resume at all. */
  Material *ma = nullptr;
  MaterialPaintLayer *row = nullptr;
  if (CTX_wm_window(C) == nullptr || !curve_patch_layer_active_effect_get(*C, ma, row)) {
    return OPERATOR_CANCELLED;
  }
  op->customdata = WM_event_timer_add(CTX_wm_manager(C), CTX_wm_window(C), TIMER, 0.05);
  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus curve_patch_layer_resume_modal(bContext *C,
                                                       wmOperator *op,
                                                       const wmEvent *event)
{
  wmTimer *timer = static_cast<wmTimer *>(op->customdata);
  if (event->type != TIMER || event->customdata != timer) {
    return OPERATOR_RUNNING_MODAL | OPERATOR_PASS_THROUGH;
  }
  WM_event_timer_remove(CTX_wm_manager(C), CTX_wm_window(C), timer);
  op->customdata = nullptr;
  curve_patch_layer_enter_active_row(*C);
  return OPERATOR_FINISHED;
}

static void curve_patch_layer_resume_cancel(bContext *C, wmOperator *op)
{
  if (op->customdata != nullptr) {
    WM_event_timer_remove(
        CTX_wm_manager(C), CTX_wm_window(C), static_cast<wmTimer *>(op->customdata));
    op->customdata = nullptr;
  }
}

void PAINT_OT_curve_patch_layer_resume(wmOperatorType *ot)
{
  ot->name = "Resume Curve Patch Edit";
  ot->idname = "PAINT_OT_curve_patch_layer_resume";
  ot->description = "Enter the active Curve Patch effect's tool once the mode change has settled";
  ot->invoke = curve_patch_layer_resume_invoke;
  ot->modal = curve_patch_layer_resume_modal;
  ot->cancel = curve_patch_layer_resume_cancel;
  ot->poll = ED_operator_object_active_editable_mesh;
  ot->flag = OPTYPE_INTERNAL;
}

bool curve_patch_layer_enter_active_2d_row(bContext &C)
{
  Object *ob = CTX_data_active_object(&C);
  if (ob == nullptr || ob->type != OB_MESH) {
    return false;
  }
  Material *ma = BKE_object_material_get(ob, ob->actcol);
  if (ma == nullptr) {
    return false;
  }
  MaterialPaintLayer *row = BKE_paint_layers_active_layer_get(*ma);
  if (row == nullptr || row->source != MA_PAINT_LAYER_SOURCE_CURVE_PATCH ||
      row->curve_patch == nullptr || row->curve_patch->vectors == nullptr ||
      (row->flag & MA_PAINT_LAYER_ENABLED) == 0 ||
      curve_patch_layer_row_space(*row) != MA_CURVE_PATCH_SPACE_2D ||
      row->curve_patch->stored_space != MA_CURVE_PATCH_SPACE_2D ||
      image_curve_patch_session_edits_row(*ma, row->marker))
  {
    return false;
  }
  curve_patch_layer_activate_2d(C, *ma, *row);
  return true;
}

bool curve_patch_layer_enter_if_sculpting(bContext &C, Material &ma, MaterialPaintLayer &row)
{
  const Object *ob = CTX_data_active_object(&C);
  if (ob == nullptr) {
    return false;
  }
  /* Selecting an effect while in another mode only selects it; the tool is entered with the
   * explicit action, which switches the mode itself. */
  if (curve_patch_layer_row_space(row) == MA_CURVE_PATCH_SPACE_2D) {
    /* The 2D tool runs in the Image Editor's Texture Paint mode, not in Sculpt Mode: resume only
     * when that editor is the one showing it. */
    const SpaceImage *sima = CTX_wm_space_image(&C);
    if (sima == nullptr || sima->mode != SI_MODE_PAINT) {
      return false;
    }
    return curve_patch_layer_activate(C, ma, row);
  }
  if ((ob->mode & OB_MODE_SCULPT) == 0) {
    return false;
  }
  return curve_patch_layer_activate(C, ma, row);
}

/** The write half the two document stores share: resolve the row the session was entered from and
 * write the live document into the row's container and, for the curves that name an external
 * object, into the objects. Returns the row, or null when there is nothing to write (no row, or
 * its sources are gone). */
MaterialPaintLayer *curve_patch_layer_document_to_row(CurvePatchSession &session,
                                                      const bool for_save)
{
  MaterialPaintLayer *row = curve_patch_layer_session_row(session);
  if (row == nullptr || row->curve_patch == nullptr) {
    return nullptr;
  }
  curve_patch_write_back_document(session, *row, for_save);
  return row;
}

void curve_patch_layer_store_document(CurvePatchSession &session)
{
  MaterialPaintLayer *row = curve_patch_layer_document_to_row(session, false);
  if (row == nullptr) {
    return;
  }
  if (session.layer_material != nullptr &&
      BKE_paint_layers_text_decal_bend_owner_get(*session.layer_material, *row) != nullptr)
  {
    /* A bend row is a container: nothing is rendered from it, only the decal follows the curve. */
    BKE_paint_layers_tag_edited(*session.layer_material);
    return;
  }
  /* The container tag lives in the vectors source's write-back; the object mode's curves were
   * written into the object, whose data edit tags itself. Either way the bake follows the row's
   * curves through the request below. */
  BKE_paint_layers_bake_request(*row);
}

void curve_patch_layer_sync_objects(CurvePatchSession &session)
{
  if (!session.is_layer_effect() || session.layer_object_sync_suspended) {
    return;
  }
  MaterialPaintLayer *row = curve_patch_layer_session_row(session);
  if (row == nullptr) {
    return;
  }
  curve_patch_document_to_objects(session, *row);
}

void curve_patch_layer_store_document_for_save(CurvePatchSession &session)
{
  /* The file write only carries the curves into the container (the object source writes nothing:
   * the object's own data is saved with the file). The live maps are not stale, and a loaded file
   * re-bakes anyway -- the blend read clears the row's bake hash, so the first bake plan renders
   * the row again. Tagging the PaintVector changed here would therefore mark a staleness nobody
   * acts on, so neither the revision nor the bake is touched. */
  curve_patch_layer_document_to_row(session, true);
}

void curve_patch_layer_commit_document(bContext &C, CurvePatchSession &session)
{
  MaterialPaintLayer *row = curve_patch_layer_session_row(session);
  if (row == nullptr) {
    return;
  }
  curve_patch_layer_store_document(session);
  /* A write into a curve object's own data is a geometry edit the object's editors follow too
   * (the same notification the paint curve's sync to its source object sends); the memfile undo
   * step below records the object's data with it. */
  if (row->curve_patch != nullptr && BKE_paint_layers_curve_patch_reads_object(*row)) {
    WM_event_add_notifier(&C, NC_GEOM | ND_DATA, nullptr);
  }
  ED_undo_memfile_push(&C, "Curve Patch Layer Edit");
  WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, session.layer_material);
}

/** The row a live 2D layer session edits, or null when it was deleted (or is not a Curve Patch
 * row). The session addresses the row by marker for the same reason the 3D one does. */
MaterialPaintLayer *curve_patch_layer_session_row_2d(ImageCurvePatchSession &session)
{
  if (!curve_patch_layer_material_alive(session.layer_material) ||
      BLI_uuid_is_nil(session.layer_row_marker))
  {
    return nullptr;
  }
  MaterialPaintLayer *row = BKE_paint_layers_find(*session.layer_material,
                                                  session.layer_row_marker);
  if (row == nullptr || row->source != MA_PAINT_LAYER_SOURCE_CURVE_PATCH ||
      row->curve_patch == nullptr)
  {
    return nullptr;
  }
  return row;
}

/**
 * Re-express the stored curve coordinates of \a row in the space its payload now asks for: object
 * space to canvas UV through the nearest surface point, canvas UV to object space through the
 * reverse UV lookup. Without this a switched-over curve would be read in the wrong space and sit
 * somewhere meaningless. The frozen stamp parameters are re-derived from the row's brush because
 * their radius is in world units in 3D and in image pixels in 2D.
 *
 * The mesh is the ACTIVE object's, which must use \a ma: with several objects or slots the first
 * one found would be an arbitrary pick. All or nothing: on a failure no curve and not
 * #MaterialPaintLayerCurvePatch::stored_space changes, and the user is told why.
 */
static bool curve_patch_layer_space_convert(bContext &C, Material &ma, MaterialPaintLayer &row)
{
  MaterialPaintLayerCurvePatch &payload = *row.curve_patch;
  if (payload.stored_space == payload.space) {
    return true;
  }
  const bool to_2d = payload.space == MA_CURVE_PATCH_SPACE_2D;
  /* A curve that names an external object has no UV coordinates to carry over: its curve is the
   * object's 3D positions, and 2D (UV) coordinates cannot round-trip through a curves object. Only
   * the move to 2D is refused; the object already lives in the 3D space it asks for. */
  if (to_2d && payload.vectors != nullptr) {
    const PaintVector &object_pv = *payload.vectors;
    for (const int item_i : IndexRange(object_pv.items_num)) {
      if (object_pv.items[item_i].type == PAINT_VECTOR_ITEM_CURVE_PATCH &&
          object_pv.items[item_i].curve_object != nullptr)
      {
        BKE_report(CTX_wm_reports(&C),
                   RPT_WARNING,
                   "A Curve Patch with curves that read a Curve Object works in 3D Space only");
        return false;
      }
    }
  }
  MaterialPaintLayer *text_row = const_cast<MaterialPaintLayer *>(
      BKE_paint_layers_text_decal_bend_owner_get(ma, row));

  Vector<PaintVectorItem *> items;
  Vector<bke::CurvesGeometry *> geometries;
  if (payload.vectors != nullptr) {
    PaintVector &pv = *payload.vectors;
    for (const int item_i : IndexRange(pv.items_num)) {
      PaintVectorItem &item = pv.items[item_i];
      if (item.type == PAINT_VECTOR_ITEM_CURVE_PATCH &&
          BKE_paint_vector_item_curve_geometry_is_valid(item))
      {
        items.append(&item);
        geometries.append(&item.curve_patch_geometry.wrap());
      }
    }
  }
  if (items.is_empty()) {
    /* Nothing to carry over, so no mesh is needed. */
    if (text_row != nullptr) {
      curve_patch_layer_bend_text_space_sync(ma, *text_row, to_2d);
    }
    payload.stored_space = payload.space;
    return true;
  }

  ReportList *reports = CTX_wm_reports(&C);
  Object *ob = CTX_data_active_object(&C);
  if (ob == nullptr || ob->type != OB_MESH || BKE_object_material_slot_find_index(ob, &ma) == 0) {
    BKE_report(reports,
               RPT_WARNING,
               "Select a mesh object that uses this material to switch the Curve Patch Space");
    return false;
  }
  if (!curve_patch_layer_object_has_uv(*ob)) {
    BKE_report(reports, RPT_WARNING, "The active object has no UV map to convert the curves with");
    return false;
  }
  /* The stored curves live on the original data, so the original mesh is the reference. */
  const Mesh *mesh = BKE_object_get_original_mesh(ob);
  int failed_points_num = 0;
  if (!curve_patch_space_convert(*mesh, geometries, to_2d, failed_points_num)) {
    /* A bend curve is a guide for the decal, and a canvas curve may run through empty canvas
     * outside the mesh's UV islands. Carrying it back through the decal frame keeps the switch
     * useful (the curve then lies on the frame plane, where the projected text is read) instead of
     * reverting the Space because of a few stray points. 3D to 2D stays strict: the surface bind
     * has to be exact or the curve would land somewhere meaningless. */
    const bool frame_fallback = !to_2d && text_row != nullptr &&
                                text_row->projection != nullptr;
    if (!frame_fallback) {
      if (failed_points_num > 0) {
        BKE_reportf(reports,
                    RPT_WARNING,
                    "%d curve point(s) lie outside the UV layout; the Space was not changed",
                    failed_points_num);
      }
      else {
        BKE_report(reports,
                   RPT_WARNING,
                   "The active object cannot convert the curves to this Space");
      }
      return false;
    }
    const auto convert_geometry_frame = [&](bke::CurvesGeometry &geom) {
      const MutableSpan<float3> positions = geom.positions_for_write();
      for (float3 &position : positions) {
        const float uv[2] = {position.x, position.y};
        float p[3];
        if (BKE_paint_layers_text_decal_bend_point_from_uv(ma, *text_row, ob, uv, p)) {
          position = float3(p[0], p[1], p[2]);
        }
      }
      const auto convert_handles = [&](const std::optional<Span<float3>> &handles,
                                       const MutableSpan<float3> &out) {
        if (!handles.has_value()) {
          return;
        }
        Vector<float3> converted(handles->size());
        for (const int i : handles->index_range()) {
          const float3 handle = (*handles)[i];
          const float uv[2] = {handle.x, handle.y};
          float p[3];
          if (BKE_paint_layers_text_decal_bend_point_from_uv(ma, *text_row, ob, uv, p)) {
            converted[i] = float3(p[0], p[1], p[2]);
          }
          else {
            converted[i] = handle;
          }
        }
        out.copy_from(converted.as_span());
      };
      convert_handles(geom.handle_positions_left(), geom.handle_positions_left_for_write());
      convert_handles(geom.handle_positions_right(), geom.handle_positions_right_for_write());
      geom.tag_positions_changed();
    };
    for (bke::CurvesGeometry *geometry : geometries) {
      convert_geometry_frame(*geometry);
    }
  }

  /* The frozen radius is in world units in 3D and in image pixels in 2D. */
  float radius;
  float radius_per_size;
  const Brush *brush = payload.brush;
  curve_patch_layer_brush_radius_for_space(brush, to_2d, radius, radius_per_size);
  for (PaintVectorItem *item : items) {
    curve_patch_layer_radius_attribute_ensure(item->curve_patch_geometry.wrap());
    if (brush != nullptr) {
      curve_patch_layer_item_params_refresh(
          *item, *brush, radius, radius_per_size, float3(0.0f, 0.0f, 1.0f));
    }
  }
  payload.stored_space = payload.space;
  BKE_paint_vector_tag_changed(*payload.vectors);
  if (text_row != nullptr) {
    curve_patch_layer_bend_text_space_sync(ma, *text_row, to_2d);
  }
  return true;
}

/**
 * Apply a change of the row's Space: end the live session of the OLD space (its commit writes the
 * curves back in the space they were edited in), carry the curves over, and enter the NEW space's
 * tool. The entry happens when a session was live, or when the new Space's editor is on screen:
 * the passive preview there is not editable, and a stroke over it would start a new anchor curve
 * instead of grabbing the effect's own points.
 */
static bool curve_patch_layer_space_switch(bContext &C, Material &ma, MaterialPaintLayer &row)
{
  Main &bmain = *CTX_data_main(&C);
  const bool was_live = ED_curve_patch_session_edits_row(bmain, ma, row.marker);
  if (was_live) {
    if (image_curve_patch_session_edits_row(ma, row.marker)) {
      image_curve_patch_session_commit(&C, image_curve_patch_session_active_get());
    }
    else {
      for (Object &ob : bmain.objects) {
        const CurvePatchSession *session = ED_curve_patch_session_get(ob);
        if (session != nullptr && session->layer_material == &ma &&
            BLI_uuid_equal(session->layer_row_marker, row.marker))
        {
          curve_patch_layer_commit_live_session(C, ob);
          break;
        }
      }
    }
  }
  const bool converted = curve_patch_layer_space_convert(C, ma, row);
  const bool to_2d = row.curve_patch->space == MA_CURVE_PATCH_SPACE_2D;
  /* Switching the Space opens the new Space's tool whenever that editor is on the screen, even
   * without a live session: the user asked for the other editor, and leaving the points unpickable
   * until they find and click the effect row again reads as a broken switch. */
  bScreen *screen = CTX_wm_screen(&C);
  const bool target_editor_open = BKE_screen_find_big_area(
                                      screen, to_2d ? SPACE_IMAGE : SPACE_VIEW3D, 0) != nullptr;
  if (!converted) {
    /* The curves stay in the space they are stored in, so the Space must not claim otherwise. */
    row.curve_patch->space = row.curve_patch->stored_space;
  }
  /* The stored coordinates changed meaning (or the Space property did): every baked map of the row
   * is stale, and the evaluated copies of the material and container must pick the new Space up or
   * the 3D Viewport goes on drawing the curve in the space it was switched out of. A bend row is a
   * container, not a canvas, so its conversion only re-tags the decal. */
  if (BKE_paint_layers_text_decal_bend_owner_get(ma, row) != nullptr) {
    /* The decal's raster follows the curve: re-bake it now so the new Space shows at once, not on
     * the next unrelated edit. */
    BKE_paint_layers_text_decal_invalidate(ma);
    ma.paint_layers_flag |= MA_PAINT_LAYERS_REGEN;
    BKE_paint_layers_tag_edited(ma);
  }
  else {
    BKE_paint_layers_bake_request(row);
  }
  DEG_id_tag_update(&ma.id, ID_RECALC_SYNC_TO_EVAL | ID_RECALC_SHADING);
  if (row.curve_patch->vectors != nullptr) {
    DEG_id_tag_update(&row.curve_patch->vectors->id, ID_RECALC_SYNC_TO_EVAL);
  }
  WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &ma.id);
  if (converted && (was_live || target_editor_open)) {
    curve_patch_layer_activate(C, ma, row);
  }
  return converted;
}

static bool curve_patch_layer_session_edits_row(const Main &bmain,
                                                const Material &ma,
                                                const bUUID &row_marker)
{
  if (BLI_uuid_is_nil(row_marker)) {
    return false;
  }
  /* The 3D sessions: one per sculpt-mode object, each remembering the row it was entered from. */
  for (const Object &ob : bmain.objects) {
    if (ob.runtime == nullptr || ob.runtime->sculpt_session == nullptr ||
        ob.runtime->sculpt_session->curve_patch_session == nullptr)
    {
      continue;
    }
    const CurvePatchSession &session = *ob.runtime->sculpt_session->curve_patch_session;
    if (session.layer_material == &ma && BLI_uuid_equal(session.layer_row_marker, row_marker)) {
      return true;
    }
  }
  /* The Image Editor's 2D session. */
  return image_curve_patch_session_edits_row(ma, row_marker);
}

void curve_patch_layer_commit_session_2d(bContext &C, ImageCurvePatchSession &session)
{
  MaterialPaintLayer *row = curve_patch_layer_session_row_2d(session);
  if (row == nullptr || row->curve_patch->vectors == nullptr) {
    return;
  }
  PaintVector &pv = *row->curve_patch->vectors;
  /* Fixed for the session's life: the 2D modal adds and removes points, never patches. */
  BLI_assert(session.uv_curves.size() == session.doc.patches.size() &&
             session.ref_tiles.size() == session.doc.patches.size());

  /* The CANONICAL UV curve is what persists -- `CurvePatchItem::control_curve` on the document is
   * the pixel-space copy the rasterizer builds from it and is never written back. The 2D modal
   * cannot create patches today, so the append pass only fires for a stale index. */
  curve_patch_document_write_items(
      session.doc,
      pv,
      [&](const int patch_i) -> const bke::CurvesGeometry * {
        return session.uv_curves.index_range().contains(patch_i) ? &session.uv_curves[patch_i] :
                                                                   nullptr;
      },
      true);
  BKE_paint_vector_tag_changed(pv);
  if (session.is_bend_curve_only) {
    /* The container is only a bend source: no map is rendered from it, so the commit re-tags the
     * material and lets the Text Decal refresh re-rasterize along the new curve. */
    if (session.layer_material != nullptr) {
      BKE_paint_layers_tag_edited(*session.layer_material);
    }
  }
  else {
    /* The row's content is its own channel maps: the committed document invalidates the bake,
     * which re-renders the maps in the background the next time the plan runs. */
    BKE_paint_layers_bake_request(*row);
  }
  ED_undo_memfile_push(&C, "Curve Patch Layer Edit");
  WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, session.layer_material);
}

void curve_patch_layer_cancel_session_2d(ImageCurvePatchSession &session)
{
  /* Nothing to write back and no undo step -- the pixels went back through the session's tile
   * map. Only the stack-visible exit is notified, so the UI unlocks the row's editing (and any
   * row panel that polled the session refreshes). */
  /* The material may be gone (an undo step freed it under the modal): then there is no stack row
   * left to refresh. */
  if (!curve_patch_layer_material_alive(session.layer_material)) {
    return;
  }
  WM_main_add_notifier(NC_MATERIAL | ND_SHADING, &session.layer_material->id);
}

void curve_patch_layer_active_item_sync(const CurvePatchSession &session)
{
  MaterialPaintLayer *row = curve_patch_layer_session_row(session);
  if (row == nullptr || row->curve_patch->vectors == nullptr || !session.doc.has_active_item()) {
    return;
  }
  PaintVector &pv = *row->curve_patch->vectors;
  const int item_i = session.doc.active_item().source_curve_index;
  if (item_i >= 0 && item_i < pv.items_num) {
    pv.active_item = item_i;
  }
}

bool curve_patch_layer_active_item_follow(CurvePatchSession &session, bool *r_look_changed)
{
  MaterialPaintLayer *row = curve_patch_layer_session_row(session);
  if (row == nullptr || row->curve_patch->vectors == nullptr) {
    return false;
  }
  const PaintVector &pv = *row->curve_patch->vectors;
  /* A rename or a per-curve look edit in the list writes the container item only; the document is
   * what undo and the mirror work from. */
  bool look_changed = false;
  for (CurvePatchItem &patch : session.doc.patches) {
    if (patch.source_curve_index < 0 || patch.source_curve_index >= pv.items_num) {
      continue;
    }
    const PaintVectorItem &pvi = pv.items[patch.source_curve_index];
    if (patch.name != pvi.curve_name) {
      patch.name = pvi.curve_name;
    }
    const bke::CurvePatchParams stored = BKE_paint_vector_curve_params_decode(
        pvi.curve_patch_params);
    if (patch.params.opacity != stored.opacity || patch.params.radius != stored.radius ||
        patch.params.hidden != stored.hidden ||
        patch.params.symmetry_flags != stored.symmetry_flags)
    {
      /* `radius_per_size` is deliberately left alone. It is the curve's world-units-per-pixel
       * ratio -- the basis the Width slider's 0-100 scale and the absolute-jitter conversion are
       * both expressed in -- so scaling it with the radius would make Width snap back to its old
       * value on every edit. */
      patch.params.opacity = stored.opacity;
      patch.params.radius = stored.radius;
      patch.params.hidden = stored.hidden;
      patch.params.symmetry_flags = stored.symmetry_flags;
      look_changed = true;
    }
  }
  if (r_look_changed != nullptr) {
    *r_look_changed = look_changed;
  }
  const int item_i = pv.active_item;
  for (const int patch_i : session.doc.patches.index_range()) {
    if (session.doc.patches[patch_i].source_curve_index != item_i) {
      continue;
    }
    if (patch_i == session.doc.active_patch) {
      return look_changed;
    }
    curve_patch_document_active_patch_set(session.doc, patch_i);
    return true;
  }
  return look_changed;
}

bool curve_patch_layer_follow_2d(ImageCurvePatchSession &session)
{
  MaterialPaintLayer *row = curve_patch_layer_session_row_2d(session);
  if (row == nullptr || row->curve_patch->vectors == nullptr) {
    return false;
  }
  const PaintVector &pv = *row->curve_patch->vectors;
  /* The same contract as the 3D session: the Curve Patch panel and the curve list write the
   * container item, and the live document follows it. The 2D document is the truth while the
   * session runs and is written back on commit, so an edit that skips this would be overwritten. */
  bool changed = false;
  for (CurvePatchItem &patch : session.doc.patches) {
    if (patch.source_curve_index < 0 || patch.source_curve_index >= pv.items_num) {
      continue;
    }
    const PaintVectorItem &pvi = pv.items[patch.source_curve_index];
    if (patch.name != pvi.curve_name) {
      patch.name = pvi.curve_name;
    }
    const bke::CurvePatchParams stored = BKE_paint_vector_curve_params_decode(
        pvi.curve_patch_params);
    if (patch.params.opacity != stored.opacity || patch.params.radius != stored.radius ||
        patch.params.hidden != stored.hidden ||
        patch.params.symmetry_flags != stored.symmetry_flags)
    {
      /* `radius_per_size` stays: the Width scale is expressed in it (see the 3D follow). */
      patch.params.opacity = stored.opacity;
      patch.params.radius = stored.radius;
      patch.params.hidden = stored.hidden;
      patch.params.symmetry_flags = stored.symmetry_flags;
      changed = true;
    }
  }
  for (const int patch_i : session.doc.patches.index_range()) {
    if (session.doc.patches[patch_i].source_curve_index != pv.active_item) {
      continue;
    }
    if (patch_i != session.doc.active_patch) {
      curve_patch_document_active_patch_set(session.doc, patch_i);
      changed = true;
    }
    break;
  }
  return changed;
}

void curve_patch_layer_mirror_vector(CurvePatchSession &session)
{
  MaterialPaintLayer *row = curve_patch_layer_session_row(session);
  if (row == nullptr || row->curve_patch->vectors == nullptr) {
    return;
  }
  PaintVector &pv = *row->curve_patch->vectors;

  /* Items are matched by id, not by position: an item whose patch is still there is updated in
   * place, which keeps the fields the document does not carry (flag, surface binding). */
  Vector<int> item_of_patch(session.doc.patches.size(), -1);
  Vector<bool> claimed(pv.items_num, false);
  for (const int patch_i : session.doc.patches.index_range()) {
    const int curve_id = session.doc.patches[patch_i].curve_id;
    if (curve_id <= 0) {
      /* A curve without an id yet (a file from before the id existed) is still the item it was
       * published from: matching it by that item keeps the fields the document does not carry --
       * above all its external curve object -- instead of deleting the item and adding a bare one
       * in its place. */
      const int published_item = session.doc.patches[patch_i].source_curve_index;
      if (published_item >= 0 && published_item < pv.items_num && !claimed[published_item] &&
          pv.items[published_item].type == PAINT_VECTOR_ITEM_CURVE_PATCH)
      {
        item_of_patch[patch_i] = published_item;
        claimed[published_item] = true;
      }
      continue;
    }
    for (const int i : IndexRange(pv.items_num)) {
      if (!claimed[i] && pv.items[i].type == PAINT_VECTOR_ITEM_CURVE_PATCH &&
          pv.items[i].curve_id == curve_id)
      {
        item_of_patch[patch_i] = i;
        claimed[i] = true;
        break;
      }
    }
  }
  /* Curve items nobody claims belong to a patch that was deleted (or never confirmed). */
  for (int i = pv.items_num - 1; i >= 0; i--) {
    if (pv.items[i].type != PAINT_VECTOR_ITEM_CURVE_PATCH || claimed[i]) {
      continue;
    }
    BKE_paint_vector_item_remove(pv, i);
    for (int &item_i : item_of_patch) {
      if (item_i > i) {
        item_i--;
      }
    }
  }
  for (const int patch_i : session.doc.patches.index_range()) {
    CurvePatchItem &patch = session.doc.patches[patch_i];
    if (patch.curve_id <= 0) {
      patch.curve_id = curve_patch_layer_next_curve_id(pv);
    }
    if (item_of_patch[patch_i] < 0) {
      BKE_paint_vector_item_add(pv);
      item_of_patch[patch_i] = pv.items_num - 1;
      pv.items[item_of_patch[patch_i]].type = PAINT_VECTOR_ITEM_CURVE_PATCH;
    }
    PaintVectorItem &pvi = pv.items[item_of_patch[patch_i]];
    pvi.curve_id = patch.curve_id;
    pvi.curve_patch_geometry.wrap() = patch.control_curve;
    pvi.curve_patch_params = BKE_paint_vector_curve_params_encode(patch.params);
    BLI_strncpy(pvi.curve_name, patch.name.c_str(), sizeof(pvi.curve_name));
    patch.source_curve_index = item_of_patch[patch_i];
  }
  /* The list shows the container's order while the effect stamps in the document's, so the
   * container's curve items are put in the document's order: the curve items keep their slots
   * (a Shape item between them stays where it is) and trade places until each patch has the slot
   * its position asks for. */
  Vector<int> curve_slots;
  for (const int i : IndexRange(pv.items_num)) {
    if (pv.items[i].type == PAINT_VECTOR_ITEM_CURVE_PATCH) {
      curve_slots.append(i);
    }
  }
  if (curve_slots.size() == session.doc.patches.size()) {
    for (const int patch_i : session.doc.patches.index_range()) {
      const int wanted = session.doc.patches[patch_i].source_curve_index;
      const int slot = curve_slots[patch_i];
      if (wanted == slot || !BKE_paint_vector_item_swap(pv, wanted, slot)) {
        continue;
      }
      /* The item that sat in `slot` now sits where the wanted one was. */
      for (CurvePatchItem &other : session.doc.patches) {
        if (other.source_curve_index == slot) {
          other.source_curve_index = wanted;
        }
      }
      session.doc.patches[patch_i].source_curve_index = slot;
    }
  }
  /* `BKE_paint_vector_item_add` moves the active item to the new one; the document decides. */
  if (session.doc.has_active_item()) {
    pv.active_item = session.doc.active_item().source_curve_index;
  }
  else {
    pv.active_item = std::clamp(pv.active_item, 0, std::max(pv.items_num - 1, 0));
  }
}

bool curve_patch_layer_solo(const CurvePatchSession &session)
{
  const MaterialPaintLayer *row = curve_patch_layer_session_row(session);
  return row != nullptr && row->curve_patch->vectors != nullptr &&
         (row->curve_patch->vectors->flag & PAINT_VECTOR_SHOW_ALL_CURVES) == 0;
}

bool curve_patch_layer_session_blocks_row(const CurvePatchSession &session,
                                          const MaterialPaintLayer &row)
{
  return !BLI_uuid_equal(session.layer_row_marker, row.marker);
}

void curve_patch_layer_commit_live_session(bContext &C, Object &ob)
{
  /* Commit through the session's OWNER: the session-ending commit refuses any object but the
   * owner, and a modal event can arrive with a secondary target as the active object -- committing
   * through \a ob then would strand the live session. */
  const CurvePatchSession *session = ED_curve_patch_session_get(ob);
  curve_patch_commit_on_session_end(
      C, session != nullptr && session->owner != nullptr ? *session->owner : ob);
}

/** A live Curve Patch session as #for_each_live_curve_patch_session reports it: a 3D session an
 * object runs, or the Image Editor's 2D one. Exactly one of #session and #session_2d is set. */
struct LiveCurvePatchSession {
  /** The object the 3D session runs on, null for the 2D session. Non-const because a commit acts
   * through the object; the session itself is only read, through the const session getter. */
  Object *ob = nullptr;
  /** The 3D session, null when this describes the 2D session. */
  const CurvePatchSession *session = nullptr;
  /** The Image Editor's 2D session, null when this describes a 3D session. */
  ImageCurvePatchSession *session_2d = nullptr;

  /** The material of the effect row the session edits, or null when it is free-standing: it has no
   * row to keep its curves in and writes into the layer that was active when it started. */
  Material *row_material() const
  {
    if (session != nullptr) {
      return session->layer_material;
    }
    /* Unlike the 3D session, which marks a free-standing patch by a null material, the 2D session
     * says so through its `is_layer` flag. */
    return session_2d->is_layer ? session_2d->layer_material : nullptr;
  }

  /** The marker of the effect row the session edits. Only meaningful when #row_material is set. */
  const bUUID &row_marker() const
  {
    return session != nullptr ? session->layer_row_marker : session_2d->layer_row_marker;
  }
};

/** Call \a callback for every live Curve Patch session of \a bmain: the 3D ones its objects run,
 * then the Image Editor's 2D one. \a callback returns false to stop the walk. The one walk the
 * row-switch, stroke-gate and commit paths share, so the free-standing and row-bound rules live in
 * #LiveCurvePatchSession instead of a copy per caller. Committing inside the callback is safe: a
 * commit never adds or removes objects, so the object walk stays valid (it does evaluate the
 * depsgraph, so a committing callback must not carry state from one session to the next). */
static void for_each_live_curve_patch_session(Main &bmain,
                                              FunctionRef<bool(LiveCurvePatchSession)> callback)
{
  for (Object &ob : bmain.objects) {
    const CurvePatchSession *session = ED_curve_patch_session_get(ob);
    if (session != nullptr && !callback(LiveCurvePatchSession{&ob, session, nullptr})) {
      return;
    }
  }
  ImageCurvePatchSession *session_2d = image_curve_patch_session_active_get();
  if (session_2d != nullptr) {
    callback(LiveCurvePatchSession{nullptr, nullptr, session_2d});
  }
}

/** Whether some object runs a live layer session for the row \a row_marker of \a material. */
static bool curve_patch_layer_row_is_live(Main &bmain,
                                          const Material &material,
                                          const bUUID &row_marker)
{
  bool live = false;
  for_each_live_curve_patch_session(bmain, [&](const LiveCurvePatchSession view) {
    /* Only the sessions published on objects carry the row's snapshot in the hand, which is what
     * the caller guards; the Image Editor's 2D session is checked by
     * #curve_patch_layer_row_session_live where the row being live matters. */
    live = view.session != nullptr && view.row_material() == &material &&
           BLI_uuid_equal(view.row_marker(), row_marker);
    return !live;
  });
  return live;
}

void curve_patch_layer_commit_live_sessions(bContext &C, const bUUID *target_marker)
{
  Main *bmain = CTX_data_main(&C);
  if (bmain == nullptr) {
    return;
  }
  /* Sessions are published on objects, and a secondary target's owner can hold one while another
   * object is active -- so the search cannot be narrowed by the scene or the active object: every
   * live session the switch would strand (its brush still in the hand, its stroke polls blocking
   * the paint) commits. The session of the row being (re)activated stays, unless \a target_marker
   * is null: a switch into mask work ends it even when it is its own row. */
  for_each_live_curve_patch_session(*bmain, [&](const LiveCurvePatchSession view) {
    if (view.session != nullptr) {
      if (view.session->layer_material == nullptr) {
        /* A free-standing patch has no row to stay with: it writes the surface it started on, and a
         * switch of the stack below it (or the removal of the row it was started from) must not
         * leave its overlay and its stroke polls behind. Commit it, as its own Enter/Esc would. */
        curve_patch_commit_on_session_end(C, *view.ob);
        return true;
      }
      if (target_marker != nullptr && BLI_uuid_equal(view.session->layer_row_marker, *target_marker))
      {
        return true;
      }
      curve_patch_layer_commit_live_session(C, *view.ob);
      return true;
    }
    /* The Image Editor's 2D session is not published on an object, so its commit goes through the
     * session itself. Its row-bound session stays when it is the target row; a free-standing 2D
     * patch has no row and commits on every switch, like the 3D one. */
    if (!(view.session_2d->is_layer && target_marker != nullptr &&
          BLI_uuid_equal(view.session_2d->layer_row_marker, *target_marker)))
    {
      image_curve_patch_session_commit(&C, view.session_2d);
    }
    return true;
  });
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Active curve look (the radial control)
 *
 * While a layer session runs, the brush Size ring is the ACTIVE curve's Width and the Strength
 * ring its Opacity: the per-curve look belongs to the curve, and the snapshot brush's own Size and
 * Strength are frozen for it, so the rings must write the row's container item instead of the
 * brush. A write goes to the persisted item only -- the modal editor's poll syncs it into the
 * live document on the next event (its re-stamp), and the RNA update callback re-stamps once for
 * callers that bypass a running modal.
 * \{ */

/** The persisted container item of the session's active patch: null when the session was not
 * entered from a row, the row was deleted, or the active patch has no curve item yet. */
static PaintVectorItem *curve_patch_layer_active_curve_item(const CurvePatchSession &session)
{
  const MaterialPaintLayer *row = curve_patch_layer_session_row(session);
  if (row == nullptr || row->curve_patch->vectors == nullptr || !session.doc.has_active_item()) {
    return nullptr;
  }
  PaintVector &pv = *row->curve_patch->vectors;
  const int item_i = session.doc.active_item().source_curve_index;
  if (item_i < 0 || item_i >= pv.items_num ||
      pv.items[item_i].type != PAINT_VECTOR_ITEM_CURVE_PATCH)
  {
    return nullptr;
  }
  return &pv.items[item_i];
}

/** Edit the active curve's persisted params and tag the container, so the modal's poll re-syncs. */
static bool curve_patch_layer_active_curve_params_edit(
    const Object &ob, FunctionRef<void(PaintVectorCurveParams &)> edit)
{
  const CurvePatchSession *session = ED_curve_patch_session_get(ob);
  PaintVectorItem *item = (session != nullptr) ?
                              curve_patch_layer_active_curve_item(*session) :
                              nullptr;
  if (item == nullptr) {
    return false;
  }
  edit(item->curve_patch_params);
  BKE_paint_vector_tag_changed(*curve_patch_layer_session_row(*session)->curve_patch->vectors);
  return true;
}

/** The container item of the 2D session's active patch, or null (row deleted, stale index). */
static PaintVectorItem *curve_patch_layer_session_2d_item(const ImageCurvePatchSession &session)
{
  const MaterialPaintLayer *row = curve_patch_layer_session_row_2d(
      const_cast<ImageCurvePatchSession &>(session));
  if (row == nullptr || row->curve_patch->vectors == nullptr || !session.doc.has_active_item()) {
    return nullptr;
  }
  PaintVector &pv = *row->curve_patch->vectors;
  const int item_i = session.doc.active_item().source_curve_index;
  if (item_i < 0 || item_i >= pv.items_num ||
      pv.items[item_i].type != PAINT_VECTOR_ITEM_CURVE_PATCH)
  {
    return nullptr;
  }
  return &pv.items[item_i];
}

/** The Image Editor's live layer session when it has an active curve, or null. Its look lives in
 * the live document (`doc.patches[].params`, written to the container on commit), not in the
 * container, so the 2D look accessors edit the document directly. */
static ImageCurvePatchSession *curve_patch_layer_session_2d_live()
{
  ImageCurvePatchSession *session = image_curve_patch_session_active_get();
  return (session != nullptr && session->is_layer && session->doc.has_active_item()) ? session :
                                                                                       nullptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Stack row activation
 * \{ */

void paint_stack_gradient_tool_activate(bContext &C, const MaterialPaintLayer *layer)
{
  Object *object = CTX_data_active_object(&C);
  if (object == nullptr || (object->mode & OB_MODE_SCULPT) == 0 ||
      object->runtime->sculpt_session == nullptr)
  {
    return;
  }
  std::string &saved_tool_id = object->runtime->sculpt_session->layers.gradient_saved_tool_id;
  constexpr const char *gradient_tool_id = "builtin.color_gradient";

  if (layer != nullptr && BKE_paint_layers_gradient_supported(*layer)) {
    const std::string current_tool_id = ED_paint_view3d_tool_id_get(&C);
    if (current_tool_id != gradient_tool_id) {
      saved_tool_id = current_tool_id;
    }
    ED_paint_view3d_tool_id_set(&C, gradient_tool_id);
    return;
  }

  /* Only restored while the Gradient tool is still active: a tool the user picked during the
   * Gradient row is a choice that the row change should not overrule. */
  if (!saved_tool_id.empty() && ED_paint_view3d_tool_id_get(&C) == gradient_tool_id) {
    ED_paint_view3d_tool_id_set(&C, saved_tool_id.c_str());
  }
  saved_tool_id.clear();
}

/** Whether a free-standing Curve Patch session (3D or 2D) is alive, so the active row must not
 * change: it has no effect row to keep its curves in, so it writes into the layer that was active
 * when it started and that layer must not change under it. A session owned by an effect row never
 * blocks: the row's container holds its state, so a switch just commits it. */
bool curve_patch_layer_row_switch_blocked(Main &bmain)
{
  bool blocked = false;
  for_each_live_curve_patch_session(bmain, [&](const LiveCurvePatchSession view) {
    if (view.row_material() == nullptr) {
      blocked = true;
      return false;
    }
    return true;
  });
  return blocked;
}

bool curve_patch_layer_row_switch_prepare(bContext &C)
{
  Main *bmain = CTX_data_main(&C);
  if (bmain == nullptr || !curve_patch_layer_row_switch_blocked(*bmain)) {
    return true;
  }
  /* The preference (`Preferences > Editing > Miscellaneous`, `view.paint_layers_commit_patch_on_
   * switch`): switch layers by committing a free-standing patch instead of refusing. */
  if (U.paint_layers_commit_patch_on_switch) {
    /* The user asked for the switch to apply the patch: commit it here, so every caller that only
     * asks "may the row change" finds nothing left to refuse it. */
    curve_patch_layer_commit_live_sessions(C);
    return true;
  }
  BKE_report(CTX_wm_reports(&C),
             RPT_INFO,
             TIP_("Commit the Curve Patch (Enter or Esc) before switching layers"));
  return false;
}

CurvePatchStrokeTarget curve_patch_layer_stroke_target(const MaterialPaintLayer &row,
                                                       const Brush *brush,
                                                       const bool row_session_live)
{
  if (row.source != MA_PAINT_LAYER_SOURCE_CURVE_PATCH || row.curve_patch == nullptr) {
    return CurvePatchStrokeTarget::Allow;
  }
  const Brush *snapshot = row.curve_patch->brush;
  if (brush == nullptr) {
    /* Not a brush stroke: an entry or a shape. It has its own checks (see the session entry). */
    return CurvePatchStrokeTarget::Allow;
  }
  if (snapshot == brush) {
    /* The row's own snapshot is what the stroke paints with: while the row's session runs, its
     * modal owns the stroke. Without one the tool is entered instead, so a stray press can never
     * start a free-standing patch whose curve is not stored in the effect. */
    return row_session_live ? CurvePatchStrokeTarget::Allow : CurvePatchStrokeTarget::Redirect;
  }
  /* A geometry brush (Draw, Clay, ...) with an ordinary stroke method neither writes the effect's
   * maps nor starts a patch: it sculpts, and an active effect row must not take that away. Only a
   * brush that paints (it would write dabs into the maps) or runs the Curve Patch stroke method
   * (it would start a patch outside the effect) is the effect's business. */
  const bool writes_maps = ELEM(brush->sculpt_brush_type,
                                SCULPT_BRUSH_TYPE_PAINT,
                                SCULPT_BRUSH_TYPE_ERASE,
                                SCULPT_BRUSH_TYPE_SMEAR,
                                SCULPT_BRUSH_TYPE_BLUR);
  if (!writes_maps && brush->stroke_method != BRUSH_STROKE_CURVE_PATCH) {
    return CurvePatchStrokeTarget::Allow;
  }
  /* Only a Paint Brush is taken or entered for the effect without being named: a geometry brush
   * that runs the Curve Patch stroke method is refused instead of becoming the effect's snapshot,
   * which is what the effect was changed to stop doing. */
  if (snapshot == nullptr) {
    return BKE_paint_layers_curve_patch_brush_is_assignable(*brush) ?
               CurvePatchStrokeTarget::Adopt :
               CurvePatchStrokeTarget::Refuse;
  }
  /* The effect has a snapshot, but another brush is in hand. A Paint Brush means the user meant
   * this effect: entering its tool puts the snapshot back. Anything else (Mask Smooth, Erase,
   * another row's snapshot, a geometry brush with the Curve Patch stroke method) is refused. */
  return BKE_paint_layers_curve_patch_brush_is_assignable(*brush) ?
             CurvePatchStrokeTarget::Redirect :
             CurvePatchStrokeTarget::Refuse;
}

/** Whether some live session (3D on any object, or the Image Editor's) edits \a row. */
static bool curve_patch_layer_row_session_live(Main &bmain,
                                               const Material &material,
                                               const MaterialPaintLayer &row)
{
  /* The row is addressed by its marker, and the material by its pointer: a session of another
   * material can carry the same marker in a copied file only, so both are checked. */
  bool live = false;
  for_each_live_curve_patch_session(bmain, [&](const LiveCurvePatchSession view) {
    live = view.row_material() == &material && BLI_uuid_equal(view.row_marker(), row.marker);
    return !live;
  });
  return live;
}

/** Carry out the pure decision's actions: #Adopt takes \a brush in hand as the row's snapshot,
 * #Adopt and #Redirect enter the row's tool. The messages depend on what the actions did, so they
 * are reported here; the refusal's reason is reported by
 * #curve_patch_layer_stroke_target_report. */
static CurvePatchStrokeTarget curve_patch_layer_stroke_target_apply(bContext &C,
                                                                    Material &material,
                                                                    MaterialPaintLayer &row,
                                                                    const Brush *brush,
                                                                    ReportList *reports)
{
  Main *bmain = CTX_data_main(&C);
  /* A 2D effect is edited in the Image Editor; the brush in hand is irrelevant there and the
   * stroke-target table would otherwise refuse a brush that cannot stamp a patch. */
  if (curve_patch_layer_row_space(row) == MA_CURVE_PATCH_SPACE_2D) {
    if (curve_patch_layer_activate(C, material, row)) {
      BKE_report(reports, RPT_INFO, TIP_("The 2D Curve Patch opened in the Image Editor"));
      return CurvePatchStrokeTarget::Redirect;
    }
    BKE_report(reports,
               RPT_WARNING,
               TIP_("Open an Image Editor to edit this Curve Patch in its 2D Space"));
    return CurvePatchStrokeTarget::Refuse;
  }
  const bool session_live = bmain != nullptr &&
                            curve_patch_layer_row_session_live(*bmain, material, row);
  const CurvePatchStrokeTarget target = curve_patch_layer_stroke_target(row, brush, session_live);

  switch (target) {
    case CurvePatchStrokeTarget::Allow:
    case CurvePatchStrokeTarget::Refuse:
      return target;
    case CurvePatchStrokeTarget::Adopt:
      /* When the row cannot take the brush, the refusal's reason below covers it: the row's own
       * snapshot is still null, so the answer reads as the plain "no brush" case. */
      if (bmain == nullptr || !BKE_paint_layers_curve_patch_brush_set(material, *bmain, row, *brush))
      {
        return CurvePatchStrokeTarget::Refuse;
      }
      /* The row now has its snapshot: enter the tool so the press that would have painted edits
       * the effect's curves instead. The stroke itself is cancelled. */
      if (curve_patch_layer_activate(C, material, row)) {
        BKE_report(
            reports, RPT_INFO, TIP_("The Curve Patch took the brush in hand: draw on its curves"));
      }
      else {
        BKE_report(reports,
                   RPT_WARNING,
                   TIP_("The Curve Patch brush was taken, but its tool could not be opened"));
      }
      return target;
    case CurvePatchStrokeTarget::Redirect:
      if (curve_patch_layer_activate(C, material, row)) {
        BKE_report(reports,
                   RPT_INFO,
                   TIP_("This Curve Patch effect is drawn with its own brush: its curve tool is "
                        "opened"));
      }
      else {
        BKE_report(reports,
                   RPT_WARNING,
                   TIP_("This Curve Patch effect is drawn with its own brush, and its tool could "
                        "not be opened"));
      }
      return target;
  }
  BLI_assert_unreachable();
  return CurvePatchStrokeTarget::Refuse;
}

/** Report why a stroke may not paint \a row: the #Refuse answer's reason. The other answers report
 * what they did, see #curve_patch_layer_stroke_target_apply. */
static void curve_patch_layer_stroke_target_report(const MaterialPaintLayer &row,
                                                   const CurvePatchStrokeTarget target,
                                                   ReportList *reports)
{
  if (target != CurvePatchStrokeTarget::Refuse) {
    return;
  }
  /* The 2D path reports its own result (or the missing Image Editor) in the apply step. */
  if (curve_patch_layer_row_space(row) == MA_CURVE_PATCH_SPACE_2D) {
    return;
  }
  if (row.curve_patch != nullptr && row.curve_patch->brush == nullptr) {
    BKE_report(reports,
               RPT_WARNING,
               TIP_("This Curve Patch effect has no brush: assign a Paint Brush in the Curve "
                    "Patch panel, then click the effect row"));
  }
  else {
    BKE_report(reports,
               RPT_WARNING,
               TIP_("This brush cannot draw a Curve Patch: click the effect row to enter its "
                    "tool"));
  }
}

CurvePatchStrokeTarget curve_patch_layer_stroke_target_resolve(bContext &C,
                                                               Material &material,
                                                               MaterialPaintLayer &row,
                                                               const Brush *brush,
                                                               ReportList *reports)
{
  const CurvePatchStrokeTarget target = curve_patch_layer_stroke_target_apply(
      C, material, row, brush, reports);
  curve_patch_layer_stroke_target_report(row, target, reports);
  return target;
}

bool paint_stack_row_activate(bContext &C,
                              Material &material,
                              const bUUID &row_marker,
                              const int curve_item_index)
{
  if (!ID_IS_EDITABLE(&material.id) || ID_IS_OVERRIDE_LIBRARY(&material.id)) {
    BKE_report(CTX_wm_reports(&C), RPT_ERROR, "Layered material is not editable");
    return false;
  }
  const MaterialPaintLayer *row = BKE_paint_layers_find(material, row_marker);
  if (row == nullptr) {
    return false;
  }
  /* A live session keeps the active row until it is confirmed or cancelled: a switch would leave
   * the curves going to a layer the user no longer sees as the one being edited. */
  if (!curve_patch_layer_row_switch_prepare(C)) {
    return false;
  }
  /* A Smart Filter definition member cannot become the active row: the members are the chain's
   * saved corrections, and the stack tools aimed at the active row (the brush target, the slots,
   * the projection frame) have nothing to put a stroke into on one -- activating it would point
   * the editor at a row nothing reads. The stack view draws the members inert and refuses the
   * click there; this is the same refusal for the paths that arrive by marker (a remembered
   * active row from before the rows were guarded, a script naming one). The row is already in
   * hand from the existence check above: nothing between runs a lookup that could move it.
   *
   * The member predicate answers "not a layer", which is also every ordinary correction's and mask
   * item's role; those hang on a row and have no owner list, while a definition member sits at the
   * definition's stack root. The owner-list test is what keeps this from refusing to activate every
   * correction in an ordinary material. */
  if (BKE_paint_layers_owner_list(material, *row) != nullptr &&
      BKE_paint_layers_asset_row_is_definition_member(*row))
  {
    BKE_report(CTX_wm_reports(&C),
               RPT_ERROR,
               "A member of a Smart Filter definition cannot be activated");
    return false;
  }
  /* A live Curve Patch session owns the tool while it runs. Switching the active row away from
   * its effect is a deliberate step the user can undo, so the session just commits its curves
   * into the effect and the switch goes on, with no question and no warning. Every live layer
   * session commits, whatever object runs it: a session on a secondary target's owner would
   * otherwise keep that effect's brush in the hand after the switch restored the user's own. */
  curve_patch_layer_commit_live_sessions(C, &row_marker);
  BKE_paint_layers_active_set(material, row_marker);
  if (curve_item_index >= 0) {
    /* The session builds its document from the container's active item, so the clicked curve is
     * the one it starts on. Done after the editability check above, so a refusal left it alone. */
    if (MaterialPaintLayer *effect = BKE_paint_layers_find(material, row_marker);
        effect != nullptr && effect->source == MA_PAINT_LAYER_SOURCE_CURVE_PATCH &&
        effect->curve_patch != nullptr && effect->curve_patch->vectors != nullptr &&
        curve_item_index < effect->curve_patch->vectors->items_num)
    {
      effect->curve_patch->vectors->active_item = curve_item_index;
    }
  }
  /* The Projection Frame tool follows the row first, so a row that is both a volume projection and
   * a Gradient ends up with the Gradient tool, like before the frame tool existed. */
  projection::ED_paint_projection_frame_tool_activate_if_needed(
      &C, material, BKE_paint_layers_find(material, row_marker));
  paint_stack_gradient_tool_activate(C, BKE_paint_layers_find(material, row_marker));
  if (MaterialPaintLayer *active = BKE_paint_layers_find(material, row_marker);
      active != nullptr && active->source == MA_PAINT_LAYER_SOURCE_CURVE_PATCH)
  {
    /* Selecting a Curve Patch effect in Sculpt Mode puts its brush snapshot in the user's hand
     * and opens its curves, like the Gradient tool follows its rows. A refusal is reported by
     * the entry itself and leaves the effect merely selected. */
    curve_patch_layer_enter_if_sculpting(C, material, *active);
  }
  /* Mask editing only makes sense on a Layer row; leaving it for a correction or mask item ends
   * it. */
  if (Scene *scene = CTX_data_scene(&C)) {
    const MaterialPaintLayer *active = BKE_paint_layers_find(material, row_marker);
    if (scene->toolsettings != nullptr && active != nullptr &&
        active->role != MA_PAINT_LAYER_ROLE_LAYER &&
        scene->toolsettings->paint_mode.layer_object_mask_edit != 0)
    {
      scene->toolsettings->paint_mode.layer_object_mask_edit = 0;
      WM_event_add_notifier(&C, NC_SPACE | ND_SPACE_PROPERTIES, nullptr);
    }
  }
  /* The mode is global: a layer with no mask is painted on its content, not left as a stroke
   * into nothing, and a mask correction is only ever worked on in the mask. */
  const MaterialPaintLayer *activated = BKE_paint_layers_active_layer_get(material);
  if (BKE_paint_layers_active_mask_item_get(material) != nullptr) {
    paint_stack_target_mode_set(C, PAINT_LAYER_TARGET_MASK);
  }
  else if (activated != nullptr && BKE_paint_layers_mask_base(*activated) == nullptr) {
    paint_stack_target_mode_set(C, PAINT_LAYER_TARGET_CONTENT);
  }
  /* The mode may have stayed Mask while the row changed: the slots hold the new row's mask map,
   * and a change of mode above already refreshed them once more, which is cheap. */
  paint_stack_texpaint_slots_refresh(C);
  WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
  WM_event_add_notifier(&C, NC_SCENE | ND_TOOLSETTINGS, nullptr);
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Curve list operators (Active Tool panel)
 *
 * The small Add / Delete / Duplicate operators the curve list panel puts beside the template list.
 * The persisted container is a DNA array, so these are C operators, not property edits.
 * \{ */

/** The live Curve Patch session of the active object (its modal owns the tool), or null. */
static CurvePatchSession *curve_patch_layer_live_session(bContext *C)
{
  const Object *ob = CTX_data_active_object(C);
  if (ob == nullptr || ob->runtime == nullptr || ob->runtime->sculpt_session == nullptr) {
    return nullptr;
  }
  return ob->runtime->sculpt_session->curve_patch_session;
}

static void curve_patch_layer_report_other_effect(wmOperator *op)
{
  BKE_report(op->reports, RPT_WARNING, "Finish the live Curve Patch of the other effect first");
}

/** Re-stamp and redraw after the live document changed, with the step on the session's own undo
 * stack (the modal's Tab / swap-axis path does the same). */
static void curve_patch_layer_session_changed(bContext &C, CurvePatchSession &session)
{
  curve_patch_layer_active_item_sync(session);
  curve_patch_undo_push(session);
  curve_patch_edit_status_set(&C, session);
  curve_patch_restore_and_restamp(C, session);
  curve_patch_tag_viewports_redraw_after_edit(C, session);
}

/** Add a curve to the live document, placed under the view like the first one, without leaving the
 * session. The default item is made in the container only to get its geometry placed on the
 * surface; the mirror then rebuilds the container from the document. */
static bool curve_patch_layer_session_curve_add(bContext &C,
                                                CurvePatchSession &session,
                                                MaterialPaintLayerCurvePatch &payload)
{
  const ToolSettings *tool_settings = CTX_data_tool_settings(&C);
  /* The seeds are placed in the document's (owner's) object space; the active object can be a
   * secondary target while this panel action runs. */
  Object &ob = session.owner != nullptr ? *session.owner : *CTX_data_active_object(&C);
  if (tool_settings == nullptr || tool_settings->sculpt == nullptr) {
    return false;
  }
  Sculpt &sd = *tool_settings->sculpt;
  const Brush *brush = BKE_paint_brush_for_read(&sd.paint);
  if (brush == nullptr) {
    return false;
  }
  curve_patch_layer_active_item_follow(session);
  PaintVector &pv = *payload.vectors;
  const int item_i = pv.items_num;
  curve_patch_layer_curve_item_default(pv, payload.brush);
  curve_patch_layer_seeds_place(C, ob, session.view_context, sd.paint, *brush, pv);

  /* The temporary item is dropped by the mirror: the patch gets an id of its own there. */
  const PaintVectorItem &item = pv.items[item_i];
  /* At the top of the list, which is also the top of the stack: the newest curve lies over the
   * others. */
  CurvePatchItem patch;
  patch.control_curve = item.curve_patch_geometry.wrap();
  patch.params = BKE_paint_vector_curve_params_decode(item.curve_patch_params);
  patch.name = item.curve_name;
  session.doc.patches.insert(0, std::move(patch));
  curve_patch_document_active_patch_set(session.doc, 0);
  session.doc.active_point = -1;
  curve_patch_layer_mirror_vector(session);
  curve_patch_layer_session_changed(C, session);
  return true;
}

/** Delete the active curve of the live document; the mirror drops its container item with it. */
static bool curve_patch_layer_session_curve_remove(bContext &C, CurvePatchSession &session)
{
  if (session.doc.patches.size() < 2) {
    BKE_report(CTX_wm_reports(&C), RPT_WARNING, "A Curve Patch needs at least one curve");
    return false;
  }
  curve_patch_layer_active_item_follow(session);
  const int patch_i = session.doc.active_patch;
  session.doc.patches.remove(patch_i);
  curve_patch_document_active_patch_set(
      session.doc, std::min(patch_i, int(session.doc.patches.size()) - 1));
  session.doc.active_point = -1;
  curve_patch_layer_mirror_vector(session);
  curve_patch_layer_session_changed(C, session);
  return true;
}

/** Copy the active curve of the live document, in place: it becomes the active one, ready to be
 * moved with the usual handle gestures. */
static bool curve_patch_layer_session_curve_duplicate(bContext &C, CurvePatchSession &session)
{
  curve_patch_layer_active_item_follow(session);
  if (!session.doc.has_active_item()) {
    return false;
  }
  CurvePatchItem copy;
  {
    const CurvePatchItem &source = session.doc.active_item();
    copy.control_curve = source.control_curve;
    copy.params = source.params;
    copy.name = source.name + " Copy";
  }
  /* A copy lies over the curve it was made from, so it goes to the top of the stack. */
  session.doc.patches.insert(0, std::move(copy));
  curve_patch_document_active_patch_set(session.doc, 0);
  session.doc.active_point = -1;
  curve_patch_layer_mirror_vector(session);
  curve_patch_layer_session_changed(C, session);
  return true;
}

/** The active row's payload when the active row of the context material is a Curve Patch row. */
static MaterialPaintLayerCurvePatch *curve_patch_layer_active_payload(bContext *C,
                                                                     MaterialPaintLayer **r_row)
{
  const Object *ob = CTX_data_active_object(C);
  Material *ma = (ob != nullptr) ? BKE_object_material_get(const_cast<Object *>(ob),
                                                          ob->actcol) :
                                   nullptr;
  if (ma == nullptr) {
    return nullptr;
  }
  MaterialPaintLayer *row = BKE_paint_layers_active_layer_get(*ma);
  if (row == nullptr || row->source != MA_PAINT_LAYER_SOURCE_CURVE_PATCH ||
      row->curve_patch == nullptr || row->curve_patch->vectors == nullptr)
  {
    return nullptr;
  }
  if (r_row != nullptr) {
    *r_row = row;
  }
  return row->curve_patch;
}

/** The payload's active curve item, or null when the active item is a Shape one. */
static PaintVectorItem *curve_patch_layer_active_item(MaterialPaintLayerCurvePatch &payload)
{
  PaintVector &pv = *payload.vectors;
  if (pv.active_item < 0 || pv.active_item >= pv.items_num) {
    return nullptr;
  }
  PaintVectorItem &item = pv.items[pv.active_item];
  return item.type == PAINT_VECTOR_ITEM_CURVE_PATCH ? &item : nullptr;
}

/** Bring the curve item at \a item_i to the top of the curve list (the first curve item), the
 * others keeping their relative order, and return its new index. The list's top is the top of the
 * stack, which is where a new curve belongs. */
static int curve_patch_layer_item_move_to_top(PaintVector &pv, int item_i)
{
  while (true) {
    int previous = -1;
    for (int i = item_i - 1; i >= 0; i--) {
      if (pv.items[i].type == PAINT_VECTOR_ITEM_CURVE_PATCH) {
        previous = i;
        break;
      }
    }
    if (previous < 0 || !BKE_paint_vector_item_swap(pv, item_i, previous)) {
      return item_i;
    }
    item_i = previous;
  }
}

static wmOperatorStatus paint_curve_patch_layer_curve_add_exec(bContext *C, wmOperator *op)
{
  MaterialPaintLayer *row = nullptr;
  MaterialPaintLayerCurvePatch *payload = curve_patch_layer_active_payload(C, &row);
  if (payload == nullptr || payload->brush == nullptr) {
    return OPERATOR_CANCELLED;
  }
  if (CurvePatchSession *session = curve_patch_layer_live_session(C)) {
    if (curve_patch_layer_session_row(*session) != row) {
      curve_patch_layer_report_other_effect(op);
      return OPERATOR_CANCELLED;
    }
    return curve_patch_layer_session_curve_add(*C, *session, *payload) ? OPERATOR_FINISHED :
                                                                         OPERATOR_CANCELLED;
  }
  curve_patch_layer_curve_item_default(*payload->vectors, payload->brush);
  payload->vectors->active_item = curve_patch_layer_item_move_to_top(
      *payload->vectors, payload->vectors->items_num - 1);
  BKE_paint_vector_tag_changed(*payload->vectors);
  BKE_paint_layers_bake_request(*row);
  ED_undo_memfile_push(C, "Curve Patch Layer Curve Add");
  WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, nullptr);
  return OPERATOR_FINISHED;
}

void PAINT_OT_curve_patch_layer_curve_add(wmOperatorType *ot)
{
  ot->name = "Add Curve";
  ot->idname = "PAINT_OT_curve_patch_layer_curve_add";
  ot->description = "Add a control curve to the active Curve Patch effect";
  ot->exec = paint_curve_patch_layer_curve_add_exec;
  ot->poll = ED_operator_object_active_editable_mesh;
  ot->flag = OPTYPE_REGISTER;
}

static wmOperatorStatus paint_curve_patch_layer_curve_remove_exec(bContext *C, wmOperator *op)
{
  MaterialPaintLayer *row = nullptr;
  MaterialPaintLayerCurvePatch *payload = curve_patch_layer_active_payload(C, &row);
  if (payload == nullptr || curve_patch_layer_active_item(*payload) == nullptr) {
    return OPERATOR_CANCELLED;
  }
  if (CurvePatchSession *session = curve_patch_layer_live_session(C)) {
    if (curve_patch_layer_session_row(*session) != row) {
      curve_patch_layer_report_other_effect(op);
      return OPERATOR_CANCELLED;
    }
    return curve_patch_layer_session_curve_remove(*C, *session) ? OPERATOR_FINISHED :
                                                                  OPERATOR_CANCELLED;
  }
  PaintVector &pv = *payload->vectors;
  BKE_paint_vector_item_remove(pv, pv.active_item);
  BKE_paint_vector_tag_changed(pv);
  BKE_paint_layers_bake_request(*row);
  ED_undo_memfile_push(C, "Curve Patch Layer Curve Remove");
  WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, nullptr);
  return OPERATOR_FINISHED;
}

void PAINT_OT_curve_patch_layer_curve_remove(wmOperatorType *ot)
{
  ot->name = "Delete Curve";
  ot->idname = "PAINT_OT_curve_patch_layer_curve_remove";
  ot->description = "Delete the active curve of the active Curve Patch effect";
  ot->exec = paint_curve_patch_layer_curve_remove_exec;
  ot->poll = ED_operator_object_active_editable_mesh;
  ot->flag = OPTYPE_REGISTER;
}

static wmOperatorStatus paint_curve_patch_layer_curve_duplicate_exec(bContext *C, wmOperator *op)
{
  MaterialPaintLayer *row = nullptr;
  MaterialPaintLayerCurvePatch *payload = curve_patch_layer_active_payload(C, &row);
  PaintVectorItem *source = (payload != nullptr) ? curve_patch_layer_active_item(*payload) :
                                                   nullptr;
  if (source == nullptr || !BKE_paint_vector_item_curve_geometry_is_valid(*source)) {
    return OPERATOR_CANCELLED;
  }
  if (CurvePatchSession *session = curve_patch_layer_live_session(C)) {
    if (curve_patch_layer_session_row(*session) != row) {
      curve_patch_layer_report_other_effect(op);
      return OPERATOR_CANCELLED;
    }
    return curve_patch_layer_session_curve_duplicate(*C, *session) ? OPERATOR_FINISHED :
                                                                     OPERATOR_CANCELLED;
  }
  PaintVector &pv = *payload->vectors;
  /* Copied before the add: the item array may be reallocated by it. */
  const bke::CurvesGeometry geometry = source->curve_patch_geometry.wrap();
  const PaintVectorCurveParams params = source->curve_patch_params;
  char name[64];
  SNPRINTF(name, "%s Copy", source->curve_name);
  const int curve_id = curve_patch_layer_next_curve_id(pv);
  PaintVectorItem &copy = BKE_paint_vector_item_add(pv);
  copy.type = PAINT_VECTOR_ITEM_CURVE_PATCH;
  copy.curve_id = curve_id;
  copy.curve_patch_geometry.wrap() = geometry;
  copy.curve_patch_params = params;
  BLI_strncpy(copy.curve_name, name, sizeof(copy.curve_name));
  pv.active_item = curve_patch_layer_item_move_to_top(pv, pv.items_num - 1);
  BKE_paint_vector_tag_changed(pv);
  BKE_paint_layers_bake_request(*row);
  ED_undo_memfile_push(C, "Curve Patch Layer Curve Duplicate");
  WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, nullptr);
  return OPERATOR_FINISHED;
}

void PAINT_OT_curve_patch_layer_curve_duplicate(wmOperatorType *ot)
{
  ot->name = "Duplicate Curve";
  ot->idname = "PAINT_OT_curve_patch_layer_curve_duplicate";
  ot->description = "Duplicate the active curve of the active Curve Patch effect";
  ot->exec = paint_curve_patch_layer_curve_duplicate_exec;
  ot->poll = ED_operator_object_active_editable_mesh;
  ot->flag = OPTYPE_REGISTER;
}

static const EnumPropertyItem curve_move_direction_items[] = {
    {0, "UP", 0, "Up", "Move the active curve up the list"},
    {1, "DOWN", 0, "Down", "Move the active curve down the list"},
    {0, nullptr, 0, nullptr, nullptr},
};

/** Move the active curve of the live document one place up or down the list. The document's order
 * is the stacking order; the mirror puts the container, which the list shows, in the same order. */
static bool curve_patch_layer_session_curve_move(bContext &C,
                                                 CurvePatchSession &session,
                                                 const bool move_down)
{
  curve_patch_layer_active_item_follow(session);
  if (!session.doc.has_active_item()) {
    return false;
  }
  const int patch_a = session.doc.active_patch;
  const int patch_b = patch_a + (move_down ? 1 : -1);
  if (patch_b < 0 || patch_b >= int(session.doc.patches.size())) {
    return false;
  }
  std::swap(session.doc.patches[patch_a], session.doc.patches[patch_b]);
  curve_patch_document_active_patch_set(session.doc, patch_b);
  session.doc.active_point = -1;
  curve_patch_layer_mirror_vector(session);
  curve_patch_layer_session_changed(C, session);
  return true;
}

static wmOperatorStatus paint_curve_patch_layer_curve_move_exec(bContext *C, wmOperator *op)
{
  MaterialPaintLayer *row = nullptr;
  MaterialPaintLayerCurvePatch *payload = curve_patch_layer_active_payload(C, &row);
  PaintVectorItem *active = (payload != nullptr) ? curve_patch_layer_active_item(*payload) : nullptr;
  if (active == nullptr) {
    return OPERATOR_CANCELLED;
  }
  PaintVector &pv = *payload->vectors;
  const bool move_down = RNA_enum_get(op->ptr, "direction") == 1;
  if (CurvePatchSession *session = curve_patch_layer_live_session(C)) {
    if (curve_patch_layer_session_row(*session) != row) {
      curve_patch_layer_report_other_effect(op);
      return OPERATOR_CANCELLED;
    }
    return curve_patch_layer_session_curve_move(*C, *session, move_down) ? OPERATOR_FINISHED :
                                                                           OPERATOR_CANCELLED;
  }
  int target = -1;
  if (move_down) {
    for (int i = pv.active_item + 1; i < pv.items_num; i++) {
      if (pv.items[i].type == PAINT_VECTOR_ITEM_CURVE_PATCH) {
        target = i;
        break;
      }
    }
  }
  else {
    for (int i = pv.active_item - 1; i >= 0; i--) {
      if (pv.items[i].type == PAINT_VECTOR_ITEM_CURVE_PATCH) {
        target = i;
        break;
      }
    }
  }
  if (target < 0 || !BKE_paint_vector_item_swap(pv, pv.active_item, target)) {
    return OPERATOR_CANCELLED;
  }
  BKE_paint_vector_tag_changed(pv);
  BKE_paint_layers_bake_request(*row);
  ED_undo_memfile_push(C, "Curve Patch Layer Curve Move");
  WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, nullptr);
  return OPERATOR_FINISHED;
}

void PAINT_OT_curve_patch_layer_curve_move(wmOperatorType *ot)
{
  ot->name = "Move Curve";
  ot->idname = "PAINT_OT_curve_patch_layer_curve_move";
  ot->description = "Move the active curve up or down in the active Curve Patch effect";
  ot->exec = paint_curve_patch_layer_curve_move_exec;
  ot->poll = ED_operator_object_active_editable_mesh;
  ot->flag = OPTYPE_REGISTER;

  ot->prop = RNA_def_enum(
      ot->srna, "direction", curve_move_direction_items, 0, "Direction", "Where to move the curve");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tool entry and brush assignment
 *
 * The tool button of the Active Tool panel (and any other caller with nothing but a context): the
 * active row of the object's active material is the one the tool acts on. The brush is the row's
 * private snapshot, so replacing it duplicates the chosen brush; the layer never shares a brush
 * with the user's own asset library.
 * \{ */

struct CurvePatchLayerTarget {
  Material *material = nullptr;
  MaterialPaintLayer *row = nullptr;
};

/** The active Curve Patch row of the context object's active material, or an empty target. */
static CurvePatchLayerTarget curve_patch_layer_target_get(bContext *C)
{
  CurvePatchLayerTarget target;
  const Object *ob = CTX_data_active_object(C);
  if (ob == nullptr) {
    return target;
  }
  target.material = BKE_object_material_get(const_cast<Object *>(ob), ob->actcol);
  if (target.material == nullptr) {
    return target;
  }
  MaterialPaintLayer *row = BKE_paint_layers_active_layer_get(*target.material);
  if (row == nullptr || row->source != MA_PAINT_LAYER_SOURCE_CURVE_PATCH ||
      row->curve_patch == nullptr)
  {
    return target;
  }
  target.row = row;
  return target;
}

static bool curve_patch_layer_edit_poll(bContext *C)
{
  /* Not gated on Sculpt Mode: the activation enters it when needed, so the button works from the
   * panel in Object Mode too. */
  return ED_operator_object_active_editable_mesh(C) &&
         curve_patch_layer_target_get(C).row != nullptr;
}

static wmOperatorStatus curve_patch_layer_edit_exec(bContext *C, wmOperator *op)
{
  const CurvePatchLayerTarget target = curve_patch_layer_target_get(C);
  if (target.row == nullptr) {
    BKE_report(op->reports, RPT_WARNING, "Activate a Curve Patch effect first");
    return OPERATOR_CANCELLED;
  }
  if (!curve_patch_layer_activate(*C, *target.material, *target.row)) {
    return OPERATOR_CANCELLED;
  }
  return OPERATOR_FINISHED;
}

void PAINT_OT_curve_patch_layer_edit(wmOperatorType *ot)
{
  ot->name = "Edit Curve Patch Effect";
  ot->idname = "PAINT_OT_curve_patch_layer_edit";
  ot->description =
      "Enter the active Curve Patch effect's tool: its brush snapshot becomes the active brush and "
      "its curves open in the modal editor";
  ot->exec = curve_patch_layer_edit_exec;
  ot->poll = curve_patch_layer_edit_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static bool curve_patch_layer_brush_assign_poll(bContext *C)
{
  return ED_operator_object_active_editable_mesh(C) &&
         curve_patch_layer_target_get(C).row != nullptr;
}

static wmOperatorStatus curve_patch_layer_brush_assign_exec(bContext *C, wmOperator *op)
{
  const CurvePatchLayerTarget target = curve_patch_layer_target_get(C);
  if (target.row == nullptr) {
    BKE_report(op->reports, RPT_WARNING, "Activate a Curve Patch effect first");
    return OPERATOR_CANCELLED;
  }
  /* An operator cannot carry an ID pointer, so the brush travels by name. */
  char brush_name[MAX_ID_NAME - 2];
  RNA_string_get(op->ptr, "brush_name", brush_name);
  Brush *source = nullptr;
  if (brush_name[0] != '\0') {
    for (Brush &brush : CTX_data_main(C)->brushes) {
      if (STREQ(brush.id.name + 2, brush_name)) {
        source = &brush;
        break;
      }
    }
  }
  if (source == nullptr) {
    /* No argument: the brush the user currently holds is the obvious source. */
    const Scene *scene = CTX_data_scene(C);
    if (scene != nullptr && scene->toolsettings != nullptr &&
        scene->toolsettings->sculpt != nullptr)
    {
      source = BKE_paint_brush(&scene->toolsettings->sculpt->paint);
    }
  }
  if (source == nullptr) {
    BKE_report(op->reports, RPT_WARNING, "No active brush to assign");
    return OPERATOR_CANCELLED;
  }
  /* A linked brush is a valid source: the snapshot is a local copy, so it stays editable even
   * when the source came from an asset library. The assign entry validates the brush, ends a live
   * session, renews the curves and pushes the one undo step (hence no OPTYPE_UNDO below). */
  if (!ED_curve_patch_layer_brush_assign(
          *C, *target.material, *target.row->curve_patch, *source, op->reports))
  {
    return OPERATOR_CANCELLED;
  }
  return OPERATOR_FINISHED;
}

void PAINT_OT_curve_patch_layer_brush_assign(wmOperatorType *ot)
{
  ot->name = "Assign Curve Patch Brush";
  ot->idname = "PAINT_OT_curve_patch_layer_brush_assign";
  ot->description =
      "Give the active Curve Patch effect a new private brush snapshot (the active brush when no "
      "brush is given)";
  ot->exec = curve_patch_layer_brush_assign_exec;
  ot->poll = curve_patch_layer_brush_assign_poll;
  ot->flag = OPTYPE_REGISTER;

  RNA_def_string(ot->srna,
                 "brush_name",
                 nullptr,
                 MAX_ID_NAME - 2,
                 "Brush",
                 "Name of the brush to duplicate as the effect's tool state; empty uses the active "
                 "brush");
}

/** \} */

}  // namespace blender::ed::sculpt_paint

/* The public entry points are declared in `ED_curve_patch.hh` under `blender::`, so they are
 * defined here, in one block outside the sculpt_paint namespace, to link with their callers (RNA,
 * the drop target, windowmanager). */
namespace blender {

/* -------------------------------------------------------------------- */
/** \name Public Curve Patch layer entry points
 * \{ */

bool ED_curve_patch_layer_brush_assign(bContext &C,
                                       Material &material,
                                       MaterialPaintLayerCurvePatch &payload,
                                       const Brush &source,
                                       ReportList *reports)
{
  using namespace ed::sculpt_paint;
  Main *bmain = CTX_data_main(&C);
  if (bmain == nullptr) {
    return false;
  }
  if (!BKE_paint_layers_curve_patch_brush_is_assignable(source)) {
    BKE_report(reports, RPT_WARNING, BKE_paint_layers_curve_patch_brush_refusal_message());
    return false;
  }
  MaterialPaintLayer *row = BKE_paint_layers_curve_patch_row_find(material, payload);
  if (row == nullptr) {
    BKE_report(reports, RPT_ERROR, "Could not assign that brush to the Curve Patch");
    return false;
  }
  const bUUID row_marker = row->marker;
  const bool was_live = curve_patch_layer_row_is_live(*bmain, material, row_marker);
  /* The snapshot is the active brush while a session runs, so it commits before the old snapshot
   * is freed. */
  curve_patch_layer_commit_live_sessions(C);

  if (!BKE_paint_layers_curve_patch_brush_set(material, *bmain, *row, source)) {
    BKE_report(reports, RPT_ERROR, "Could not assign that brush to the Curve Patch");
    return false;
  }
  /* The brush set already invalidated the bake and tagged the material as edited. */
  curve_patch_layer_curves_reset_from_brush(payload);
  WM_main_add_notifier(NC_BRUSH | NA_EDITED, nullptr);
  WM_main_add_notifier(NC_MATERIAL | ND_SHADING, &material.id);

  if (was_live) {
    /* The same entry a click on the row takes: the new snapshot comes into the hand. */
    paint_stack_row_activate(C, material, row_marker);
  }
  ED_undo_memfile_push(&C, "Assign Curve Patch Brush");
  return true;
}

bool ED_curve_patch_radial_control_retarget(bContext &C, PointerRNA &r_ptr, PropertyRNA *&r_prop)
{
  Object *ob = CTX_data_active_object(&C);
  if (ob == nullptr || !ED_curve_patch_layer_look_available(*ob)) {
    return false;
  }
  /* Keyed on the property the keymap asked for, so it works for any binding of the ring and
   * leaves every other ring (the texture angle) untouched. The ring then reads `ui_range`, unit
   * and value from the retargeted property: `curve_width` has the list slider's 0..100 soft range,
   * and Esc restores the old value through the same setter. */
  const char *prop_id = RNA_property_identifier(r_prop);
  const char *retarget_id = STREQ(prop_id, "size")     ? "curve_width" :
                            STREQ(prop_id, "strength") ? "curve_opacity" :
                                                         nullptr;
  if (retarget_id == nullptr) {
    return false;
  }
  PointerRNA look_ptr = RNA_pointer_create_discrete(&ob->id, RNA_CurvePatchLayerLook, ob);
  PropertyRNA *retarget_prop = RNA_struct_find_property(&look_ptr, retarget_id);
  if (retarget_prop == nullptr) {
    return false;
  }
  r_ptr = look_ptr;
  r_prop = retarget_prop;
  return true;
}

bool ED_curve_patch_layer_look_available(const Object &ob)
{
  const ed::sculpt_paint::CurvePatchSession *session = ED_curve_patch_session_get(ob);
  if (session != nullptr && session->is_layer_effect()) {
    return true;
  }
  return ed::sculpt_paint::curve_patch_layer_session_2d_live() != nullptr;
}

void ED_curve_patch_layer_look_restamp(bContext &C, Object &ob)
{
  const ed::sculpt_paint::CurvePatchSession *session = ED_curve_patch_session_get(ob);
  if (session != nullptr && session->is_layer_effect()) {
    ED_curve_patch_session_restamp(C, ob);
    return;
  }
  ed::sculpt_paint::ImageCurvePatchSession *session_2d =
      ed::sculpt_paint::curve_patch_layer_session_2d_live();
  if (session_2d == nullptr) {
    return;
  }
  /* The rebuild reads the brush and the canvas through the Image Editor's context; the edit may
   * come from the 3D Viewport's panel or a ring in either editor. */
  ScrArea *image_area = BKE_screen_find_big_area(CTX_wm_screen(&C), SPACE_IMAGE, 0);
  ARegion *image_region = image_area ? BKE_area_find_region_type(image_area, RGN_TYPE_WINDOW) :
                                       nullptr;
  ed::sculpt_paint::curve_patch_layer_with_area_context(C, image_area, image_region, [&]() {
    ed::sculpt_paint::image_curve_patch_session_restore_and_restamp(&C, session_2d);
  });
  WM_event_add_notifier(&C, NC_SPACE | ND_SPACE_IMAGE, nullptr);
}

int ED_curve_patch_layer_look_active_point(const Object &ob)
{
  if (const ed::sculpt_paint::CurvePatchSession *session = ED_curve_patch_session_get(ob)) {
    return ED_curve_patch_session_active_point(session);
  }
  if (const ed::sculpt_paint::ImageCurvePatchSession *session_2d =
          ed::sculpt_paint::curve_patch_layer_session_2d_live())
  {
    const int point = session_2d->doc.active_point;
    return (point >= 0 && point < session_2d->curve().points_num()) ? point : -1;
  }
  return -1;
}

float ED_curve_patch_layer_look_point_opacity(const Object &ob)
{
  if (const ed::sculpt_paint::CurvePatchSession *session = ED_curve_patch_session_get(ob)) {
    return ED_curve_patch_session_active_point_opacity(session);
  }
  const int point = ED_curve_patch_layer_look_active_point(ob);
  const ed::sculpt_paint::ImageCurvePatchSession *session_2d =
      ed::sculpt_paint::curve_patch_layer_session_2d_live();
  return (point >= 0 && session_2d != nullptr) ?
             paintcurve_geom_opacity_get(session_2d->curve(), point) :
             1.0f;
}

bool ED_curve_patch_layer_look_point_opacity_set(Object &ob, float opacity)
{
  if (ED_curve_patch_session_get(ob) != nullptr) {
    return ED_curve_patch_session_active_point_opacity_set(ob, opacity);
  }
  const int point = ED_curve_patch_layer_look_active_point(ob);
  ed::sculpt_paint::ImageCurvePatchSession *session_2d =
      ed::sculpt_paint::curve_patch_layer_session_2d_live();
  if (point < 0 || session_2d == nullptr) {
    return false;
  }
  paintcurve_geom_opacity_set(session_2d->curve(), point, opacity);
  return true;
}

float ED_curve_patch_layer_look_point_falloff(const Object &ob)
{
  if (const ed::sculpt_paint::CurvePatchSession *session = ED_curve_patch_session_get(ob)) {
    return ED_curve_patch_session_active_point_opacity_falloff(session);
  }
  const int point = ED_curve_patch_layer_look_active_point(ob);
  const ed::sculpt_paint::ImageCurvePatchSession *session_2d =
      ed::sculpt_paint::curve_patch_layer_session_2d_live();
  return (point >= 0 && session_2d != nullptr) ?
             paintcurve_geom_opacity_falloff_get(session_2d->curve(), point) :
             1.0f;
}

bool ED_curve_patch_layer_look_point_falloff_set(Object &ob, float falloff)
{
  if (ED_curve_patch_session_get(ob) != nullptr) {
    return ED_curve_patch_session_active_point_opacity_falloff_set(ob, falloff);
  }
  const int point = ED_curve_patch_layer_look_active_point(ob);
  ed::sculpt_paint::ImageCurvePatchSession *session_2d =
      ed::sculpt_paint::curve_patch_layer_session_2d_live();
  if (point < 0 || session_2d == nullptr) {
    return false;
  }
  paintcurve_geom_opacity_falloff_set(session_2d->curve(), point, falloff);
  return true;
}

float ED_curve_patch_session_curve_width(const Object &ob)
{
  const ed::sculpt_paint::CurvePatchSession *session = ED_curve_patch_session_get(ob);
  if (session == nullptr) {
    if (const ed::sculpt_paint::ImageCurvePatchSession *session_2d =
            ed::sculpt_paint::curve_patch_layer_session_2d_live())
    {
      return BKE_paint_vector_curve_width_get(
          BKE_paint_vector_curve_params_encode(session_2d->doc.active_item().params));
    }
    return 0.0f;
  }
  const PaintVectorItem *item = ed::sculpt_paint::curve_patch_layer_active_curve_item(*session);
  return item != nullptr ? BKE_paint_vector_curve_width_get(item->curve_patch_params) : 0.0f;
}

bool ED_curve_patch_session_curve_width_set(const Object &ob, float width)
{
  if (ED_curve_patch_session_get(ob) == nullptr) {
    ed::sculpt_paint::ImageCurvePatchSession *session_2d =
        ed::sculpt_paint::curve_patch_layer_session_2d_live();
    if (session_2d == nullptr) {
      return false;
    }
    bke::CurvePatchParams &params = session_2d->doc.active_item().params;
    PaintVectorCurveParams stored = BKE_paint_vector_curve_params_encode(params);
    BKE_paint_vector_curve_width_set(stored, width);
    params = BKE_paint_vector_curve_params_decode(stored);
    /* The container item too: the panel reads it, and the session follows it. */
    if (PaintVectorItem *item = ed::sculpt_paint::curve_patch_layer_session_2d_item(*session_2d)) {
      BKE_paint_vector_curve_width_set(item->curve_patch_params, width);
    }
    return true;
  }
  return ed::sculpt_paint::curve_patch_layer_active_curve_params_edit(
      ob, [&](PaintVectorCurveParams &params) { BKE_paint_vector_curve_width_set(params, width); });
}

float ED_curve_patch_session_curve_opacity(const Object &ob)
{
  const ed::sculpt_paint::CurvePatchSession *session = ED_curve_patch_session_get(ob);
  if (session == nullptr) {
    if (const ed::sculpt_paint::ImageCurvePatchSession *session_2d =
            ed::sculpt_paint::curve_patch_layer_session_2d_live())
    {
      return session_2d->doc.active_item().params.opacity;
    }
    return 0.0f;
  }
  const PaintVectorItem *item = ed::sculpt_paint::curve_patch_layer_active_curve_item(*session);
  return item != nullptr ? item->curve_patch_params.opacity : 0.0f;
}

bool ED_curve_patch_session_curve_opacity_set(const Object &ob, float opacity)
{
  if (ED_curve_patch_session_get(ob) == nullptr) {
    ed::sculpt_paint::ImageCurvePatchSession *session_2d =
        ed::sculpt_paint::curve_patch_layer_session_2d_live();
    if (session_2d == nullptr) {
      return false;
    }
    const float value = std::clamp(opacity, 0.0f, 1.0f);
    session_2d->doc.active_item().params.opacity = value;
    if (PaintVectorItem *item = ed::sculpt_paint::curve_patch_layer_session_2d_item(*session_2d)) {
      item->curve_patch_params.opacity = value;
    }
    return true;
  }
  return ed::sculpt_paint::curve_patch_layer_active_curve_params_edit(
      ob, [&](PaintVectorCurveParams &params) {
        params.opacity = std::clamp(opacity, 0.0f, 1.0f);
      });
}

bool ED_curve_patch_session_edits_row(const Main &bmain,
                                      const Material &ma,
                                      const bUUID &row_marker)
{
  return ed::sculpt_paint::curve_patch_layer_session_edits_row(bmain, ma, row_marker);
}

bool ED_curve_patch_layer_space_switch(bContext &C, Material &ma, MaterialPaintLayer &row)
{
  return ed::sculpt_paint::curve_patch_layer_space_switch(C, ma, row);
}

/** \} */

}  // namespace blender
