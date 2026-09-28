/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Brush influence visualization for sculpt curves mode.
 *
 * While a deforming brush stroke is running, the brush executors publish the per-point zone
 * intensity they apply: the radius falloff combined with the curve/point selection factors, i.e.
 * the brush weight without the brush strength and without the per-brush operational scale
 * factors. Keeping the strength out normalizes the value to 0..1, so the highlight shows "how
 * much inside the brush zone a point is" and does not dim when the brush strength is low.
 *
 * The hover (pre-stroke) preview publishes into the same registry, so the draw engine can render
 * hover and stroke with one shared pass (see #overlay_sculpt.hh). A stroke record has priority
 * over a hover record for the same data-block: while a stroke runs, the hover callback does not
 * overwrite it.
 *
 * The weights are keyed by the original #Curves ID. Stroke records are cleared when the stroke
 * operator finishes or is cancelled; hover records are refreshed on cursor movement and cleared
 * when the cursor leaves the curves or the mode is left.
 */

#include <cstdint>

#include "BLI_array.hh"
#include "BLI_map.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "BKE_curves.h"

#include "ED_curves_sculpt.hh"

#include "sculpt_intern.hh"

namespace blender::ed::sculpt_paint {

enum class Source {
  Hover,
  Stroke,
};

struct CurvesSculptInfluence {
  Array<float> weights;
  /** The original curves ID, used to tag the draw cache when the data is cleared. */
  Curves *curves_orig;
  Source source = Source::Stroke;
};

static Map<const Curves *, CurvesSculptInfluence> &get_curves_sculpt_influence_map()
{
  static Map<const Curves *, CurvesSculptInfluence> map;
  return map;
}

/**
 * Monotonic version per original Curves, bumped on every publish/removal. The evaluated draw cache
 * stores the last built version and discards only the influence buffers when it changes, which is
 * what lets hover update without a depsgraph re-evaluation (the batch cache lives on the evaluated
 * copy, not on the original ID this registry is keyed by).
 */
static uint32_t &influence_version_counter()
{
  static uint32_t counter = 0;
  return counter;
}

static Map<const Curves *, uint32_t> &get_influence_versions()
{
  static Map<const Curves *, uint32_t> versions;
  return versions;
}

static void bump_influence_version(const Curves &curves_orig)
{
  get_influence_versions().add_overwrite(&curves_orig, ++influence_version_counter());
}

/** Discard only the influence draw data, without re-evaluating the curves geometry. */
static void curves_sculpt_influence_dirty_tag(Curves &curves_orig)
{
  bump_influence_version(curves_orig);
  BKE_curves_batch_cache_dirty_tag(&curves_orig, BKE_CURVES_BATCH_DIRTY_SCULPT_INFLUENCE);
}

void curves_sculpt_influence_viz_publish(Curves &curves_orig, Array<float> point_weights)
{
  BLI_assert(point_weights.size() == curves_orig.geometry.wrap().points_num());
  /* No draw cache tag needed here: the brushes tag #ID_RECALC_GEOMETRY on every step of the
   * stroke, which rebuilds the caches after the new weights are published. */
  CurvesSculptInfluence data;
  data.weights = std::move(point_weights);
  data.curves_orig = &curves_orig;
  data.source = Source::Stroke;
  get_curves_sculpt_influence_map().add_overwrite(&curves_orig, std::move(data));
}

void curves_sculpt_hover_viz_publish(Curves &curves_orig, Array<float> point_weights)
{
  Map<const Curves *, CurvesSculptInfluence> &map = get_curves_sculpt_influence_map();
  CurvesSculptInfluence *existing = map.lookup_ptr(&curves_orig);

  /* A running stroke owns the record: the hover must not override it. */
  if (existing != nullptr && existing->source == Source::Stroke) {
    return;
  }

  if (point_weights.is_empty()) {
    /* The cursor no longer hits any curve of this data-block: drop the stale record. */
    if (existing != nullptr) {
      map.remove(&curves_orig);
      curves_sculpt_influence_dirty_tag(curves_orig);
    }
    return;
  }

  CurvesSculptInfluence data;
  data.weights = std::move(point_weights);
  data.curves_orig = &curves_orig;
  data.source = Source::Hover;
  map.add_overwrite(&curves_orig, std::move(data));
  curves_sculpt_influence_dirty_tag(curves_orig);
}

bool curves_sculpt_hover_viz_clear(const Curves *keep)
{
  Map<const Curves *, CurvesSculptInfluence> &map = get_curves_sculpt_influence_map();
  Vector<Curves *> to_remove;
  for (auto item : map.items()) {
    if (item.value.source == Source::Hover && item.key != keep) {
      to_remove.append(item.value.curves_orig);
    }
  }
  for (Curves *curves : to_remove) {
    map.remove(curves);
    curves_sculpt_influence_dirty_tag(*curves);
  }
  return !to_remove.is_empty();
}

void curves_sculpt_influence_viz_clear()
{
  /* Positions may have been edited in place, which the hover cache key cannot see: drop it so
   * the next hover recomputes instead of drawing stale points. */
  curves_sculpt_hover_preview_cache_clear();
  Map<const Curves *, CurvesSculptInfluence> &map = get_curves_sculpt_influence_map();
  if (map.is_empty()) {
    return;
  }
  /* The last drawn batches were built while the weights were published, so discard the influence
   * draw data (the geometry itself did not change). */
  for (const CurvesSculptInfluence &data : map.values()) {
    if (data.source == Source::Stroke) {
      curves_sculpt_influence_dirty_tag(*data.curves_orig);
    }
  }
  Vector<Curves *> to_remove;
  for (auto item : map.items()) {
    if (item.value.source == Source::Stroke) {
      to_remove.append(item.value.curves_orig);
    }
  }
  for (Curves *curves : to_remove) {
    map.remove(curves);
  }
}

}  // namespace blender::ed::sculpt_paint

namespace blender {

bool ED_curves_sculpt_has_influence(const Curves &curves_orig)
{
  return ed::sculpt_paint::get_curves_sculpt_influence_map().contains(&curves_orig);
}

bool ED_curves_sculpt_influence_is_hover(const Curves &curves_orig)
{
  const ed::sculpt_paint::CurvesSculptInfluence *data =
      ed::sculpt_paint::get_curves_sculpt_influence_map().lookup_ptr(&curves_orig);
  return data != nullptr && data->source == ed::sculpt_paint::Source::Hover;
}

Span<float> ED_curves_sculpt_get_influence(const Curves &curves_orig)
{
  const ed::sculpt_paint::CurvesSculptInfluence *data =
      ed::sculpt_paint::get_curves_sculpt_influence_map().lookup_ptr(&curves_orig);
  return data != nullptr ? data->weights.as_span() : Span<float>();
}

uint32_t ED_curves_sculpt_influence_version(const Curves &curves_orig)
{
  return ed::sculpt_paint::get_influence_versions().lookup_default(&curves_orig, 0);
}

}  // namespace blender
