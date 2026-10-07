/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The Vector mode of the shape drawing tools: a floating editing session modeled on the
 * selection gradient's. The user creates the shape with the shared input gestures; instead of
 * baking, the session opens: per-tile full backups are taken, the shape is composited as a
 * preview, and it stays movable (drag to move) until it is confirmed (Enter, the header
 * button, a tool switch) or cancelled (Esc).
 *
 * On confirm the backups are restored first and the final shapes are baked through the shared
 * compositor with the session's own targets, so the whole session collapses into a single image
 * undo step no matter which context drives the commit. On cancel the backups are restored and
 * nothing is recorded. Backups for tiles the shape moves onto are taken lazily, so a cancel
 * leaves no preview traces anywhere.
 *
 * The gestures themselves live in #ShapeVectorEditor (see #paint_vector_editor.hh); this file
 * only provides the 2D target behind it (#ImageShapeVectorHost) and the session lifetime.
 */

#include "paint_image_shape_composite.hh"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <memory>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_math_base.hh"
#include "BLI_math_color.h"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_utildefines.h"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_object_types.h"
#include "DNA_paint_vector_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_userdef_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_paint.hh"
#include "BKE_paint_vector.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"

#include "BLT_translation.hh"

#include "DEG_depsgraph.hh"

#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_space_api.hh"
#include "ED_undo.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../selection/paint_image_select_intern.hh"
#include "paint_shape_vector_3d.hh"
#include "../shapes/paint_shape_target.hh"
#include "../shapes/paint_shape_edit.hh"
#include "../shapes/paint_shape_render.hh"
#include "../shapes/paint_vector_editor.hh"
#include "../shapes/paint_vector_document.hh"

namespace blender::ed::sculpt_paint::shape {

/** Undo-step name of the Vector session's commit. It must differ from the draw operator's name
 * ("Paint Shape", its own Pixel bake's step): the redo machinery finds a step by the operator
 * name, and an F9 after a vector action must never pop this session's commit. */
constexpr char SHAPE_VECTOR_UNDO_NAME[] = "Apply Paint Shape";

/* -------------------------------------------------------------------- */
/** \name Session state
 * \{ */

struct ImageShapeVectorState : public PaintSelectFloatingSession {
  static constexpr PaintSelectTool tool_type = PaintSelectTool::Shape;

  /** The edited shapes (the model supports many; the UX keeps one for now). */
  shape::ShapeEditSession edit;
  /** Style snapshot the preview was built with; re-evaluated from the settings on edits. */
  shape::ShapeStyle style;

  /**
   * The session's write target: the PBR channel images when the material canvas source is active
   * (so the session edits the channels, like the Pixel bake), the editor's image otherwise. It
   * owns the tile backups the live preview restores from, so the session itself never touches an
   * ImBuf. Resolved once at session begin.
   */
  std::unique_ptr<shape::ShapeTargetBackend> backend;

  /** The draw operator's modal drives the session (see #image_shape_vector_modal_active). */
  bool modal_active = false;

  /** Runtime-only "Transform" mode of a parametric Polygon: shows the transform cage.
   * Never persisted. */
  bool transform_mode = false;

  /** Last canvas-symmetry signature the preview was built with; a brush / ND_TOOLSETTINGS event
   * that leaves both the brush-derived style and the symmetry unchanged skips the re-composite. */
  int symmetry_line_flag = 0;
  int symmetry_type = 0;
  float symmetry_pivot[2] = {0.0f, 0.0f};
  float symmetry_angle = 0.0f;
  float symmetry_circle_radius = 0.0f;
  float symmetry_parallel_width = 0.0f;
  int symmetry_parallel_count = 0;

  /** Region that shows the preview, for the settings-update redraw. */
  ARegion *owner_region = nullptr;

  /**
   * When set, the session edits this persisted PaintVector. Commit writes the document back
   * into the ID (one memfile step) and then bakes into the target maps. The style source is the
   * element's snapshot, not the current tool settings.
   */
  PaintVector *source_id = nullptr;
  /** `source_id->id.session_uid` at begin; a write is refused when the ID was replaced. */
  uint32_t source_session_uid = 0;
  /** Scene / Main of the session, so an ID commit can read the current settings and validate the
   * ID without a context (the context-less settle path). */
  Scene *owner_scene = nullptr;
  Main *owner_bmain = nullptr;
  /** The ID was already baked when the session opened: until Stack Layers a re-edit only writes
   * the document (no re-bake). */
  bool source_was_baked = false;

  /**
   * The session's own deep copy of `imapaint.shape`. The Image Editor UI of the owning
   * space edits this copy (exposed to RNA as `space_data.paint_shape_session_settings`), so a
   * settings change in another space never reaches the session, and the shared global block is
   * never modified while the session lives. Filled at begin from the global settings (ordinary
   * session) or from the edited element's style (`begin_from_id`); freed in the destructor.
   */
  PaintShapeSettings settings = {};

  explicit ImageShapeVectorState() : PaintSelectFloatingSession(tool_type) {}
  ~ImageShapeVectorState()
  {
    BKE_paint_vector_style_free(settings);
  }
};

/** Live Vector sessions, so a settings change refreshes every preview without scanning windows
 *. Registered at begin, removed on free. */
static Vector<ImageShapeVectorState *> g_live_sessions;

static void image_shape_vector_session_register(ImageShapeVectorState *state)
{
  g_live_sessions.append(state);
}

static void image_shape_vector_session_unregister(const ImageShapeVectorState *state)
{
  for (const int i : g_live_sessions.index_range()) {
    if (g_live_sessions[i] == state) {
      g_live_sessions.remove(i);
      return;
    }
  }
}

ImageShapeVectorState *image_shape_vector_state_get(const SpaceImage *sima)
{
  return image_select_session_get<ImageShapeVectorState>(sima);
}

ShapeEditSession *image_shape_vector_edit_get(const SpaceImage *sima)
{
  ImageShapeVectorState *state = image_shape_vector_state_get(sima);
  return state == nullptr ? nullptr : &state->edit;
}

bool image_shape_vector_transform_mode_get(const SpaceImage *sima)
{
  ImageShapeVectorState *state = image_shape_vector_state_get(sima);
  return state != nullptr && state->transform_mode;
}

void image_shape_vector_transform_mode_toggle(SpaceImage *sima)
{
  ImageShapeVectorState *state = image_shape_vector_state_get(sima);
  if (state != nullptr) {
    state->transform_mode = !state->transform_mode;
  }
}

bool image_shape_vector_is_floating_in_space(const SpaceImage *sima)
{
  return image_shape_vector_state_get(sima) != nullptr;
}

bool image_shape_vector_modal_active(const ImageShapeVectorState *state)
{
  return state->modal_active;
}

void image_shape_vector_modal_set_active(ImageShapeVectorState *state, const bool active)
{
  state->modal_active = active;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name 2D edit host
 * \{ */

/** The already-prepared input of one refresh / commit: the edited shapes expanded by canvas
 * symmetry, and the style with the final per-channel values. */
struct ShapeVectorInput {
  Vector<shape::PaintShape> expanded;
  shape::ShapeStyle style;
};

/**
 * The Image Editor target behind #VectorEditHost. It talks to the #ShapeEditSession and the write
 * backend directly; the gestures and the session history live in #ShapeVectorEditor.
 *
 * The scene is captured at construction rather than re-read from the context on every call: the
 * frontend builds the host per event, so this is the scene the frontend is acting in.
 */
class ImageShapeVectorHost : public VectorEditHost {
 public:
  ImageShapeVectorHost(ImageShapeVectorState &session, Scene *scene, const float tolerance_px)
      : session_(session), scene_(scene), tolerance_px_(tolerance_px)
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
    return tolerance_px_;
  }
  int ref_tile() const override
  {
    return session_.edit.canvas_tile().ref_tile;
  }
  int2 ref_tile_size() const override
  {
    return session_.edit.canvas_tile().ref_tile_size;
  }

  /**
   * Re-evaluate the style from the scene's tool settings and expand the edited shapes by canvas
   * symmetry, so the live preview and the commit bake exactly the same input. The Pixel bake fills
   * the channel values from the brush at bake time; the preview applies them here on every refresh
   * so it shows exactly what the commit will write. The prepared style is kept on the session for
   * the next hit test.
   */
  ShapeVectorInput prepare_input()
  {
    ShapeVectorInput input;
    /* The style comes from the session's own settings copy, never the shared global block. The
     * brush only contributes the global strength / channels (Brush params stay shared). */
    input.style = shape::style_from_settings(session_.settings);
    if (scene_ && scene_->toolsettings) {
      shape::style_brush_values_from_brush(scene_->toolsettings->imapaint.paint, input.style);
      shape::style_channels_from_brush(scene_->toolsettings->imapaint.paint, input.style);
    }
    /* The session style still holds the values the last refresh used. */
    shape::shape_apply_style_edits_to_active(session_.edit, session_.style, input.style);
    const Vector<shape::PaintShape> edited = session_.edit.shapes();
    if (scene_ && scene_->toolsettings) {
      input.expanded = shape::shapes_expand_symmetry(
          edited, session_.edit.canvas_tile(), *scene_->toolsettings);
    }
    if (input.expanded.is_empty()) {
      input.expanded.extend(edited);
    }
    session_.style = input.style;
    return input;
  }

  /** (Re)composite the live preview from freshly prepared input. This is the whole live preview;
   * the write target owns the backups and the shading-tag throttle. False when the backend is gone
   * or no tile is reachable. */
  bool refresh(bContext *C)
  {
    if (!session_.backend) {
      return false;
    }
    const ShapeVectorInput input = this->prepare_input();
    if (input.expanded.is_empty()) {
      return false;
    }
    return session_.backend->preview(C, this->reports(), input.expanded, input.style);
  }

  bool settings_apply(bContext *C, const bool is_width, const float value) override
  {
    BLI_assert(C != nullptr);
    if (!is_width) {
      /* Shift+F drives the brush/unified Strength as on brushes; the next refresh folds it into
       * the stroke and fill opacity (#style_brush_values_from_brush). */
      if (scene_ == nullptr || scene_->toolsettings == nullptr) {
        return false;
      }
      Paint &paint = scene_->toolsettings->imapaint.paint;
      Brush *brush = BKE_paint_brush(&paint);
      if (brush == nullptr || BKE_brush_alpha_get(&paint, brush) == value) {
        return false;
      }
      BKE_brush_alpha_set(&paint, brush, value);
      /* The brush values are folded into the preview on refresh; the session's own settings copy
       * is untouched, and the shared global block is not re-read. */
      this->refresh(C);
      return true;
    }
    PaintShapeSettings &settings = this->settings();
    const bool changed = settings.stroke_width != value;
    settings.stroke_width = value;
    if (changed) {
      this->refresh(C);
    }
    return changed;
  }

  float overall_opacity() override
  {
    if (scene_ != nullptr && scene_->toolsettings != nullptr) {
      const Paint &paint = scene_->toolsettings->imapaint.paint;
      if (const Brush *brush = BKE_paint_brush_for_read(&paint)) {
        return BKE_brush_alpha_get(&paint, brush);
      }
    }
    return this->settings().stroke_opacity;
  }

  void restamp(bContext *C) override
  {
    this->refresh(C);
  }
  void redraw(bContext *C) override
  {
    BLI_assert(C != nullptr);
    if (ARegion *region = CTX_wm_region(C)) {
      ED_region_tag_redraw(region);
    }
  }

  /**
   * Mirror the active shape back into the tool settings (the header/popover feedback) after a
   * gesture or an active-shape change, and keep the session's own style in step so the next
   * #prepare_input does not read the freshly written settings as a user change and apply it a
   * second time. No preview refresh is triggered (the caller already re-stamped).
   *
   * A parametric Rect/Ellipse reports its size and (wrapped) rotation; a spline-backed shape has
   * no stored angle (the geometry carries it), so the Angle slider is reset to 0 rather than
   * showing a stale value.
   */
  void sync_settings_from_active() override
  {
    shape::shape_sync_settings_from_active(session_.edit, session_.settings, session_.style);
  }

  ImageShapeVectorState &session_;
  Scene *scene_ = nullptr;
  float tolerance_px_ = 0.0f;
};

/**
 * Convert a hit tolerance given in REGION pixels into the session's shape pixels, so the grab
 * zone follows the on-screen size of the markers: the overlay draws them at a fixed region size,
 * and without this a far-zoomed tile makes a point impossible to grab while a close one makes the
 * zone enormous. Uses the same mapping as the overlay (one shape pixel measured in region pixels).
 *
 * 3D has scale 1 -- there the shape space already IS the region -- so its host can return
 * the tolerance unchanged.
 */
static float image_shape_vector_tolerance_to_shape(const ARegion *region,
                                                   const ImageShapeVectorState &state,
                                                   const float tolerance_region_px)
{
  if (region == nullptr || state.edit.items().is_empty()) {
    return tolerance_region_px;
  }
  const shape::CanvasTile tile = state.edit.canvas_tile();
  const float2 r0 = shape::shape_px_to_region(*region, tile, float2(0.0f));
  const float2 r1 = shape::shape_px_to_region(*region, tile, float2(1.0f, 0.0f));
  const float region_px_per_shape_px = math::distance(r0, r1);
  if (region_px_per_shape_px < 1e-6f) {
    /* Degenerate mapping (a collapsed view): fall back to the unscaled tolerance. */
    return tolerance_region_px;
  }
  return tolerance_region_px / region_px_per_shape_px;
}

std::unique_ptr<VectorEditHost> image_shape_vector_host_create(bContext &C,
                                                               ImageShapeVectorState &session,
                                                               const float tolerance_region_px)
{
  /* The scale must come from the region the SESSION lives in (owner_region), not from the region
   * the event happened in: a queued/foreign event can carry another region's context. */
  const float tolerance_shape_px = image_shape_vector_tolerance_to_shape(
      session.owner_region, session, tolerance_region_px);
  return std::make_unique<ImageShapeVectorHost>(session, CTX_data_scene(&C), tolerance_shape_px);
}

/** Re-composite the session from the current settings; the whole live preview. */
static bool image_shape_vector_refresh(Scene *scene, ImageShapeVectorState &state)
{
  ImageShapeVectorHost host(state, scene, 0.0f);
  return host.refresh(nullptr);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Overlay drawing
 * \{ */

static void image_shape_vector_draw(const bContext *C, ARegion *region, void *arg)
{
  ImageShapeVectorState *state = static_cast<ImageShapeVectorState *>(arg);
  if (state == nullptr || state->edit.items().is_empty()) {
    return;
  }
  if (CTX_wm_space_image(C) != state->owner_sima) {
    return;
  }

  /* The GPU work (outlines, the active shape's control points and origin) lives in the shared
   * overlay; this only maps shape-space pixels to Image Editor region pixels, so 3D can
   * reuse the same code with its own mapping. */
  const shape::CanvasTile tile = state->edit.canvas_tile();
  shape::shape_draw_session_overlay(
      state->edit.items(),
      state->edit.active_index(),
      [&](const shape::PaintShape & /*shape*/, const float2 &p) {
        return shape::shape_px_to_region(*region, tile, p);
      });
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Session lifetime
 * \{ */

void image_shape_vector_state_free(PaintSelectFloatingSession *session)
{
  ImageShapeVectorState *state = static_cast<ImageShapeVectorState *>(session);
  if (state == nullptr) {
    return;
  }
  image_select_floating_draw_handle_clear(*state);
  image_shape_vector_session_unregister(state);
  /* Defensive: leave no dangling slot (or canvas borrow) behind if a caller freed the state
   * without clearing the session first. Every current path clears explicitly, so this is a no-op
   * there, but it makes a forgotten clear a safe no-op instead of a use-after-free. */
  if (state->owner_sima != nullptr && state->owner_sima->runtime != nullptr &&
      state->owner_sima->runtime->paint_select.active == state)
  {
    image_select_session_clear(state->owner_sima);
  }
  MEM_delete(state);
}

/** Shared tail of both session-begin paths: resolve the target, preview once and install the
 * session. Returns false (and frees \a state) when nothing can be written or previewed. */
[[nodiscard]] static bool image_shape_vector_session_open(bContext *C,
                                                          SpaceImage *sima,
                                                          ImageShapeVectorState *state)
{
  state->owner_scene = CTX_data_scene(C);
  state->owner_bmain = CTX_data_main(C);
  /* Resolve the write target; it owns the per-tile backups and the preview, the session only asks
   * it to preview. */
  state->backend = std::make_unique<shape::ImageTilesBackend>(state->settings,
                                                              state->edit.canvas_tile());
  if (!state->backend->begin(*C, CTX_wm_reports(C))) {
    image_shape_vector_state_free(state);
    return false;
  }
  if (!image_shape_vector_refresh(CTX_data_scene(C), *state)) {
    /* No tile the shape reaches: nothing to preview, so do not open a session. */
    image_shape_vector_state_free(state);
    return false;
  }

  if (ARegion *region = CTX_wm_region(C)) {
    state->owner_region = region;
    state->owner_region_type = region->runtime->type;
    state->draw_handle = ED_region_draw_cb_activate(
        state->owner_region_type, image_shape_vector_draw, state, REGION_DRAW_POST_PIXEL);
  }

  image_select_session_set(sima, state);
  image_shape_vector_session_register(state);
  ED_region_tag_redraw(CTX_wm_region(C));
  return true;
}

void image_shape_vector_session_begin(bContext *C,
                                      SpaceImage *sima,
                                      shape::PaintShape shape,
                                      const shape::CanvasTile &tile,
                                      const shape::ShapeStyle &style)
{
  if (!sima || !sima->image) {
    return;
  }
  /* Any session floating here first — including a previous Shape session, which
   * #image_select_floating_sessions_end() deliberately ignores for the same tool — must be
   * settled before this one takes the slot: a new begin always allocates fresh state and would
   * otherwise overwrite (and leak) the old one, tripping the empty-slot assert in
   * #image_select_session_set. Its edit is committed per its own takeover semantics. */
  image_select_floating_sessions_end_all(C, sima);

  auto *state = MEM_new<ImageShapeVectorState>(__func__);
  state->owner_sima = sima;
  state->style = style;
  if (Scene *scene = CTX_data_scene(C)) {
    if (scene->toolsettings) {
      /* The session owns a deep copy of the settings; the shared global block is never touched
       * so nothing has to be restored on teardown. */
      PaintShapeSettings &global = BKE_paint_shape_settings_get(*scene->toolsettings);
      BKE_paint_vector_style_copy(state->settings, global);
      shape::style_channels_from_brush(scene->toolsettings->imapaint.paint, state->style);
    }
  }
  state->edit.append_shape(std::move(shape), tile);
  state->iuser = sima->iuser;

  /* A false return already freed the state; this wrapper owns nothing else. */
  if (!image_shape_vector_session_open(C, sima, state)) {
    return;
  }
}

void image_shape_vector_session_begin_from_id(
    bContext *C, SpaceImage *sima, PaintVector &pv, int item_index)
{
  if (!sima || !sima->image || item_index < 0 || item_index >= pv.items_num) {
    return;
  }
  image_select_floating_sessions_end_all(C, sima);

  shape::VectorDocument doc;
  shape::paint_vector_to_document(pv, doc);
  if (doc.items.is_empty()) {
    return;
  }

  Scene *scene = CTX_data_scene(C);
  if (scene == nullptr || scene->toolsettings == nullptr) {
    return;
  }

  auto *state = MEM_new<ImageShapeVectorState>(__func__);
  state->owner_sima = sima;
  state->source_id = &pv;
  state->source_session_uid = pv.id.session_uid;
  state->source_was_baked = BKE_paint_vector_was_baked(pv);
  /* The element's style becomes the session's own settings copy (deep copy, profiles
   * included), so the header, popovers and F/Shift+F edit this element's style, and commit writes
   * it back (self-contained snapshot). The shared global block is never touched. */
  BKE_paint_vector_style_copy(state->settings, pv.items[item_index].style);
  state->style = shape::style_from_settings(state->settings);
  shape::vector_document_set_items(state->edit.document(), std::move(doc.items));
  state->edit.active_set(std::clamp(item_index, 0, state->edit.shapes_num() - 1));
  state->iuser = sima->iuser;

  /* A false return already freed the state; this wrapper owns nothing else. */
  if (!image_shape_vector_session_open(C, sima, state)) {
    return;
  }
}

/**
 * A self-contained settings snapshot for persisting an element : the current tool settings,
 * with the active brush's PBR channels folded into the per-part channel arrays and the override
 * flag pinned. A future rebake then does not depend on the current brush.
 *
 * \note The caller owns the returned snapshot's profiles; free them with
 * #BKE_paint_vector_style_free once it has been written into the ID.
 */
static PaintShapeSettings paint_vector_settings_snapshot(const PaintShapeSettings &settings,
                                                         Paint *paint)
{
  PaintShapeSettings snapshot = {};
  BKE_paint_vector_style_copy(snapshot, settings);

  ShapeStyle style = shape::style_from_settings(settings);
  if (paint != nullptr) {
    style_channels_from_brush(*paint, style);
  }
  for (int i = 0; i < PAINT_MATERIAL_CHANNEL_NUM; i++) {
    snapshot.stroke_channels[i] = style.stroke_channels[i];
    snapshot.fill_channels[i] = style.fill_channels[i];
    /* #style_from_settings converted the DNA colors (picked sRGB) to scene linear; store them
     * back in the DNA convention so the next read does not convert twice. */
    linearrgb_to_srgb_v3_v3(snapshot.stroke_channels[i].color, style.stroke_channels[i].color);
    linearrgb_to_srgb_v3_v3(snapshot.fill_channels[i].color, style.fill_channels[i].color);
  }
  snapshot.flag |= PAINT_SHAPE_CHANNELS_OVERRIDE;
  return snapshot;
}

/**
 * Give the session's style to the element being edited and to every element that has none yet
 * (shapes added during the session). The other elements keep the style they carry.
 *
 * \note This runs at commit, outside any gesture, so the undo history is deliberately left alone:
 * the history only restores geometry-relevant states and the style is re-applied on the next
 * commit.
 */
static void image_shape_vector_style_apply(shape::ShapeEditSession &edit,
                                           const PaintShapeSettings &snapshot)
{
  const shape::VectorItemStyle style = shape::vector_item_style_make(snapshot);
  for (shape::VectorItem &item : edit.document().items) {
    if (!item.style) {
      item.style = style;
    }
  }
  if (shape::VectorItem *active = edit.active_item()) {
    active->style = style;
  }
}

/** Bake the current shapes with a single undo step and tear the session down. The write target
 * restores its preview backups before the undo-captured bake; the context is only used for the
 * scene and the active object, which are shared per window. */
static void image_shape_vector_commit(bContext *C, ImageShapeVectorState *state)
{
  /* \a C may be null (a settle from a context-less path): the style then stays as last previewed
   * and the symmetry copies are skipped (an edge case). The ID write still happens, using the
   * session's stored scene. */
  Scene *scene = C ? CTX_data_scene(C) : state->owner_scene;
  ImageShapeVectorHost host(*state, scene, 0.0f);
  const ShapeVectorInput input = host.prepare_input();

  /* An ID-backed session writes the document back before baking. If the ID was removed
   * (undo / deletion) during the session, never touch it: cancel the preview and report. The
   * stored pointer may already be dangling, so it is only compared, never dereferenced, until the
   * session_uid lookup confirms it is still alive. */
  if (state->source_id != nullptr) {
    if (state->owner_bmain != nullptr) {
      ID *found = BKE_libblock_find_session_uid(
          state->owner_bmain, ID_PV, state->source_session_uid);
      if (found != id_cast<ID *>(state->source_id)) {
        image_select_session_clear(state->owner_sima);
        state->backend->cancel();
        if (C != nullptr) {
          BKE_report(
              CTX_wm_reports(C),
              RPT_WARNING,
              RPT_("Paint Shape: the edited Paint Vector was removed; the edit was discarded"));
        }
        image_shape_vector_state_free(state);
        return;
      }
    }
    /* Write the session's own settings copy back (self-contained snapshot): the session edits that
     * copy, so this keeps preview, bake and the persisted document in sync. The global block was
     * never touched. */
    Paint *paint = (scene != nullptr && scene->toolsettings != nullptr) ?
                       &scene->toolsettings->imapaint.paint :
                       nullptr;
    PaintShapeSettings snapshot = paint_vector_settings_snapshot(state->settings, paint);
    image_shape_vector_style_apply(state->edit, snapshot);
    shape::paint_vector_from_document(*state->source_id, state->edit.document());
    BKE_paint_vector_style_free(snapshot);
    BKE_paint_vector_tag_changed(*state->source_id);
    /* One memfile step captures the document write; the bake below opens its own image step
     * (without a context, a settle, no memfile step is created). */
    if (C != nullptr) {
      ED_undo_memfile_push(C, "Paint Vector Edit");
    }
  }

  image_select_session_clear(state->owner_sima);
  if (state->source_id != nullptr && state->source_was_baked) {
    /* Before Stack Layers a re-edit of an already baked PaintVector does **not** re-bake :
     * baking over the existing map would accumulate a ghost / double relief. Restore the preview
     * to the session-start (baked) pixels; the document write above is the result, and
     * `needs_rebake` stays true. */
    state->backend->cancel();
    if (C != nullptr) {
      BKE_report(
          CTX_wm_reports(C),
          RPT_INFO,
          RPT_("Paint Shape: changes saved in the Paint Vector; re-baking into the maps will be "
               "available with Stack Layers"));
    }
  }
  else {
    /* The backend refuses to bake into a target Image an undo / material edit replaced.
     * Surface that as a visible refusal instead of silently dropping the edit; the session is
     * still torn down (there is nothing left to edit into). */
    const bool baked = state->backend->commit(
        C, C != nullptr ? CTX_wm_reports(C) : nullptr, input.expanded, input.style, SHAPE_VECTOR_UNDO_NAME);
    if (!baked && C != nullptr && !state->backend->targets_alive()) {
      BKE_report(CTX_wm_reports(C),
                 RPT_WARNING,
                 RPT_("Paint Shape: a paint target was removed during editing; the shape was not "
                      "saved"));
    }
    if (state->source_id != nullptr && baked) {
      BKE_paint_vector_tag_baked(*state->source_id);
    }
  }
  image_shape_vector_state_free(state);
}

void image_shape_vector_session_end_for_takeover(bContext *C, SpaceImage *sima)
{
  ImageShapeVectorState *state = image_shape_vector_state_get(sima);
  if (!state) {
    return;
  }
  /* Committed rather than discarded: switching tools with an unconfirmed shape bakes it and
   * pushes a complete undo step, keeping the user's in-progress edit (the gradient's takeover
   * semantics). Explicit cancel still restores the backups.
   *
   * When \a C is this editor's context, the commit runs through the apply operator so the apply
   * op registers and a later F9 resolves to it instead of an older Pixel bake whose step would
   * undo this commit along with itself (see the flag comment on
   * #PAINT_OT_image_shape_vector_apply). From a foreign context (Save All settling every editor,
   * a cross-editor tool switch) it commits directly: the F9 anchor stays the operator that
   * caused the takeover, whose own undo step has an unrelated name, so it never pops this
   * commit. */
  if (C && CTX_wm_space_image(C) == sima) {
    WM_operator_name_call(
        C, "PAINT_OT_image_shape_vector_apply", wm::OpCallContext::InvokeDefault, nullptr, nullptr);
    return;
  }
  image_shape_vector_commit(C, state);
}

void image_shape_vector_session_cancel(bContext *C, SpaceImage *sima)
{
  ImageShapeVectorState *state = image_shape_vector_state_get(sima);
  if (!state) {
    return;
  }
  image_select_session_clear(sima);
  state->backend->cancel();
  image_shape_vector_state_free(state);
  /* \a C is null on editor teardown (the space-free path cancels without a context). */
  if (C) {
    if (ARegion *region = CTX_wm_region(C)) {
      ED_region_tag_redraw(region);
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Apply operator body
 * \{ */

/** Commit the live session of the current Image Editor: restore the backups, bake the shapes
 * with a single undo step, free. The apply operator's body; also driven by the creation
 * operator's modal (Enter). */
static wmOperatorStatus image_shape_vector_session_apply(bContext *C)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  ImageShapeVectorState *state = image_shape_vector_state_get(sima);
  if (!state) {
    return OPERATOR_CANCELLED;
  }
  image_shape_vector_commit(C, state);
  if (ARegion *region = CTX_wm_region(C)) {
    ED_region_tag_redraw(region);
  }
  return OPERATOR_FINISHED;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Paint Vector ID backend
 * \{ */

/** Save the live session's document into a new PaintVector and link it to the active material.
 * The session stays live and becomes ID-backed, so a later Apply writes back and bakes. */
bool image_shape_vector_session_save_to_id(bContext *C, SpaceImage *sima, const char *name)
{
  ImageShapeVectorState *state = image_shape_vector_state_get(sima);
  if (state == nullptr || state->source_id != nullptr) {
    return false;
  }
  Scene *scene = CTX_data_scene(C);
  Object *ob = CTX_data_active_object(C);
  if (scene == nullptr || scene->toolsettings == nullptr || ob == nullptr) {
    return false;
  }
  Material *ma = BKE_object_material_get(ob, ob->actcol);
  if (ma == nullptr) {
    return false;
  }

  Main *bmain = CTX_data_main(C);
  PaintVector *pv = BKE_paint_vector_add(bmain, name);
  PaintShapeSettings snapshot = paint_vector_settings_snapshot(
      state->settings, &scene->toolsettings->imapaint.paint);
  image_shape_vector_style_apply(state->edit, snapshot);
  shape::paint_vector_from_document(*pv, state->edit.document());
  BKE_paint_vector_style_free(snapshot);
  BKE_paint_vector_tag_changed(*pv);
  BKE_paint_vector_material_link(*ma, *pv);
  /* `BKE_id_new` gave one user, the ID-property link added another; the material link is now the
   * single owner ("create + assign"). */
  id_us_min(&pv->id);

  state->source_id = pv;
  state->source_session_uid = pv->id.session_uid;
  /* A freshly saved document has never been baked: the next Apply bakes it. */
  state->source_was_baked = false;

  ED_undo_memfile_push(C, "Save Paint Vector");
  return true;
}

/** Open the live 2D session from a PaintVector linked to the active material. */
bool image_shape_vector_session_open_id(bContext *C,
                                        SpaceImage *sima,
                                        const char *name,
                                        const int item_index)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr) {
    return false;
  }
  Material *ma = BKE_object_material_get(ob, ob->actcol);
  if (ma == nullptr) {
    return false;
  }
  PaintVector *pv = (name != nullptr && name[0] != '\0') ?
                        BKE_paint_vector_material_get(*ma, name) :
                        BKE_paint_vector_material_get_index(*ma, 0);
  if (pv == nullptr) {
    return false;
  }
  image_shape_vector_session_begin_from_id(C, sima, *pv, item_index);
  return image_shape_vector_state_get(sima) != nullptr;
}

/** \} */

}  // namespace blender::ed::sculpt_paint::shape

namespace blender {

namespace shape = ed::sculpt_paint::shape;

/* -------------------------------------------------------------------- */
/** \name Confirm / cancel operators
 * \{ */

static bool image_shape_vector_apply_poll(bContext *C)
{
  if (shape::image_shape_vector_state_get(CTX_wm_space_image(C)) != nullptr) {
    return true;
  }
  /* A live 3D Sculpt shape session shown by this Image Editor: Enter applies it. */
  return ED_image_paint_shape3d_session_linked(C);
}

static wmOperatorStatus image_shape_vector_apply_exec(bContext *C, wmOperator * /*op*/)
{
  if (shape::image_shape_vector_state_get(CTX_wm_space_image(C)) != nullptr) {
    return shape::image_shape_vector_session_apply(C);
  }
  return ED_image_paint_shape3d_session_apply(C) ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

static wmOperatorStatus image_shape_vector_cancel_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (shape::image_shape_vector_state_get(sima)) {
    shape::image_shape_vector_session_cancel(C, sima);
    return OPERATOR_FINISHED;
  }
  return ED_image_paint_shape3d_session_cancel(C) ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

void PAINT_OT_image_shape_vector_apply(wmOperatorType *ot)
{
  ot->name = "Apply Shape";
  ot->idname = "PAINT_OT_image_shape_vector_apply";
  ot->description = "Bake the shape being edited into the texture";
  ot->exec = image_shape_vector_apply_exec;
  ot->poll = image_shape_vector_apply_poll;
  /* No OPTYPE_UNDO: the commit opens and closes its own image undo step named
   * #SHAPE_VECTOR_UNDO_NAME inside the compositor (the convention of the image-undo-pushing
   * operators of this feature, see #IMAGE_SELECT_GESTURE_OPTYPE_FLAGS in
   * paint_image_select_mask.cc).
   *
   * OPTYPE_REGISTER is still needed: the commit must become the last registered operator, or an
   * F9 after it would fall back to an older Pixel "Paint Shape" operator and pop every step
   * above that one -- erasing this commit (#ED_undo_pop_op undoes everything above the step it
   * finds by name). With this op registered, F9 resolves to *this* operator, whose poll fails
   * once the session is gone, so the redo safely does nothing instead. */
  ot->flag = OPTYPE_REGISTER;
}

void PAINT_OT_image_shape_vector_cancel(wmOperatorType *ot)
{
  ot->name = "Cancel Shape";
  ot->idname = "PAINT_OT_image_shape_vector_cancel";
  ot->description = "Discard the shape being edited and restore the original texture";
  ot->exec = image_shape_vector_cancel_exec;
  ot->poll = image_shape_vector_apply_poll;
  /* Unregistered: a cancel records nothing, so it must not become the redo anchor either. */
  ot->flag = 0;
}

/* -------------------------------------------------------------------- */
/** \name Session undo
 * \{ */

static bool image_shape_vector_undo_poll(bContext *C)
{
  /* Only the linked 3D Sculpt session: an Image Vector session is owned by its own modal, whose
   * Ctrl+Z handling already steps the stack (and passes an empty stack through to global undo). */
  return ED_image_paint_shape3d_session_linked(C);
}

/** Steps the linked 3D session's undo stack. The 3D session has no modal in this editor, so this
 * routes Ctrl+Z to the shape instead of the global stack (which would cancel the session). An
 * empty stack cancels the session, exactly like the 3D modal's own Ctrl+Z handling. */
static wmOperatorStatus image_shape_vector_undo_exec(bContext *C, wmOperator * /*op*/)
{
  Object *ob = CTX_data_active_object(C);
  shape::PaintShapeSession *session = ob != nullptr ? shape::paint_shape_session_get(*ob) :
                                                      nullptr;
  if (session == nullptr) {
    return OPERATOR_CANCELLED;
  }
  std::unique_ptr<shape::VectorEditHost> host = shape::paint_shape_session_host_create(
      *C, *session, 0.0f);
  shape::ShapeVectorEditor editor;
  if (editor.undo(C, *host)) {
    return OPERATOR_FINISHED;
  }
  shape::paint_shape_session_cancel(C, *ob);
  return OPERATOR_CANCELLED;
}

void PAINT_OT_image_shape_vector_undo(wmOperatorType *ot)
{
  ot->name = "Undo Shape Edit";
  ot->idname = "PAINT_OT_image_shape_vector_undo";
  ot->description = "Step the shape session's own undo, keeping Ctrl+Z off the global stack";
  ot->exec = image_shape_vector_undo_exec;
  ot->poll = image_shape_vector_undo_poll;
  /* No undo step of its own: it steps the session's in-memory history. */
  ot->flag = 0;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Transform-mode toggle
 * \{ */

bool ED_image_shape_transform_is_active(SpaceImage *sima)
{
  return shape::image_shape_vector_transform_mode_get(sima);
}

static bool image_shape_transform_toggle_poll(bContext *C)
{
  if (shape::image_shape_vector_state_get(CTX_wm_space_image(C)) != nullptr) {
    return true;
  }
  /* A linked 3D Sculpt session: the button toggles its cage. */
  return ED_image_paint_shape3d_session_linked(C);
}

static wmOperatorStatus image_shape_transform_toggle_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (shape::image_shape_vector_state_get(sima)) {
    shape::image_shape_vector_transform_mode_toggle(sima);
  }
  else {
    Object *ob = CTX_data_active_object(C);
    if (ob == nullptr || !ED_image_paint_shape3d_session_linked(C)) {
      return OPERATOR_CANCELLED;
    }
    /* Same toggle the 3D Viewport's button runs; notify so the cage updates there and in every
     * linked Image Editor, not just in this region. */
    shape::paint_shape_session_transform_toggle(*ob);
    WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
  }
  if (ARegion *region = CTX_wm_region(C)) {
    ED_region_tag_redraw(region);
  }
  return OPERATOR_FINISHED;
}

void PAINT_OT_image_shape_transform_toggle(wmOperatorType *ot)
{
  ot->name = "Toggle Shape Transform";
  ot->idname = "PAINT_OT_image_shape_transform_toggle";
  ot->description = "Show the transform cage of the active Polygon shape";
  ot->exec = image_shape_transform_toggle_exec;
  ot->poll = image_shape_transform_toggle_poll;
  /* UI state only; records no undo step. */
  ot->flag = 0;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Paint Vector ID operators
 * \{ */

static bool vector_save_poll(bContext *C)
{
  return shape::image_shape_vector_state_get(CTX_wm_space_image(C)) != nullptr;
}

static wmOperatorStatus vector_save_exec(bContext *C, wmOperator *op)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (sima == nullptr ||
      !shape::image_shape_vector_session_save_to_id(C, sima, "Paint Vector"))
  {
    BKE_report(op->reports,
               RPT_WARNING,
               RPT_("Paint Vector: no live shape session or the active object has no material"));
    return OPERATOR_CANCELLED;
  }
  if (ARegion *region = CTX_wm_region(C)) {
    ED_region_tag_redraw(region);
  }
  return OPERATOR_FINISHED;
}

static bool vector_edit_poll(bContext *C)
{
  if (CTX_wm_space_image(C) == nullptr) {
    return false;
  }
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr) {
    return false;
  }
  Material *ma = BKE_object_material_get(ob, ob->actcol);
  return ma != nullptr && BKE_paint_vector_material_count(*ma) > 0;
}

static wmOperatorStatus vector_edit_exec(bContext *C, wmOperator *op)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  char name[MAX_ID_NAME - 2] = "";
  RNA_string_get(op->ptr, "paint_vector", name);
  const int item = RNA_int_get(op->ptr, "item");
  if (sima == nullptr ||
      !shape::image_shape_vector_session_open_id(C, sima, name, item))
  {
    BKE_report(op->reports,
               RPT_WARNING,
               RPT_("Paint Vector: no such Paint Vector on the active material"));
    return OPERATOR_CANCELLED;
  }
  return OPERATOR_FINISHED;
}

void PAINT_OT_vector_save(wmOperatorType *ot)
{
  ot->name = "Save Paint Vector";
  ot->idname = "PAINT_OT_vector_save";
  ot->description =
      "Save the shapes being edited into a new Paint Vector linked to the active material";
  ot->exec = vector_save_exec;
  ot->poll = vector_save_poll;
  /* The document write pushes its own memfile step; OPTYPE_UNDO would add a duplicate. */
  ot->flag = OPTYPE_REGISTER;
}

void PAINT_OT_vector_edit(wmOperatorType *ot)
{
  ot->name = "Edit Paint Vector";
  ot->idname = "PAINT_OT_vector_edit";
  ot->description = "Open a Paint Vector of the active material for editing";
  ot->exec = vector_edit_exec;
  ot->poll = vector_edit_poll;
  ot->flag = OPTYPE_REGISTER;

  RNA_def_string(ot->srna,
                 "paint_vector",
                 nullptr,
                 MAX_ID_NAME - 2,
                 "Paint Vector",
                 "Name of the Paint Vector to edit (empty: the first one)");
  RNA_def_int(ot->srna,
              "item",
              0,
              0,
              INT_MAX,
              "Item",
              "Element index to edit",
              0,
              INT_MAX);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Settings update
 * \{ */

void ED_paint_shape_settings_update(Main *bmain, Scene *scene, PaintShapeSettings *changed)
{
  if (bmain == nullptr || changed == nullptr) {
    return;
  }

  /* A live Vector session owns a deep copy of the settings, so a change to the shared
   * global block must NOT refresh it; only a change to a session's own copy refreshes that
   * session. Address containment (not exact equality) also catches the channel-value properties,
   * whose setter passes the address of one array element. */
  for (shape::ImageShapeVectorState *state : shape::g_live_sessions) {
    if (!shape::shape_settings_contains(state->settings, changed)) {
      continue;
    }
    shape::image_shape_vector_refresh(scene, *state);
    if (state->owner_region != nullptr) {
      ED_region_tag_redraw(state->owner_region);
    }
  }

  /* The Sculpt Mode Vector sessions own their copy too; refresh the matching one. */
  shape::paint_shape_settings_update_3d(bmain, scene, changed);
}

/** Compare the canvas-symmetry fields the 2D preview expands with against the session's snapshot. */
static bool image_shape_symmetry_equal(const shape::ImageShapeVectorState &state,
                                       const ToolSettings &ts)
{
  const ImagePaintSettings &ip = ts.imapaint;
  return state.symmetry_line_flag == ip.symmetry_line_flag &&
         state.symmetry_type == ip.symmetry_type &&
         state.symmetry_pivot[0] == ip.symmetry_line_pivot[0] &&
         state.symmetry_pivot[1] == ip.symmetry_line_pivot[1] &&
         state.symmetry_angle == ip.symmetry_line_angle &&
         state.symmetry_circle_radius == ip.symmetry_circle_radius &&
         state.symmetry_parallel_width == ip.symmetry_parallel_width &&
         state.symmetry_parallel_count == ip.symmetry_parallel_count;
}

static void image_shape_symmetry_store(shape::ImageShapeVectorState &state, const ToolSettings &ts)
{
  const ImagePaintSettings &ip = ts.imapaint;
  state.symmetry_line_flag = ip.symmetry_line_flag;
  state.symmetry_type = ip.symmetry_type;
  state.symmetry_pivot[0] = ip.symmetry_line_pivot[0];
  state.symmetry_pivot[1] = ip.symmetry_line_pivot[1];
  state.symmetry_angle = ip.symmetry_line_angle;
  state.symmetry_circle_radius = ip.symmetry_circle_radius;
  state.symmetry_parallel_width = ip.symmetry_parallel_width;
  state.symmetry_parallel_count = ip.symmetry_parallel_count;
}

void ED_paint_shape_brush_update(const Main *bmain, const Scene *scene, const Brush *brush)
{
  if (bmain == nullptr || scene == nullptr) {
    return;
  }
  for (shape::ImageShapeVectorState *state : shape::g_live_sessions) {
    /* Only sessions that actually read this brush (their scene's image-paint brush); the brush
     * contributes Strength/blend/channels to the preview, never a shape setting. */
    if (state->owner_scene != scene || scene->toolsettings == nullptr) {
      continue;
    }
    if (brush != nullptr &&
        BKE_paint_brush_for_read(&scene->toolsettings->imapaint.paint) != brush)
    {
      continue;
    }
    /* Skip the re-composite when neither the brush-derived fields nor the canvas symmetry changed:
     * NC_BRUSH / ND_TOOLSETTINGS fire for every unrelated change (brush size, any other setting),
     * and re-baking the whole preview on each would lag the live session. */
    ToolSettings &ts = *scene->toolsettings;
    shape::ShapeStyle candidate = shape::style_from_settings(state->settings);
    shape::style_brush_values_from_brush(ts.imapaint.paint, candidate);
    shape::style_channels_from_brush(ts.imapaint.paint, candidate);
    if (shape::shape_brush_style_equal(candidate, state->style) &&
        image_shape_symmetry_equal(*state, ts))
    {
      continue;
    }
    /* Refresh through the session's own (non-const) scene pointer; it is the same scene. */
    shape::image_shape_vector_refresh(state->owner_scene, *state);
    image_shape_symmetry_store(*state, ts);
    if (state->owner_region != nullptr) {
      ED_region_tag_redraw(state->owner_region);
    }
  }

  /* Same for the Sculpt Mode Vector sessions. */
  shape::paint_shape_brush_update_3d(bmain, scene, brush);
}

PaintShapeSettings *ED_image_shape_session_settings_get(SpaceImage *sima)
{
  shape::ImageShapeVectorState *state = shape::image_shape_vector_state_get(sima);
  return state == nullptr ? nullptr : &state->settings;
}

/** \} */

}  // namespace blender
