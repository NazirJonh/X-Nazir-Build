/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Implementation of the Curve Pattern tile; see #paint_shape_pattern.hh.
 */

#include "paint_shape_pattern.hh"

#include <cmath>

#include "MEM_guardedalloc.h"

#include "BLI_array.hh"
#include "BLI_hash.hh"
#include "BLI_index_range.hh"
#include "BLI_listbase_wrapper.hh"
#include "BLI_math_base.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_task.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_collection_types.h"
#include "DNA_curve_types.h"
#include "DNA_layer_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_collection.hh"
#include "BKE_context.hh"
#include "BKE_curve.hh"
#include "BKE_main.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "paint_shape_texture.hh"
#include "paint_shape_render.hh"

namespace blender::ed::sculpt_paint::shape {

/* -------------------------------------------------------------------- */
/** \name Curve reading
 * \{ */

namespace {

/** Whether \a ob is a usable pattern source: a legacy Curve object with 2D dimensions. */
bool curve_object_is_valid(const Object *ob)
{
  if (ob == nullptr || ob->type != OB_CURVES_LEGACY) {
    return false;
  }
  const Curve *curve = id_cast<const Curve *>(ob->data);
  return curve != nullptr && (curve->flag & CU_3D) == 0;
}

/** The 2D curve objects of \a settings' curve source (the object, or every object of the
 * collection, children included). */
Vector<const Object *> pattern_curve_objects(const PaintShapeSettings &settings)
{
  Vector<const Object *> objects;
  if (settings.curve_source_mode == PAINT_SHAPE_CURVE_SOURCE_COLLECTION) {
    Collection *collection = settings.curve_source_collection;
    if (collection == nullptr) {
      return objects;
    }
    /* The collection cache lists every object of the hierarchy (as #Base); filter to usable
     * 2D curves. */
    const ListBaseT<Base> bases = BKE_collection_object_cache_get(collection);
    for (const Base &base : bases) {
      if (curve_object_is_valid(base.object)) {
        objects.append(base.object);
      }
    }
  }
  else if (curve_object_is_valid(settings.curve_source_object)) {
    objects.append(settings.curve_source_object);
  }
  return objects;
}

/** One read spline: its control points in the tile's pixel space. */
struct PatternSpline {
  bool is_bezier = false;
  bool cyclic = false;
  struct Point {
    float2 co = float2(0.0f);
    float2 handle_left = float2(0.0f);
    float2 handle_right = float2(0.0f);
    bool corner = false;
  };
  Vector<Point> points;
};

/** Map a curve-space point to tile pixels: crop square -> [0, resolution]. */
static float2 pattern_space_to_tile(const PaintShapeCurvePattern &pattern,
                                    const float resolution,
                                    const float2 &xy)
{
  const float2 crop_min(pattern.crop_min[0], pattern.crop_min[1]);
  const float2 crop_size(pattern.crop_max[0] - pattern.crop_min[0],
                         pattern.crop_max[1] - pattern.crop_min[1]);
  const float2 safe_size = math::max(math::abs(crop_size), float2(1e-6f));
  return (xy - crop_min) / safe_size * resolution;
}

/** Read one nurb into \a r_spline in tile pixels, dropping the z axis after \a to_world. */
void pattern_spline_from_nurb(const Nurb &nu,
                              const PaintShapeCurvePattern &pattern,
                              const float resolution,
                              const float4x4 &to_world,
                              PatternSpline &r_spline)
{
  const auto world_xy = [&](const float3 &co) {
    return pattern_space_to_tile(pattern, resolution, float2(math::transform_point(to_world, co)));
  };
  const bool cyclic = (nu.flagu & CU_NURB_CYCLIC) != 0;

  if (nu.type == CU_BEZIER) {
    r_spline.is_bezier = true;
    r_spline.cyclic = cyclic;
    for (const int i : IndexRange(nu.pntsu)) {
      const BezTriple &bezt = nu.bezt[i];
      PatternSpline::Point point;
      point.co = world_xy(float3(bezt.vec[1]));
      point.handle_left = world_xy(float3(bezt.vec[0]));
      point.handle_right = world_xy(float3(bezt.vec[2]));
      point.corner = bezt.h1 == HD_FREE || bezt.h2 == HD_FREE;
      r_spline.points.append(point);
    }
  }
  else if (nu.type == CU_POLY) {
    r_spline.is_bezier = false;
    r_spline.cyclic = cyclic;
    for (const int i : IndexRange(nu.pntsu)) {
      const BPoint &bp = nu.bp[i];
      PatternSpline::Point point;
      point.co = world_xy(float3(bp.vec));
      point.corner = true;
      r_spline.points.append(point);
    }
  }
  else if (nu.type == CU_NURBS && nu.pntsv == 1 && nu.knotsu != nullptr && nu.pntsu >= 2) {
    /* Evaluate the NURBS at its resolution: the tile rasterizer speaks polylines and Bézier
     * splines, not NURBS bases. */
    const int resolu = std::max<int>(nu.resolu, 4);
    const int out_num = nu.pntsu * resolu;
    Array<float3> evaluated(out_num);
    BKE_nurb_makeCurve(&nu,
                       reinterpret_cast<float *>(evaluated.data()),
                       nullptr,
                       nullptr,
                       nullptr,
                       resolu,
                       sizeof(float[3]));
    r_spline.is_bezier = false;
    r_spline.cyclic = cyclic;
    int point_num = out_num;
    /* The cyclic evaluation repeats the first point at the end; the spline's cyclic flag wraps
     * it, so the duplicate is dropped. */
    if (cyclic && out_num > 1) {
      const float2 first(evaluated[0].x, evaluated[0].y);
      const float2 last(evaluated[out_num - 1].x, evaluated[out_num - 1].y);
      if (math::almost_equal_relative(first, last, 1e-4f)) {
        point_num = out_num - 1;
      }
    }
    for (const int i : IndexRange(point_num)) {
      PatternSpline::Point point;
      point.co = world_xy(evaluated[i]);
      point.corner = true;
      r_spline.points.append(point);
    }
  }
}

/** The pattern's curves as tile-space splines (the rasterizer's input, and the shared source of
 * the "import curve as shape" path). */
Vector<PatternSpline> pattern_splines_from_objects(
    const Span<const Object *> objects,
    const PaintShapeCurvePattern &pattern,
    const float resolution,
    const Depsgraph *depsgraph)
{
  Vector<PatternSpline> splines;
  const bool world_space = (pattern.flag & PAINT_SHAPE_CURVE_PATTERN_WORLD_SPACE) != 0;
  for (const Object *ob : objects) {
    const Object *ob_used = ob;
    if (depsgraph != nullptr) {
      if (Object *ob_eval = DEG_get_evaluated(depsgraph, const_cast<Object *>(ob))) {
        ob_used = ob_eval;
      }
    }
    const Curve *curve = id_cast<const Curve *>(ob_used->data);
    if (curve == nullptr) {
      continue;
    }
    /* World space reads the object's world transform; local space reads the curve data as-is
     * (the user arranges the curves inside the crop frame through the object itself). */
    const float4x4 to_world = world_space ? ob_used->object_to_world() :
                                            float4x4::identity();
    for (const Nurb &nu : curve->nurb) {
      PatternSpline spline;
      pattern_spline_from_nurb(nu, pattern, resolution, to_world, spline);
      if (spline.points.size() >= 2) {
        splines.append(std::move(spline));
      }
    }
  }
  return splines;
}

/** The pattern splines as #PaintShape splines for the rasterizer. */
PaintShape pattern_shape_from_splines(const Span<PatternSpline> splines)
{
  PaintShape shape;
  shape.type = PAINT_SHAPE_CURVE;
  for (const PatternSpline &src : splines) {
    ShapeSpline spline;
    spline.cyclic = src.cyclic;
    spline.is_bezier = src.is_bezier;
    for (const PatternSpline::Point &point : src.points) {
      ShapePoint out;
      out.co = point.co;
      out.handle_left = point.handle_left;
      out.handle_right = point.handle_right;
      out.corner = point.corner;
      out.auto_handles = false;
      out.width_factor = 1.0f;
      spline.points.append(out);
    }
    shape.splines.append(std::move(spline));
  }
  return shape;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tile rasterization
 * \{ */

/** The rasterizer style of one pattern mode. */
ShapeStyle pattern_style(const PaintShapeCurvePattern &pattern, const float resolution)
{
  ShapeStyle style;
  style.flag = 0;
  const bool do_fill = pattern.mode != PAINT_SHAPE_CURVE_PATTERN_STROKE;
  const bool do_stroke = pattern.mode != PAINT_SHAPE_CURVE_PATTERN_FILL;
  if (do_fill) {
    style.flag |= PAINT_SHAPE_USE_FILL;
  }
  if (do_stroke) {
    style.flag |= PAINT_SHAPE_USE_STROKE;
    style.stroke_width = std::max(pattern.line_width, 1e-4f) * resolution;
  }
  style.fill_rule = PAINT_SHAPE_FILL_NONZERO;
  return style;
}

/** Rasterize the pattern into \a r_buffer (RGBA float, `resolution` squared): R = coverage,
 * G = relief (SDF depth), B = 0, A = coverage. The WRAP_CROSSING flag rasterizes a 3x3
 * neighborhood so border-crossing strokes wrap seamlessly. */
void pattern_tile_rasterize(const PaintShape &shape,
                            const PaintShapeCurvePattern &pattern,
                            const float resolution,
                            float *r_buffer)
{
  const ShapeStyle style = pattern_style(pattern, resolution);
  const rcti rect{0, int(resolution), 0, int(resolution)};
  const ShapeRasterOutputs outputs = ShapeRasterOutputs::Fill | ShapeRasterOutputs::Stroke |
                                     ShapeRasterOutputs::StrokeT | ShapeRasterOutputs::FillD |
                                     ShapeRasterOutputs::StrokeS | ShapeRasterOutputs::StrokeV;

  const bool wrap = (pattern.flag & PAINT_SHAPE_CURVE_PATTERN_WRAP_CROSSING) != 0;
  /* A seamless tile renders the 3x3 neighborhood of shape copies into the center tile: strokes
   * crossing the crop border come back through the opposite side. Without the flag the single
   * copy is used and anything outside the tile is clipped. */
  const int copies = wrap ? 3 : 1;
  /* Coverage accumulates with max over the copies; the relief of the most covered copy wins. */
  Array<float> coverage(size_t(resolution) * resolution, 0.0f);
  Array<float> relief(size_t(resolution) * resolution, 0.0f);
  /* The relief band of the SDF depth: 5% of the tile side reaches the plateau. */
  const float relief_px = resolution * 0.05f;
  const bool use_relief = (pattern.flag & PAINT_SHAPE_CURVE_PATTERN_USE_SDF_RELIEF) != 0;

  for (const int dy : IndexRange(copies)) {
    for (const int dx : IndexRange(copies)) {
      const int sx = dx - 1;
      const int sy = dy - 1;
      /* A shape copy shifted by s tiles reads the same tile at offset -s. */
      const float2 tile_offset_px = float2(-float(sx) * resolution, -float(sy) * resolution);
      const ShapeCoverage cov = shape_rasterize(Span<PaintShape>(&shape, 1), style, rect,
                                                tile_offset_px, outputs);
      const int64_t pixel_num = int64_t(resolution) * resolution;
      threading::parallel_for(IndexRange(int64_t(resolution)), 64, [&](const IndexRange rows) {
        for (const int y : rows) {
          for (const int x : IndexRange(int64_t(resolution))) {
            const int64_t idx = int64_t(y) * resolution + x;
            float part_coverage = 0.0f;
            float part_relief = 0.0f;
            if (!cov.fill.is_empty()) {
              part_coverage = cov.fill[idx];
              float depth = 1.0f;
              if (use_relief) {
                const float fill_d = cov.fill_d.is_empty() ? 0.0f : cov.fill_d[idx];
                depth = math::clamp(fill_d / relief_px, 0.0f, 1.0f);
              }
              part_relief = cov.fill[idx] * depth;
            }
            if (!cov.stroke.is_empty() && cov.stroke[idx] > part_coverage) {
              /* The stroke is a rounded ridge: the across-stroke coordinate peaks at the
               * centerline. */
              part_coverage = cov.stroke[idx];
              part_relief = cov.stroke[idx] * (1.0f - (use_relief ? cov.stroke_t[idx] : 0.0f));
            }
            if (part_coverage > coverage[idx]) {
              coverage[idx] = part_coverage;
              relief[idx] = part_relief;
            }
          }
        }
      });
    }
  }

  threading::parallel_for(IndexRange(int64_t(resolution)), 64, [&](const IndexRange rows) {
    for (const int y : rows) {
      for (const int x : IndexRange(int64_t(resolution))) {
        const int64_t idx = int64_t(y) * resolution + x;
        const float c = coverage[idx];
        float *px = r_buffer + idx * 4;
        px[0] = c;
        px[1] = relief[idx];
        px[2] = 0.0f;
        px[3] = c;
      }
    }
  });
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Cache
 * \{ */

/* The key is the content of the tile-space splines, not the source objects' addresses: the
 * evaluated copies are updated in place (their pointers survive an edit), and a freed object's
 * address may be reused by another one. */
struct PatternCacheEntry {
  uint64_t geometry_hash = 0;
  PaintShapeCurvePattern pattern;
  std::shared_ptr<const ShapeTexture> tile;
};

uint64_t pattern_geometry_hash(const Span<PatternSpline> splines)
{
  uint64_t hash = get_default_hash(splines.size());
  for (const PatternSpline &spline : splines) {
    hash = get_default_hash(hash, spline.is_bezier, spline.cyclic, spline.points.size());
    for (const PatternSpline::Point &point : spline.points) {
      hash = get_default_hash(
          hash, point.co, point.handle_left, point.handle_right, point.corner);
    }
  }
  return hash;
}

Vector<PatternCacheEntry> &pattern_cache()
{
  static Vector<PatternCacheEntry> cache;
  return cache;
}

bool pattern_cache_entry_valid(const PatternCacheEntry &entry,
                               const uint64_t geometry_hash,
                               const PaintShapeCurvePattern &pattern)
{
  if (entry.geometry_hash != geometry_hash || entry.pattern.resolution != pattern.resolution ||
      entry.pattern.line_width != pattern.line_width || entry.pattern.mode != pattern.mode ||
      entry.pattern.flag != pattern.flag ||
      entry.pattern.crop_min[0] != pattern.crop_min[0] ||
      entry.pattern.crop_min[1] != pattern.crop_min[1] ||
      entry.pattern.crop_max[0] != pattern.crop_max[0] ||
      entry.pattern.crop_max[1] != pattern.crop_max[1])
  {
    return false;
  }
  return true;
}

/** \} */

}  // namespace

void shape_curve_pattern_tile_rasterize(const PaintShape &shape,
                                        const PaintShapeCurvePattern &pattern,
                                        const float resolution,
                                        float *r_buffer)
{
  pattern_tile_rasterize(shape, pattern, resolution, r_buffer);
}

std::shared_ptr<const ShapeTexture> shape_curve_pattern_texture_get(
    bContext *C, const PaintShapeSettings &settings)
{
  const PaintShapeCurvePattern &pattern = settings.curve_pattern;
  if (pattern.resolution < 8) {
    return nullptr;
  }
  const Vector<const Object *> objects = pattern_curve_objects(settings);
  if (objects.is_empty()) {
    return nullptr;
  }
  const Depsgraph *depsgraph = C != nullptr ? CTX_data_depsgraph_pointer(C) : nullptr;

  /* Read the curves (evaluated, world XY) and map them into the tile's pixel square. */
  const float resolution = float(pattern.resolution);
  const Vector<PatternSpline> splines = pattern_splines_from_objects(
      objects, pattern, resolution, depsgraph);
  if (splines.is_empty()) {
    return nullptr;
  }

  const uint64_t geometry_hash = pattern_geometry_hash(splines);
  for (const PatternCacheEntry &entry : pattern_cache()) {
    if (pattern_cache_entry_valid(entry, geometry_hash, pattern)) {
      return entry.tile;
    }
  }

  const PaintShape shape = pattern_shape_from_splines(splines);
  if (shape.is_empty()) {
    return nullptr;
  }

  auto buffer = std::make_unique<float[]>(size_t(pattern.resolution) * pattern.resolution * 4);
  pattern_tile_rasterize(shape, pattern, resolution, buffer.get());

  auto texture = std::make_shared<ShapeTexture>();
  texture->is_tile = true;
  ShapeTextureChannel &canvas = texture->canvas;
  canvas.kind = ShapeTextureChannel::Kind::Buffer;
  canvas.size = int2(pattern.resolution, pattern.resolution);
  canvas.float_px = buffer.get();
  canvas.is_linear = true;
  /* The tile feeds the color (through the canvas), the Height relief (its G channel) and the
   * Alpha mask (its coverage) of whichever part samples it. */
  texture->channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR] = canvas;
  texture->channels[PAINT_MATERIAL_CHANNEL_HEIGHT] = canvas;
  texture->channels[PAINT_MATERIAL_CHANNEL_ALPHA] = canvas;
  texture->native_size = canvas.size;

  /* Keep the pixel buffer alive with the texture. */
  std::shared_ptr<void> buffer_guard(buffer.release(), [](void *data) {
    delete[] static_cast<float *>(data);
  });
  texture->keeper = std::move(buffer_guard);

  PatternCacheEntry entry;
  entry.geometry_hash = geometry_hash;
  entry.pattern = pattern;
  entry.tile = texture;
  pattern_cache().append(std::move(entry));
  /* A tiny LRU bounded by memory as well as count: a tile is up to 8192^2 RGBA floats. The entry
   * just added always stays. */
  const auto cache_bytes = []() {
    size_t bytes = 0;
    for (const PatternCacheEntry &cached : pattern_cache()) {
      const int2 size = cached.tile->native_size;
      bytes += size_t(size.x) * size_t(size.y) * sizeof(float[4]);
    }
    return bytes;
  };
  constexpr size_t cache_max_bytes = size_t(256) * 1024 * 1024;
  while (pattern_cache().size() > 1 &&
         (pattern_cache().size() > 8 || cache_bytes() > cache_max_bytes))
  {
    pattern_cache().remove(0);
  }
  return texture;
}

void shape_curve_pattern_cache_clear()
{
  pattern_cache().clear();
}

}  // namespace blender::ed::sculpt_paint::shape
