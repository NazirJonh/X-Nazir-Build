/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Idle-hover state for the Extract Loop tool: the #g_hover_state singleton and
 * the hover API consumed by the paint cursor. The tool-agnostic scaffolding
 * (modal BMesh construction, session validity tracking, preview-position
 * refresh) lives in the shared #extract layer.
 */

#include "BKE_context.hh"
#include "BKE_object.hh"

#include "DEG_depsgraph.hh"

#include "ED_view3d.hh"

#include "bmesh.hh"

#include "sculpt_extract_loop_intern.hh"
#include "sculpt_intern.hh"

namespace blender::ed::sculpt_paint::extract_loop {

/* -------------------------------------------------------------------- */
/** \name Hover State
 * \{ */

struct HoverState {
  ExtractLoopSharedData shared;
  bool is_initialized = false;
  bool is_activated = false;
};

static HoverState &g_hover_state_get()
{
  static HoverState state;
  return state;
}

/**
 * (Re)build the modal BMesh and preview positions via the shared scaffolding,
 * then clear the loop-specific walker/preview results.
 */
static bool loop_hover_setup_bmesh(bContext *C, ExtractLoopSharedData &shared, Object *obact)
{
  if (!extract::hover_setup_bmesh(C, shared.base, obact)) {
    return false;
  }
  shared.loop_edges.clear();
  shared.preview_points.clear();
  shared.seed_edge = nullptr;
  shared.is_cyclic = false;
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Hover API
 * \{ */

void extract_loop_hover_init(bContext *C,
                             Object *ob,
                             ARegion *region,
                             RegionView3D *rv3d,
                             ExtractionMode mode,
                             LoopOrientation loop_orientation)
{
  HoverState &state = g_hover_state_get();

  if (state.shared.base.bm) {
    BM_mesh_free(state.shared.base.bm);
    state = HoverState{};
  }

  state.shared.base.region = region;
  state.shared.base.rv3d = rv3d;
  state.shared.mode = mode;
  state.shared.loop_orientation = loop_orientation;

  if (!loop_hover_setup_bmesh(C, state.shared, ob)) {
    return;
  }

  state.is_initialized = true;
}

void extract_loop_hover_free()
{
  HoverState &state = g_hover_state_get();
  if (state.shared.base.bm) {
    BM_mesh_free(state.shared.base.bm);
    state.shared.base.bm = nullptr;
  }
  state.shared.base.preview_faces.clear();
  state.shared.preview_points.clear();
  state.shared.loop_edges.clear();
  state.shared.seed_edge = nullptr;
  state.shared.base.obact = nullptr;
  state.shared.base.session_mesh = nullptr;
  state.is_initialized = false;
  state.is_activated = false;
}

void extract_loop_hover_update(bContext *C,
                               const float2 &mval,
                               ExtractionMode mode,
                               LoopOrientation loop_orientation)
{
  HoverState &state = g_hover_state_get();
  if (!state.is_initialized) {
    return;
  }

  Object *obact = CTX_data_active_object(C);
  if (!obact) {
    extract_loop_hover_free();
    return;
  }

  state.shared.mode = mode;
  state.shared.loop_orientation = loop_orientation;
  state.shared.base.region = CTX_wm_region(C);
  state.shared.base.rv3d = CTX_wm_region_view3d(C);

  if (!extract::hover_session_is_valid(state.shared.base, obact)) {
    if (!loop_hover_setup_bmesh(C, state.shared, obact)) {
      extract_loop_hover_free();
      return;
    }
  }
  else {
    extract::hover_refresh_preview_positions(C, state.shared.base, obact);
  }

  if (!state.shared.base.bm) {
    extract_loop_hover_free();
    return;
  }

  state.shared.preview_points.clear();
  state.shared.base.preview_faces.clear();
  state.shared.loop_edges.clear();
  state.shared.seed_edge = nullptr;
  state.shared.is_cyclic = false;

  CursorGeometryInfo cgi;
  if (!cursor_geometry_info_update(C, &cgi, mval, false)) {
    return;
  }
  state.shared.base.hit_location = cgi.location;

  state.shared.seed_edge = find_seed_edge_screen_space(state.shared, mval);
  if (!state.shared.seed_edge) {
    return;
  }

  bool dummy = false;
  run_walker(state.shared, true /* hover always follows boundary */, &dummy);
}

void extract_loop_hover_draw()
{
  HoverState &state = g_hover_state_get();
  if (!state.is_initialized || !state.shared.base.bm || !state.shared.base.obact) {
    return;
  }
  if (!extract::hover_session_is_valid(state.shared.base, state.shared.base.obact)) {
    return;
  }
  draw_loop_preview(state.shared);
}

bool extract_loop_hover_is_enabled()
{
  return g_hover_state_get().is_initialized;
}

void extract_loop_hover_activate()
{
  g_hover_state_get().is_activated = true;
}

void extract_loop_hover_deactivate()
{
  g_hover_state_get().is_activated = false;
  if (extract_loop_hover_is_enabled()) {
    extract_loop_hover_free();
  }
}

bool extract_loop_hover_is_activated()
{
  return g_hover_state_get().is_activated;
}

/** \} */

/**
 * When idle hover already resolved the loop under the cursor, reuse its seed edge
 * in the modal BMesh (element indices match because both are built from the same mesh).
 */
void sync_preview_from_hover(ExtractLoopModalData &data)
{
  const HoverState &hover = g_hover_state_get();
  const bool hover_has_preview = hover.shared.mode == ExtractionMode::FaceStrip ?
                                     !hover.shared.base.preview_faces.is_empty() :
                                     !hover.shared.loop_edges.is_empty();
  if (!hover.is_initialized || !hover_has_preview || !hover.shared.base.bm ||
      !data.shared.base.bm)
  {
    return;
  }
  if (hover.shared.base.obact != data.shared.base.obact || !hover.shared.seed_edge) {
    return;
  }

  data.shared.base.hit_location = hover.shared.base.hit_location;
  data.shared.loop_orientation = hover.shared.loop_orientation;
  data.initial_hit = true;
  data.shared.seed_edge = BM_edge_at_index(data.shared.base.bm,
                                           BM_elem_index_get(hover.shared.seed_edge));
  run_walker(data.shared, true, &data.has_boundary_seed);
  if (data.shared.mode != ExtractionMode::FaceStrip) {
    for (BMEdge *e : data.shared.loop_edges) {
      data.loop_edges_set.add(e);
    }
  }
}

}  // namespace blender::ed::sculpt_paint::extract_loop
