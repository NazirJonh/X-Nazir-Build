/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Multi-object half of a Curve Patch session: secondary-target registration, the world-space
 * mirror of the document, the reach test, and per-target teardown.
 */

#include <cfloat>
#include <cmath>
#include <optional>
#include <string>

#include "paint_curve_patch_targets.hh"

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_attribute.hh"
#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_curve_patch.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_paint_types.hh"
#include "BKE_report.hh"

#include "BLI_bounds_types.hh"
#include "BLI_index_range.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_string_ref.hh"
#include "BLI_virtual_array.hh"

#include "MEM_guardedalloc.h"

#include "ED_paint.hh"
#include "ED_view3d.hh"

#include "paint_curve_patch_apply.hh"
#include "paint_curve_patch_effect.hh"
#include "paint_curve_patch_session.hh"
#include "paint_curve_screen.hh"

#include "mesh/sculpt_intern.hh"

namespace blender::ed::sculpt_paint {

void curve_patch_control_curve_transform(const float4x4 &matrix, bke::CurvesGeometry &curve)
{
  if (curve.points_num() == 0) {
    return;
  }
  MutableSpan<float3> positions = curve.positions_for_write();
  for (float3 &position : positions) {
    position = math::transform_point(matrix, position);
  }
  /* The `_for_write` accessors are only reached behind an existence check, so a plain curve never
   * grows handle attributes here. */
  if (curve.handle_positions_left().has_value()) {
    MutableSpan<float3> handles = curve.handle_positions_left_for_write();
    for (float3 &handle : handles) {
      handle = math::transform_point(matrix, handle);
    }
  }
  if (curve.handle_positions_right().has_value()) {
    MutableSpan<float3> handles = curve.handle_positions_right_for_write();
    for (float3 &handle : handles) {
      handle = math::transform_point(matrix, handle);
    }
  }
  /* The per-point surface normals are directions in the same object space: left untouched they
   * would still describe the owner's orientation and steer the target's shrinkwrap wrongly. A
   * non-uniform scale skews directions, hence the renormalization. */
  if (bke::SpanAttributeWriter<float3> normals =
          curve.attributes_for_write().lookup_for_write_span<float3>(
              bke::CURVE_PATCH_ATTR_SURFACE_NORMAL))
  {
    for (float3 &normal : normals.span) {
      const float3 moved = math::transform_direction(matrix, normal);
      normal = math::length_squared(moved) > 1e-12f ? math::normalize(moved) : normal;
    }
    normals.finish();
  }
  curve.tag_positions_changed();
}

float curve_patch_object_max_scale(const Object &ob)
{
  const float3 scale = math::to_scale(ob.object_to_world());
  return math::max(math::max(math::abs(scale.x), math::abs(scale.y)), math::abs(scale.z));
}

static bool curve_patch_scale_is_nonuniform(const float3 &scale)
{
  const float max_scale = math::max(math::max(math::abs(scale.x), math::abs(scale.y)),
                                    math::abs(scale.z));
  if (max_scale <= 1e-8f) {
    return false;
  }
  const float epsilon = max_scale * 1e-4f;
  return math::abs(scale.x - scale.y) > epsilon || math::abs(scale.y - scale.z) > epsilon ||
         math::abs(scale.z - scale.x) > epsilon;
}

/** Rebuild ONE secondary target's mirror of a document item: the control curve carried through the
 * primary's object-to-world and back down into the target's own space, and the parameters converted
 * into the target's local units. The item identity is carried by its stable document index. */
static void curve_patch_mirror_item_update(const float4x4 &primary_to_world,
                                           const float4x4 &world_to_target,
                                           const float primary_max_scale,
                                           const float target_max_scale,
                                           const CurvePatchItem &src,
                                           CurvePatchItem &dst)
{
  dst.source_curve_index = src.source_curve_index;
  dst.curve_id = src.curve_id;
  dst.name = src.name;

  /* Carry the curve: primary's local -> world -> the target's local. */
  dst.control_curve = src.control_curve;
  curve_patch_control_curve_transform(primary_to_world, dst.control_curve);
  curve_patch_control_curve_transform(world_to_target, dst.control_curve);

  /* `radius`, `radius_per_size` and `jitter_amount` are lengths in the primary's object space.
   * Lifting them through the max-axis scale is the only scalar answer a non-uniform transform
   * admits; the target is warned about that approximation once. */
  dst.params = src.params;
  if (target_max_scale > 1e-8f) {
    const float scale_ratio = primary_max_scale / target_max_scale;
    dst.params.radius = src.params.radius * scale_ratio;
    dst.params.radius_per_size = src.params.radius_per_size * scale_ratio;
    dst.params.jitter_amount = src.params.jitter_amount * scale_ratio;
  }
  const float3 world_plane = math::normalize(
      math::transform_direction(primary_to_world, src.params.plane_normal));
  dst.params.plane_normal = math::normalize(
      math::transform_direction(world_to_target, world_plane));
}

void curve_patch_target_mirror_sync(CurvePatchSession &session,
                                    CurvePatchObjectTarget &target,
                                    ReportList *reports)
{
  if (session.owner == nullptr || target.ob == nullptr) {
    return;
  }
  const float4x4 primary_to_world = session.owner->object_to_world();
  const float4x4 world_to_target = target.ob->world_to_object();
  if (target.mirror_document_revision == session.doc.revision &&
      target.mirror_primary_to_world == primary_to_world &&
      target.mirror_target_world_to_object == world_to_target)
  {
    return;
  }

  const float3 target_scale = math::to_scale(target.ob->object_to_world());
  const float primary_max_scale = curve_patch_object_max_scale(*session.owner);
  const float target_max_scale = curve_patch_object_max_scale(*target.ob);
  target.mirror_items.resize(session.doc.patches.size());
  for (const int i : session.doc.patches.index_range()) {
    curve_patch_mirror_item_update(primary_to_world,
                                   world_to_target,
                                   primary_max_scale,
                                   target_max_scale,
                                   session.doc.patches[i],
                                   target.mirror_items[i]);
  }

  if (!target.reported_nonuniform_scale && curve_patch_scale_is_nonuniform(target_scale)) {
    target.reported_nonuniform_scale = true;
    BKE_reportf(reports,
                RPT_WARNING,
                "Curve Patch: object '%s' has non-uniform scale; the max-axis approximation is "
                "used for its radius",
                target.ob->id.name + 2);
  }

  target.mirror_document_revision = session.doc.revision;
  target.mirror_primary_to_world = primary_to_world;
  target.mirror_target_world_to_object = world_to_target;
}

CurvePatchWorldStamp curve_patch_world_stamp_from_primary(const Object &primary,
                                                          const CurvePatchSession &session)
{
  CurvePatchWorldStamp stamp;
  const float4x4 to_world = primary.object_to_world();
  const float primary_max_scale = curve_patch_object_max_scale(primary);
  float3 bbox_min(FLT_MAX);
  float3 bbox_max(-FLT_MAX);
  float max_radius_world = 0.0f;
  float max_jitter_world = 0.0f;

  const auto include_points = [&](const Span<float3> points) {
    for (const float3 &point : points) {
      const float3 world = math::transform_point(to_world, point);
      bbox_min = math::min(bbox_min, world);
      bbox_max = math::max(bbox_max, world);
    }
  };

  for (const CurvePatchItem &item : session.doc.patches) {
    if (item.params.hidden || item.control_curve.points_num() < 2) {
      continue;
    }
    stamp.any_buildable = true;
    include_points(item.control_curve.positions());
    if (const std::optional<Span<float3>> handles = item.control_curve.handle_positions_left()) {
      include_points(*handles);
    }
    if (const std::optional<Span<float3>> handles = item.control_curve.handle_positions_right()) {
      include_points(*handles);
    }
    float point_radius = 1.0f;
    for (const float radius : VArraySpan(item.control_curve.radius())) {
      point_radius = math::max(point_radius, math::abs(radius));
    }
    max_radius_world = math::max(max_radius_world,
                                 item.params.radius * primary_max_scale * point_radius);
    max_jitter_world = math::max(max_jitter_world,
                                 math::abs(item.params.jitter_amount) * primary_max_scale);
  }
  if (!stamp.any_buildable) {
    return stamp;
  }
  stamp.bbox_min = bbox_min;
  stamp.bbox_max = bbox_max;
  stamp.center = (bbox_min + bbox_max) * 0.5f;
  stamp.reach_radius = math::distance(bbox_max, stamp.center) + max_radius_world * 2.0f +
                       max_jitter_world;
  return stamp;
}

bool curve_patch_target_in_reach(const Object &target_ob,
                                 const float3 &world_center,
                                 const float world_reach_radius)
{
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(target_ob);
  if (pbvh == nullptr) {
    return true;
  }
  const Bounds<float3> bounds = bke::pbvh::bounds_get(*pbvh);
  const float4x4 to_world = target_ob.object_to_world();
  float3 bbox_min(FLT_MAX);
  float3 bbox_max(-FLT_MAX);
  for (const int i : IndexRange(8)) {
    const float3 corner(bounds.min.x + ((i & 1) ? bounds.max.x - bounds.min.x : 0.0f),
                        bounds.min.y + ((i & 2) ? bounds.max.y - bounds.min.y : 0.0f),
                        bounds.min.z + ((i & 4) ? bounds.max.z - bounds.min.z : 0.0f));
    const float3 world_corner = math::transform_point(to_world, corner);
    bbox_min = math::min(bbox_min, world_corner);
    bbox_max = math::max(bbox_max, world_corner);
  }
  const float3 closest = math::clamp(world_center, bbox_min, bbox_max);
  return math::distance(closest, world_center) <= world_reach_radius;
}

bool curve_patch_target_mesh_claim(Set<const ID *> &taken_meshes, const Object &ob)
{
  const ID *mesh_id = ob.data;
  if (mesh_id == nullptr || taken_meshes.contains(mesh_id)) {
    return false;
  }
  taken_meshes.add(mesh_id);
  return true;
}

/** Surfaces and canvases already stamped by a session, for the shared duplicate check. */
struct CurvePatchTargetFilter {
  Set<const ID *> taken_meshes;
  /* Image identity, not #BKE_paint_canvas_key_get: that key describes the pixel LAYOUT (UV map,
   * tile sizes), so two objects with their own equally sized maps would collide. */
  Set<const Image *> taken_images;
};

/** The images \a ob's canvas writes into, for the shared-canvas check. */
static Vector<const Image *> curve_patch_canvas_images(Object &ob,
                                                       PaintModeSettings &paint_mode_settings,
                                                       const Brush &brush,
                                                       const int visible_material_channels)
{
  Vector<const Image *> images;
  if (paint_mode_settings.canvas_source == PAINT_CANVAS_SOURCE_MATERIAL) {
    for (const PaintMaterialImageTarget &target : BKE_paint_material_image_targets_get(
             ob, paint_mode_settings, brush.material_paint, visible_material_channels))
    {
      images.append(target.image);
    }
    return images;
  }
  Image *image = nullptr;
  ImageUser *image_user = nullptr;
  if (BKE_paint_canvas_image_get(&paint_mode_settings, &ob, &image, &image_user) &&
      image != nullptr)
  {
    images.append(image);
  }
  return images;
}

/** Seed the dedup sets from every target already in the session, the primary included. */
static void curve_patch_target_filter_seed(CurvePatchSession &session,
                                           const Brush &brush,
                                           PaintModeSettings &paint_mode_settings,
                                           CurvePatchTargetFilter &r_filter)
{
  for (const CurvePatchObjectTarget &target : session.targets) {
    if (target.ob == nullptr) {
      continue;
    }
    if (session.effect_type == CurvePatchEffectType::Image) {
      const Paint *paint = target.cache != nullptr ? target.cache->paint : nullptr;
      if (paint != nullptr) {
        for (const Image *image : curve_patch_canvas_images(
                 *target.ob, paint_mode_settings, brush, paint->visible_material_channels))
        {
          r_filter.taken_images.add(image);
        }
      }
    }
    else {
      r_filter.taken_meshes.add(target.ob->data);
    }
  }
}

/** Outcome of the compatibility filter for one candidate secondary target. */
enum class CurvePatchCandidateResult {
  /** The candidate can join the session. */
  Ok,
  /** No paint context the stroke's effect could resolve on this object. */
  NoPaintContext,
  /** The candidate's own brush/canvas resolve to a different effect kind than the session's. */
  EffectKindMismatch,
  /** No Mesh-typed PBVH: none at all, Dynamic Topology's BMesh, or Multires' Grids. */
  Geometry,
  /** The Paint Mode canvas is an image/material map, but no canvas resolved on this object. */
  NoImageCanvas,
  /** The brush type has no Curve Patch meaning. */
  BrushType,
};

/** Result of the compatibility filter, with the paint context it resolved for the candidate. */
struct CurvePatchCandidateCheck {
  CurvePatchCandidateResult result = CurvePatchCandidateResult::NoPaintContext;
  const Paint *paint = nullptr;
};

/**
 * Shared compatibility rules for a secondary target: a Mesh-typed PBVH, the same effect kind as the
 * session, and a resolvable paint context. Duplicate-surface/canvas checks stay with the caller,
 * which owns the #CurvePatchTargetFilter.
 */
static CurvePatchCandidateCheck curve_patch_target_check(Object &cand,
                                                         const Paint *fallback_paint,
                                                         const Brush &brush,
                                                         PaintModeSettings &paint_mode_settings,
                                                         const CurvePatchEffectType want_type)
{
  CurvePatchCandidateCheck check;
  const SculptSession *cand_ss = cand.runtime->sculpt_session;
  check.paint =
      (cand_ss != nullptr && cand_ss->cache != nullptr && cand_ss->cache->paint != nullptr) ?
          cand_ss->cache->paint :
          fallback_paint;
  if (check.paint == nullptr) {
    return check;
  }

  /* The Mesh-typed PBVH is the contract of everything downstream (the per-element snapshot keys
   * and the faces/corners topology counters both assume it), so it is decided FIRST: no PBVH at
   * all, Dynamic Topology's BMesh and Multires' Grids are refused the same way, before any effect
   * kind is resolved -- a Grids object must never slip through on a matching brush kind. */
  const bke::pbvh::Tree *cand_pbvh = bke::object::pbvh_get(cand);
  if (cand_pbvh == nullptr || cand_pbvh->type() != bke::pbvh::Type::Mesh) {
    check.result = CurvePatchCandidateResult::Geometry;
    return check;
  }
  if (const std::optional<CurvePatchEffectType> cand_type = curve_patch_effect_type_for_brush(
          brush, *check.paint, cand, paint_mode_settings))
  {
    if (*cand_type != want_type) {
      check.result = CurvePatchCandidateResult::EffectKindMismatch;
    }
    else {
      check.result = CurvePatchCandidateResult::Ok;
    }
    return check;
  }
  /* The effect kind did not resolve. The brush type itself is fine for the PRIMARY (it got past
   * `supports_curve_patch`), so what failed here is either this object's canvas or the brush. */
  if (ELEM(paint_mode_settings.canvas_source,
           PAINT_CANVAS_SOURCE_MATERIAL,
           PAINT_CANVAS_SOURCE_IMAGE))
  {
    check.result = CurvePatchCandidateResult::NoImageCanvas;
  }
  else {
    check.result = CurvePatchCandidateResult::BrushType;
  }
  return check;
}

/** The refusal messages #curve_patch_refusal_message already carries, for the shared cases. */
static CurvePatchRefusal curve_patch_candidate_refusal(const CurvePatchCandidateResult result)
{
  switch (result) {
    case CurvePatchCandidateResult::Geometry:
      return CurvePatchRefusal::Geometry;
    case CurvePatchCandidateResult::NoImageCanvas:
      return CurvePatchRefusal::NoImageCanvas;
    case CurvePatchCandidateResult::BrushType:
      return CurvePatchRefusal::BrushType;
    default:
      return CurvePatchRefusal::None;
  }
}

/** Report why #curve_patch_target_check refused, when it did. Returns whether it was adoptable. */
static bool curve_patch_target_report_check(const Object &cand,
                                            const CurvePatchCandidateCheck &check,
                                            ReportList *reports)
{
  switch (check.result) {
    case CurvePatchCandidateResult::Ok:
      return true;
    case CurvePatchCandidateResult::NoPaintContext:
      BKE_reportf(reports,
                  RPT_WARNING,
                  "Curve Patch: object '%s' skipped -- no paint context for the stroke",
                  cand.id.name + 2);
      return false;
    case CurvePatchCandidateResult::EffectKindMismatch:
      /* One effect kind per session: a different kind would leave its undo data without a step
       * owner. */
      BKE_reportf(reports,
                  RPT_WARNING,
                  "Curve Patch: object '%s' skipped -- a different patch target kind than the "
                  "anchor object",
                  cand.id.name + 2);
      return false;
    case CurvePatchCandidateResult::Geometry:
    case CurvePatchCandidateResult::NoImageCanvas:
    case CurvePatchCandidateResult::BrushType:
      BKE_reportf(reports,
                  RPT_WARNING,
                  "Curve Patch: object '%s' skipped -- %s",
                  cand.id.name + 2,
                  curve_patch_refusal_message(curve_patch_candidate_refusal(check.result)));
      return false;
  }
  return false;
}

/**
 * Whether the candidate shares a surface/canvas with a target already in the session, and claim it
 * when it does not. Two per-target snapshots of the same vertices or tiles cannot both restore.
 */
static bool curve_patch_target_claim_unique(Object &cand,
                                            const Paint &cand_paint,
                                            const Brush &brush,
                                            PaintModeSettings &paint_mode_settings,
                                            const CurvePatchEffectType want_type,
                                            CurvePatchTargetFilter &filter,
                                            ReportList *reports)
{
  if (want_type == CurvePatchEffectType::Image) {
    const Vector<const Image *> images = curve_patch_canvas_images(
        cand, paint_mode_settings, brush, cand_paint.visible_material_channels);
    for (const Image *image : images) {
      if (filter.taken_images.contains(image)) {
        BKE_reportf(reports,
                    RPT_WARNING,
                    "Curve Patch: object '%s' skipped -- another target already stamps its canvas",
                    cand.id.name + 2);
        return false;
      }
    }
    for (const Image *image : images) {
      filter.taken_images.add(image);
    }
    return true;
  }
  if (!curve_patch_target_mesh_claim(filter.taken_meshes, cand)) {
    BKE_reportf(reports,
                RPT_WARNING,
                "Curve Patch: object '%s' skipped -- another target already stamps its mesh",
                cand.id.name + 2);
    return false;
  }
  return true;
}

/**
 * The ONE place a non-primary target is built and published: append it to the session, fill its
 * fields and topology counters, publish the shared session (and the cache) on the object's
 * `SculptSession`, and tag the layer UI. The cache must already be published on the object's
 * `SculptSession::cache` by the caller when the effect reads it (the Image factory does); the
 * publication here is the one that survives teardown.
 *
 * The new target is appended at the end of #CurvePatchSession::targets; `[0]` stays the owner.
 */
static CurvePatchObjectTarget &curve_patch_target_init(CurvePatchSession &session,
                                                       Object &ob,
                                                       StrokeCache &cache,
                                                       std::unique_ptr<CurvePatchEffect> effect)
{
  SculptSession *ss = ob.runtime->sculpt_session;
  BLI_assert(ss != nullptr);

  const int64_t target_index = session.targets.append_and_get_index_as();
  CurvePatchObjectTarget &target = session.targets[target_index];
  target.ob = &ob;
  target.cache = &cache;
  target.effect = std::move(effect);
  target.is_primary = false;
  target.use_face_set_on_commit = session.use_face_set_on_commit;
  target.in_reach = false;
  /* Force #curve_patch_target_mirror_sync on the first re-stamp: a fresh target has no mirror yet
   * and the default revision can legitimately equal the document's. */
  target.mirror_document_revision = ~uint64_t(0);

  target.apply.element_num = target.effect->element_num(ob);
  target.apply.faces_num = -1;
  target.apply.corners_num = -1;
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob);
  if (const Mesh *mesh = pbvh != nullptr && pbvh->type() == bke::pbvh::Type::Mesh ?
                            BKE_object_get_original_mesh(&ob) :
                            nullptr)
  {
    target.apply.faces_num = mesh->faces_num;
    target.apply.corners_num = mesh->corners_num;
  }

  /* Publish before the re-stamp: the effect resolves the session through the object the symmetry
   * pass drives, and the detach callback gives it a teardown path of its own. The session's
   * ImageUndoStep stays owned by the primary, so the new effect pushes its tiles into that step
   * instead of opening a second one. */
  ss->cache = &cache;
  ss->curve_patch_session = &session;
  ss->free_curve_patch_session = curve_patch_target_detach_on_session_end;
  /* The stroke's own ViewContext (when there was one) dies with its operator; the session's owned
   * copy lives for the whole edit. */
  cache.vc = &session.view_context;
  curve_patch_session_tag_layer_ui(ob);
  return target;
}

bool curve_patch_session_add_target(bContext &C,
                                    CurvePatchSession &session,
                                    Object &ob,
                                    ReportList *reports)
{
  Object *owner = session.owner;
  if (owner == nullptr || !session.doc.has_active_item()) {
    return false;
  }
  if (&ob == owner || session.target_for(ob) != nullptr) {
    return false;
  }

  SculptSession *ss = ob.runtime->sculpt_session;
  if (ss == nullptr) {
    BKE_reportf(reports,
                RPT_WARNING,
                "Curve Patch: object '%s' skipped -- it has no Sculpt session",
                ob.id.name + 2);
    return false;
  }
  if (ss->curve_patch_session != nullptr) {
    BKE_reportf(reports,
                RPT_WARNING,
                "Curve Patch: object '%s' skipped -- another Curve Patch session is active",
                ob.id.name + 2);
    return false;
  }
  if (ss->cache != nullptr) {
    BKE_reportf(reports,
                RPT_WARNING,
                "Curve Patch: object '%s' skipped -- a stroke is already in progress",
                ob.id.name + 2);
    return false;
  }

  ToolSettings *tool_settings = CTX_data_tool_settings(&C);
  if (tool_settings == nullptr || tool_settings->sculpt == nullptr) {
    return false;
  }
  Sculpt &sd = *tool_settings->sculpt;
  Brush *brush = BKE_paint_brush(&sd.paint);
  if (brush == nullptr) {
    BKE_report(reports, RPT_WARNING, "Curve Patch: no active brush to add the object with");
    return false;
  }
  PaintModeSettings &paint_mode_settings = tool_settings->paint_mode;
  const CurvePatchEffectType want_type = session.effect_type;

  /* A stroke prepares every participant's canvas before it asks what the object can paint
   * (`init_material_canvas_for_object`); this object joins without a stroke, so do the same here.
   * Without it a layered material whose paint layer has no image map yet resolves to no image
   * targets, and the object is refused as "no paintable canvas". */
  if (want_type == CurvePatchEffectType::Image && brush->material_paint != nullptr) {
    BKE_paint_material_channel_cache_invalidate(BKE_object_material_get(&ob, ob.actcol));
    BKE_paint_material_images_ensure_writable(*CTX_data_main(&C),
                                              ob,
                                              *brush->material_paint,
                                              paint_mode_settings,
                                              sd.paint.visible_material_channels);
  }

  CurvePatchTargetFilter filter;
  curve_patch_target_filter_seed(session, *brush, paint_mode_settings, filter);

  const Paint *fallback_paint = session.primary_target().cache != nullptr ?
                                    session.primary_target().cache->paint :
                                    nullptr;
  const CurvePatchCandidateCheck check = curve_patch_target_check(
      ob, fallback_paint, *brush, paint_mode_settings, want_type);
  if (!curve_patch_target_report_check(ob, check, reports)) {
    return false;
  }
  if (!curve_patch_target_claim_unique(
          ob, *check.paint, *brush, paint_mode_settings, want_type, filter, reports))
  {
    return false;
  }

  /* No stroke created a cache for this object: build the same stand-in the headless apply and the
   * Stack Layers entry use. The effect reads the canvas targets and symmetry state through it. */
  StrokeCache *cache = MEM_new<StrokeCache>(__func__);
  const CurvePatchApplyInput input;
  curve_patch_apply_cache_init(*cache,
                               ob,
                               sd,
                               *brush,
                               paint_mode_settings,
                               session.active_item().params,
                               want_type,
                               input);
  cache->vc = &session.view_context;

  /* The Image effect factory reads the canvas targets off `SculptSession::cache`, so the cache has
   * to be published before the effect is created. A failure frees it again (which also clears the
   * pointer). */
  ss->cache = cache;
  std::unique_ptr<CurvePatchEffect> effect = curve_patch_effect_create(
      want_type, ob, paint_mode_settings);
  if (effect == nullptr) {
    BKE_reportf(reports,
                RPT_WARNING,
                "Curve Patch: object '%s' skipped -- the object cannot carry the effect",
                ob.id.name + 2);
    curve_patch_stroke_cache_free(ss, cache);
    return false;
  }

  /* A new target has no mirror yet; the full re-stamp below builds it, applies the patch to the
   * new object, and restores/re-applies every existing target. */
  curve_patch_target_init(session, ob, *cache, std::move(effect));

  curve_patch_restore_and_restamp(C, session);
  /* Looked up again: the re-stamp may have detached the target, and a reference into `targets`
   * would not survive that. */
  if (const CurvePatchObjectTarget *added = session.target_for(ob)) {
    flush_update_done(&C, ob, added->effect->update_type());
  }
  return true;
}

bool curve_patch_drag_point_onto_other_object(bContext &C,
                                              CurvePatchSession &session,
                                              Object &owner,
                                              const ViewContext &vc,
                                              const float2 &mval,
                                              const float3 &pivot_world,
                                              PaintCurveSnapContext *&snap_ctx,
                                              Vector<const Object *> &refused,
                                              float3 &r_owner_co,
                                              float3 &r_owner_no)
{
  const ToolSettings *tool_settings = CTX_data_tool_settings(&C);
  const Sculpt *sd = tool_settings ? tool_settings->sculpt : nullptr;
  const Brush *brush = sd ? BKE_paint_brush_for_read(&sd->paint) : nullptr;
  if (sd == nullptr || brush == nullptr || vc.depsgraph == nullptr) {
    return false;
  }

  const float mval_fl[2] = {mval.x, mval.y};
  /* #stroke_get_location_bvh takes a mutable ViewContext but never writes through it; a copy
   * keeps the caller's const one intact. */
  ViewContext vc_search = vc;
  Object *hit_ob = nullptr;
  float hit_co[3];
  if (!stroke_get_location_bvh(
          *vc.depsgraph, vc_search, sd, brush, hit_co, mval_fl, false, &hit_ob) ||
      hit_ob == nullptr || hit_ob == &owner)
  {
    return false;
  }

  if (session.target_for(*hit_ob) == nullptr && !refused.contains(hit_ob)) {
    if (curve_patch_session_add_target(C, session, *hit_ob, CTX_wm_reports(&C))) {
      BKE_reportf(CTX_wm_reports(&C),
                  RPT_INFO,
                  "Curve Patch: added '%s' as a target",
                  hit_ob->id.name + 2);
    }
    else {
      /* One report per object per drag; a MOUSEMOVE would otherwise repeat it. */
      refused.append(hit_ob);
    }
  }
  if (session.target_for(*hit_ob) == nullptr) {
    return false;
  }

  /* Ray-cast the object itself so its own surface normal is used, then carry the hit back into the
   * owner's space. */
  ViewContext vc_target = vc;
  vc_target.obact = hit_ob;
  float target_co[3];
  float target_no[3];
  if (snap_ctx == nullptr) {
    snap_ctx = ED_paintcurve_snap_context_create();
  }
  if (!paintcurve_surface_place(&C,
                                snap_ctx,
                                vc_target,
                                mval_fl,
                                pivot_world,
                                /*use_depth_fallback=*/false,
                                target_co,
                                target_no))
  {
    return false;
  }
  const float3 world_co = math::transform_point(hit_ob->object_to_world(), float3(target_co));
  r_owner_co = math::transform_point(owner.world_to_object(), world_co);
  const float3 world_no = math::transform_direction(hit_ob->object_to_world(), float3(target_no));
  r_owner_no = math::transform_direction(owner.world_to_object(), world_no);
  if (math::length_squared(r_owner_no) > 1e-20f) {
    r_owner_no = math::normalize(r_owner_no);
  }
  return true;
}

bool curve_patch_session_remove_target(CurvePatchSession &session, const Object &ob)
{
  const int64_t old_size = session.targets.size();
  session.targets.remove_if([&](const CurvePatchObjectTarget &target) {
    return target.ob == &ob;
  });
  return session.targets.size() != old_size;
}

void curve_patch_target_detach_on_session_end(Object &ob)
{
  SculptSession *ss = ob.runtime->sculpt_session;
  if (ss == nullptr || ss->curve_patch_session == nullptr) {
    return;
  }
  CurvePatchSession &session = *ss->curve_patch_session;
  if (session.owner == &ob) {
    /* The owner's sculpt session is going away: nothing of the session can outlive it. */
    curve_patch_discard_on_session_end(ob);
    return;
  }

  CurvePatchObjectTarget *target = session.target_for(ob);
  if (target != nullptr) {
    /* Restore through the still-alive mesh/PBVH before the caller frees them. */
    if (target->effect != nullptr && target->cache != nullptr &&
        !curve_patch_apply_target_changed(ob, *target))
    {
      CurvePatchTargetState state = session.target_state(*target);
      state.effect.restore(state);
      state.effect.sync_targets(ob);
    }
    curve_patch_stroke_cache_free(ss, target->cache);
    target->cache = nullptr;
    ss->curve_patch_session = nullptr;
    ss->free_curve_patch_session = nullptr;
    curve_patch_session_remove_target(session, ob);
    curve_patch_session_tag_layer_ui(ob);
  }
  else {
    /* Already detached: do not leave a pointer to a session this object no longer participates
     * in. */
    ss->curve_patch_session = nullptr;
    ss->free_curve_patch_session = nullptr;
  }
}

}  // namespace blender::ed::sculpt_paint
