/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Implementation of the Stack Layers <-> Curve Patch bridge; see #paint_curve_patch_layer.hh.
 */

#include "paint_curve_patch_layer.hh"

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>

#include "MEM_guardedalloc.h"

#include "BLI_index_range.hh"
#include "BLI_hash.h"
#include "BLI_listbase_iterator.hh"
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
#include "DNA_view3d_types.h"

#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_curve_patch.hh"
#include "BKE_curves.hh"
#include "BKE_customdata.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_generate.hh"
#include "BKE_paint_layers_target.hh"
#include "BKE_paint_vector.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"

#include "ED_curve_patch.hh"
#include "ED_object.hh"
#include "ED_screen.hh"
#include "ED_undo.hh"
#include "ED_view3d.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"

#include "mesh/sculpt_intern.hh"
#include "paint_curve_geometry.hh"
#include "paint_curve_patch_document.hh"
#include "paint_curve_patch_edit_intern.hh"
#include "paint_curve_screen.hh"
#include "paint_curve_patch_effect.hh"
#include "paint_curve_patch_session.hh"

namespace blender::ed::sculpt_paint {


/* -------------------------------------------------------------------- */
/** \name Brush restore
 * \{ */

PrevBrushRestore::PrevBrushRestore(Scene &scene, Paint &paint)
    : scene_(&scene), paint_(&paint), prev_brush_(BKE_paint_brush(&paint))
{
}

PrevBrushRestore::~PrevBrushRestore()
{
  if (this->released_ || this->scene_ == nullptr || this->paint_ == nullptr) {
    return;
  }
  /* The same user-facing switch the entry used, so the outgoing PBR state (here: the snapshot's)
   * is snapshotted before the previous brush comes back with its own. */
  BKE_paint_brush_set_synced(*this->scene_, *this->paint_, this->prev_brush_);
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

void curve_patch_document_from_vector(const PaintVector &pv, CurvePatchDocument &r_doc)
{
  /* Field-wise reset rather than assignment: the document holds an `Array<MTex>` texture binding,
   * which is neither copy- nor move-assignable (DNA types delete both). */
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

void curve_patch_document_to_vector(const CurvePatchDocument &doc, PaintVector &r_pv)
{
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
    pvi.curve_patch_geometry.wrap() = item.control_curve;
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
    pvi.curve_patch_geometry.wrap() = item.control_curve;
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

/** Append one default curve item to \a pv: a two-point segment sized from \a brush, with the
 * frozen parameters the brush would stamp with. Used by the row seed and the curve-add operator. */
static PaintVectorItem &curve_patch_layer_curve_item_default(PaintVector &pv, const Brush *brush)
{
  /* The brush's Unprojected Size is the DIAMETER in Blender units; the patch works with the
   * radius (the same value a stroke freezes as #StrokeCache::initial_radius). */
  const float diameter = (brush != nullptr) ? brush->unprojected_size : 0.0f;
  const float radius = diameter > 0.0f ? diameter * 0.5f : 0.1f;
  /* The snapshot's own size slider, not the unified setting: at Add time there is no Paint the
   * unified state could be read from, and the frozen ratio belongs to the snapshot anyway. */
  const int brush_size = (brush != nullptr && brush->size > 0) ? brush->size : 1;
  const float radius_per_size = radius / float(brush_size);

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
                                           BLI_hash_string(brush->id.name + 2),
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
  const uint16_t channel_set = BKE_paint_layers_channel_set_mask_get(ma);
  int created_num = 0;
  auto ensure_channel = [&](const int channel) -> bool {
    if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM ||
        (channel_set & (uint16_t(1) << channel)) == 0)
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

/** Whether the object the session will run on has an active UV map: the renderer projects the
 * object-space curves through it, so without one the tool has no canvas to write. */
static bool curve_patch_layer_object_has_uv(const Object &ob)
{
  const Mesh *mesh = BKE_object_get_original_mesh(&ob);
  /* UV maps are attributes of the mesh, not CustomData layers, so the legacy layer lookup always
   * came back empty. */
  return mesh != nullptr && !mesh->uv_map_names().is_empty();
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
      const bke::CurvePatchParams previous = BKE_paint_vector_curve_params_decode(
          item.curve_patch_params);
      const float3 plane_normal = math::normalize(normal[0] + normal[1]);
      const bke::CurvePatchParams params = curve_patch_params_from_brush(
          brush,
          radius,
          brush_size > 0 ? radius / float(brush_size) : 0.0f,
          plane_normal,
          previous.stamp_seed,
          brush.curve_patch.swap_axis != 0);
      item.curve_patch_params = BKE_paint_vector_curve_params_encode(params);
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

/** The row a live layer session edits, or null when it was deleted (or is not a Curve Patch row). */
static MaterialPaintLayer *curve_patch_layer_session_row(const CurvePatchSession &session)
{
  if (session.layer_material == nullptr || BLI_uuid_is_nil(session.layer_row_marker)) {
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
  if (!curve_patch_layer_object_has_uv(*ob)) {
    BKE_report(reports, RPT_WARNING, "A Curve Patch effect needs a UV map on the object to project its curves onto");
    return false;
  }
  /* The entry may come from the Outliner or a panel; the session needs a real View3D context for
   * its re-stamps and symmetry passes. */
  const std::optional<ViewContext> view_context = curve_patch_layer_view3d_context(C, *ob);
  if (!view_context) {
    BKE_report(reports, RPT_WARNING, "Open a 3D Viewport to edit a Curve Patch effect");
    return false;
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
   * way (older rows may carry a brush from before Curve Patch was its stroke method). */
  Brush *brush = row.curve_patch->brush;
  if (brush == nullptr) {
    BKE_report(reports, RPT_WARNING, "This Curve Patch effect has no brush; assign one from the Curve Patch panel");
    return false;
  }
  if (!bke::brush::supports_curve_patch(*brush)) {
    BKE_report(reports, RPT_WARNING, "This brush type cannot stamp a Curve Patch; assign another brush to the effect");
    return false;
  }
  if (ID_IS_LINKED(&brush->id)) {
    BKE_report(reports, RPT_WARNING, "This Curve Patch effect's brush is linked; assign a local brush first");
    return false;
  }
  if (brush->stroke_method != BRUSH_STROKE_CURVE_PATCH) {
    brush->stroke_method = BRUSH_STROKE_CURVE_PATCH;
  }
  if (brush->material_paint == nullptr) {
    BKE_brush_material_paint_ensure(brush);
  }
  if (!BKE_paint_can_use_brush(&sd->paint, brush)) {
    brush->ob_mode |= OB_MODE_SCULPT;
  }

  /* A Curve Patch effect stores material channel maps: the session must paint the Material canvas,
   * not the vertex-color or Poly Paint one, or the strokes would land where the row never reads. */
  PaintModeSettings &paint_mode = scene->toolsettings->paint_mode;
  paint_mode.canvas_source = PAINT_CANVAS_SOURCE_MATERIAL;
  paint_mode.layer_target_mode = PAINT_LAYER_TARGET_CONTENT;

  /* The row must be the active one: the image effect resolves its targets through it, and the
   * operator also activates the object mask editing target. */
  if (!BLI_uuid_equal(ma.active_layer_marker, row.marker)) {
    BKE_paint_layers_active_set(ma, row.marker);
  }

  PaintVector &pv = *row.curve_patch->vectors;
  if (!curve_patch_layer_row_has_curves(pv)) {
    ED_paint_layers_curve_patch_row_seed_curve(*row.curve_patch);
    BKE_paint_layers_bake_request(row);
  }
  curve_patch_layer_curve_ids_ensure(pv);
  curve_patch_layer_seeds_place(C, *ob, *view_context, sd->paint, *brush, pv);
  /* The interactive Image effect resolves its targets from the row's channel maps and refuses a
   * canvas without one; the same maps are what the bake renders into. Create them all before the
   * session resolves its targets. */
  if (!ED_paint_layers_curve_patch_row_maps_ensure(C, ma, row)) {
    BKE_report(reports, RPT_WARNING, "Could not create a channel map for this Curve Patch effect");
    return false;
  }
  /* The new maps and the moved active row are visible in the properties/Outliner before the
   * modal takes over the events. */
  WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &ma.id);
  WM_event_add_notifier(&C, NC_SPACE | ND_SPACE_PROPERTIES, nullptr);

  /* Switch to the snapshot, then jump to its tool: switching the tool restores that tool's last
   * brush, which would replace the snapshot, so the snapshot is activated again afterwards. The
   * restore captured the user's outgoing brush and puts it back on every exit. */
  auto restore = std::make_unique<PrevBrushRestore>(*scene, sd->paint);
  if (!restore->activate(*brush)) {
    BKE_report(reports, RPT_WARNING, "This brush cannot be activated in Sculpt Mode");
    return false;
  }
  WM_toolsystem_activate_brush_and_tool(&C, &sd->paint, brush);
  /* The tool jump may have restored the tool's own brush; the row's snapshot is the session's. */
  if (!restore->activate(*brush)) {
    BKE_report(reports, RPT_WARNING, "This brush cannot be activated in Sculpt Mode");
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
  CurvePatchSession *session = curve_patch_start_from_layer(
      C, *ob, *sd, pv, *view_context, reset_maps);
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

  /* The interactive effect now owns the row's maps. Stamp the bake hash of the document the
   * session started from, so a planner tick during the session does not re-render the maps from
   * the (still unedited) document over the live preview. The commit invalidates it again; a
   * cancel keeps it, because the effect's undo restores exactly the pixels that hash describes. */
  BKE_paint_layers_bake_request(row);
  BKE_paint_layers_bake_finalize(ma, row);
  /* The bake state decides which images the generated graph samples for this effect, and it just
   * changed (the cache struct now exists and carries the session's hash). Rebuilding the graph here,
   * before the modal captures the evaluated material, is what makes the first stamps visible; it
   * used to take an unrelated regeneration (selecting another layer) to bring the maps in. */
  BKE_paint_layers_tag_edited(ma);
  BKE_paint_layers_regenerate_tagged(*CTX_data_main(&C));
  CTX_data_ensure_evaluated_depsgraph(&C);

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

bool curve_patch_layer_enter_if_sculpting(bContext &C, Material &ma, MaterialPaintLayer &row)
{
  const Object *ob = CTX_data_active_object(&C);
  /* Selecting an effect while in another mode only selects it; the tool is entered with the
   * explicit action, which switches the mode itself. */
  if (ob == nullptr || (ob->mode & OB_MODE_SCULPT) == 0) {
    return false;
  }
  return curve_patch_layer_activate(C, ma, row);
}

void curve_patch_layer_store_document(CurvePatchSession &session)
{
  MaterialPaintLayer *row = curve_patch_layer_session_row(session);
  if (row == nullptr || row->curve_patch->vectors == nullptr) {
    return;
  }
  /* A look edit in the list is only picked up by the modal's poll; the last one may not have been. */
  curve_patch_layer_active_item_follow(session);
  PaintVector &pv = *row->curve_patch->vectors;
  curve_patch_document_to_vector(session.doc, pv);
  BKE_paint_vector_tag_changed(pv);
  /* The row's content is its own channel maps: the changed document invalidates the bake, which
   * re-renders the maps in the background the next time the plan runs. */
  BKE_paint_layers_bake_request(*row);
}

void curve_patch_layer_commit_document(bContext &C, CurvePatchSession &session)
{
  if (curve_patch_layer_session_row(session) == nullptr) {
    return;
  }
  curve_patch_layer_store_document(session);
  ED_undo_memfile_push(&C, "Curve Patch Layer Edit");
  WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, session.layer_material);
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
        patch.params.hidden != stored.hidden)
    {
      /* The size-relative values follow the edited radius, or a later conversion of absolute
       * jitter would use the old one. */
      if (patch.params.radius > 0.0f) {
        patch.params.radius_per_size *= stored.radius / patch.params.radius;
      }
      patch.params.opacity = stored.opacity;
      patch.params.radius = stored.radius;
      patch.params.hidden = stored.hidden;
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
    if (item_of_patch[patch_i] < 0) {
      if (patch.curve_id <= 0) {
        patch.curve_id = curve_patch_layer_next_curve_id(pv);
      }
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
  curve_patch_commit_on_session_end(C, ob);
}

bool curve_patch_layer_session_blocks_row(const Object &ob, const MaterialPaintLayer &row)
{
  const CurvePatchSession *session = ED_curve_patch_session_get(ob);
  return session != nullptr && session->layer_material != nullptr &&
         curve_patch_layer_session_blocks_row(*session, row);
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
  Object &ob = *CTX_data_active_object(&C);
  curve_patch_layer_active_item_sync(session);
  curve_patch_undo_push(session);
  curve_patch_edit_status_set(&C, session);
  curve_patch_restore_and_restamp(C, ob, session);
  curve_patch_tag_viewports_redraw_after_edit(C, ob, session);
}

/** Add a curve to the live document, placed under the view like the first one, without leaving the
 * session. The default item is made in the container only to get its geometry placed on the
 * surface; the mirror then rebuilds the container from the document. */
static bool curve_patch_layer_session_curve_add(bContext &C,
                                                CurvePatchSession &session,
                                                MaterialPaintLayerCurvePatch &payload)
{
  const ToolSettings *tool_settings = CTX_data_tool_settings(&C);
  Object &ob = *CTX_data_active_object(&C);
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
  session.doc.patches.append_as();
  CurvePatchItem &patch = session.doc.patches.last();
  patch.control_curve = item.curve_patch_geometry.wrap();
  patch.params = BKE_paint_vector_curve_params_decode(item.curve_patch_params);
  patch.name = item.curve_name;
  curve_patch_document_active_patch_set(session.doc, int(session.doc.patches.size()) - 1);
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
  session.doc.patches.append(std::move(copy));
  curve_patch_document_active_patch_set(session.doc, int(session.doc.patches.size()) - 1);
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
  payload->vectors->active_item = payload->vectors->items_num - 1;
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
  pv.active_item = pv.items_num - 1;
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
  /* Replacing the snapshot would free the brush the live modal is editing through. */
  if (curve_patch_layer_live_session(C) != nullptr) {
    BKE_report(op->reports, RPT_WARNING, "Finish the live Curve Patch first");
    return OPERATOR_CANCELLED;
  }
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
   * when the source came from an asset library. */
  if (!bke::brush::supports_curve_patch(*source)) {
    BKE_reportf(op->reports,
                RPT_WARNING,
                "Brush \"%s\" cannot stamp a Curve Patch; pick a Draw, Clay, Crease or Paint "
                "type brush",
                source->id.name + 2);
    return OPERATOR_CANCELLED;
  }
  if (!BKE_paint_layers_curve_patch_brush_set(
          *target.material, *CTX_data_main(C), *target.row, *source))
  {
    BKE_report(op->reports, RPT_ERROR, "Could not duplicate that brush for the effect");
    return OPERATOR_CANCELLED;
  }
  ED_undo_memfile_push(C, "Assign Curve Patch Brush");
  WM_event_add_notifier(C, NC_BRUSH | NA_EDITED, nullptr);
  WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, &target.material->id);
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
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

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
