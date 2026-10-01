/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Implementation of the live Vector shape session of Sculpt Mode; see
 * `paint_shape_vector_3d.hh`. Mirrors the Image Editor session (`paint_image_shape_vector.cc`)
 * with the Object/SculptSession ownership of `CurvePatchSession` and the ViewProjector coordinate
 * space (shape pixels == region pixels).
 */

#include "paint_shape_vector_3d.hh"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>

#include "MEM_guardedalloc.h"

#include "BLI_function_ref.hh"
#include "BLI_listbase.h"
#include "BLI_listbase_iterator.hh"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_time.h"

#include "DNA_ID.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_view3d_types.h"

#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_main.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_vector.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"

#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_space_api.hh"
#include "ED_view3d.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "../shapes/paint_shape_edit.hh"
#include "../shapes/paint_shape_space.hh"
#include "../shapes/paint_shape_target.hh"
#include "../shapes/paint_shape_uv_trace.hh"
#include "../shapes/paint_vector_editor.hh"
#include "paint_image_shape_composite.hh"
#include "sculpt_intern.hh"
#include "sculpt_paint_shape.hh"

namespace blender::ed::sculpt_paint::shape {

/* -------------------------------------------------------------------- */
/** \name Session state
 * \{ */

/** The already-prepared input of one refresh / commit: the edited shapes and the style with the
 * final per-channel values. 3D expands no canvas symmetry, so the shapes are used as-is. */
struct ShapeVector3DInput {
  Vector<PaintShape> expanded;
  ShapeStyle style;
};

struct PaintShapeSession {
  Object *object = nullptr;
  Main *bmain = nullptr;
  Scene *scene = nullptr;
  ARegion *owner_region = nullptr;
  ARegionType *owner_region_type = nullptr;
  void *draw_handle = nullptr;

  ShapeEditSession edit;
  ShapeStyle style;
  /** The session's own settings copy; freed in the destructor. */
  PaintShapeSettings settings = {};
  ShapeSpaceDesc space_desc;
  /** The runtime space the backend projects through; owned here so the backend's pointer stays
   * valid for the session's lifetime. Declared before #backend so it outlives it. */
  std::unique_ptr<ShapeSpace> space;
  std::unique_ptr<ShapeTargetBackend> backend;
  /** The shape's UV projection for the linked Image Editors; built lazily. */
  std::unique_ptr<ShapeUVTraceCache> uv_trace;

  /** The draw operator's modal is driving the session (blocks a second re-invoke). */
  bool modal_active = false;
  /** The live preview was skipped during a drag (only the contour overlay followed the cursor);
   * the pose at the end of the gesture is composited once. */
  bool preview_stale = false;
  /** Runtime "Transform" flag (cage of a parametric shape) toggled from the UI; never persisted. */
  bool transform_mode = false;
  /** The cage mapping frozen for the duration of a linked Image Editor drag : captured when
   * the gesture begins and used until it ends, so the UV trace rebuilding for the contour cannot
   * make the cage jump under the cursor. Reset once no gesture is in flight. */
  std::optional<ShapeUVCageMap> drag_cage_map;
  /** Set while #drag_cage_map was armed by a press and no gesture has run yet: keeps the mapping
   * through the press-before-begin window (the reset below is lazy). Cleared once a gesture owns
   * (or releases) it. */
  bool drag_cage_map_pressed = false;

  /** Active tool idname when the session began; a different tool ends it (Apply). */
  std::string tool_id;

  ~PaintShapeSession()
  {
    BKE_paint_vector_style_free(settings);
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Accessors / lifetime
 * \{ */

PaintShapeSession *paint_shape_session_get(Object &ob)
{
  SculptSession *ss = ob.runtime ? ob.runtime->sculpt_session : nullptr;
  return ss != nullptr ? ss->paint_shape_session : nullptr;
}

const PaintShapeSession *paint_shape_session_get(const Object &ob)
{
  return paint_shape_session_get(const_cast<Object &>(ob));
}

bool paint_shape_session_active(const Object &ob)
{
  return paint_shape_session_get(ob) != nullptr;
}

bool paint_shape_session_modal_active(const PaintShapeSession &session)
{
  return session.modal_active;
}

void paint_shape_session_modal_set_active(PaintShapeSession &session, const bool active)
{
  session.modal_active = active;
}

bool paint_shape_session_event_to_shape_px(const PaintShapeSession &session,
                                           const ARegion &region,
                                           const wmEvent &event,
                                           float2 &r_px)
{
  return paint_shape_session_mval_to_shape_px(
      session, region, int2(event.mval[0], event.mval[1]), r_px);
}

bool paint_shape_session_mval_to_shape_px(const PaintShapeSession &session,
                                          const ARegion &region,
                                          const int2 &region_mval,
                                          float2 &r_px)
{
  if (session.object == nullptr) {
    r_px = float2(region_mval);
    return true;
  }
  const SurfaceAnchor &anchor = session.space_desc.anchor;
  const float4x4 object_to_world(session.object->object_to_world());
  const float4x4 world_to_object = math::invert(object_to_world);
  /* World plane through the anchor, normal from the inverse-transpose (non-uniform scale safe). */
  const float3 normal_world = math::normalize(
      math::transform_direction(math::transpose(world_to_object), anchor.normal));
  const float3 anchor_world = math::transform_point(object_to_world, anchor.co);
  const float plane[4] = {
      normal_world.x, normal_world.y, normal_world.z, -math::dot(normal_world, anchor_world)};
  const float mval[2] = {float(region_mval.x), float(region_mval.y)};
  float world[3];
  if (!ED_view3d_win_to_3d_on_plane(&region, plane, mval, false, world)) {
    /* Ray (near-)parallel to the plane: keep the last valid point, no Apply/Cancel. */
    return false;
  }
  const float3 co_object = math::transform_point(world_to_object, float3(world[0], world[1], world[2]));
  const float3 d = co_object - anchor.co;
  /* The one shared anchor frame (see #surface_anchor_frame): the same basis the space projection
   * and the cage use, so a gesture on an inclined surface lands where the contour shows it. */
  const SurfaceAnchorFrame frame = surface_anchor_frame(anchor.normal, anchor.tangent);
  r_px = float2(math::dot(d, frame.tangent), math::dot(d, frame.bitangent)) * anchor.px_per_unit;
  return true;
}

float2 paint_shape_session_shape_px_to_region(const PaintShapeSession &session,
                                              const ARegion &region,
                                              const float2 &px)
{
  float2 r_region = px;
  if (session.space != nullptr && session.object != nullptr) {
    session.space->to_region(px, region, float4x4(session.object->object_to_world()), r_region);
  }
  return r_region;
}

bool paint_shape_session_insert_preview(const PaintShapeSession &session,
                                        const ARegion &region,
                                        const int2 &region_mval,
                                        const float tolerance_px,
                                        float2 &r_point,
                                        float2 &r_tangent)
{
  float2 px;
  float2 shape_point, shape_tangent;
  if (!paint_shape_session_mval_to_shape_px(session, region, region_mval, px) ||
      !session.edit.insert_preview(px, tolerance_px, shape_point, shape_tangent))
  {
    return false;
  }
  /* The tangent is projected as a short step along it, so the perspective and the tilt of the
   * tangent plane are part of the direction the overlay shows. */
  const float2 step = math::normalize(shape_tangent) * 4.0f;
  r_point = paint_shape_session_shape_px_to_region(session, region, shape_point);
  const float2 ahead = paint_shape_session_shape_px_to_region(session, region, shape_point + step);
  r_tangent = ahead - r_point;
  return math::length_squared(r_tangent) > 1e-12f;
}

bool paint_shape_session_plane_matrix(const PaintShapeSession &session, float4x4 &r_space)
{
  if (session.object == nullptr) {
    return false;
  }
  const SurfaceAnchor &anchor = session.space_desc.anchor;
  /* The one shared anchor frame (see #surface_anchor_frame): the same basis the gesture mapping
   * and the space projection use, so the cage sits exactly on the authored contour. */
  const SurfaceAnchorFrame frame = surface_anchor_frame(anchor.normal, anchor.tangent);
  const float4x4 object_to_world(session.object->object_to_world());
  const float px_per_unit = std::max(anchor.px_per_unit, 1e-6f);
  /* One plane pixel in world units; the object scale is folded in, so a non-uniform object matrix
   * still yields the true plane the shape was authored on. */
  const float3 x_world = math::transform_direction(object_to_world, frame.tangent) / px_per_unit;
  const float3 y_world = math::transform_direction(object_to_world, frame.bitangent) / px_per_unit;
  if (math::length_squared(x_world) < SURFACE_ANCHOR_DEGENERATE_EPS ||
      math::length_squared(y_world) < SURFACE_ANCHOR_DEGENERATE_EPS)
  {
    return false;
  }
  const float3 z_world = math::normalize(math::cross(x_world, y_world));
  const float3 origin_world = math::transform_point(object_to_world, anchor.co);

  r_space = float4x4::identity();
  r_space[0] = float4(x_world, 0.0f);
  r_space[1] = float4(y_world, 0.0f);
  r_space[2] = float4(z_world, 0.0f);
  r_space[3] = float4(origin_world, 1.0f);
  return true;
}

bool paint_shape_session_tool_matches(const PaintShapeSession &session, const char *tool_id)
{
  return session.tool_id == (tool_id != nullptr ? tool_id : "");
}

/** The #ScrArea owning the session's overlay region, or null when the region no longer exists on
 * any screen (its area was closed or re-typed). */
static ScrArea *paint_shape_session_owner_area(const PaintShapeSession &session)
{
  if (session.owner_region == nullptr || session.bmain == nullptr) {
    return nullptr;
  }
  for (bScreen &screen : session.bmain->screens) {
    for (ScrArea &area : screen.areabase) {
      for (ARegion &region : area.regionbase) {
        if (&region == session.owner_region) {
          return &area;
        }
      }
    }
  }
  return nullptr;
}

bool paint_shape_session_region_alive(const PaintShapeSession &session)
{
  return paint_shape_session_owner_area(session) != nullptr;
}

/** Registered as the WM tool-change observer when the first session begins; defined after
 * #paint_shape_session_cancel, which it drives through #paint_shape_session_settle_forced. */
static void paint_shape_session_tool_change_observed(bContext &C, const bToolRef &tref);

/** \} */

/** Tag the owner region for redraw only while it still exists (a closed / re-typed area frees it;
 * the session is then about to be cancelled by the modal / undo / teardown, so skipping is safe). */
static void paint_shape_session_tag_redraw(PaintShapeSession &session)
{
  if (paint_shape_session_region_alive(session)) {
    ED_region_tag_redraw(session.owner_region);
  }
}

PaintShapeSettings *paint_shape_session_settings_get(Object &ob)
{
  PaintShapeSession *session = paint_shape_session_get(ob);
  return session != nullptr ? &session->settings : nullptr;
}

const char *paint_shape_session_tool_id(const PaintShapeSession &session)
{
  return session.tool_id.c_str();
}

bool paint_shape_session_linked_cage_map(PaintShapeSession &session,
                                         const bContext *C,
                                         ShapeUVCageMap &r_map)
{
  if (session.edit.gesture_active()) {
    /* A gesture owns the mapping now; after it ends the lazy reset below drops it. */
    session.drag_cage_map_pressed = false;
  }
  else if (!session.drag_cage_map_pressed) {
    /* No drag in flight and no fresh press capture: drop any leftover from the last drag. */
    session.drag_cage_map.reset();
  }

  if (session.drag_cage_map.has_value()) {
    r_map = *session.drag_cage_map;
    return true;
  }

  const ShapeUVTrace *trace = paint_shape_session_uv_trace(session, C);
  if (trace == nullptr || !shape_uv_trace_cage_map(*trace, r_map)) {
    return false;
  }
  if (session.edit.gesture_active()) {
    /* A gesture that began elsewhere (3D) with no press capture: keep the mapping stable for its
     * duration. */
    session.drag_cage_map = r_map;
  }
  return true;
}

void paint_shape_session_freeze_cage_map(PaintShapeSession &session, const bContext *C)
{
  const ShapeUVTrace *trace = paint_shape_session_uv_trace(session, C);
  ShapeUVCageMap map;
  if (trace == nullptr || !shape_uv_trace_cage_map(*trace, map)) {
    session.drag_cage_map.reset();
    session.drag_cage_map_pressed = false;
    return;
  }
  /* Capture the mapping as it is at the press, overriding any stale leftover. */
  session.drag_cage_map = map;
  session.drag_cage_map_pressed = true;
}

void paint_shape_session_clear_cage_map(PaintShapeSession &session)
{
  session.drag_cage_map.reset();
  session.drag_cage_map_pressed = false;
}

bool paint_shape_session_transform_active(const Object &ob)
{
  const PaintShapeSession *session = paint_shape_session_get(ob);
  return session != nullptr && session->transform_mode;
}

const PaintShape *paint_shape_session_active_shape(const Object &ob)
{
  const PaintShapeSession *session = paint_shape_session_get(ob);
  return session != nullptr ? session->edit.active() : nullptr;
}

const ShapeEditSession *paint_shape_session_edit(const Object &ob)
{
  const PaintShapeSession *session = paint_shape_session_get(ob);
  return session != nullptr ? &session->edit : nullptr;
}

void paint_shape_session_transform_toggle(Object &ob)
{
  PaintShapeSession *session = paint_shape_session_get(ob);
  if (session != nullptr) {
    session->transform_mode = !session->transform_mode;
  }
}

/** Live 3D shape sessions over all objects; the Image Editor display hooks use it as a cheap
 * redraw / draw guard. */
static int g_paint_shape_sessions_alive = 0;

bool paint_shape_session_targets_image(const PaintShapeSession &session, const Image &image)
{
  if (session.backend == nullptr) {
    return false;
  }
  Vector<const Image *> images;
  session.backend->target_images(images);
  return images.contains(&image);
}

const ShapeUVTrace *paint_shape_session_uv_trace(const PaintShapeSession &session,
                                                 const bContext *C)
{
  if (session.uv_trace == nullptr || session.object == nullptr) {
    return nullptr;
  }
  Depsgraph *depsgraph = (C != nullptr) ? CTX_data_depsgraph_pointer(C) : nullptr;
  return session.uv_trace->update(
      depsgraph, *session.object, session.space_desc, session.edit.items(), BLI_time_now_seconds());
}

static void paint_shape_session_publish(Object &ob, PaintShapeSession *session)
{
  SculptSession &ss = *ob.runtime->sculpt_session;
  ss.paint_shape_session = session;
  ss.free_paint_shape_session = paint_shape_session_discard_on_session_end;
  g_paint_shape_sessions_alive++;
}

/** Remove the session from the object and free it. Does NOT restore the preview: the caller runs
 * #ShapeTargetBackend::cancel or ::commit first (the destruction contract of the backend). */
static void paint_shape_session_free(Object &ob)
{
  SculptSession *ss = ob.runtime ? ob.runtime->sculpt_session : nullptr;
  if (ss == nullptr || ss->paint_shape_session == nullptr) {
    return;
  }
  PaintShapeSession *session = ss->paint_shape_session;
  ss->paint_shape_session = nullptr;
  ss->free_paint_shape_session = nullptr;
  g_paint_shape_sessions_alive--;
  if (session->draw_handle != nullptr && session->owner_region_type != nullptr) {
    ED_region_draw_cb_exit(session->owner_region_type, session->draw_handle);
  }
  MEM_delete(session);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name 3D edit host
 * \{ */

static ShapeVector3DInput paint_shape_3d_prepare_input(PaintShapeSession &session)
{
  ShapeVector3DInput input;
  input.style = style_from_settings(session.settings);
  if (session.scene != nullptr && session.scene->toolsettings != nullptr) {
    ToolSettings &ts = *session.scene->toolsettings;
    Paint &paint = ts.sculpt != nullptr ? ts.sculpt->paint : ts.imapaint.paint;
    style_brush_values_from_brush(paint, input.style);
    style_channels_from_brush(paint, input.style);
  }
  /* The session style still holds what the last refresh used. */
  shape_apply_style_edits_to_active(session.edit, session.style, input.style);
  const Vector<PaintShape> edited = session.edit.shapes();
  input.expanded.extend(edited);
  /* An open Line / Polyline / Curve has no interior, and with Stroke off its bounds collapse, so
   * the preview would write nothing and the session would never start. The resolved style is also
   * what the gestures hit-test with; the "previous value" scheme only reads size / rotation /
   * corner radius, which the resolve leaves alone. */
  input.style = style_resolve_for_shapes(input.expanded, input.style);
  session.style = input.style;
  return input;
}

/** Re-composite the session from the current settings. \a C may be null (a context-less refresh
 * from RNA / a listener): the backend then derives its depsgraph from the session's scene. */
static void paint_shape_session_refresh(PaintShapeSession &session, bContext *C)
{
  if (!session.backend) {
    return;
  }
  session.preview_stale = false;
  /* The Image Editor contour follows the shape edits immediately. */
  if (session.uv_trace != nullptr) {
    session.uv_trace->mark_shape_dirty();
  }
  const ShapeVector3DInput input = paint_shape_3d_prepare_input(session);
  if (input.expanded.is_empty()) {
    return;
  }
  if (session.backend->preview(C, nullptr, input.expanded, input.style)) {
    paint_shape_session_tag_redraw(session);
    if (C != nullptr && session.scene != nullptr && session.object != nullptr) {
      WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, session.object);
    }
  }
}

class ShapeVector3DHost : public VectorEditHost {
 public:
  ShapeVector3DHost(PaintShapeSession &session, const float tolerance_px)
      : session_(session), tolerance_px_(tolerance_px)
  {
  }

  VectorDocument &document() override
  {
    return session_.edit.document();
  }
  ShapeEditSession &edit() override
  {
    return session_.edit;
  }
  const ShapeStyle &style() const override
  {
    return session_.style;
  }
  PaintShapeSettings &settings() override
  {
    return session_.settings;
  }
  float hit_tolerance_px() const override
  {
    /* ViewProjector: shape pixels ARE region pixels, so the tolerance needs no rescaling. */
    return tolerance_px_;
  }
  int ref_tile() const override
  {
    return 1001;
  }
  int2 ref_tile_size() const override
  {
    return int2(1024);
  }

  void restamp(bContext *C) override
  {
    /* Image-map canvases (Material / Image) re-rasterize the shape into every channel tile and
     * re-upload it per step, far too slow to follow a drag: while a move / transform / point drag
     * runs, only the contour overlay follows the cursor and the result is composited when the
     * gesture ends (#gesture_ended). Vertex-attribute canvases are cheap and stay live. */
    if (session_.edit.gesture_active() && this->preview_is_deferred()) {
      session_.preview_stale = true;
      if (C != nullptr) {
        this->redraw(C);
      }
      return;
    }
    this->refresh(C);
  }
  void gesture_ended(bContext *C) override
  {
    if (session_.preview_stale) {
      this->refresh(C);
    }
  }
  void flush_deferred_preview(bContext *C) override
  {
    if (session_.preview_stale && !session_.edit.gesture_active()) {
      this->refresh(C);
    }
  }
  void sync_settings_from_active() override
  {
    shape_sync_settings_from_active(session_.edit, session_.settings, session_.style);
  }
  void interaction_end(bContext *C) override
  {
    /* Force the final preview tag past the throttle, so the last frame is always visible. */
    if (session_.backend) {
      session_.backend->preview_force_update(C);
    }
    /* A gizmo handle drag settles from a redraw callback, so a deferred preview is still stale
     * here and cannot be composited in place: have the next event do it. */
    if (C != nullptr && session_.preview_stale) {
      WM_operator_name_call(C,
                            "SCULPT_OT_paint_shape_flush_preview",
                            wm::OpCallContext::InvokeDefault,
                            nullptr,
                            nullptr);
    }
  }
  bool settings_apply(bContext *C, const bool is_width, const float value) override
  {
    if (is_width) {
      PaintShapeSettings &settings = session_.settings;
      const bool changed = settings.stroke_width != value;
      settings.stroke_width = value;
      if (changed) {
        this->refresh(C);
      }
      return changed;
    }
    /* Shift+F drives the Sculpt brush Strength, the shape's overall opacity. */
    if (session_.scene == nullptr || session_.scene->toolsettings == nullptr ||
        session_.scene->toolsettings->sculpt == nullptr)
    {
      return false;
    }
    Paint &paint = session_.scene->toolsettings->sculpt->paint;
    Brush *brush = BKE_paint_brush(&paint);
    if (brush == nullptr || BKE_brush_alpha_get(&paint, brush) == value) {
      return false;
    }
    BKE_brush_alpha_set(&paint, brush, value);
    this->refresh(C);
    return true;
  }
  float overall_opacity() override
  {
    if (session_.scene != nullptr && session_.scene->toolsettings != nullptr &&
        session_.scene->toolsettings->sculpt != nullptr)
    {
      Paint &paint = session_.scene->toolsettings->sculpt->paint;
      if (const Brush *brush = BKE_paint_brush_for_read(&paint)) {
        return BKE_brush_alpha_get(&paint, brush);
      }
    }
    return session_.settings.stroke_opacity;
  }
  void redraw(bContext *C) override
  {
    if (ARegion *region = CTX_wm_region(C)) {
      ED_region_tag_redraw(region);
    }
  }

 private:
  bool preview_is_deferred() const
  {
    if (session_.scene == nullptr || session_.scene->toolsettings == nullptr) {
      return false;
    }
    const int source = session_.scene->toolsettings->paint_mode.canvas_source;
    return source != PAINT_CANVAS_SOURCE_MATERIAL_PAINT &&
           source != PAINT_CANVAS_SOURCE_COLOR_ATTRIBUTE;
  }

  void refresh(bContext *C)
  {
    paint_shape_session_refresh(session_, C);
  }

  PaintShapeSession &session_;
  float tolerance_px_;
};

std::unique_ptr<VectorEditHost> paint_shape_session_host_create(bContext & /*C*/,
                                                                PaintShapeSession &session,
                                                                const float tolerance_region_px)
{
  return std::make_unique<ShapeVector3DHost>(session, tolerance_region_px);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Overlay
 * \{ */

static void paint_shape_session_overlay_draw(const bContext *C, ARegion *region, void *arg)
{
  PaintShapeSession *session = static_cast<PaintShapeSession *>(arg);
  if (session == nullptr || session->edit.items().is_empty()) {
    return;
  }
  if (CTX_wm_region(C) != session->owner_region) {
    return;
  }
  /* SurfaceAnchored: the shape lives in tangent-plane pixels; map every outline point through the
   * session space's live projection (the shared overlay draws in region pixels, above the mesh). */
  const float4x4 object_to_world(session->object != nullptr ? session->object->object_to_world() :
                                                             float4x4::identity());
  shape_draw_session_overlay(
      session->edit.items(),
      session->edit.active_index(),
      [session, region, &object_to_world](const PaintShape & /*shape*/, const float2 &p) {
        float2 r_region = p;
        if (session->space != nullptr) {
          session->space->to_region(p, *region, object_to_world, r_region);
        }
        return r_region;
      });
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Session begin / commit / cancel
 * \{ */

/** Re-anchor the session space to the current evaluated surface before a commit: the mesh may
 * have been sculpted / deformed since the session captured its anchor. Recreates the #ShapeSpace
 * and swaps the backend's pointer to it -- the backends read #Sculpt3DTargetContext::space on
 * every preview / commit call and cache nothing from it, so swapping it while both spaces are
 * alive (before the old one is destroyed) cannot dangle. False when the surface cannot be
 * restored; the caller refuses the Apply and the session stays alive. */
static bool paint_shape_session_reanchor(PaintShapeSession &session, bContext *C)
{
  /* Same gate as the one-shot apply: a restore cache (triangle or UV) is required to even try;
   * without one the captured anchor is the best there is and the commit proceeds on it. */
  if (session.space_desc.type != ShapeSpaceType::SurfaceAnchored || C == nullptr ||
      session.object == nullptr || session.backend == nullptr ||
      !(session.space_desc.anchor.tri >= 0 || session.space_desc.anchor.has_surface_uv))
  {
    return true;
  }
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  if (depsgraph == nullptr) {
    return true;
  }
  SurfaceAnchor anchor = session.space_desc.anchor;
  if (!surface_anchor_restore(*session.object, *depsgraph, anchor)) {
    return false;
  }
  session.space_desc.anchor = anchor;
  std::unique_ptr<ShapeSpace> space = shape_space_create(session.space_desc);
  session.backend->space_replace(*space);
  session.space = std::move(space);
  return true;
}

void paint_shape_session_begin(bContext *C,
                               wmOperator *op,
                               Object &ob,
                               ShapeSpaceDesc space_desc,
                               PaintShape shape,
                               const ShapeStyle &style)
{
  if (paint_shape_session_get(ob) != nullptr) {
    return;
  }
  SculptSession *ss = ob.runtime ? ob.runtime->sculpt_session : nullptr;
  Scene *scene = CTX_data_scene(C);
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  if (ss == nullptr || scene == nullptr || scene->toolsettings == nullptr ||
      depsgraph == nullptr)
  {
    return;
  }

  auto *session = MEM_new<PaintShapeSession>(__func__);
  session->object = &ob;
  session->bmain = CTX_data_main(C);
  session->scene = scene;
  session->style = style;
  session->space_desc = space_desc;
  if (const bToolRef *tool = WM_toolsystem_ref_from_context(C)) {
    session->tool_id = tool->idname;
  }
  BKE_paint_vector_style_copy(session->settings, BKE_paint_shape_settings_get(*scene->toolsettings));
  session->edit.append_shape(std::move(shape));
  session->space = shape_space_create(session->space_desc);
  session->uv_trace = std::make_unique<ShapeUVTraceCache>();
  ToolSettings &ts = *scene->toolsettings;
  Paint &paint = ts.sculpt != nullptr ? ts.sculpt->paint : ts.imapaint.paint;
  ViewLayer *view_layer = CTX_data_view_layer(C);
  if (view_layer == nullptr) {
    MEM_delete(session);
    return;
  }
  session->backend = shape_backend_create(
      ob, *scene, *view_layer, *session->space, ts, paint, session->settings);
  if (session->backend == nullptr || !session->backend->begin(*C, op->reports)) {
    if (session->backend != nullptr) {
      session->backend->cancel();
    }
    MEM_delete(session);
    return;
  }

  if (ARegion *region = CTX_wm_region(C)) {
    session->owner_region = region;
    session->owner_region_type = region->runtime->type;
    session->draw_handle = ED_region_draw_cb_activate(session->owner_region_type,
                                                      paint_shape_session_overlay_draw,
                                                      session,
                                                      REGION_DRAW_POST_PIXEL);
  }

  const ShapeVector3DInput input = paint_shape_3d_prepare_input(*session);
  if (input.expanded.is_empty() || !session->backend->preview(C, nullptr, input.expanded, input.style)) {
    if (session->draw_handle != nullptr) {
      ED_region_draw_cb_exit(session->owner_region_type, session->draw_handle);
    }
    session->backend->cancel();
    MEM_delete(session);
    return;
  }

  paint_shape_session_publish(ob, session);
  /* The session must end when its owning tool is switched away with no modal running; register
   * the (idempotent) WM observer while any session can be live. */
  WM_toolsystem_tool_change_callback_set(paint_shape_session_tool_change_observed);
  paint_shape_session_tag_redraw(*session);
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, &ob);
}

bool paint_shape_session_commit(bContext *C, Object &ob)
{
  PaintShapeSession *session = paint_shape_session_get(ob);
  if (session == nullptr || session->backend == nullptr) {
    return false;
  }
  /* The surface may have moved since the anchor was captured: re-anchor first, or refuse the
   * Apply with a clear message (the session stays alive, the user can still Esc / keep editing).
   */
  if (!paint_shape_session_reanchor(*session, C)) {
    if (C != nullptr) {
      BKE_report(CTX_wm_reports(C),
                 RPT_WARNING,
                 "Paint Shape: the surface under the shape cannot be restored (topology changed "
                 "or ambiguous UV); the shape was not applied");
    }
    return false;
  }
  const ShapeVector3DInput input = paint_shape_3d_prepare_input(*session);
  bool baked = false;
  if (input.expanded.is_empty()) {
    /* Nothing to bake: drop the preview instead of leaving it (the backend contract). */
    session->backend->cancel();
  }
  else {
    baked = session->backend->commit(
        C, C != nullptr ? CTX_wm_reports(C) : nullptr, input.expanded, input.style, "Paint Shape");
  }
  paint_shape_session_tag_redraw(*session);
  if (C != nullptr) {
    WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, &ob);
  }
  paint_shape_session_free(ob);
  return baked;
}

void paint_shape_session_cancel(bContext *C, Object &ob)
{
  PaintShapeSession *session = paint_shape_session_get(ob);
  if (session == nullptr) {
    return;
  }
  if (session->backend != nullptr) {
    session->backend->cancel();
  }
  paint_shape_session_tag_redraw(*session);
  if (C != nullptr) {
    WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, &ob);
  }
  paint_shape_session_free(ob);
}

/** Forced settle of a session that must end now (its owning tool was switched away): commit it,
 * and when the Apply is refused (the anchored surface cannot be restored) cancel it -- the
 * preview is undone and a "discarded" warning is reported, so no session is left alive without
 * the tool that owns it. True when the session was applied. */
bool paint_shape_session_settle_forced(bContext *C, Object &ob)
{
  if (paint_shape_session_get(ob) == nullptr) {
    return false;
  }
  paint_shape_session_commit(C, ob);
  if (paint_shape_session_get(ob) == nullptr) {
    return true;
  }
  /* The commit refused (session still alive): discard it. */
  paint_shape_session_cancel(C, ob);
  if (C != nullptr) {
    BKE_report(CTX_wm_reports(C),
               RPT_WARNING,
               "Paint Shape: the shape was discarded: the surface it is anchored to cannot be "
               "restored");
  }
  return false;
}

/** The WM tool-change observer: ends every live session whose owning 3D-viewport no longer runs
 * the tool the session began with. The shape modal only polices tool switches while it runs; the
 * observer covers the toolbar / keymap switches when no operator is active. A tool change in any
 * other space (the Image Editor, for one) never ends a 3D session: the guard below and the
 * owner-area's own active tool (which other spaces do not touch) both see to that. */
static void paint_shape_session_tool_change_observed(bContext &C, const bToolRef &tref)
{
  if (tref.space_type != SPACE_VIEW3D) {
    return;
  }
  Main *bmain = CTX_data_main(&C);
  if (bmain == nullptr) {
    return;
  }
  for (Object &ob : bmain->objects) {
    PaintShapeSession *session = paint_shape_session_get(ob);
    if (session == nullptr || paint_shape_session_modal_active(*session)) {
      /* A modal-driven session settles through its own tool-match check. */
      continue;
    }
    const ScrArea *owner = paint_shape_session_owner_area(*session);
    if (owner == nullptr) {
      continue;
    }
    const bToolRef *owner_tool = owner->runtime.tool;
    if (paint_shape_session_tool_matches(*session,
                                         owner_tool != nullptr ? owner_tool->idname : ""))
    {
      continue;
    }
    paint_shape_session_settle_forced(&C, ob);
  }
}

void paint_shape_session_discard_on_session_end(Object &ob)
{
  PaintShapeSession *session = paint_shape_session_get(ob);
  if (session == nullptr) {
    return;
  }
  if (session->backend != nullptr) {
    session->backend->cancel();
  }
  paint_shape_session_free(ob);
}

/** Walk every object in \a bmain that owns a live session and call \a callback with it. */
static void foreach_object_session(
    const Main &bmain, const FunctionRef<void(Object &, PaintShapeSession &)> callback)
{
  for (Object &ob : bmain.objects) {
    if (PaintShapeSession *session = paint_shape_session_get(ob)) {
      callback(ob, *session);
    }
  }
}

bool paint_shape_sessions_cancel_all(bContext *C)
{
  Main *bmain = CTX_data_main(C);
  if (bmain == nullptr) {
    return false;
  }
  bool cancelled_any = false;
  foreach_object_session(*bmain, [&](Object &ob, PaintShapeSession & /*session*/) {
    paint_shape_session_cancel(C, ob);
    cancelled_any = true;
  });
  return cancelled_any;
}

bool paint_shape_settings_update_3d(const Main *bmain, Scene * /*scene*/, PaintShapeSettings *changed)
{
  if (bmain == nullptr || changed == nullptr) {
    return false;
  }
  bool matched = false;
  foreach_object_session(*bmain, [&](Object & /*ob*/, PaintShapeSession &session) {
    if (!shape_settings_contains(session.settings, changed)) {
      return;
    }
    paint_shape_session_refresh(session, nullptr);
    matched = true;
  });
  return matched;
}

bool paint_shape_brush_update_3d(const Main *bmain, const Scene *scene, const Brush *brush)
{
  if (bmain == nullptr || scene == nullptr) {
    return false;
  }
  bool matched = false;
  foreach_object_session(*bmain, [&](Object & /*ob*/, PaintShapeSession &session) {
    if (session.scene != scene) {
      return;
    }
    if (brush != nullptr) {
      /* Only sessions whose Sculpt brush is the changed one; the brush contributes Strength /
       * Blend / channels to the preview, never a shape setting. */
      if (scene->toolsettings == nullptr || scene->toolsettings->sculpt == nullptr ||
          BKE_paint_brush_for_read(&scene->toolsettings->sculpt->paint) != brush)
      {
        return;
      }
    }
    /* Skip the re-composite when the brush-derived fields are unchanged: an NC_BRUSH /
     * ND_TOOLSETTINGS for an unrelated change (brush size, symmetry, any other setting) fires
     * often and would otherwise re-bake the whole preview on every event. */
    ShapeStyle candidate = style_from_settings(session.settings);
    if (session.scene != nullptr && session.scene->toolsettings != nullptr) {
      ToolSettings &ts = *session.scene->toolsettings;
      Paint &paint = ts.sculpt != nullptr ? ts.sculpt->paint : ts.imapaint.paint;
      style_brush_values_from_brush(paint, candidate);
      style_channels_from_brush(paint, candidate);
    }
    if (shape_brush_style_equal(candidate, session.style)) {
      return;
    }
    paint_shape_session_refresh(session, nullptr);
    matched = true;
  });
  return matched;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name ED_paint accessors
 * \{ */

}  // namespace blender::ed::sculpt_paint::shape

namespace blender {

namespace shape = ed::sculpt_paint::shape;

shape::PaintShapeSession *ED_paint_shape_session_get(Object &ob)
{
  return shape::paint_shape_session_get(ob);
}

bool ED_paint_shape_session_active(const Object &ob)
{
  return shape::paint_shape_session_active(ob);
}

PaintShapeSettings *ED_paint_shape_session_settings_get(Object &ob)
{
  return shape::paint_shape_session_settings_get(ob);
}

bool ED_paint_shape_transform_is_active(const Object &ob)
{
  return shape::paint_shape_session_transform_active(ob);
}

bool ED_paint_shape_session_shows_image(const Object &ob, const Image &image)
{
  const shape::PaintShapeSession *session = shape::paint_shape_session_get(ob);
  return session != nullptr && shape::paint_shape_session_targets_image(*session, image);
}

bool ED_paint_shape_transform_is_available(const Object &ob)
{
  const shape::PaintShape *active = shape::paint_shape_session_active_shape(ob);
  return active != nullptr && active->is_parametric() && !active->has_analytic_sdf();
}

bool ED_paint_shape_sessions_cancel_all(bContext *C)
{
  return shape::paint_shape_sessions_cancel_all(C);
}

bool ED_paint_shape_sessions_alive()
{
  return shape::g_paint_shape_sessions_alive != 0;
}

void ED_paint_shape_session_discard_on_session_end(Object &ob)
{
  shape::paint_shape_session_discard_on_session_end(ob);
}

}  // namespace blender
