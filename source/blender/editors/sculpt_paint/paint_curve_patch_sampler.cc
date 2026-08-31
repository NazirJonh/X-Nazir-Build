/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

#include "paint_curve_patch_sampler.hh"

#include "DNA_brush_types.h"
#include "DNA_texture_types.h"

#include "BKE_brush.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_subdiv_ccg.hh"

#include "BLI_assert.h"
#include "BLI_bounds_types.hh"
#include "BLI_index_range.hh"
#include "BLI_math_base.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"

#include "paint_curve_patch_session.hh"
#include "paint_intern.hh"

namespace blender::ed::sculpt_paint {

float3 curve_patch_canonicalize(const CurvePatchStrokeContext &ctx, const float3 &co)
{
  const float3 flipped = symmetry_flip(co, ctx.mirror_symmetry_pass);
  if (ctx.radial_symmetry_pass) {
    return math::transform_point(ctx.symm_rot_mat_inv, flipped);
  }
  return flipped;
}

float3 curve_patch_decanonicalize_dir(const CurvePatchStrokeContext &ctx, const float3 &dir)
{
  /* Inverse of #curve_patch_canonicalize, in the reverse order: the rotation is undone with the
   * transpose of its own inverse (a rotation matrix), and the mirror is its own inverse. Applied
   * to DIRECTIONS only, which is why the translation-free 3x3 part is all that is needed. */
  const float3 unrotated = ctx.radial_symmetry_pass ?
                               math::transpose(float3x3(ctx.symm_rot_mat_inv)) * dir :
                               dir;
  return symmetry_flip(unrotated, ctx.mirror_symmetry_pass);
}

float curve_patch_cull_tube_radius(const bke::CurvePatchGeometry &geometry, const float max_radius)
{
  return 2.5f * max_radius + geometry.ribbon_end_margin;
}

#if CURVE_PATCH_PROFILING
void CurvePatchSampler::BranchFunnel::add(const BranchFunnel &other)
{
  branch_calls += other.branch_calls;
  rej_radius += other.rej_radius;
  rej_normal_dist += other.rej_normal_dist;
  rej_falloff += other.rej_falloff;
  rej_endpoint += other.rej_endpoint;
  rej_s_range += other.rej_s_range;
  rej_end_falloff += other.rej_end_falloff;
  rej_late += other.rej_late;
}
#endif

CurvePatchSampler::CurvePatchSampler(const CurvePatchItem &item,
                                     const CurvePatchTextureBinding &texture,
                                     const CurvePatchStrokeContext &ctx,
                                     const Brush &brush,
                                     const CurvePatchSourceGeometry &source,
                                     const Span<float> mask,
                                     ImagePool &tex_pool)
    : item_(item),
      texture_(texture),
      ctx_(ctx),
      brush_(brush),
      source_(source),
      mask_(mask),
      tex_pool_(tex_pool),
      total_length_(item.geometry.spline.total_length()),
      start_endpoint_radius_(item.geometry.spline.radius_at(0.0f) * item.params.radius),
      end_endpoint_radius_(item.geometry.spline.radius_at(total_length_) * item.params.radius),
      mtex_size_(brush.mtex.size[0], brush.mtex.size[1]),
      mtex_ofs_(brush.mtex.ofs[0], brush.mtex.ofs[1])
{
}

std::optional<CurvePatchSample> CurvePatchSampler::sample(const int idx, const int thread_id) const
{
  const CurvePatchItem &item = item_;
  const CurvePatchTextureBinding &texture = texture_;
  const CurvePatchStrokeContext &ctx = ctx_;
  const Brush &brush = brush_;
  const MTex *mtex = &brush.mtex;
  const float2 mtex_size = mtex_size_;
  const float2 mtex_ofs = mtex_ofs_;
  const float total_length = total_length_;

  /* Pre-patch position for `idx`, READ-ONLY: from the snapshot map if an earlier symmetry pass of
   * this restamp already recorded it (in that pass's PHASE 2), else the live position -- which
   * `curve_patch_restore_only()` reset to the true original before this action ran and PHASE 1
   * never overwrites, so it is the true original either way. `lookup_ptr` only reads the map. */
  const float3 *orig_ptr = source_.orig_positions ? source_.orig_positions->lookup_ptr(idx) :
                                                    nullptr;
  const float3 orig = orig_ptr ? *orig_ptr : source_.positions[idx];

  /* Map the real vertex back into the canonical (non-mirrored, non-rotated) curve space this
   * pass represents -- see #curve_patch_canonicalize, which both node culls share so that they
   * cannot disagree with this. */
  const float3 symm_co = curve_patch_canonicalize(ctx, orig);

  /* `normal_dist`/`radial_dist` below only reject vertices by POSITION -- true 3D
   * closest-point distance cuts straight through a sharp edge/corner, so a vertex on a face
   * perpendicular to the one the curve was drawn on can still land close enough in 3D to pass
   * both checks even though it does not belong to that face at all (written vertices were
   * confirmed to include ones with `dot(normal, plane_normal) == 0`, i.e. a face turned a full
   * 90 degrees from the curve's own). Reject by SURFACE ORIENTATION instead: a vertex whose own
   * normal has drifted more
   * than ~72 degrees from `plane_normal` cannot be part of the intended face, regardless of
   * how close it measures in raw 3D distance. Mirrors `symm_co` above so the comparison stays
   * in the same canonical space `plane_normal` was frozen in. */
  /* The normal comes from the SNAPSHOT when there is one: the shrinkwrap and the window planes
   * were built against the pristine surface, whereas the live `normals[idx]` already carries the
   * relief this patch applied -- the culling would end up depending on its own result. On Grids
   * (where there is no snapshot to refine against) the live normal stays, as before -- windowing
   * itself still runs there off the control curve's own per-point normals.
   *
   * `surface.vert_normals` is indexed by MESH VERTEX, so it may only be consulted when `idx` is
   * one. A pixel source (see `CurvePatchSourceGeometry::indices_are_mesh_verts`) numbers its
   * entries per pixel within one chunk, and indexing the snapshot with such an index would return
   * the normal of an arbitrary unrelated vertex. That source is also exempt on the merits: color
   * never displaces geometry, so its live `normals[idx]` IS the pristine normal the snapshot
   * exists to recover -- the lookup would be both wrong and pointless. */
  const float3 raw_normal = (source_.indices_are_mesh_verts && item.geometry.surface.ready &&
                             idx < int(item.geometry.surface.vert_normals.size())) ?
                                item.geometry.surface.vert_normals[idx] :
                                source_.normals[idx];
  const float3 symm_normal = curve_patch_canonicalize(ctx, raw_normal);
  /* On the single-window path the culling stays here and compares against the frozen plane. On the
   * windowed path it moves into `frames.sample()`, where the normal of the SPECIFIC window is
   * known -- which is exactly what removes the break in the relief on a face turned 90 degrees. */
  if (!item.geometry.frames.ready && math::dot(symm_normal, item.params.plane_normal) <= 0.3f) {
    return std::nullopt;
  }

  /* Clip the relief by the sculpt mask, mirroring every dab-based brush's own
   * `fill_factor_from_hide_and_mask()` (`sculpt.cc:7233`): a fully-masked (mask == 1) vertex
   * must stay untouched by the projection, the same way it stays untouched by a normal brush
   * stroke. */
  const float mask_factor = mask_.is_empty() ? 1.0f : 1.0f - mask_[idx];
  if (mask_factor <= 0.0f) {
    return std::nullopt;
  }
#if CURVE_PATCH_PROFILING
  dbg_reached_lut_++;
#endif

  /* Ribbon LUT instead of `CurvePatchSpline::closest_point()`: the old global nearest-segment
   * search was multi-valued on the concave side of sharp turns (several segments near-equidistant
   * -> neighboring vertices snapping to different `s`, tearing the texture into fans). The ribbon
   * assigns UV from the specific quad covering the vertex, staying single-valued through
   * arbitrarily sharp turns -- the same construction the Roll stroke method uses. No branch at all
   * means the vertex projects outside the ribbon (off the strip, or past the curve's ends, which
   * the borders cut off exactly like the old `s` range rejection did). */
  /* Relief one stretch of the curve contributes at `sample_co`, or nullopt when that stretch does
   * not actually reach it. */
  auto branch_relief = [&](const float3 &sample_co,
                           const float2 ribbon_uv) -> std::optional<CurvePatchSample> {
#if CURVE_PATCH_PROFILING
    dbg_branch_funnel_.branch_calls++;
#endif
    const float s = ribbon_uv.y;
    /* Signed offset from the surface, measured against the curve point the ribbon mapped this
     * sample to (NOT the globally-closest point -- consistent with the ribbon's own branch
     * choice).
     *
     * Measured along the SMOOTHED surface normal at `s`, never along the window's own projection
     * plane. `normal_dist` feeds `radial_dist` below and therefore the relief's amplitude, while a
     * window normal is sharp by design and flips ~90 degrees the moment the winning window
     * changes -- which put a hard step in the amplitude right across the strip at every window
     * join, visible as a crisp stair-stepped line far outside the strip's bright core (the
     * falloff there is tiny but non-zero, and a step in it is still a normal discontinuity).
     * `normal_at()` is continuous by construction, so the depth runs through the join smoothly
     * just as the ribbon's own `(u, s)` does. Falls back to `plane_normal` when there is no
     * smoothed field (Grids, failed snapshot), collapsing to the previous formula exactly. */
    const float normal_dist = math::dot(sample_co - item.geometry.spline.evaluate(s),
                                        item.geometry.spline.normal_at(s));

    /* `CurvePatchSpline::radii` stores the control curve's per-point `radius` attribute
     * verbatim, which is a unitless UI scalar (default 1.0 = "full brush size", see
     * `curve_patch_start_from_anchor()`) -- everywhere else it is read
     * (`paint_curve_sync.cc:paintcurve_radius_handle_screen_get_from_geometry()`) it is only
     * ever turned into a SCREEN-PIXEL handle length, never a world-space distance. Scale it by
     * the frozen brush radius (the same world-space value the old dab-based re-stamp used
     * globally) to get an actual world-space falloff radius comparable to `lateral`/`s` below. */
    const float falloff_radius_at_s = item.geometry.spline.radius_at(s) * item.params.radius;
    if (falloff_radius_at_s <= 0.0f) {
#if CURVE_PATCH_PROFILING
      dbg_branch_funnel_.rej_radius++;
#endif
      return std::nullopt;
    }

    /* The ribbon's `u` is normalized across the half-width the LUT was BUILT with, which in Stamps
     * mode is widened to a jittered stamp's full reach (`CurvePatchGeometry::ribbon_radius`). So
     * turning `u` back into a world-space lateral offset must use that widened scale. The two
     * values must not be conflated: `falloff_radius_at_s` is the BRUSH's reach and stays
     * unwidened, since Ribbon mode fades over it and both modes measure the off-plane cutoff and
     * the endpoint shapes against it. Scaling `u` by it instead would under-report `lateral` and
     * compress the stamp-space `du` below, squashing stamps toward the curve. The two are equal in
     * Ribbon mode, where jitter never applies. */
    const float lateral_scale_at_s = item.geometry.spline.radius_at(s) *
                                     item.geometry.ribbon_radius;

    /* `normal_dist` is the vertex's signed offset along the anchor plane's normal from the
     * closest point on the curve -- how far it "lifts off" that plane. On a flat surface it is
     * ~0 everywhere. On a faceted surface (e.g. the side faces of a cube when the curve sits on
     * the top face) it grows large, and without rejecting those vertices the relief leaked onto
     * faces that should never receive the projection: `closest_point` picked them up (they ARE
     * close to the curve in 3D) but `lateral` alone only measured the in-plane offset, so they
     * passed the lateral falloff with a small apparent distance. Hybrid handling per the
     * redesign discussion: a hard cutoff rejects far-off-plane vertices outright (clean result
     * on faceted meshes), and the lateral falloff is computed from the TRUE 3D distance
     * `sqrt(lateral^2 + normal_dist^2)` so what survives blends smoothly across edges instead of
     * producing a hard seam at the cutoff. The cutoff is set to one brush diameter: anything
     * farther off-plane than the strip is wide is definitely not part of the target face, while
     * near-edge vertices on the intended face (small `normal_dist` from slight surface curvature)
     * still pass. */
    if (std::abs(normal_dist) > 2.0f * falloff_radius_at_s) {
#if CURVE_PATCH_PROFILING
      dbg_branch_funnel_.rej_normal_dist++;
#endif
      return std::nullopt;
    }
    /* In-plane offset reconstructed from the ribbon's normalized across-strip coordinate; the
     * falloff formula below is unchanged. */
    const float lateral = ribbon_uv.x * lateral_scale_at_s;
    /* A Round endpoint's cap is meant to read as the brush's own falloff shape wrapped around the
     * tip, not a flat disc sliced off hard at its rim: past the endpoint, the vertex's true
     * distance from the curve is measured from that endpoint, so the along-tangent overshoot has
     * to join `lateral`/`normal_dist` in quadrature the same way `normal_dist` already does above,
     * or `radial_dist` stays flat with `s` and the cap's rim -- clipped below by
     * `endpoint_contains` -- shows up as a hard edge instead of fading out with the rest of the
     * falloff curve. Zero on every interior sample and past a non-Round endpoint, so nothing else
     * changes. */
    float cap_overshoot = 0.0f;
    if (!item.geometry.spline.cyclic) {
      if (item.params.start_point_shape == CurvePatchPointShape::Round && s < 0.0f) {
        cap_overshoot = -s;
      }
      else if (item.params.end_point_shape == CurvePatchPointShape::Round && s > total_length) {
        cap_overshoot = s - total_length;
      }
    }
    const float radial_dist = std::sqrt(lateral * lateral + normal_dist * normal_dist +
                                        cap_overshoot * cap_overshoot);

    /* Stamps mode owns no centerline falloff. Each stamp already fades over its OWN half-extent
     * (`stamp_falloff` below), and measuring a second falloff from the curve on top of that is
     * what made Jitter useless: a stamp thrown sideways was both cut off at `falloff_radius_at_s`
     * and dimmed in proportion to how far it had been thrown, so the scatter the setting exists to
     * produce could never leave the band the un-jittered layout already covers. Ribbon mode, which
     * has no stamps and nothing else to shape its edge, keeps the falloff exactly as it was.
     *
     * What remains in Stamps mode is a plain admissibility bound, so vertices out where a jittered
     * stamp can reach still get to run the per-stamp test that decides their fate. It is the
     * strip's own half-width -- `lateral_scale_at_s`, i.e. the widened `ribbon_radius`, which is
     * built to cover exactly `jitter_amount` plus a stamp's corner reach. Vertices past it have no
     * ribbon `u` to place them in a stamp's frame anyway. */
    const bool is_stamps = item.params.stamp_mode == CurvePatchStampMode::Stamps;
    float lateral_falloff = 1.0f;
    if (is_stamps) {
      if (radial_dist > lateral_scale_at_s) {
#if CURVE_PATCH_PROFILING
        dbg_branch_funnel_.rej_falloff++;
#endif
        return std::nullopt;
      }
    }
    else {
      lateral_falloff = BKE_brush_curve_strength(&brush, radial_dist, falloff_radius_at_s);
      if (lateral_falloff <= 0.0f) {
#if CURVE_PATCH_PROFILING
        dbg_branch_funnel_.rej_falloff++;
#endif
        return std::nullopt;
      }
    }

    /* Shape the two endpoints independently. Square stops at the control point. Round and Triangle
     * instead add a cap OUTSIDE it: a semicircle or a taper to a point respectively. Round's
     * actual fade to zero across that semicircle already happened above, folded into
     * `radial_dist`; this disc test is only the matching hard bound that keeps a Round vertex out
     * of the reach checks below once `lateral_falloff` reaches zero, not the source of its edge
     * shape. Square and Triangle have no such falloff-side handling and still clip flat here. */
    if (!item.geometry.spline.cyclic) {
      /* Jitter throws stamps ALONG the curve as well as across it, and an end stamp overhangs the
       * control point by its own corner reach on top of that. The endpoint shapes are cut at the
       * control point, so in Stamps mode every one of those stamps lost its outer half to a
       * straight edge -- the same defect as the lateral clipping, in the other axis. Shift the
       * shape outward by exactly the overhang the strip is already built to cover
       * (`ribbon_end_margin`, which is `stamp_reach + jitter_amount`), so the shape still shapes
       * the end, just around the stamps that are actually there. Ribbon mode has no overhang and
       * is untouched. */
      const float end_overhang = is_stamps ? item.geometry.ribbon_end_margin : 0.0f;
      const auto endpoint_contains = [&](const CurvePatchPointShape shape,
                                         const float distance_from_endpoint) {
        const float d = distance_from_endpoint + end_overhang;
        switch (shape) {
          case CurvePatchPointShape::Square:
            return d >= 0.0f;
          case CurvePatchPointShape::Round:
            return d >= 0.0f || (d >= -falloff_radius_at_s &&
                                 d * d + lateral * lateral <=
                                     falloff_radius_at_s * falloff_radius_at_s);
          case CurvePatchPointShape::Triangle:
            return d >= 0.0f ||
                   (d >= -falloff_radius_at_s && std::abs(lateral) <= falloff_radius_at_s + d);
        }
        BLI_assert_unreachable();
        return false;
      };
      if (!endpoint_contains(item.params.start_point_shape, s) ||
          !endpoint_contains(item.params.end_point_shape, total_length - s))
      {
#if CURVE_PATCH_PROFILING
        dbg_branch_funnel_.rej_endpoint++;
#endif
        return std::nullopt;
      }
    }

    /* The ribbon's first/last rows sit at the curve's ends, extended outward by
     * `ribbon_end_margin` in Stamps mode (0 in Ribbon mode, where the strip stops dead at the
     * ends), so `s` from the LUT is already confined to that range and vertices past it fail
     * `sample()` above. This guard only backstops interpolation slack at the LUT's edge pixels;
     * it must admit the extension, or the overhanging halves of the end stamps would be rejected
     * here and clipped exactly as before. On a closed curve `ribbon_end_margin` is set the same as
     * on an open one (Stamps mode assigns it unconditionally), so the guard is technically looser
     * there too -- but a cyclic LUT never reports an `s` outside `[0, total_length]` in the first
     * place, so the extra slack never admits anything the sampler would otherwise have rejected.
     */
    if (s < -item.geometry.ribbon_end_margin || s > total_length + item.geometry.ribbon_end_margin)
    {
#if CURVE_PATCH_PROFILING
      dbg_branch_funnel_.rej_s_range++;
#endif
      return std::nullopt;
    }

    /* End falloff: fade the relief in and out over `zone` world-space units at each of the
     * curve's two ends, so the strip does not begin and end with a step. `s` is arc-length along
     * the WHOLE spline (not a per-branch coordinate), so where a curve overlaps itself the
     * interior crossing sits at a mid-range `s` and keeps full amplitude -- only the curve's two
     * true ends fade. The percentage is RNA-clamped to 50, so the two zones can never overlap. */
    float end_falloff = 1.0f;
    if (!item.geometry.spline.cyclic &&
        item.params.end_falloff_mode == CurvePatchEndFalloff::Smooth)
    {
      const float zone = float(item.params.end_falloff_percent) * 0.01f * total_length;
      if (zone > 1e-8f) {
        /* A non-square cap extends the actual relief boundary beyond its control point. Fade to
         * that outer boundary, rather than to the control point, or Smooth would erase every cap
         * outside the curve. Square endpoints deliberately keep the historical zero extension. */
        /* In Stamps mode the relief boundary sits `ribbon_end_margin` past the cap as well, for
         * the stamps jitter threw out there. Fading to the control point instead would dim exactly
         * those stamps to nothing and undo the endpoint relaxation above. Taken as the larger of
         * the two rather than their sum: both describe the same outer boundary, from a cap and
         * from a stamp overhang, and only the farther one bounds it. */
        const float end_overhang = is_stamps ? item.geometry.ribbon_end_margin : 0.0f;
        const float start_extension = math::max(ELEM(item.params.start_point_shape,
                                                     CurvePatchPointShape::Round,
                                                     CurvePatchPointShape::Triangle) ?
                                                    start_endpoint_radius_ :
                                                    0.0f,
                                                end_overhang);
        const float end_extension = math::max(ELEM(item.params.end_point_shape,
                                                   CurvePatchPointShape::Round,
                                                   CurvePatchPointShape::Triangle) ?
                                                  end_endpoint_radius_ :
                                                  0.0f,
                                              end_overhang);
        const float t = std::clamp(
            std::min(s + start_extension, total_length + end_extension - s) / zone, 0.0f, 1.0f);
        end_falloff = t * t * (3.0f - 2.0f * t);
      }
    }
    if (end_falloff <= 0.0f) {
#if CURVE_PATCH_PROFILING
      dbg_branch_funnel_.rej_end_falloff++;
#endif
      return std::nullopt;
    }

    /* `u` (across the strip) is normalized the same way every other brush texture mapping mode
     * normalizes its input by the brush radius -- `MTEX_MAP_MODE_VIEW`/`TILED`/`RANDOM` divide by
     * `pixel_radius`/`start_pixel_radius` (`BKE_brush_sample_tex_3d()`, `brush.cc:1001-1018`), and
     * `MTEX_MAP_MODE_AREA` bakes an equivalent `scale_m4_fl(scale, radius)` into
     * `cache.brush_local_mat` (`calc_brush_local_mat()`, `sculpt.cc:2832`).
     *
     * The ribbon's `u` is already normalized to [-1, 1] across the strip, with its sign chosen at
     * grid construction (`curve_patch_ribbon_build()`: the `cross(tangent, plane_normal)` side
     * carries +1) to match what `-lateral / lateral_scale_at_s` produced before -- the
     * orientation the paint-cursor overlay previews. */
    const float u = ribbon_uv.x;

    /* Both modes end up here with an intensity and a per-stamp amplitude multiplier, so the
     * height formula below stays common to them. Ribbon mode has no per-stamp amplitude and
     * leaves `stamp_strength` at 1.0, which keeps its result bit-for-bit what it was. */
    float tex_value = 1.0f;
    float stamp_strength = 1.0f;
    /* RGBA of whichever stamp/zone texture actually won the intensity selection below. Declared in
     * the `branch_relief` scope (not inside either branch) because the final `CurvePatchSample`
     * construction at the bottom reads it regardless of which branch produced it. Stays
     * `{1,1,1,1}` when no texture is assigned -- the same default `paint_get_tex_pixel`'s callers
     * below pre- initialize their local buffers to before the null-texture guard, so threading it
     * through changes nothing for the no-texture case. */
    float tex_rgba[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    /* Raised only where a texture is actually evaluated below; see #CurvePatchSample::tex_valid
     * for why the caller may not infer this from the brush. */
    bool tex_valid = false;
    /* Filled by whichever branch below runs; see #CurvePatchSample::patch_uv. */
    float2 patch_uv(0.0f, 0.0f);
    bool patch_uv_valid = false;
    /* World-space frame of the coordinates above, still canonical here; de-canonicalized once at
     * the bottom, where the sample is built. See #CurvePatchSample::patch_axis_u. */
    float3 patch_axis_u(1.0f, 0.0f, 0.0f);
    float3 patch_axis_v(0.0f, 1.0f, 0.0f);
    if (is_stamps) {
      /* Find the stamps whose square could cover this vertex. The list is sorted by `center_v`,
       * so a lower-bound on `s - max_extent` opens a window that closes as soon as a stamp starts
       * past `s`; with the default spacing that window holds one to three stamps.
       *
       * The window is the radius scaled by sqrt(2), not the radius itself: a stamp is a SQUARE
       * rotated by its own `angle`, so its farthest corner sits `half_extent * sqrt(2)` from its
       * center. Sizing the window to the radius alone would drop a rotated stamp whose corner
       * still covers this vertex, clipping the stamp's corners once Random Rotation is non-zero.
       * The per-stamp test below is done in the stamp's own frame and stays exact; this bound
       * only has to be conservative.
       *
       * The bound itself is resolved once per restamp into
       * `CurvePatchGeometry::stamp_search_reach` and shared with the seam wrap and the ribbon's
       * end extension -- see that field's own comment for why the three must not be allowed to
       * drift apart. */
      const float max_extent = item.geometry.stamp_search_reach;
      auto lower = std::lower_bound(
          item.geometry.stamps.begin(),
          item.geometry.stamps.end(),
          s - max_extent,
          [](const CurvePatchStamp &stamp, const float value) { return stamp.center_v < value; });
      /* Overlapping stamps are COMPOSITED, not arbitrated. Picking a single winner (the largest
       * `|candidate|`) left a visible seam wherever the winner changed, which Jitter turns from a
       * rarity into the normal case: scattered stamps overlap almost everywhere. Each stamp is
       * instead laid over the ones below it, exactly as a stack of brush dabs would be.
       *
       * Contributions are collected here and composited after the window, because "below" is
       * decided by #CurvePatchStamp::depth, which is random and therefore unrelated to the
       * `center_v` order this window walks in. The buffer is kept sorted by depth as it fills, so
       * the composite is a single back-to-front pass over it.
       *
       * The uv/axes frame and `tex_valid` cannot be composited -- one frame has to be reported --
       * so they come from the DOMINANT contributor (largest coverage), tracked alongside. That is
       * the least discontinuous single choice: coverage varies smoothly, so the reported frame
       * changes hands away from either stamp's strong interior. */
      struct StampContribution {
        float depth;
        /** Coverage of this stamp at this vertex: how much of what is under it it hides. */
        float alpha;
        /** Signed amplitude the stamp paints where it is fully opaque. */
        float amplitude;
        float4 color;
      };
      /* Sized for the densest window the layout can produce at a usable spacing (the 1% Spacing
       * floor against a sqrt(2)-radius window admits far more in theory, but such a patch is
       * already unusable). Overflow drops the least-covering contribution rather than the newest,
       * so what is lost is what would have been hidden anyway. */
      constexpr int contributions_max = 32;
      std::array<StampContribution, contributions_max> contributions;
      int contribution_num = 0;
      float dominant_alpha = 0.0f;
      for (auto it = lower; it != item.geometry.stamps.end() && it->center_v <= s + max_extent;
           ++it)
      {
        float local_v;
        float local_u;
        if (item.params.stamp_projection == CurvePatchStampProjection::Planar) {
          /* Rigid frame: the stamp's square is a square in the WORLD, so a vertex's stamp-local
           * coordinates are plain projections onto the frozen axes. The component along
           * `plane_normal` is simply dropped, which is what makes this a planar projection --
           * off-plane vertices were already rejected above by `normal_dist` and the surface
           * orientation test, so nothing new leaks onto perpendicular faces here.
           *
           * This is the whole point of the mode: the curvilinear branch below measures `dv` in
           * arc length along the centerline, and on a bend the lines of constant `u` fan out, so
           * the same world distance spans different `dv` on the inside and the outside of the
           * turn. That shear is what bends the texture. A dot product cannot shear. */
          const float3 d = sample_co - it->origin;
          local_v = math::dot(d, it->axis_v);
          local_u = math::dot(d, it->axis_u);
        }
        else {
          const float dv = s - it->center_v;
          /* `u` is the ribbon's normalized lateral coordinate while the stamp centers are in world
           * units, so scale it by the same widened `lateral_scale_at_s` the `lateral`
           * reconstruction above uses -- the scale the ribbon was actually built with, which is
           * what makes a stamp jittered all the way out to `|center_u| == jitter_amount`
           * reachable at all. */
          const float du = u * lateral_scale_at_s - it->center_u;
          /* Rotate the offset INTO the stamp's own frame, hence the negated angle. */
          const float cos_a = std::cos(-it->angle);
          const float sin_a = std::sin(-it->angle);
          local_v = dv * cos_a - du * sin_a;
          local_u = dv * sin_a + du * cos_a;
        }
        if (std::abs(local_v) > it->half_extent || std::abs(local_u) > it->half_extent) {
          continue;
        }
        /* Map into the texture's [-1, 1] domain, matching what Ribbon mode feeds
         * `paint_get_tex_pixel()`, then apply the same mapping size/offset. */
        float stamp_u = local_u / it->half_extent;
        float stamp_v = local_v / it->half_extent;
        if (item.params.swap_axis) {
          std::swap(stamp_u, stamp_v);
        }
        /* Kept before the Size / Offset transform below: consumers that sample a texture of
         * their own in this frame apply their own mapping. */
        const float2 stamp_patch_uv(stamp_u, stamp_v);
        /* The stamp's rigid world frame IS the frame `stamp_u`/`stamp_v` grow in -- in the
         * curvilinear projection too, where `local_u`/`local_v` rotate the ribbon's `(T, B)` pair
         * by `-angle`, giving exactly the axes #curve_patch_stamps_build froze. Swapped alongside
         * the coordinates so the pair keeps describing them. */
        float3 stamp_axis_u = it->axis_u;
        float3 stamp_axis_v = it->axis_v;
        if (item.params.swap_axis) {
          std::swap(stamp_axis_u, stamp_axis_v);
        }
        stamp_u = stamp_u * mtex_size.x + mtex_ofs.x;
        stamp_v = stamp_v * mtex_size.y + mtex_ofs.y;

        /* A LIST-mode stamp samples its own variant; SINGLE keeps the brush's texture. */
        const MTex &stamp_mtex = it->tex_index >= 0 &&
                                         it->tex_index < texture.stamp_texture_variants.size() ?
                                     texture.stamp_texture_variants[it->tex_index] :
                                     *mtex;
        /* An empty slot in the LIST is an unconfigured slot, not a request to sculpt flat -- skip
         * the stamp entirely BEFORE it can win the merge below with the default `sample` of 1.0.
         * SINGLE mode deliberately keeps the old behavior, where a brush with no texture sculpts
         * a smooth, full-amplitude shape. */
        if (it->tex_index >= 0 && stamp_mtex.tex == nullptr) {
          continue;
        }
        float sample = 1.0f;
        float sample_rgba[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        const bool stamp_tex_valid = stamp_mtex.tex != nullptr;
        if (stamp_mtex.tex != nullptr) {
#if CURVE_PATCH_PROFILING
          dbg_tex_evals_++;
#endif
          paint_get_tex_pixel(
              &stamp_mtex, stamp_u, stamp_v, &tex_pool_, thread_id, &sample, sample_rgba);
        }
        /* The brush's own falloff curve fades the stamp toward its rim, so overlapping stamps
         * meet along a smooth seam instead of showing the square's hard edge. */
        const float dist = std::sqrt(local_u * local_u + local_v * local_v);
        const float stamp_falloff = BKE_brush_curve_strength(&brush, dist, it->half_extent);
        /* A square's corners reach `sqrt(2) * half_extent`, past the falloff's own length, and a
         * CUSTOM brush curve is free to return a negative value out there -- which would invert
         * the relief in the corners instead of fading it out. The Ribbon path guards its own
         * falloff the same way. */
        if (stamp_falloff <= 0.0f) {
          continue;
        }
        /* How much of what lies under this stamp it hides. Three factors, and each is needed:
         * - the falloff, so stamps meet along a smooth seam instead of a square edge;
         * - the texture's ALPHA, so a brush alpha with real transparency lets the stamps below
         *   show through where it is transparent;
         * - the texture's INTENSITY, so a black region of a grayscale mask -- the ordinary way a
         *   Blender brush texture says "nothing here" -- does not punch a hole through the stamps
         *   underneath while contributing nothing itself.
         * Clamped because a color texture's intensity is a luminance and an HDR one can exceed 1.
         */
        const float tex_alpha = stamp_tex_valid ? sample_rgba[3] : 1.0f;
        const float alpha = std::clamp(stamp_falloff * tex_alpha * sample, 0.0f, 1.0f);
        if (alpha <= 0.0f) {
          /* Fully transparent here: it neither paints nor hides, so it is not a contribution at
           * all. A stamp whose texture is black throughout no longer claims the vertex. */
          continue;
        }

        if (alpha > dominant_alpha) {
          dominant_alpha = alpha;
          tex_valid = stamp_tex_valid;
          patch_uv = stamp_patch_uv;
          patch_uv_valid = true;
          patch_axis_u = stamp_axis_u;
          patch_axis_v = stamp_axis_v;
        }

        /* The amplitude is the stamp's own strength: the texture's shape has already been folded
         * into `alpha`, so `alpha * amplitude` reproduces the single-stamp result the winner-take-
         * all merge used to hand on (`sample * stamp_falloff * strength`) exactly. */
        const StampContribution contribution{it->depth,
                                             alpha,
                                             it->strength,
                                             float4(sample_rgba[0],
                                                    sample_rgba[1],
                                                    sample_rgba[2],
                                                    tex_alpha)};
        if (contribution_num == contributions_max) {
          /* Evict the least-covering entry, and only for something that covers more. */
          int weakest = 0;
          for (const int i : IndexRange(1, contributions_max - 1)) {
            if (contributions[i].alpha < contributions[weakest].alpha) {
              weakest = i;
            }
          }
          if (contributions[weakest].alpha >= alpha) {
            continue;
          }
          std::move(contributions.begin() + weakest + 1,
                    contributions.begin() + contribution_num,
                    contributions.begin() + weakest);
          contribution_num--;
        }
        /* Insert keeping the buffer sorted by depth ascending, so the composite below is one
         * back-to-front pass. The window holds a handful of stamps, so a linear insert beats
         * sorting afterwards. */
        int insert_at = contribution_num;
        while (insert_at > 0 && contributions[insert_at - 1].depth > contribution.depth) {
          contributions[insert_at] = contributions[insert_at - 1];
          insert_at--;
        }
        contributions[insert_at] = contribution;
        contribution_num++;
      }

      /* Back-to-front `over`, accumulated PREMULTIPLIED so the result never has to be composited
       * against a background the patch does not have -- the surface under the patch is not the
       * sampler's to read. `coverage` is the alpha that accumulation implies, and un-premultiplying
       * by it at the end recovers the paint color and the texture alpha in the form the effects
       * already expect. With one contribution this reduces to that contribution unchanged, in
       * every channel. */
      float height_premul = 0.0f;
      float3 color_premul(0.0f);
      float tex_alpha_premul = 0.0f;
      float coverage = 0.0f;
      for (const int i : IndexRange(contribution_num)) {
        const StampContribution &c = contributions[i];
        const float transmit = 1.0f - c.alpha;
        height_premul = c.amplitude * c.alpha + height_premul * transmit;
        color_premul = float3(c.color.x, c.color.y, c.color.z) * c.alpha + color_premul * transmit;
        tex_alpha_premul = c.color.w * c.alpha + tex_alpha_premul * transmit;
        coverage = c.alpha + coverage * transmit;
      }

      if (coverage <= 0.0f) {
        /* Between stamps the surface is simply untouched. */
#if CURVE_PATCH_PROFILING
        dbg_branch_funnel_.rej_late++;
#endif
        return std::nullopt;
      }

      /* The whole composited product goes into `tex_value`; there is no longer a single stamp
       * whose intensity and amplitude could be reported apart from each other. */
      tex_value = height_premul;
      stamp_strength = 1.0f;
      const float3 color = color_premul / coverage;
      tex_rgba[0] = color.x;
      tex_rgba[1] = color.y;
      tex_rgba[2] = color.z;
      tex_rgba[3] = tex_alpha_premul / coverage;
    }
    else {
      /* Zone + along-length coordinate in one call. The local radius controls the projection's
       * capture width, but must not control the texture's along-curve scale: changing a radius
       * point should narrow the ribbon without re-phasing or shifting the texture. The patch's
       * base radius is therefore used for the texture tile span. With caps off this is the same
       * formula, in the same operand order, that used to be inlined here -- the contract the
       * regression test `texture_zone_caps_disabled_matches_reference` pins down. That test
       * compares against a verbatim copy of the old formula to within `1e-5f` rather than
       * bit-exactly, because the two live in different translation units and cross-TU float
       * codegen need not reproduce identical bits for textually identical expressions; the
       * operand-order guarantee is what keeps SINGLE mode's output unchanged, and it is verified
       * by inspection. */
      const CurvePatchTextureZoneSample zone_sample = bke::curve_patch_texture_zone_at(
          s,
          total_length,
          item.params.radius,
          texture.caps_enabled,
          texture.world_cap_start,
          texture.world_cap_end,
          item.params.length_mode,
          item.params.length_repeat,
          item.geometry.spline.cyclic);
      if (!zone_sample.valid) {
        /* Two oversized caps squeezed the middle to nothing; leave that stretch untouched. */
#if CURVE_PATCH_PROFILING
        dbg_branch_funnel_.rej_late++;
#endif
        return std::nullopt;
      }

      const MTex &zone_mtex = texture.caps_enabled ?
                                  texture.ribbon_zone_variants[int(zone_sample.zone)] :
                                  *mtex;
      if (texture.caps_enabled && zone_mtex.tex == nullptr) {
        /* An unassigned zone leaves its stretch of the ribbon alone, which is what makes a
         * caps-only ribbon (no Middle texture) possible. */
#if CURVE_PATCH_PROFILING
        dbg_branch_funnel_.rej_late++;
#endif
        return std::nullopt;
      }

      float tex_u = u;
      float tex_v = zone_sample.v;
      if (item.params.swap_axis) {
        std::swap(tex_u, tex_v);
      }
      /* The ribbon frame, before Size / Offset -- this is the "along the curve" orientation a
       * PBR channel source has to be sampled in too. */
      patch_uv = float2(tex_u, tex_v);
      patch_uv_valid = true;
      /* Same frame the LUT was rasterized in: `B = cross(T, plane_normal)` is the ribbon's `+u`
       * side (#curve_patch_ribbon_build), and `s` -- hence `v` -- grows along `T`. Reported as a
       * pair rather than a single angle so a swapped or mirrored patch stays describable. */
      const float3 ribbon_along = item.geometry.spline.tangent_at(s);
      float3 ribbon_across = math::cross(ribbon_along, item.geometry.spline.plane_normal);
      const float ribbon_across_len = math::length(ribbon_across);
      /* A tangent parallel to the plane normal leaves no lateral direction to report; the
       * default axes stand in, and the consumer's own degeneracy handling takes over. */
      if (ribbon_across_len > 1e-6f) {
        patch_axis_u = ribbon_across / ribbon_across_len;
        patch_axis_v = ribbon_along;
        if (item.params.swap_axis) {
          std::swap(patch_axis_u, patch_axis_v);
        }
      }
      tex_u = tex_u * mtex_size.x + mtex_ofs.x;
      tex_v = tex_v * mtex_size.y + mtex_ofs.y;

      /* Mirrors the null-texture guard `sculpt_apply_texture()` used to apply:
       * `RE_texture_evaluate()` returns `false` without writing its intensity output when `tex` is
       * null, so calling `paint_get_tex_pixel()` unconditionally read an uninitialized
       * `tex_value`. The RGBA buffer is the outer-scope `tex_rgba` (initialized to `{1,1,1,1}`
       * above), so the null-texture case leaves a usable identity color for `ColorEffect` exactly
       * as the no-texture branch intends. */
      if (zone_mtex.tex != nullptr) {
#if CURVE_PATCH_PROFILING
        dbg_tex_evals_++;
#endif
        paint_get_tex_pixel(&zone_mtex, tex_u, tex_v, &tex_pool_, thread_id, &tex_value, tex_rgba);
        tex_valid = true;
      }
    }

    const float height = tex_value * stamp_strength * lateral_falloff * end_falloff * mask_factor *
                         ctx.bstrength;
    /* `end_falloff` scales the claim WEIGHT as well as the height. Not for the branch merge just
     * above -- `relief_at()` picks the branch with the largest `|height|` and never reads the
     * weight -- but for the cross-pass blend in PHASE 2 (`pass_weight_accum`), which averages
     * every symmetry pass claiming this vertex as `sum(weight * height) / sum(weight)`. A faded
     * end that kept its full weight would enter that average with the same authority as a
     * mirrored pass at full amplitude and dent the relief along the symmetry plane. */
    return CurvePatchSample{orig,
                            height,
                            lateral_falloff * end_falloff,
                            float4(tex_rgba[0], tex_rgba[1], tex_rgba[2], tex_rgba[3]),
                            tex_valid,
                            patch_uv,
                            patch_uv_valid,
                            /* Back into the space the caller's own surface data lives in -- every
                             * axis above was derived from geometry frozen in the canonical
                             * frame. */
                            curve_patch_decanonicalize_dir(ctx, patch_axis_u),
                            curve_patch_decanonicalize_dir(ctx, patch_axis_v)};
  };

  /* Relief at one sample position: the stretches covering it merged by keeping the strongest
   * displacement rather than averaging them.
   *
   * Where the curve runs alongside itself, both stretches genuinely cover the sample, and each
   * one's own falloff already fades it out with distance from that stretch's center line. Taking
   * the strongest is the union of two embossed strips: the relief keeps full amplitude
   * everywhere, and the transition between them follows the (smoothly varying) line where the
   * two displacements happen to be equal, so it reads as one strip passing over the other.
   * Returns nullopt when no stretch reaches the sample at all.
   *
   * Averaging was the alternative -- and is what `pass_weight_accum` does for symmetry passes
   * below -- but it is right there for a different reason: those passes describe the SAME surface
   * mirrored, so averaging reconstructs it. Two stretches of one curve carry DIFFERENT texture
   * positions, and averaging them cancels the pattern into a visibly flattened band along the
   * overlap. */
  auto relief_at = [&](const float3 &sample_co) -> std::optional<CurvePatchSample> {
    float2 branch_uv[2];
    /* Which window served each branch. Deliberately NOT fed into `branch_relief()`: the relief's
     * own depth measurement has to stay continuous across a window join, and this value does not
     * (see the `normal_at()` note there). Kept because it is the one observable that says which
     * plane a branch came from, which is what the frames tests assert on. */
    float3 branch_normal[2] = {item.params.plane_normal, item.params.plane_normal};
    const int branch_num = item.geometry.frames.ready ?
                               item.geometry.frames.sample(
                                   sample_co, symm_normal, branch_uv, branch_normal) :
                               item.geometry.ribbon.sample(sample_co, branch_uv);
#if CURVE_PATCH_PROFILING
    if (branch_num > 0) {
      dbg_reached_relief_++;
    }
#endif
    std::optional<CurvePatchSample> merged;
    for (const int b : IndexRange(branch_num)) {
      const std::optional<CurvePatchSample> relief = branch_relief(sample_co, branch_uv[b]);
      if (!relief) {
        continue;
      }
      if (!merged || std::abs(relief->value) > std::abs(merged->value)) {
        merged = relief;
      }
    }
    return merged;
  };

  return relief_at(symm_co);
}

float curve_patch_max_radius(const bke::CurvePatchGeometry &geometry)
{
  /* Largest world-space half-width anywhere on the curve. Drives the node/grid cull tube below,
   * and serves as the fall-back supersampling kernel size for vertices that sit just outside the
   * strip (where no local half-width is available because the ribbon reports no branch for them).
   */
  float max_radius = 0.0f;
  for (const float r : geometry.spline.radii) {
    max_radius = std::max(max_radius, r);
  }
  /* Scaled by the RIBBON's radius, not the (unwidened) frozen one: in Stamps mode a jittered stamp
   * legitimately reaches `jitter_amount` further out than the frozen radius, and a node culled
   * here never gets to run the per-vertex test that would have claimed it. Identical to the frozen
   * radius in Ribbon mode, where the two values are equal. */
  max_radius *= geometry.ribbon_radius;
  return max_radius;
}

IndexMask curve_patch_cull_nodes(const CurvePatchItem &item,
                                 const CurvePatchStrokeContext &ctx,
                                 const bke::pbvh::Tree &pbvh,
                                 const IndexMask &query_mask,
                                 const float max_radius,
                                 IndexMaskMemory &memory)
{
  /* Node cull: `calc_brush_node_mask()` returns every node inside the whole-curve encompassing
   * SPHERE, but the relief only lands in a thin tube along the polyline. On a long curve over a
   * broad surface that sphere holds many nodes whose (up-facing) vertices each pay for
   * `closest_point()` only to be rejected by the lateral falloff. Drop any node whose bounds fall
   * entirely outside the falloff tube before the per-vertex walk. Conservative and
   * result-identical: a displaced vertex must have `radial_dist < falloff_radius_at_s` for a
   * non-zero falloff (`compute_vertex()`), i.e. it sits within `max_radius` of the polyline. Node
   * centres are mapped into the same canonical space the sampler uses, so symmetry passes cull
   * correctly. See #curve_patch_cull_tube_radius for where the margin comes from. */
  const float tube_radius = curve_patch_cull_tube_radius(item.geometry, max_radius);
  BitVector<> keep(pbvh.nodes_num(), false);
  auto cull_nodes = [&](const auto nodes) {
    query_mask.foreach_index([&](const int i) {
      const Bounds<float3> &bounds = nodes[i].bounds();
      const float3 center = (bounds.min + bounds.max) * 0.5f;
      const float node_radius = math::distance(center, bounds.max);
      const float3 canonical = curve_patch_canonicalize(ctx, center);
      const float reach = tube_radius + node_radius;
      if (item.geometry.spline.distance_sq_to(canonical) <= reach * reach) {
        keep[i].set();
      }
    });
  };
  if (pbvh.type() == bke::pbvh::Type::Grids) {
    cull_nodes(pbvh.nodes<bke::pbvh::GridsNode>());
  }
  else {
    cull_nodes(pbvh.nodes<bke::pbvh::MeshNode>());
  }
  return IndexMask::from_bits(keep, memory);
}

BitVector<> curve_patch_cull_grids(const CurvePatchItem &item,
                                   const CurvePatchStrokeContext &ctx,
                                   const bke::pbvh::Tree &pbvh,
                                   const SubdivCCG &subdiv_ccg,
                                   const CCGKey &key,
                                   const Span<float3> positions,
                                   const IndexMask &node_mask,
                                   const float max_radius)
{
  /* Grid cull (Multires only): the node cull above cannot shrink a `bke::pbvh::Tree::from_grids()`
   * leaf below one base-mesh face's worth of grids -- `pbvh.cc`'s `leaf_limit = max(800 /
   * key.grid_area, 1)` hits that floor once `grid_area` (vertices per grid, quadratic in the
   * Multires level) passes ~800, so at high subdivision a single surviving node can still bundle
   * many grids spanning far more surface than the falloff tube actually touches -- unlike a Mesh
   * leaf, whose size is a flat constant (`leaf_limit = 2500`) regardless of density. Cull
   * individual GRIDS within each surviving node the same way `cull_nodes` above culls whole nodes,
   * before either PHASE 1's `compute_vertex()` walk or PHASE 2's whole-grid snapshot touches them.
   * A plain min/max scan per grid is worth doing even though it is itself `O(grid_area)`: it is a
   * handful of comparisons per vertex, versus `compute_vertex()`'s closest-point search,
   * curve-falloff lookup, and texture sample -- culling a whole far-away grid this way is strictly
   * cheaper than walking it with the real relief formula only to have every vertex rejected. */
  const float tube_radius = curve_patch_cull_tube_radius(item.geometry, max_radius);
  BitVector<> grid_keep;
  grid_keep.resize(subdiv_ccg.grids_num, false);
  const Span<bke::pbvh::GridsNode> grids_nodes = pbvh.nodes<bke::pbvh::GridsNode>();
  node_mask.foreach_index([&](const int i) {
    for (const int grid : grids_nodes[i].grids()) {
      const Span<float3> grid_positions = positions.slice(bke::ccg::grid_range(key, grid));
      float3 grid_min = grid_positions[0];
      float3 grid_max = grid_positions[0];
      for (const float3 &p : grid_positions) {
        grid_min = math::min(grid_min, p);
        grid_max = math::max(grid_max, p);
      }
      const float3 center = (grid_min + grid_max) * 0.5f;
      const float grid_radius = math::distance(center, grid_max);
      const float3 canonical = curve_patch_canonicalize(ctx, center);
      const float reach = tube_radius + grid_radius;
      if (item.geometry.spline.distance_sq_to(canonical) <= reach * reach) {
        grid_keep[grid].set();
      }
    }
  });
  return grid_keep;
}

}  // namespace blender::ed::sculpt_paint
