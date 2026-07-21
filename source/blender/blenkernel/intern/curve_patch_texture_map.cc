/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * How a texture is laid out along the Curve Patch control curve: tile span per length mode, the
 * three Ribbon CAPS zones, and the per-stamp texture-slot draw.
 */

#include <algorithm>
#include <cmath>

#include "BKE_curve_patch.hh"

namespace blender::bke {

int curve_patch_stamp_pick_texture(const Span<float> weights_cdf, const float random01)
{
  if (weights_cdf.is_empty()) {
    return -1;
  }
  const float total = weights_cdf.last();
  if (!(total > 0.0f)) {
    return -1;
  }
  /* Clamped rather than asserted: `random01` comes from a hash and a caller is free to pass exactly
   * 1.0, which would otherwise select past the last slot. */
  const float target = std::clamp(random01, 0.0f, 1.0f) * total;
  for (const int i : weights_cdf.index_range()) {
    if (target < weights_cdf[i]) {
      return i;
    }
  }
  /* Only reachable when `target` lands exactly on the total, i.e. `random01 == 1.0`. */
  return weights_cdf.size() - 1;
}

float curve_patch_texture_tile_span(const CurvePatchLengthMode length_mode,
                                    const int repeat,
                                    const float total_length,
                                    const float radius_at_s,
                                    const bool cyclic)
{
  float span;
  switch (length_mode) {
    case CurvePatchLengthMode::Repeat:
      /* `max(1, repeat)` guards a repeat count that bypassed RNA's 1..64 range (Python API, an
       * older/edited file) from producing a divide-by-zero or a negative span. */
      span = total_length / float(std::max(1, repeat));
      break;
    case CurvePatchLengthMode::Stretch:
      span = total_length;
      break;
    case CurvePatchLengthMode::Default:
    default:
      span = std::min(total_length, 2.0f * radius_at_s);
      break;
  }

  /* On a closed curve the pattern has to meet itself at `s == 0`, which only happens when the loop
   * holds a WHOLE number of tiles -- otherwise the last tile is cut mid-pattern and shows as a seam.
   * Repeat and Stretch already divide the length into an integer count and come out of the snap
   * unchanged; Default, whose tile follows the brush radius, generally does not, and gets its tile
   * stretched or squeezed by up to ~1.5x to the nearest whole count. */
  if (cyclic && span > 1e-8f && total_length > 1e-8f) {
    const float tiles = std::max(1.0f, std::round(total_length / span));
    span = total_length / tiles;
  }
  return span;
}

/* The along-length coordinate for one tile spanning `[0, span]`, mapped onto the texture's
 * `[-1, 1]` domain. Shared by both caps, which each carry exactly one tile. */
static float zone_single_tile_v(const float offset_in_zone, const float span)
{
  if (!(span > 1e-6f)) {
    return 0.0f;
  }
  return offset_in_zone / span * 2.0f - 1.0f;
}

CurvePatchTextureZoneSample curve_patch_texture_zone_at(const float s,
                                                        const float total_length,
                                                        const float radius_for_middle_tile,
                                                        const bool caps_enabled,
                                                        const float cap_start_length,
                                                        const float cap_end_length,
                                                        const CurvePatchLengthMode length_mode,
                                                        const int length_repeat,
                                                        const bool cyclic)
{
  CurvePatchTextureZoneSample result;

  float middle_length = total_length;
  float middle_offset = s;
  bool middle_cyclic = cyclic;

  if (caps_enabled) {
    float start_len = std::max(cap_start_length, 0.0f);
    float end_len = std::max(cap_end_length, 0.0f);
    const float caps_total = start_len + end_len;
    /* NOTE: `caps_total > 1e-6f` guards the division below, not a separate case -- without it, a
     * near-zero `caps_total` (e.g. both caps unset) that still exceeds an even smaller or negative
     * `total_length` would divide `total_length` by a near-zero `caps_total` and produce `inf`/NaN.
     * Skipping the scale here is safe: `start_len`/`end_len` are left at their raw (tiny) values,
     * and #zone_single_tile_v independently guards its own near-zero `span` by returning `0.0f`
     * instead of dividing by it, so no NaN or divide-by-zero can reach the caller even though
     * `end_begin` below may go slightly negative in this regime. */
    if (caps_total > total_length && caps_total > 1e-6f) {
      /* Scale both by the same factor so their ratio survives; the middle collapses instead. */
      const float scale = total_length / caps_total;
      start_len *= scale;
      end_len *= scale;
    }

    if (s < start_len) {
      result.zone = CurvePatchTextureZone::Start;
      result.v = zone_single_tile_v(s, start_len);
      return result;
    }
    const float end_begin = total_length - end_len;
    if (s > end_begin) {
      result.zone = CurvePatchTextureZone::End;
      result.v = zone_single_tile_v(s - end_begin, end_len);
      return result;
    }

    middle_length = total_length - start_len - end_len;
    middle_offset = s - start_len;
    if (!(middle_length > 1e-6f)) {
      result.valid = false;
      return result;
    }
    /* The caps already break the loop at the seam, so the middle is tiled as an open stretch. */
    middle_cyclic = false;
  }

  const float tile_span = curve_patch_texture_tile_span(
      length_mode, length_repeat, middle_length, radius_for_middle_tile, middle_cyclic);
  /* Centering on the stretch's midpoint keeps an OPEN strip's pattern symmetric between its two
   * ends. A closed curve has no midpoint to be symmetric about; its anchor is the join at
   * `middle_offset == 0`, where a tile has to START -- hence `v = -1` there (the tile domain is
   * [-1, 1], matching the relief's across-strip `u`), running to `+1` one tile later. At
   * `middle_offset == middle_length`, which `tile_span` above snapped to a whole tile count `n`,
   * that yields `2n - 1`, congruent to `-1` modulo the texture's period of 2: the pattern closes on
   * itself. Dropping the `- 1` would put the join mid-tile and, in Stretch/Default (which do not
   * wrap `v` below), push the whole loop outside the tile the texture actually occupies. */
  float v = tile_span > 1e-8f ? (middle_cyclic ? middle_offset / tile_span * 2.0f - 1.0f :
                                                 (middle_offset - middle_length * 0.5f) /
                                                     tile_span * 2.0f) :
                                0.0f;
  /* REPEAT mode must show a full copy of the texture in every tile regardless of the texture's
   * own extension mode (an image set to Extend/Clip, or a procedural texture with no natural
   * period, would otherwise never visibly repeat -- only the [-1, 1] center tile would carry the
   * pattern and the rest would smear the edge). Wrap the along-length coordinate back into a
   * single tile's [-1, 1) domain (period 2, sawtooth) so each of the N tiles re-samples the whole
   * texture. Default/Stretch keep the continuous coordinate: Stretch is a single tile already,
   * and Default deliberately relies on the texture's own tiling for its hybrid look. */
  if (length_mode == CurvePatchLengthMode::Repeat) {
    v -= 2.0f * std::floor((v + 1.0f) * 0.5f);
  }
  result.v = v;
  return result;
}

}  // namespace blender::bke
