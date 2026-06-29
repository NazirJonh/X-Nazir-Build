/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Idle-hover state for the Extract Region tool: the #g_hover_state singleton and
 * the hover API consumed by the paint cursor. Mirrors the Extract Loop hover but
 * resolves the previewed faces through the region flood-fill selector instead of
 * the loop walker, reusing the shared #extract scaffolding.
 */

#include "BKE_context.hh"
#include "BKE_object.hh"

#include "DEG_depsgraph.hh"

#include "ED_view3d.hh"

#include "bmesh.hh"

#include "sculpt_extract_region_intern.hh"
#include "sculpt_intern.hh"

namespace blender::ed::sculpt_paint::extract_region {

/* -------------------------------------------------------------------- */
/** \name Hover State
 * \{ */

struct HoverState {
  extract::ExtractSharedData shared;
  RegionSource source = RegionSource::FaceSet;
  float mask_threshold = 0.5f;
  /* #BM_elem_index_get of the seed face, used by #sync_preview_from_hover. */
  int seed_face_index = -1;
  bool is_initialized = false;
  bool is_activated = false;
};

static HoverState &g_hover_state_get()
{
  static HoverState state;
  return state;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Hover API
 * \{ */

void extract_region_hover_init(bContext *C,
                               Object *ob,
                               ARegion *region,
                               RegionView3D *rv3d,
                               RegionSource source,
                               float mask_threshold)
{
  HoverState &state = g_hover_state_get();

  if (state.shared.bm) {
    BM_mesh_free(state.shared.bm);
    state = HoverState{};
  }

  state.shared.region = region;
  state.shared.rv3d = rv3d;
  state.source = source;
  state.mask_threshold = mask_threshold;

  if (!extract::hover_setup_bmesh(C, state.shared, ob)) {
    return;
  }

  state.is_initialized = true;
}

void extract_region_hover_free()
{
  HoverState &state = g_hover_state_get();
  if (state.shared.bm) {
    BM_mesh_free(state.shared.bm);
    state.shared.bm = nullptr;
  }
  state.shared.preview_faces.clear();
  state.shared.obact = nullptr;
  state.shared.session_mesh = nullptr;
  state.seed_face_index = -1;
  state.is_initialized = false;
  state.is_activated = false;
}

void extract_region_hover_update(bContext *C,
                                 const float2 &mval,
                                 RegionSource source,
                                 float mask_threshold)
{
  HoverState &state = g_hover_state_get();
  if (!state.is_initialized) {
    return;
  }

  Object *obact = CTX_data_active_object(C);
  if (!obact) {
    extract_region_hover_free();
    return;
  }

  state.source = source;
  state.mask_threshold = mask_threshold;
  state.shared.region = CTX_wm_region(C);
  state.shared.rv3d = CTX_wm_region_view3d(C);

  if (!extract::hover_session_is_valid(state.shared, obact)) {
    if (!extract::hover_setup_bmesh(C, state.shared, obact)) {
      extract_region_hover_free();
      return;
    }
  }
  else {
    extract::hover_refresh_preview_positions(C, state.shared, obact);
  }

  if (!state.shared.bm) {
    extract_region_hover_free();
    return;
  }

  state.shared.preview_faces.clear();
  state.seed_face_index = -1;

  CursorGeometryInfo cgi;
  if (!cursor_geometry_info_update(C, &cgi, mval, false)) {
    return;
  }
  state.shared.hit_location = cgi.location;

  BMFace *seed = find_seed_face(state.shared, mval);
  select_region(state.shared, state.source, state.mask_threshold, seed);
  state.seed_face_index = seed ? BM_elem_index_get(seed) : -1;
}

void extract_region_hover_draw()
{
  HoverState &state = g_hover_state_get();
  if (!state.is_initialized || !state.shared.bm || !state.shared.obact) {
    return;
  }
  if (!extract::hover_session_is_valid(state.shared, state.shared.obact)) {
    return;
  }
  extract::draw_faces_preview(state.shared);
}

bool extract_region_hover_is_enabled()
{
  return g_hover_state_get().is_initialized;
}

void extract_region_hover_activate()
{
  g_hover_state_get().is_activated = true;
}

void extract_region_hover_deactivate()
{
  g_hover_state_get().is_activated = false;
  if (extract_region_hover_is_enabled()) {
    extract_region_hover_free();
  }
}

bool extract_region_hover_is_activated()
{
  return g_hover_state_get().is_activated;
}

/** \} */

/**
 * When idle hover already resolved the region under the cursor, reuse its seed
 * face in the modal BMesh (element indices match because both are built from the
 * same mesh).
 */
void sync_preview_from_hover(ExtractRegionModalData &data)
{
  const HoverState &hover = g_hover_state_get();
  if (!hover.is_initialized || hover.shared.preview_faces.is_empty() || !hover.shared.bm ||
      !data.shared.bm)
  {
    return;
  }
  if (hover.shared.obact != data.shared.obact || hover.seed_face_index < 0) {
    return;
  }

  data.shared.hit_location = hover.shared.hit_location;
  data.source = hover.source;
  data.mask_threshold = hover.mask_threshold;
  data.seed_face = BM_face_at_index(data.shared.bm, hover.seed_face_index);
  select_region(data.shared, data.source, data.mask_threshold, data.seed_face);
}

}  // namespace blender::ed::sculpt_paint::extract_region
