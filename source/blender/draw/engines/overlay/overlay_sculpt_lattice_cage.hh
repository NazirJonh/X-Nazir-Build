/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup overlay
 *
 * Virtual cage overlay for the Sculpt Lattice tool (Plan 2). Reads live tool state through
 * ED_sculpt_lattice_draw.hh and draws control points + cage wires in 3D world space.
 * Modeled on the Relations overlay (world-space lines + loose points). */

#include "ED_sculpt_lattice_draw.hh"

#include "overlay_base.hh"

namespace blender::draw::overlay {

class SculptLatticeCage : Overlay {
 private:
  PassSimple ps_ = {"SculptLatticeCage"};

  LinePrimitiveBuf lines_buf_;
  PointPrimitiveBuf points_buf_;

  /**
   * Set once the cage geometry has been gathered, so #end_sync knows there is a pass to build.
   *
   * Doubles as a "first match wins" latch in #object_sync. Unlike most overlays, which accumulate
   * over every object they are handed, there is at most one cage in a scene: it belongs to the
   * single object in sculpt mode. Bailing out on the objects that follow is therefore not a
   * limitation but the invariant made explicit.
   */
  bool has_cage_ = false;

 public:
  /* The buffers take a #SelectionType because their base class requires one, not because the cage
   * takes part in engine-side selection: #begin_sync disables the whole overlay during selection
   * passes, #append is called without a #select::ID and no #select_bind is issued. Picking a
   * control point is done in screen space by #SCULPT_OT_lattice_pick instead, which needs no GPU
   * readback for a handful of points. */
  SculptLatticeCage(SelectionType selection_type)
      : lines_buf_(selection_type, "sculpt_lattice_lines"),
        points_buf_(selection_type, "sculpt_lattice_points")
  {
  }

  void begin_sync(Resources &res, const State &state) final
  {
    /* Only enable/disable and buffer resets here: per the #Overlay contract there is no access to
     * object data at this point. The cage itself is gathered in #object_sync. */
    enabled_ = state.is_space_v3d() && !state.hide_overlays && !res.is_selection() &&
               state.object_mode == OB_MODE_SCULPT;
    has_cage_ = false;

    lines_buf_.clear();
    points_buf_.clear();
  }

  void object_sync(Manager & /*manager*/,
                   const ObjectRef &ob_ref,
                   Resources &res,
                   const State &state) final
  {
    if (!enabled_ || has_cage_) {
      return;
    }
    /* The cage belongs to the object being sculpted, not to instances of it. */
    if (is_from_dupli_or_set(ob_ref)) {
      return;
    }
    if (!ed::sculpt_paint::lattice::ED_sculpt_lattice_cage_is_relevant(state.depsgraph,
                                                                      ob_ref.object))
    {
      return;
    }

    ed::sculpt_paint::lattice::LatticeCageDrawData cage;
    ed::sculpt_paint::lattice::ED_sculpt_lattice_cage_build(state.depsgraph, ob_ref.object, cage);
    if (!cage.valid) {
      return;
    }
    has_cage_ = true;

    /* The two phases answer different questions — "where does the cage go" versus "how does the
     * mesh bend" — and LMB means something different in each, so they must not look alike. */
    const float4 &wire_col = cage.placement_phase ? res.theme.colors.wire_edit :
                                                    res.theme.colors.wire;
    const float4 &vert_col = res.theme.colors.vert_select;
    const float4 &active_col = res.theme.colors.active_object;
    for (const int2 &e : cage.edges) {
      lines_buf_.append(cage.points[e.x], cage.points[e.y], wire_col);
    }
    for (const int i : cage.points.index_range()) {
      points_buf_.append(cage.points[i], i == cage.active_point ? active_col : vert_col);
    }
  }

  void end_sync(Resources &res, const State &state) final
  {
    if (!enabled_ || !has_cage_) {
      return;
    }
    /* Both sub-passes below deliberately omit #DRW_STATE_WRITE_DEPTH and #DRW_STATE_DEPTH_LESS_EQUAL,
     * unlike the Relations overlay this class is otherwise modeled on. The cage is a manipulation
     * handle, not scene geometry: it has to stay grabbable when it sits inside the very mesh it
     * deforms, which is the normal case once it has been fitted to a region. */
    ps_.init();
    ps_.bind_ubo(OVERLAY_GLOBALS_SLOT, &res.globals_buf);
    ps_.bind_ubo(DRW_CLIPPING_UBO_SLOT, &res.clip_planes_buf);
    {
      PassSimple::Sub &sub = ps_.sub("lines");
      sub.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND_ALPHA, state.clipping_plane_count);
      sub.shader_set(res.shaders->extra_wire.get());
      lines_buf_.end_sync(sub);
    }
    {
      PassSimple::Sub &sub = ps_.sub("points");
      sub.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND_ALPHA, state.clipping_plane_count);
      sub.shader_set(res.shaders->extra_loose_points.get());
      points_buf_.end_sync(sub);
    }
  }

  void draw_line(Framebuffer &framebuffer, Manager &manager, View &view) final
  {
    if (!enabled_ || !has_cage_) {
      return;
    }
    GPU_framebuffer_bind(framebuffer);
    manager.submit(ps_, view);
  }
};

}  // namespace blender::draw::overlay
