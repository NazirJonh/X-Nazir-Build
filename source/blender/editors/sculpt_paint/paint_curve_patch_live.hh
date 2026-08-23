/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 *
 * The brush state a live Curve Patch re-stamp depends on that has no home in
 * #bke::CurvePatchParams.
 *
 * Nothing pushes a brush edit at a running Curve Patch session -- the panels write straight into
 * the `Brush` and RNA only broadcasts `NC_BRUSH | NA_EDITED` -- so every mode's modal polls
 * instead, capturing this snapshot on each event and comparing it against the last stamped one.
 *
 * Mode-independent on purpose: 3D Sculpt Mode and the 2D Image Editor held two near-identical
 * copies of this, and a field added to one but not the other shows up as "the UI changes and the
 * canvas does not", which no compiler catches.
 */

#include <cstdint>

#include "BLI_math_vector_types.hh"

namespace blender {
struct Brush;
struct Paint;
}  // namespace blender

namespace blender::ed::sculpt_paint {

/**
 * Live inputs that affect a re-stamp but are not in #bke::CurvePatchParams.
 *
 * A new brush field that is not frozen must land in one of two places:
 * - #bke::CurvePatchParams, if it participates in the geometry build. It is then covered by
 *   #curve_patch_params_live_overlay, whose result each host compares against its own per-patch
 *   frozen base -- deliberately NOT a member here, see the note below.
 * - Here, if it is sampling / strength / color / texture identity. Missing a field means the UI
 *   changes and the target does not.
 *
 * \note #bke::CurvePatchParams is not a member. It is derived state, resolved against a per-patch
 * frozen base that differs per patch and that session-owned edits (the Y hotkey's `swap_axis`,
 * the session undo stack) write without touching the brush. Comparing it against a snapshot rather
 * than against that base would report a change the session itself had just made, and re-stamp a
 * second time for it. Every host therefore keeps its own resolved-params compare alongside this
 * struct.
 *
 * \note #symm and #blend are each read by exactly one mode. The unused one keeps its sentinel and
 * compares as a constant, which costs nothing; a template or a derived struct would not pay for
 * itself over two fields. #curve_patch_live_inputs_capture fills neither -- see its doc-string.
 */
struct CurvePatchLiveInputs {
  float alpha = -1.0f;
  bool dir_in = false;
  float3 brush_color = float3(-1.0f);

  /** 3D Sculpt Mode only: `mesh_symmetry_xyz_get(ob)`. -1 where there is no mesh symmetry. */
  int symm = -1;
  /** 2D Image Editor only: `Brush::blend` (#IMB_BlendMode). -1 where the writer does not blend. */
  int blend = -1;

  int stamp_tex_source = -1;
  int ribbon_tex_source = -1;
  float cap_start_length = -1.0f;
  float cap_end_length = -1.0f;
  uint64_t texture_list_digest = 0;
  uint64_t texture_pointer_digest = 0;
  const void *cap_tex_start = nullptr;
  const void *cap_tex_middle = nullptr;
  const void *cap_tex_end = nullptr;

  float2 tex_size = float2(0.0f);
  float2 tex_ofs = float2(0.0f);
  const void *tex = nullptr;
  uint64_t tex_edit_count = 0;

  int falloff_preset = -1;
  int falloff_curve_ts = -1;

  /** PBR Paint / Poly Paint: everything a re-stamp reads out of #BrushMaterialPaint -- every
   * channel's paint value, blend mode and enable flag, the Base Color, both Alpha switches and
   * the shared source mapping.
   *
   * A digest rather than ten spelled-out channels: #BrushMaterialPaint carries
   * `PAINT_MATERIAL_CHANNEL_NUM` of them, and listing the fields one by one is exactly the
   * shape of bug this struct's doc-string warns about -- one forgotten member shows up as
   * "the Roughness slider moves and the texture does not". */
  uint64_t material_paint_digest = 0;
  /** The channels' SOURCE texture identities alone. Split out of the digest above because a
   * source swap changes which images are sampled, which is what drives the pool rebuild -- a
   * plain value-slider drag must not free and reallocate the pool on every tick. */
  uint64_t material_source_digest = 0;
  /** #Paint.visible_material_channels: hiding a channel removes its paint target. */
  int visible_material_channels = -1;

  friend bool operator==(const CurvePatchLiveInputs &a, const CurvePatchLiveInputs &b) = default;

  /** True when the set of sampled images or their mapping changed. A cap-length or slot-weight
   * edit compares unequal above but returns false here, so dragging a slider re-stamps without
   * freeing and reallocating the texture pool on every tick. */
  bool needs_texture_pool_rebuild(const CurvePatchLiveInputs &prev) const
  {
    return tex != prev.tex || tex_edit_count != prev.tex_edit_count || tex_size != prev.tex_size ||
           tex_ofs != prev.tex_ofs || texture_pointer_digest != prev.texture_pointer_digest ||
           cap_tex_start != prev.cap_tex_start || cap_tex_middle != prev.cap_tex_middle ||
           cap_tex_end != prev.cap_tex_end ||
           material_source_digest != prev.material_source_digest;
  }
};

/**
 * Capture every field the brush and paint settings can supply on their own.
 *
 * #CurvePatchLiveInputs::symm and #CurvePatchLiveInputs::blend are left at their sentinels: each
 * is read by exactly one mode, and filling both here would make the other mode re-stamp for a
 * value its target never reads. The caller sets the one its target uses.
 */
CurvePatchLiveInputs curve_patch_live_inputs_capture(const Paint &paint, const Brush &brush);

}  // namespace blender::ed::sculpt_paint
