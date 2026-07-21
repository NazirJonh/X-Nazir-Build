/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include <cmath>
#include <optional>
#include <utility>

#include "paint_curve_patch_apply.hh"

#include "DNA_brush_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_brush.hh"
#include "BKE_colortools.hh"
#include "BKE_context.hh"
#include "BKE_curves.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_paint_types.hh"
#include "BKE_report.hh"

#include "BLI_index_range.hh"
#include "BLI_math_vector.hh"
#include "BLI_rand.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "MEM_guardedalloc.h"

#include "DEG_depsgraph.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "ED_paint.hh"
#include "ED_undo.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "paint_curve_intern.hh"
#include "paint_curve_patch_session.hh"
#include "paint_intern.hh"

#include "mesh/sculpt_intern.hh"

namespace blender::ed::sculpt_paint {

/* -------------------------------------------------------------------- */
/** \name Headless Apply
 * \{ */

void curve_patch_prepare_brush_for_headless(Paint &paint, Brush &brush)
{
  /* The same three lines `PaintStroke::start()` runs before its first dab (`paint_stroke.cc`). */
  bke::brush::common_pressure_curves_init(brush);
  if (paint.flags & PAINT_USE_CAVITY_MASK) {
    BKE_curvemapping_init(paint.cavity_curve);
  }
  /* Dereferenced rather than null-checked on purpose: `brush_strength()` reads `*paint.runtime`
   * unconditionally anyway, and a silent skip here is exactly the zero-amplitude patch this
   * function exists to prevent. */
  paint.runtime->overlap_factor = paint_stroke_integrate_overlap(brush, 1.0f);
}

/* Undo the piece of session state #curve_patch_apply borrows from the sculpt session. The texture
 * pool is deliberately not touched: it belongs to the session for as long as the session lives
 * (see #SculptSession::tex_pool_ensure). */
static void curve_patch_apply_release(SculptSession &ss)
{
  MEM_delete(ss.cache);
  ss.cache = nullptr;
}

/* Stand in for the `StrokeCache` an interactive patch inherits from its anchor stroke. Only the
 * fields the Curve Patch re-stamp and the symmetry machinery actually read are set; everything else
 * keeps the defaults `StrokeCache`'s own member initializers give it. */
static void curve_patch_apply_cache_init(StrokeCache &cache,
                                         Object &ob,
                                         Sculpt &sd,
                                         Brush &brush,
                                         PaintModeSettings &paint_mode_settings,
                                         const bke::CurvePatchParams &params,
                                         const CurvePatchEffectType effect_type,
                                         const CurvePatchApplyInput &input)
{
  cache.paint = &sd.paint;
  cache.brush = &brush;

  /* Taken from the patch's own parameters rather than from the input, so the ribbon half-width and
   * the projection plane have exactly one source of truth. `location`/`radius` are overwritten by
   * the re-stamp with the control curve's encompassing sphere before anything is sampled; they are
   * set here only so the state is coherent for the symmetry setup that runs first. */
  cache.initial_radius = params.radius;
  cache.radius = params.radius;
  cache.radius_squared = params.radius * params.radius;
  cache.sculpt_normal = params.plane_normal;
  cache.initial_normal = params.plane_normal;

  cache.location = input.location;
  cache.initial_location = input.location;
  cache.view_normal = input.view_normal;
  cache.pressure = input.pressure;

  /* Symmetry and tiling divide by this; verbatim from `SculptPaintStroke::stroke_cache_init()`,
   * including its lack of a zero guard -- a zero object scale degenerates the stroke path the same
   * way. */
  float max_scale = 0.0f;
  for (int i = 0; i < 3; i++) {
    max_scale = math::max(max_scale, std::abs(ob.scale[i]));
  }
  cache.scale = float3(max_scale / ob.scale[0], max_scale / ob.scale[1], max_scale / ob.scale[2]);

  /* Also from `stroke_cache_init()`: a Curve Patch reads original coordinates (it is recomputed
   * from scratch on every re-stamp), except on the image canvas, which does not use the sculpt undo
   * system that serves them. */
  cache.accum = effect_type == CurvePatchEffectType::Image;
  if (effect_type == CurvePatchEffectType::Image) {
    /* `ImageColorEffect` reaches its canvas exclusively through this handle and silently does
     * nothing without it -- a blocker the Stage 0 measurement never hit, because it measured
     * relief. */
    cache.image_data = paint::image::ImageData::init_active_image(ob, paint_mode_settings);
  }
}

bool curve_patch_apply(const Scene &scene,
                       const Depsgraph &depsgraph,
                       Object &ob,
                       Sculpt &sd,
                       PaintModeSettings &paint_mode_settings,
                       const Span<bke::CurvesGeometry> control_curves,
                       const Span<bke::CurvePatchParams> params,
                       const CurvePatchEffectType effect_type,
                       const CurvePatchApplyInput &input,
                       ReportList *reports)
{
  BLI_assert(control_curves.size() == params.size());
  SculptSession *ss = ob.runtime->sculpt_session;
  if (ss == nullptr) {
    BKE_report(reports, RPT_ERROR, "Curve Patch: the object is not in Sculpt Mode");
    return false;
  }
  /* Both are taken over below, and both belong to something still running -- a live stroke or an
   * interactive patch. */
  if (ss->cache != nullptr || ss->curve_patch_session != nullptr) {
    BKE_report(reports, RPT_ERROR, "Curve Patch: a stroke or another patch is already in progress");
    return false;
  }
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob);
  if (pbvh == nullptr) {
    BKE_report(reports, RPT_ERROR, "Curve Patch: the object has no sculpt geometry");
    return false;
  }
  /* Dynamic Topology has no stable per-element index for the effect's snapshot to key into; the
   * interactive entry points refuse it the same way. */
  if (pbvh->type() == bke::pbvh::Type::BMesh) {
    BKE_report(reports, RPT_WARNING, "Curve Patch does not support Dynamic Topology");
    return false;
  }
  if (control_curves.is_empty()) {
    BKE_report(reports, RPT_ERROR, "Curve Patch: no control curve to stamp");
    return false;
  }
  /* Every curve is checked, not just the first: a caller that assembled the list itself must not be
   * able to smuggle a degenerate one past the build and into an empty patch. */
  for (const bke::CurvesGeometry &curve : control_curves) {
    if (curve.points_num() < 2) {
      BKE_report(reports, RPT_ERROR, "Curve Patch: the control curve needs at least two points");
      return false;
    }
  }
  /* `--background` leaves `wmWindowManager::undo_stack` null (`wm_files.cc`, guarded by
   * `!G.background`), and every undo push this call reaches -- the relief's position step, the
   * color attribute step, the image canvas step -- dereferences it without a check. Refusing here
   * turns what was an access violation deep inside `undosys_stack_validate()` into a message a
   * script can act on: `bpy.ops.ed.undo_push()` creates the stack on demand (`ed_undo.cc`, issue
   * #60934). The interactive paths need no such gate -- a GUI session always has a stack. */
  if (ED_undo_stack_get() == nullptr) {
    BKE_report(reports,
               RPT_ERROR,
               "Curve Patch: no undo stack (background mode); call bpy.ops.ed.undo_push() first");
    return false;
  }
  /* The brush the symmetry machinery reads (`do_symmetrical_brush_actions()` resolves it from
   * `sd.paint` itself), which is why it is not a parameter of this function. */
  Brush *brush = BKE_paint_brush(&sd.paint);
  if (brush == nullptr) {
    BKE_report(reports, RPT_ERROR, "Curve Patch: no active brush");
    return false;
  }

  curve_patch_prepare_brush_for_headless(sd.paint, *brush);

  ss->cache = MEM_new<StrokeCache>(__func__);
  /* The first patch stands for all of them here: every field this seeds is either overwritten by
   * the re-stamp with the whole session's encompassing sphere, or -- like `sculpt_normal` -- only
   * read by the symmetry setup that runs before any patch is sampled. */
  curve_patch_apply_cache_init(
      *ss->cache, ob, sd, *brush, paint_mode_settings, params[0], effect_type, input);

  auto *session = MEM_new<CurvePatchSession>(__func__);
  session->patches.resize(control_curves.size());
  session->active_patch = 0;
  for (const int i : control_curves.index_range()) {
    session->patches[i].control_curve = control_curves[i];
    session->patches[i].params = params[i];
  }
  /* One shot: there is no interactive preview for a cheaper build to serve, and the commit below
   * expects the smoothed profile a final-quality re-stamp produces. */
  curve_patch_set_final_quality(*session, true);

  if (!curve_patch_session_publish(ob, *session, effect_type, paint_mode_settings)) {
    BKE_report(reports, RPT_ERROR, "Curve Patch: this object cannot carry the requested effect");
    MEM_delete(session);
    curve_patch_apply_release(*ss);
    return false;
  }
  /* Left default-constructed, so `region` stays null and the re-stamp skips the interactive
   * viewport flush -- see `session_apply()`. The cache still points at it for parity with the
   * interactive path, where `StrokeCache::vc` must outlive the spawning stroke. */
  ss->cache->vc = &session->view_context;

  /* The interactive paths call this after aborting the spawning stroke's undo transaction. Nothing
   * opened a transaction here, so the hook runs before the only re-stamp instead -- which is what
   * puts the pre-patch canvas tiles into the image effect's undo step at all. */
  session->effect->session_undo_begin();

  curve_patch_restore_and_restamp(scene, depsgraph, sd, paint_mode_settings, ob, *session, reports);

  /* `invalidated` means the element count changed underneath the session, which cannot happen in a
   * single synchronous call -- checked anyway, because committing in that state writes a stale
   * snapshot into an unrelated mesh. */
  const bool applied = !session->apply.invalidated;
  if (applied) {
    curve_patch_finish_commit(scene, depsgraph, ob, *session);
  }

  /* Cache first, then the session -- the order #curve_patch_commit_on_session_end established, and
   * the one the image effect's destructor (which closes its undo step) was shown to survive. */
  curve_patch_apply_release(*ss);
  MEM_delete(session);
  ss->curve_patch_session = nullptr;

  if (applied && effect_type != CurvePatchEffectType::Image) {
    /* No viewport flush was issued (a caller with no window manager has nothing to repaint), so the
     * ID is tagged here instead -- the same pair #curve_patch_discard_on_session_end places. The
     * image canvas is excluded because it is not the mesh that changed; `ImageColorEffect` marks
     * its own image dirty as it writes. */
    Mesh &mesh = *id_cast<Mesh *>(ob.data);
    BKE_mesh_batch_cache_dirty_tag(&mesh, BKE_MESH_BATCH_DIRTY_ALL);
    DEG_id_tag_update(&mesh.id, ID_RECALC_GEOMETRY);
  }

  return applied;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operator
 * \{ */

/* Plane the patch is projected onto, fitted to the control curve itself.
 *
 * Newell's method over the control points, treated as a closed loop: it is exact for a planar
 * curve, degrades gracefully for a nearly planar one, and -- unlike anything derived from the
 * view -- gives the same answer in `--background` as it does with a viewport open. A straight
 * curve spans no plane at all and falls back to the object's own Z, which costs nothing on a Mesh:
 * there the surface snapshot supplies real per-point normals and `plane_normal` is only consulted
 * where that snapshot misses. */
static float3 curve_patch_plane_normal_from_curve(const bke::CurvesGeometry &curve)
{
  const Span<float3> positions = curve.positions();
  float3 normal(0.0f);
  for (const int64_t i : positions.index_range()) {
    const float3 &a = positions[i];
    const float3 &b = positions[(i + 1) % positions.size()];
    normal.x += (a.y - b.y) * (a.z + b.z);
    normal.y += (a.z - b.z) * (a.x + b.x);
    normal.z += (a.x - b.x) * (a.y + b.y);
  }
  if (math::length_squared(normal) < 1e-12f) {
    return float3(0.0f, 0.0f, 1.0f);
  }
  return math::normalize(normal);
}

static float3 curve_patch_curve_center(const bke::CurvesGeometry &curve)
{
  const Span<float3> positions = curve.positions();
  float3 min = positions[0];
  float3 max = positions[0];
  for (const float3 &position : positions) {
    min = math::min(min, position);
    max = math::max(max, position);
  }
  return (min + max) * 0.5f;
}

/* The paint curve this apply reads, named by the operator's `paint_curve` property or, when that is
 * empty, the active brush's own. Null with a report when neither resolves to something usable. */
static const PaintCurve *curve_patch_apply_resolve_paint_curve(bContext *C,
                                                               wmOperator *op,
                                                               const Brush &brush)
{
  const PaintCurve *paint_curve = brush.paint_curve;

  char name[MAX_ID_NAME - 2];
  RNA_string_get(op->ptr, "paint_curve", name);
  if (name[0] != '\0') {
    paint_curve = reinterpret_cast<const PaintCurve *>(
        BKE_libblock_find_name(CTX_data_main(C), ID_PC, name));
    if (paint_curve == nullptr) {
      BKE_reportf(op->reports, RPT_ERROR, "Paint curve '%s' not found", name);
      return nullptr;
    }
  }

  if (paint_curve == nullptr || !paintcurve_geometry_is_valid(paint_curve->geometry.wrap()) ||
      paint_curve->geometry.wrap().points_num() < 2)
  {
    BKE_report(op->reports, RPT_ERROR, "Curve Patch: no paint curve with at least two points");
    return nullptr;
  }
  /* Nothing here about the spline COUNT: a Curve Patch is one strip along one spline (see
   * `CurvePatchEditState::control_curve`), and which spline that is comes from the operator's
   * `spline_index` or the curve's own active one -- see #ED_paintcurve_control_curve_for_patch.
   * The point count of the chosen spline can only be checked once it has been sliced out. */
  return paint_curve;
}

/* AUTO is -1 rather than an extra #CurvePatchEffectType value: the enumeration names what an effect
 * IS, and "work it out from the brush" is not one of those. The remaining items mirror it exactly,
 * so the cast in the exec below is the whole conversion. */
static const EnumPropertyItem curve_patch_apply_effect_items[] = {
    {-1, "AUTO", 0, "Automatic", "Choose the target the way the interactive tool does"},
    {int(CurvePatchEffectType::Relief), "RELIEF", 0, "Relief", "Displace the mesh"},
    {int(CurvePatchEffectType::Color), "COLOR", 0, "Color", "Write the mesh color attribute"},
    {int(CurvePatchEffectType::Image), "IMAGE", 0, "Image", "Write the image canvas"},
    {0, nullptr, 0, nullptr, nullptr},
};

/* The object this apply writes to, named by the operator's `object` property or, when that is
 * empty, the active one. Null with a report when it cannot carry a patch.
 *
 * The mesh and mode checks are here rather than left to `curve_patch_apply()` so the message names
 * the object a script asked for; the deeper checks (sculpt session, PBVH type, dyntopo) stay where
 * they are and still apply. */
static Object *curve_patch_apply_resolve_object(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);

  char name[MAX_ID_NAME - 2];
  RNA_string_get(op->ptr, "object", name);
  if (name[0] != '\0') {
    ob = reinterpret_cast<Object *>(BKE_libblock_find_name(CTX_data_main(C), ID_OB, name));
    if (ob == nullptr) {
      BKE_reportf(op->reports, RPT_ERROR, "Object '%s' not found", name);
      return nullptr;
    }
  }

  if (ob == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "Curve Patch: no object to apply to");
    return nullptr;
  }
  if (ob->type != OB_MESH || (ob->mode & OB_MODE_SCULPT) == 0) {
    BKE_reportf(op->reports,
                RPT_ERROR,
                "Curve Patch: object '%s' is not a mesh in Sculpt Mode",
                ob->id.name + 2);
    return nullptr;
  }
  return ob;
}

static wmOperatorStatus curve_patch_apply_exec(bContext *C, wmOperator *op)
{
  Object *ob_ptr = curve_patch_apply_resolve_object(C, op);
  if (ob_ptr == nullptr) {
    return OPERATOR_CANCELLED;
  }
  Object &ob = *ob_ptr;
  ToolSettings &tool_settings = *CTX_data_tool_settings(C);
  Sculpt &sd = *tool_settings.sculpt;

  Brush *brush = BKE_paint_brush(&sd.paint);
  if (brush == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "Curve Patch: no active brush");
    return OPERATOR_CANCELLED;
  }

  const PaintCurve *paint_curve = curve_patch_apply_resolve_paint_curve(C, op, *brush);
  if (paint_curve == nullptr) {
    return OPERATOR_CANCELLED;
  }

  /* The three preparation steps `brush_stroke_init()` performs before an ordinary stroke's first
   * dab. They come before the effect is chosen, not after: that choice reads the Paint BVH, which
   * `BKE_sculpt_update_object_for_edit()` is what guarantees exists. */
  const bool is_paint_brush = brush_type_is_paint(brush->sculpt_brush_type);
  if (is_paint_brush && !SCULPT_use_image_paint_brush(tool_settings.paint_mode, ob)) {
    BKE_sculpt_color_layer_create_if_needed(&ob);
  }
  Depsgraph &depsgraph = *CTX_data_ensure_evaluated_depsgraph(C);
  BKE_sculpt_update_object_for_edit(&depsgraph, &ob, is_paint_brush);

  /* A negative value is the AUTO item: infer the target from the brush exactly as the interactive
   * tool does. Anything else is the caller stating it outright, which `curve_patch_session_publish()`
   * below still refuses if this object cannot carry it. */
  const int effect_choice = RNA_enum_get(op->ptr, "effect");
  std::optional<CurvePatchEffectType> effect_type;
  if (effect_choice < 0) {
    effect_type = curve_patch_effect_type_for_brush(*brush, ob, tool_settings.paint_mode);
    if (!effect_type) {
      BKE_report(op->reports, RPT_ERROR, "Curve Patch: this brush does not support Curve Patch");
      return OPERATOR_CANCELLED;
    }
  }
  else {
    effect_type = CurvePatchEffectType(effect_choice);
  }

  /* The point count is only meaningful once the spline is chosen: a multi-spline curve can hold
   * plenty of points in total and a single one in the spline that was asked for. */
  Vector<bke::CurvesGeometry> control_curves;
  if (RNA_boolean_get(op->ptr, "use_all_splines")) {
    /* A short spline is skipped rather than fatal here: asking for every spline of a curve must not
     * fail because one of them is a stray single point. The single-spline branch below still
     * refuses, because there the caller named exactly which spline it wanted. */
    for (const int i : IndexRange(paint_curve->geometry.wrap().curves_num())) {
      bke::CurvesGeometry curve = ED_paintcurve_control_curve_for_patch(*paint_curve, i);
      if (curve.points_num() >= 2) {
        control_curves.append(std::move(curve));
      }
    }
    if (control_curves.is_empty()) {
      BKE_report(op->reports, RPT_ERROR, "Curve Patch: no spline has at least two points");
      return OPERATOR_CANCELLED;
    }
  }
  else {
    bke::CurvesGeometry curve = ED_paintcurve_control_curve_for_patch(
        *paint_curve, RNA_int_get(op->ptr, "spline_index"));
    if (curve.points_num() < 2) {
      BKE_report(op->reports, RPT_ERROR, "Curve Patch: the chosen spline needs at least two points");
      return OPERATOR_CANCELLED;
    }
    control_curves.append(std::move(curve));
  }

  /* One source for every build parameter a brush implies outside a stroke -- shared with the RNA
   * functions that read a patch back out without applying it. Resolved PER CURVE: the projection
   * plane is fitted to the curve itself, so two splines lying on different faces get the plane each
   * of them actually needs. */
  Vector<bke::CurvePatchParams> params;
  params.reserve(control_curves.size());
  for (const bke::CurvesGeometry &curve : control_curves) {
    params.append(ED_curve_patch_params_from_brush(sd.paint, *brush, curve));
  }

  CurvePatchApplyInput input;
  input.location = curve_patch_curve_center(control_curves[0]);
  /* No view to read: the projection plane doubles as the view direction, which only a TUBE-falloff
   * brush's node query consults. */
  input.view_normal = params[0].plane_normal;

  if (!curve_patch_apply(*CTX_data_scene(C),
                         depsgraph,
                         ob,
                         sd,
                         tool_settings.paint_mode,
                         control_curves,
                         params,
                         *effect_type,
                         input,
                         op->reports))
  {
    return OPERATOR_CANCELLED;
  }

  /* `curve_patch_apply()` issues none of these itself -- it has no `bContext` by design. */
  switch (*effect_type) {
    case CurvePatchEffectType::Relief:
      flush_update_done(C, ob, UpdateType::Position);
      break;
    case CurvePatchEffectType::Color:
      flush_update_done(C, ob, UpdateType::Color);
      break;
    case CurvePatchEffectType::Image:
      flush_update_done(C, ob, UpdateType::Image);
      break;
  }

  return OPERATOR_FINISHED;
}

void SCULPT_OT_curve_patch_apply(wmOperatorType *ot)
{
  ot->name = "Apply Curve Patch";
  ot->description = "Stamp a Curve Patch along a paint curve without entering the live editor";
  ot->idname = "SCULPT_OT_curve_patch_apply";

  ot->exec = curve_patch_apply_exec;
  /* Deliberately NOT `sculpt_mode_poll_view3d()`: this operator never reads a region, and requiring
   * one would rule out the background runs it exists for. */
  ot->poll = sculpt_mode_poll;

  /* `OPTYPE_UNDO` is load-bearing, not decoration: `ReliefEffect::push_position_step()` closes the
   * patch's undo step with an unforced `push_end_ex()`, which PARKS it for the calling operator to
   * file. Without this flag the step is left hanging and the next unrelated undo push in the
   * application adopts it. */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_string(ot->srna,
                 "paint_curve",
                 nullptr,
                 MAX_ID_NAME - 2,
                 "Paint Curve",
                 "Name of the paint curve to stamp along; the active brush's own when empty");

  RNA_def_string(ot->srna,
                 "object",
                 nullptr,
                 MAX_ID_NAME - 2,
                 "Object",
                 "Name of the sculpt-mode mesh to stamp onto; the active object when empty");

  RNA_def_enum(ot->srna,
               "effect",
               curve_patch_apply_effect_items,
               -1,
               "Effect",
               "What the patch writes; inferred from the brush when Automatic");

  RNA_def_int(ot->srna,
              "spline_index",
              -1,
              -1,
              INT_MAX,
              "Spline",
              "Index of the spline to stamp along; the curve's active spline when negative",
              -1,
              INT_MAX);

  RNA_def_boolean(ot->srna,
                  "use_all_splines",
                  false,
                  "All Splines",
                  "Stamp a patch along every spline of the paint curve instead of a single one. "
                  "Overlapping patches blend through the same accumulator symmetry passes use, so "
                  "they average rather than stack");
}

/** \} */

}  // namespace blender::ed::sculpt_paint

namespace blender {

/* Declared in `ED_paint.hh`, which puts the `ED_*` API in `blender` rather than in this file's own
 * `blender::ed::sculpt_paint` -- hence the separate namespace block, as in `paint_curve_sync.cc`. */
bke::CurvePatchParams ED_curve_patch_params_from_brush(const Paint &paint,
                                                       const Brush &brush,
                                                       const bke::CurvesGeometry &control_curve)
{
  /* World-space, view-independent, and kept in sync with the Size slider by every stroke that ends
   * (`BKE_brush_unprojected_size_set()`, `mesh/sculpt.cc`). The interactive path measures the
   * radius from the anchor dab's depth instead, which needs a viewport. */
  const float radius = BKE_brush_unprojected_radius_get(&paint, &brush);
  const int brush_size = BKE_brush_size_get(&paint, &brush);
  const float radius_per_size = brush_size > 0 ? radius / float(brush_size) : 0.0f;
  const float3 plane_normal = ed::sculpt_paint::curve_patch_plane_normal_from_curve(control_curve);
  /* Rolled once here for the same reason `curve_patch_begin_editing()` rolls it once: everything
   * downstream hashes this seed rather than touching an RNG of its own. */
  const uint32_t stamp_seed = RandomNumberGenerator::from_random_seed().get_uint32();

  return ed::sculpt_paint::curve_patch_params_from_brush(
      brush, radius, radius_per_size, plane_normal, stamp_seed, brush.curve_patch.swap_axis != 0);
}

}  // namespace blender
