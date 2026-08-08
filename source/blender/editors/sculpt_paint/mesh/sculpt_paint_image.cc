/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Paint a color made from hash of node pointer. */
// #define DEBUG_PIXEL_NODES

#include "DNA_brush_types.h"
#include "DNA_image_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"

#include <atomic>
#include <cstdio>

#include "ED_paint.hh"
#include "ED_view3d.hh"

#include "BLI_bit_vector.hh"
#include "BLI_bounds.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_listbase.h"
#include "BLI_math_base.hh"
#include "BLI_math_color_blend.h"
#include "BLI_math_geom.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_task.h"
#include "BLI_time.h"
#ifdef DEBUG_PIXEL_NODES
#  include "BLI_hash.h"
#endif

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"

#include "BKE_attribute.hh"
#include "BKE_brush.hh"
#include "BKE_image_wrappers.hh"
#include "BKE_mesh.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_paint_bvh_pixels.hh"
#include "BKE_paint_material_channel_perf_debug.hh"

#include "mesh_brush_common.hh"
#include "paint_material_source.hh"
#include "sculpt_automask.hh"
#include "sculpt_intern.hh"

namespace blender {

namespace paint_material_channel_perf = bke::paint_material_channel_perf;

namespace ed::sculpt_paint::paint::image {

/* Temporary diagnostic output for validating the projection basis on Cube UV islands. */
constexpr bool DEBUG_NORMAL_PROJECTION = false;
constexpr int DEBUG_NORMAL_PROJECTION_MAX_PRIMITIVE = 32;

using namespace blender::bke::pbvh::pixels;
using namespace blender::bke::image;

/* WORKAROUND: temporary printf profiling to check whether the Base Color channel's cost (much
 * higher than Roughness/Normal for the same pixel count, per the bke.paint_channel_perf report)
 * is the byte<->scene-linear colorspace conversion in #read_image_pixels/#write_image_pixels, as
 * opposed to #blend_colors itself. Remove once the perf work is done. */
#define PBR_PAINT_IMAGE_PROFILE 1

#ifdef PBR_PAINT_IMAGE_PROFILE
namespace {
struct ChannelWriteProfile {
  std::atomic<int64_t> pixel_num{0};
  std::atomic<double> sample_seconds{0.0};
  std::atomic<double> read_seconds{0.0};
  std::atomic<double> blend_seconds{0.0};
  std::atomic<double> write_seconds{0.0};
  std::atomic<int> noop_processor{-1}; /* -1 unknown, 0 has conversion, 1 is_noop. */
};
}  // namespace

/* Indexed by eMaterialPaintChannel; PAINT_MATERIAL_CHANNEL_NUM channels max. */
static ChannelWriteProfile g_channel_write_profile[7];
#endif

ImageData::~ImageData()
{
  if (!image || !image_user) {
    return;
  }

  BLI_assert(buffers.size() <= image->tiles.count());
  for (ImBuf *buffer : buffers.values()) {
    BKE_image_release_ibuf(image, buffer, nullptr);
  }
  buffers.clear();
}

std::unique_ptr<ImageData> ImageData::from_image(Image *image, ImageUser *image_user)
{
  if (image == nullptr || image_user == nullptr) {
    return nullptr;
  }
  std::unique_ptr<ImageData> image_data = std::make_unique<ImageData>();
  image_data->image = image;
  image_data->image_user = image_user;
  return image_data;
}

Vector<ImagePaintTarget> init_image_paint_targets(Object &ob,
                                                  PaintModeSettings &paint_mode_settings,
                                                  const Brush *brush)
{
  Vector<ImagePaintTarget> targets;

  switch (paint_mode_settings.canvas_source) {
    case PAINT_CANVAS_SOURCE_MATERIAL: {
      const BrushMaterialPaint *brush_paint = brush ? brush->material_paint : nullptr;
      const Vector<PaintMaterialImageTarget> material_targets =
          BKE_paint_material_image_targets_get(ob, paint_mode_settings, brush_paint);
      for (const PaintMaterialImageTarget &material_target : material_targets) {
        ImagePaintTarget target;
        target.data = ImageData::from_image(material_target.image, material_target.iuser);
        if (!target.data) {
          continue;
        }
        target.is_color_channel = material_target.is_color_channel;
        target.is_normal_channel = material_target.is_normal_channel;
        target.channel = material_target.channel;
        target.channel_name = BKE_paint_material_channel_info(material_target.channel).ui_name;
        if (material_target.is_color_channel) {
          const float3 rgb = float3(material_target.color);
          target.color_override = float4(rgb, 1.0f);
        }
        else if (material_target.is_normal_channel) {
          /* Pack tangent into 0..1 so byte and float blend paths share one brush color. */
          float packed[3];
          BKE_pbr_normal_pack(material_target.color, false, packed);
          target.color_override = float4(packed[0], packed[1], packed[2], 1.0f);
        }
        else {
          const float v = material_target.value;
          target.color_override = float4(v, v, v, 1.0f);
        }
        targets.append(std::move(target));
      }
      break;
    }
    case PAINT_CANVAS_SOURCE_IMAGE: {
      Image *image = nullptr;
      ImageUser *image_user = nullptr;
      if (!BKE_paint_canvas_image_get(&paint_mode_settings, &ob, &image, &image_user)) {
        break;
      }
      ImagePaintTarget target;
      target.data = ImageData::from_image(image, image_user);
      if (target.data) {
        target.channel_name = image->id.name + 2;
        targets.append(std::move(target));
      }
      break;
    }
    case PAINT_CANVAS_SOURCE_COLOR_ATTRIBUTE:
    case PAINT_CANVAS_SOURCE_MATERIAL_PAINT:
      break;
  }

  return targets;
}

static void fetch_image_buffers(ImageData &image_data,
                                bke::pbvh::Node & /*node*/,
                                PixelNode &pixel_node)
{
  PRF_scope(ProfileCategory::Editor);
  for (const UDIMTilePixels &tile : pixel_node.tiles) {
    const ImBuf *buffer = image_data.buffers.lookup_or_add_cb(tile.tile_number, [&]() {
      ImageUser tile_user = *image_data.image_user;
      tile_user.tile = tile.tile_number;

      return BKE_image_acquire_ibuf(image_data.image, &tile_user, nullptr);
    });

    if (buffer) {
      image_data.processors.lookup_or_add_cb(tile.tile_number, [&]() {
        const StringRefNull buffer_colorspace_name =
            buffer->float_data() ? IMB_colormanagement_get_float_colorspace(buffer) :
                                   IMB_colormanagement_get_byte_colorspace(buffer);

        const ColorSpace *buffer_colorspace = IMB_colormanagement_space_get_named(
            buffer_colorspace_name);

        TileColorspaceProcessor processor;
        if (!buffer_colorspace) {
          return processor;
        }
        ColormanageProcessor buffer_to_linear =
            ColormanageProcessor::colorspace_processor_to_scene_linear_new(*buffer_colorspace);
        if (buffer_to_linear.is_noop()) {
          return processor;
        }

        processor.buffer_to_linear_processor = std::move(buffer_to_linear);
        processor.linear_to_buffer_processor =
            ColormanageProcessor::colorspace_processor_from_scene_linear_new(*buffer_colorspace);
        processor.is_noop = false;

        return processor;
      });
    }
  }
}

static float3 calc_pixel_position(const Span<float3> vert_positions,
                                  const Span<int3> vert_tris,
                                  const int tri_index,
                                  const float2 &barycentric_weight)
{
  PRF_scope(ProfileCategory::Editor);
  const int3 &verts = vert_tris[tri_index];
  const float3 weights(barycentric_weight.x,
                       barycentric_weight.y,
                       1.0f - barycentric_weight.x - barycentric_weight.y);
  float3 result;
  interp_v3_v3v3v3(result,
                   vert_positions[verts[0]],
                   vert_positions[verts[1]],
                   vert_positions[verts[2]],
                   weights);
  return result;
}

static void calc_pixel_row_positions(const Span<float3> vert_positions,
                                     const Span<int3> vert_tris,
                                     const Span<int> tri_indices,
                                     const Span<float2> delta_barycentric_coords,
                                     const PackedPixelRow &pixel_row,
                                     const IndexRange range,
                                     const MutableSpan<float3> positions)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(positions.size() == range.size());
  const float3 first = calc_pixel_position(vert_positions,
                                           vert_tris,
                                           tri_indices[pixel_row.uv_primitive_index],
                                           pixel_row.start_barycentric_coord);
  const float3 second = calc_pixel_position(
      vert_positions,
      vert_tris,
      tri_indices[pixel_row.uv_primitive_index],
      pixel_row.start_barycentric_coord + delta_barycentric_coords[pixel_row.uv_primitive_index]);
  const float3 delta = second - first;

  const float3 start = first + delta * range.start();

  for (const int i : positions.index_range()) {
    positions[i] = start + delta * i;
  }
}

static BitVector<> init_uv_primitives_brush_test(SculptSession &ss,
                                                 const Span<int3> vert_tris,
                                                 const Span<int> tri_indices,
                                                 const Span<float3> positions)
{
  PRF_scope(ProfileCategory::Editor);
  const float3 location = ss.cache ? ss.cache->location_symm : ss.cursor_location;
  const float radius = ss.cache ? ss.cache->radius : ss.cursor_radius;
  const Bounds<float3> brush_bounds(location - radius, location + radius);

  BitVector<> brush_test(tri_indices.size());
  for (const int i : tri_indices.index_range()) {
    const int3 verts = vert_tris[tri_indices[i]];

    Bounds<float3> tri_bounds(positions[verts[0]]);
    math::min_max(positions[verts[1]], tri_bounds.min, tri_bounds.max);
    math::min_max(positions[verts[2]], tri_bounds.min, tri_bounds.max);

    brush_test[i].set(
        isect_aabb_aabb_v3(brush_bounds.min, brush_bounds.max, tri_bounds.min, tri_bounds.max));
  }
  return brush_test;
}

/** Apply the per-pixel factor to the initial brush color. */
static void calc_brush_colors(MutableSpan<float4> buffer_colors,
                              Span<float> factors,
                              const float4 &brush_color)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(buffer_colors.size() == factors.size());

  for (const int i : buffer_colors.index_range()) {
    buffer_colors[i] = brush_color * factors[i];
  }
}

static MutableSpan<float4> read_image_pixels(MutableSpan<float4> image_pixels,
                                             const TileColorspaceProcessor &processors,
                                             const PackedPixelRow &pixel_row,
                                             const IndexRange range,
                                             const int width)
{
  PRF_scope(ProfileCategory::Editor);
  const int start_offset = int(pixel_row.start_image_coordinate.y) * width +
                           int(pixel_row.start_image_coordinate.x) + range.start();
  MutableSpan<float4> scene_linear_pixels = image_pixels.slice(start_offset, range.size());

  if (processors.is_noop) {
    return scene_linear_pixels;
  }

  processors.buffer_to_linear_processor.apply(
      reinterpret_cast<float *>(scene_linear_pixels.data()), range.size(), 1, 4, false);

  return scene_linear_pixels;
}

static MutableSpan<float4> read_image_pixels(Span<uchar4> image_pixels,
                                             const TileColorspaceProcessor &processors,
                                             const PackedPixelRow &pixel_row,
                                             const IndexRange range,
                                             const int width,
                                             Vector<float4> &storage)
{
  PRF_scope(ProfileCategory::Editor);
  storage.resize(range.size());
  const int start_offset = int(pixel_row.start_image_coordinate.y) * width +
                           int(pixel_row.start_image_coordinate.x) + range.start();

  for (int i = 0; i < range.size(); i++) {
    rgba_uchar_to_float(storage[i], image_pixels[start_offset + i]);
  }

  if (processors.is_noop) {
    return storage;
  }

  processors.buffer_to_linear_processor.apply(
      reinterpret_cast<float *>(storage.data()), range.size(), 1, 4, false);

  return storage;
}

static void write_image_pixels(MutableSpan<float4> scene_linear_pixels,
                               MutableSpan<uchar4> image_pixels,
                               const TileColorspaceProcessor &processors,
                               const PackedPixelRow &pixel_row,
                               const IndexRange range,
                               const int width)
{
  PRF_scope(ProfileCategory::Editor);
  if (!processors.is_noop) {
    processors.linear_to_buffer_processor.apply(
        reinterpret_cast<float *>(scene_linear_pixels.data()), range.size(), 1, 4, false);
  }

  const int start_offset = int(pixel_row.start_image_coordinate.y) * width +
                           int(pixel_row.start_image_coordinate.x) + range.start();

  for (int i = 0; i < range.size(); i++) {
    rgba_float_to_uchar(image_pixels[start_offset + i], scene_linear_pixels[i]);
  }
}

static void write_image_pixels(MutableSpan<float4> scene_linear_pixels,
                               MutableSpan<float4> image_pixels,
                               const TileColorspaceProcessor &processors,
                               const PackedPixelRow &pixel_row,
                               const IndexRange range,
                               const int width)
{
  PRF_scope(ProfileCategory::Editor);
  if (!processors.is_noop) {
    processors.linear_to_buffer_processor.apply(
        reinterpret_cast<float *>(scene_linear_pixels.data()), range.size(), 1, 4, false);
  }

  const int start_offset = int(pixel_row.start_image_coordinate.y) * width +
                           int(pixel_row.start_image_coordinate.x) + range.start();

  std::copy_n(scene_linear_pixels.begin(), range.size(), image_pixels.begin() + start_offset);
}

static void blend_colors(MutableSpan<float4> paint_pixels,
                         Span<float4> scene_linear_pixels,
                         const Brush &brush,
                         const IMB_BlendMode blend_mode,
                         const bool is_float_storage)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(paint_pixels.size() == scene_linear_pixels.size());

  if (blend_mode == IMB_BLEND_NORMAL_MIX) {
    for (const int i : paint_pixels.index_range()) {
      const float t = math::clamp(paint_pixels[i][3] * brush.alpha, 0.0f, 1.0f);
      float target_n[3] = {paint_pixels[i][0] * 2.0f - 1.0f,
                           paint_pixels[i][1] * 2.0f - 1.0f,
                           paint_pixels[i][2] * 2.0f - 1.0f};
      float out[3];
      BKE_pbr_normal_blend_mix(scene_linear_pixels[i], target_n, t, is_float_storage, out);
      paint_pixels[i][0] = out[0];
      paint_pixels[i][1] = out[1];
      paint_pixels[i][2] = out[2];
      paint_pixels[i][3] = scene_linear_pixels[i][3];
    }
    return;
  }

  /* Mix the initial image color with the paint color. */
  for (const int i : paint_pixels.index_range()) {
    blend_color_mix_float(paint_pixels[i], scene_linear_pixels[i], paint_pixels[i]);
    paint_pixels[i] *= brush.alpha;
  }

  /* Apply the blended color to the original image with the brush alpha. */
  IMB_blend_color_float(paint_pixels, scene_linear_pixels, paint_pixels, blend_mode);
}

#ifdef DEBUG_PIXEL_NODES
static void apply_debug_color(MutableSpan<float4> paint_pixels, const PackedPixelRow &pixel_row)
{
  if ((pixel_row.start_image_coordinate.y >> 3) & 1) {
    for (const int i : paint_pixels.index_range()) {
      paint_pixels[i][0] *= 0.5f;
      paint_pixels[i][1] *= 0.5f;
      paint_pixels[i][2] *= 0.5f;
    }
  }
}
#endif

struct PaintLocalData {
  Vector<float3> pixel_positions;
  Vector<float3> pixel_normals;
  Vector<float> distances;
  Vector<float> factors;

  Vector<float4> byte_to_float_pixels;
  Vector<float4> paint_pixels;

  MutableSpan<float4> scene_linear_pixels;

  /** Undecoded Base Color source samples for one chunk, decoded to scene-linear in one batched
   * call instead of per-pixel (see #ChannelSourceSampler::color and its `decode_linear` param). */
  Vector<float3> raw_source_colors;
};

static Bounds<int2> merge_bounds(const Bounds<int2> &a, const Bounds<int2> &b)
{
  return bounds::merge(a, b);
}

static Bounds<int2> negative_bounds()
{
  return {int2(std::numeric_limits<int>::max()), int2(std::numeric_limits<int>::lowest())};
}

/** Per-face material filter for Mode=Material image painting (active slot only). */
struct MaterialPaintFilter {
  bool use_filter = false;
  int active_material_index = 0;
  std::optional<int> face_material_single;
  Span<int> face_materials;
  Span<int> corner_tri_faces;

  static MaterialPaintFilter from_object(Object &object, const bool material_canvas_mode)
  {
    MaterialPaintFilter filter;
    if (!material_canvas_mode || object.totcol <= 1) {
      return filter;
    }

    const Mesh &mesh = *id_cast<const Mesh *>(object.data);
    filter.use_filter = true;
    filter.active_material_index = math::max(object.actcol - 1, 0);
    filter.corner_tri_faces = mesh.corner_tri_faces();

    const VArray<int> material_indices = *mesh.attributes().lookup_or_default<int>(
        "material_index", bke::AttrDomain::Face, 0);
    if (material_indices.is_single()) {
      filter.face_material_single = material_indices.get_internal_single();
    }
    else {
      filter.face_materials = material_indices.get_internal_span();
    }
    return filter;
  }

  bool uv_primitive_matches(const PixelNode &pixel_node, const int uv_primitive_index) const
  {
    if (!use_filter) {
      return true;
    }
    const int tri_index = pixel_node.uv_primitives.tri_indices[uv_primitive_index];
    const int face_i = corner_tri_faces[tri_index];
    const int face_material_index = face_material_single ? *face_material_single :
                                                           face_materials[face_i];
    return face_material_index == active_material_index;
  }
};

/**
 * Per-tile cache of brush falloff/hardness/strength/texture factors for one pixel node.
 * These only depend on brush, stroke cache and geometry, never on which Material channel image
 * is being written, so they are computed once per dab and shared by every enabled channel
 * instead of being re-evaluated per channel.
 */
struct RowFactorCache {
  /* IndexMaskMemory is non-movable; heap-own it so RowFactorCache itself stays movable
   * (needed to live in a resizable Array without disturbing pointers the IndexMask holds
   * into it). */
  std::unique_ptr<IndexMaskMemory> memory = std::make_unique<IndexMaskMemory>();
  IndexMask valid_rows;
  Array<bool> row_changed;
  /** Indexed like #UDIMTilePixels::pixel_rows; only entries in #valid_rows are populated. */
  Array<Array<float>> row_factors;
  /** Object-space texel positions, indexed like #row_factors. Only filled when a channel has a
   * source texture to sample; otherwise left empty so ordinary strokes pay nothing. */
  Array<Array<float3>> row_positions;
  /** #material::TexelSampleContext built once per texel from #row_positions, indexed the same
   * way. #apply_paint_channel is called once per enabled channel and each call used to rebuild
   * this (a view-projection matrix multiply, among other things) per pixel per channel; building
   * it once here and reusing it across every channel's call removes that per-channel repeat. */
  Array<Array<material::TexelSampleContext>> row_contexts;
  /** Object-space interpolated (unit) vertex normal at each texel, indexed like #row_positions.
   * Only the Normal channel consumes this, to re-express a sampled source normal relative to the
   * destination surface's own tangent basis; computed alongside #row_positions regardless, since
   * building it there is cheap next to the source-texture sampling #row_positions exists for. */
  Array<Array<float3>> row_normals;
  Bounds<int2> dirty_bounds = negative_bounds();
};

static Array<RowFactorCache> compute_paint_row_factors(
    SculptSession &ss,
    const PixelData &pbvh_data,
    const Span<float3> positions,
    const Span<float3> vert_normals,
    const Brush &brush,
    const MaterialPaintFilter &material_filter,
    const ed::sculpt_paint::material::ChannelSourceSampler *active_sampler,
    PixelNode &pixel_node)
{
  PRF_scope(ProfileCategory::Editor);
  const StrokeCache &cache = *ss.cache;
  /* Must mirror the sampler actually passed to #apply_paint_channel: computing this from
   * `cache.material_source_sampler` and `invert` again here could silently diverge from it. */
  const bool needs_positions = active_sampler != nullptr;

  BitVector<> brush_test = init_uv_primitives_brush_test(
      ss, pbvh_data.vert_tris, pixel_node.uv_primitives.tri_indices, positions);

  Array<RowFactorCache> tile_caches(pixel_node.tiles.size());

  for (const int tile_i : pixel_node.tiles.index_range()) {
    UDIMTilePixels &tile_data = pixel_node.tiles[tile_i];
    RowFactorCache &tile_cache = tile_caches[tile_i];

    tile_cache.valid_rows = IndexMask::from_predicate(
        tile_data.pixel_rows.index_range(), *tile_cache.memory, [&](const int i) {
          const PackedPixelRow &pixel_row = tile_data.pixel_rows[i];
          if (!brush_test[pixel_row.uv_primitive_index]) {
            return false;
          }
          return material_filter.uv_primitive_matches(pixel_node, pixel_row.uv_primitive_index);
        });

    tile_cache.row_changed = Array<bool>(tile_cache.valid_rows.min_array_size(), false);
    tile_cache.row_factors.reinitialize(tile_cache.valid_rows.min_array_size());
    if (needs_positions) {
      tile_cache.row_positions.reinitialize(tile_cache.valid_rows.min_array_size());
      tile_cache.row_normals.reinitialize(tile_cache.valid_rows.min_array_size());
      tile_cache.row_contexts.reinitialize(tile_cache.valid_rows.min_array_size());
    }

    threading::EnumerableThreadSpecific<PaintLocalData> all_factor_tls;
    tile_cache.valid_rows.foreach_index(
        [&](const int row_i) {
          const PackedPixelRow pixel_row = tile_data.pixel_rows[row_i];
          const int row_size = pixel_row.num_pixels;
          Array<float> &row_factors = tile_cache.row_factors[row_i];
          row_factors.reinitialize(row_size);
          if (needs_positions) {
            tile_cache.row_positions[row_i].reinitialize(row_size);
            tile_cache.row_normals[row_i].reinitialize(row_size);
            tile_cache.row_contexts[row_i].reinitialize(row_size);
          }

          threading::parallel_for(IndexRange(row_size), 512, [&](const IndexRange range) {
            PaintLocalData &tls = all_factor_tls.local();
            tls.pixel_positions.resize(range.size());
            calc_pixel_row_positions(positions,
                                     pbvh_data.vert_tris,
                                     pixel_node.uv_primitives.tri_indices,
                                     pixel_node.uv_primitives.delta_barycentric_coords,
                                     pixel_row,
                                     range,
                                     tls.pixel_positions);
            if (needs_positions) {
              tile_cache.row_positions[row_i].as_mutable_span().slice(range).copy_from(
                  tls.pixel_positions);

              /* Built once per texel here instead of once per channel in #apply_paint_channel;
               * see #RowFactorCache::row_contexts. */
              MutableSpan<material::TexelSampleContext> contexts =
                  tile_cache.row_contexts[row_i].as_mutable_span().slice(range);
              for (const int i : range.index_range()) {
                contexts[i] = material::sculpt_texel_sample_context(ss, tls.pixel_positions[i]);
              }

              tls.pixel_normals.resize(range.size());
              calc_pixel_row_positions(vert_normals,
                                       pbvh_data.vert_tris,
                                       pixel_node.uv_primitives.tri_indices,
                                       pixel_node.uv_primitives.delta_barycentric_coords,
                                       pixel_row,
                                       range,
                                       tls.pixel_normals);
              for (float3 &normal : tls.pixel_normals) {
                /* Barycentric interpolation of unit vertex normals is not itself unit length. */
                normal = math::normalize(normal);
              }
              tile_cache.row_normals[row_i].as_mutable_span().slice(range).copy_from(
                  tls.pixel_normals);
            }

            MutableSpan<float> factors = row_factors.as_mutable_span().slice(range);
            factors.fill(1.0f);

            tls.distances.resize(range.size());
            calc_brush_distances(
                ss, tls.pixel_positions, eBrushFalloffShape(brush.falloff_shape), tls.distances);
            filter_distances_with_radius(cache.radius, tls.distances, factors);
            apply_hardness_to_distances(cache, tls.distances);
            calc_brush_strength_factors(cache, brush, tls.distances, factors);
            calc_brush_texture_factors(ss, brush, tls.pixel_positions, factors);
            scale_factors(factors, cache.bstrength);
          });

          if (std::ranges::all_of(row_factors, [](const float factor) { return factor == 0.0f; }))
          {
            paint_material_channel_perf::add_rows_skipped(1);
            return;
          }
          tile_cache.row_changed[row_i] = true;
          paint_material_channel_perf::add_rows_painted(1);

          const int2 start(pixel_row.start_image_coordinate.x, pixel_row.start_image_coordinate.y);
          const int2 end = start + int2(pixel_row.num_pixels + 1, 0);
          tile_cache.dirty_bounds = bounds::merge(tile_cache.dirty_bounds,
                                                  Bounds<int2>(start, end));
        },
        exec_mode::grain_size(2));
  }

  return tile_caches;
}

/** Apply cached brush factors to one Material paint channel's image (read/blend/write only). */
static void apply_paint_channel(ImageData &image_data,
                                const Brush &brush,
                                const float4 &brush_color,
                                const IMB_BlendMode blend_mode,
                                PixelNode &pixel_node,
                                Span<RowFactorCache> tile_caches,
                                const material::ChannelSourceSampler *sampler,
                                const eMaterialPaintChannel channel,
                                const bool is_color_channel,
                                const bool is_normal_channel,
                                const float3 &view_right,
                                const float3 & /*view_up*/,
                                const ARegion *region,
                                const float4x4 &projection_mat)
{
  PRF_scope(ProfileCategory::Editor);

#ifdef PBR_PAINT_IMAGE_PROFILE
  ChannelWriteProfile &channel_wprof = g_channel_write_profile[channel];
  channel_wprof.pixel_num.store(0);
  channel_wprof.sample_seconds.store(0.0);
  channel_wprof.read_seconds.store(0.0);
  channel_wprof.blend_seconds.store(0.0);
  channel_wprof.write_seconds.store(0.0);
  const double apply_start = BLI_time_now_seconds();
#endif

#ifdef DEBUG_PIXEL_NODES
  float4 debug_color;
  uint hash = BLI_hash_int(POINTER_AS_UINT(&pixel_node));

  debug_color[0] = float(hash & 255) / 255.0f;
  debug_color[1] = float((hash >> 8) & 255) / 255.0f;
  debug_color[2] = float((hash >> 16) & 255) / 255.0f;
  debug_color[3] = 1.0f;
#endif

  bool pixels_updated = false;
  for (const int tile_i : pixel_node.tiles.index_range()) {
    UDIMTilePixels &tile_data = pixel_node.tiles[tile_i];
    const RowFactorCache &tile_cache = tile_caches[tile_i];

    ImBuf *image_buffer = image_data.buffers.lookup_default(tile_data.tile_number, nullptr);
    if (image_buffer == nullptr) {
      continue;
    }

    MutableSpan<float4> float_buffer;
    MutableSpan<uchar4> byte_buffer;

    if (image_buffer->float_data()) {
      BLI_assert(ELEM(image_buffer->channels, 0, 4));
      float_buffer = MutableSpan(reinterpret_cast<float4 *>(image_buffer->float_data_for_write()),
                                 image_buffer->x * image_buffer->y);
    }
    else {
      byte_buffer = MutableSpan(reinterpret_cast<uchar4 *>(image_buffer->byte_data_for_write()),
                                image_buffer->x * image_buffer->y);
    }

    const TileColorspaceProcessor *processors = image_data.processors.lookup_ptr(
        tile_data.tile_number);

    threading::EnumerableThreadSpecific<PaintLocalData> all_tls;
    tile_cache.valid_rows.foreach_index(
        [&](const int row_i) {
          if (!tile_cache.row_changed[row_i]) {
            return;
          }
          const PackedPixelRow pixel_row = tile_data.pixel_rows[row_i];
          const int row_size = pixel_row.num_pixels;
          Span<float> row_factors = tile_cache.row_factors[row_i];
          const Span<material::TexelSampleContext> row_contexts =
              sampler != nullptr ? tile_cache.row_contexts[row_i].as_span() :
                                   Span<material::TexelSampleContext>();
          const Span<float3> row_positions = sampler != nullptr ?
                                                 tile_cache.row_positions[row_i].as_span() :
                                                 Span<float3>();
          const Span<float3> row_normals = sampler != nullptr ?
                                               tile_cache.row_normals[row_i].as_span() :
                                               Span<float3>();
          /* Destination tangent basis `T_m` (flat per-triangle, from the UV parametrization) and
           * its handedness; `B_m = bitangent_sign * cross(N_m, T_m)`, `N_m` interpolated below
           * per pixel. Together these express a world/object-space Normal-channel sample in this
           * surface's own tangent space instead of writing it out unchanged. */
          const float3 tri_tangent =
              is_normal_channel && sampler != nullptr ?
                  pixel_node.uv_primitives.tangents[pixel_row.uv_primitive_index] :
                  float3(1.0f, 0.0f, 0.0f);
          const int tri_position_start = pixel_row.uv_primitive_index * 3;
          const Span<float3> tri_positions =
              pixel_node.uv_primitives.triangle_positions.as_span().slice(tri_position_start, 3);
          const Span<float2> tri_uvs = pixel_node.uv_primitives.triangle_uvs.as_span().slice(
              tri_position_start, 3);
          const float tri_bitangent_sign =
              is_normal_channel && sampler != nullptr ?
                  pixel_node.uv_primitives.bitangent_signs[pixel_row.uv_primitive_index] :
                  1.0f;

          threading::parallel_for(IndexRange(row_size), 512, [&](const IndexRange range) {
            Span<float> factors = row_factors.slice(range);
            /* Chunk may still be all-zero even though the row has some nonzero factor
             * elsewhere; skip it exactly like the un-cached path used to. */
            if (std::ranges::all_of(factors, [](const float factor) { return factor == 0.0f; })) {
              paint_material_channel_perf::add_rows_skipped(1);
              return;
            }
            paint_material_channel_perf::add_pixels_painted(range.size());

            PaintLocalData &tls = all_tls.local();
            tls.paint_pixels.resize(range.size());
#ifdef PBR_PAINT_IMAGE_PROFILE
            const double sample_phase_start = BLI_time_now_seconds();
#endif
            if (sampler != nullptr) {
              /* A source texture replaces the channel's fixed value; the cached brush factor
               * still carries falloff, so the stroke shape is unchanged. */
              BLI_assert(!row_contexts.is_empty());
              const Span<material::TexelSampleContext> contexts = row_contexts.slice(range);
              /* Only used by the debug normal-projection printf below, which needs the raw
               * object-space position rather than the derived sampling context. */
              const Span<float3> positions = row_positions.slice(range);
              const int thread_id = BLI_task_parallel_thread_id(nullptr);

              /* Base Color's source is typically a byte (non-scene-linear) image; decoding it
               * one #IMB_colormanagement_colorspace_to_scene_linear_v3 call per pixel is the
               * dominant cost of painting into that channel (confirmed by profiling: read/blend/
               * write of the destination buffer is ~8% of paint_pixels, this per-pixel decode is
               * the rest). Gather the whole chunk undecoded and decode it in one batched call
               * instead - the batch variant is documented as much faster than per-pixel. */
              const bool batch_decode_color = is_color_channel && !is_normal_channel &&
                                              sampler->needs_linear_conversion(channel);
              if (batch_decode_color) {
                tls.raw_source_colors.resize(range.size());
                for (const int i : range.index_range()) {
                  tls.raw_source_colors[i] = sampler->color(
                      channel, contexts[i], thread_id, /*decode_linear=*/false);
                }
                material::ChannelSourceSampler::decode_linear_batch(tls.raw_source_colors,
                                                                    sampler->colorspace(channel));
              }

              for (const int i : range.index_range()) {
                float3 rgb;
                if (is_normal_channel) {
                  /* `sampler->color()` returns the sample still in the decal's own basis (screen
                   * right/up/backward for a View-mapped texture). Two corrections are needed
                   * before it means anything in object space:
                   * 1) Perspective: "screen right" itself is not one fixed world direction, it is
                   *    whatever is horizontal for the specific ray from the camera to this point;
                   *    re-derive it by orthogonalizing the camera's right axis against that ray,
                   *    or every point on screen would incorrectly share one global orientation.
                   * 2) Surface-attached: a neutral decal (0,0,1) must stay neutral no matter how
                   *    the surface is turned toward the camera, so the decal's "outward" axis is
                   *    the surface normal itself, and its (ray-corrected) right/up axes are then
                   *    further projected onto the surface's own tangent plane. */
                  const float3 n_d = sampler->color(channel, contexts[i], thread_id);
                  /* Use the flat normal of the UV primitive for the destination basis. This is
                   * required for hard-surface faces such as a Cube; interpolated vertex normals
                   * would tilt the basis toward adjacent faces. */
                  const float3 edge1 = tri_positions[1] - tri_positions[0];
                  const float3 edge2 = tri_positions[2] - tri_positions[0];
                  const float3 face_normal = math::normalize(math::cross(edge1, edge2));
                  const float3 &n_m = face_normal;

                  const float2 screen[3] = {
                      ED_view3d_project_float_v2_m4(region, tri_positions[0], projection_mat),
                      ED_view3d_project_float_v2_m4(region, tri_positions[1], projection_mat),
                      ED_view3d_project_float_v2_m4(region, tri_positions[2], projection_mat)};
                  const float2 sx = screen[1] - screen[0];
                  const float2 sy = screen[2] - screen[0];
                  const float det = sx.x * sy.y - sx.y * sy.x;
                  float3 t_screen;
                  float3 b_screen;
                  if (math::abs(det) > 1e-8f) {
                    const float3 dp_dx = (edge1 * sy.y - edge2 * sx.y) / det;
                    const float3 dp_dy = (edge2 * sx.x - edge1 * sy.x) / det;
                    /* Keep only the orientation of the screen differential. Its magnitude is
                     * object-units per screen pixel and must not scale the tangent components of
                     * an already normalized tangent-space normal-map sample. */
                    t_screen = dp_dx - n_m * math::dot(dp_dx, n_m);
                    /* Complete the surface basis from the projected screen-right direction. The
                     * cross product fixes the orientation relative to the face normal; using the
                     * raw screen Y direction here produces a left-handed basis on some Cube faces
                     * because their screen winding differs. */
                    b_screen = math::cross(n_m, t_screen);
                    const float t_screen_len = math::length(t_screen);
                    const float b_screen_len = math::length(b_screen);
                    if (t_screen_len > 1e-6f) {
                      t_screen /= t_screen_len;
                    }
                    if (b_screen_len > 1e-6f) {
                      b_screen /= b_screen_len;
                    }
                  }
                  else {
                    t_screen = view_right - n_m * math::dot(view_right, n_m);
                    t_screen = math::normalize(t_screen);
                    b_screen = math::cross(n_m, t_screen);
                  }
                  const float3 n_local = n_d.x * t_screen + n_d.y * b_screen + n_d.z * n_m;

                  float3 t_m = tri_tangent - n_m * math::dot(tri_tangent, n_m);
                  const float t_len = math::length(t_m);
                  if (t_len > 1e-6f) {
                    t_m /= t_len;
                  }
                  else {
                    float fallback[3];
                    ortho_v3_v3(fallback, n_m);
                    t_m = math::normalize(float3(fallback));
                  }
                  const float3 b_m = math::cross(n_m, t_m) * tri_bitangent_sign;
                  float3 n_t(
                      math::dot(n_local, t_m), math::dot(n_local, b_m), math::dot(n_local, n_m));
                  const float n_t_len = math::length(n_t);
                  n_t = n_t_len > 1e-6f ? n_t / n_t_len : float3(0.0f, 0.0f, 1.0f);
                  float packed[3];
                  BKE_pbr_normal_pack(n_t, false, packed);
                  rgb = float3(packed[0], packed[1], packed[2]);

                  if (DEBUG_NORMAL_PROJECTION &&
                      pixel_row.uv_primitive_index < DEBUG_NORMAL_PROJECTION_MAX_PRIMITIVE &&
                      i == 0)
                  {
                    const int pixel_x = int(pixel_row.start_image_coordinate.x) +
                                        int(range.start()) + i;
                    /* `row_i` indexes the packed row array, not image-space Y. The cached
                     * positions/barycentrics for this chunk advance only along the row. */
                    const int pixel_y = int(pixel_row.start_image_coordinate.y);
                    const float2 bary12 =
                        pixel_row.start_barycentric_coord +
                        pixel_node.uv_primitives
                                .delta_barycentric_coords[pixel_row.uv_primitive_index] *
                            float2(float(range.start()) + float(i), 0.0f);
                    const float3 bary(bary12.x, bary12.y, 1.0f - bary12.x - bary12.y);
                    const float2 pixel_uv = tri_uvs[0] * bary.x + tri_uvs[1] * bary.y +
                                            tri_uvs[2] * bary.z;
                    const float2 pixel_screen = ED_view3d_project_float_v2_m4(
                        region, positions[i], projection_mat);
                    printf(
                        "[NORMAL_PROJECTION] prim=%u pixel=(%d,%d) "
                        "P=((%.6g,%.6g,%.6g),(%.6g,%.6g,%.6g),(%.6g,%.6g,%.6g)) "
                        "UV=((%.6g,%.6g),(%.6g,%.6g),(%.6g,%.6g)) "
                        "b=(%.6g,%.6g,%.6g) uv=(%.6g,%.6g) "
                        "S=((%.6g,%.6g),(%.6g,%.6g),(%.6g,%.6g)) det=%.6g "
                        "screenP=(%.6g,%.6g) "
                        "nD=(%.6g,%.6g,%.6g) nFace=(%.6g,%.6g,%.6g) "
                        "nInterp=(%.6g,%.6g,%.6g) "
                        "Ts=(%.6g,%.6g,%.6g) Bs=(%.6g,%.6g,%.6g) "
                        "Tm=(%.6g,%.6g,%.6g) Bm=(%.6g,%.6g,%.6g) sign=%.1f "
                        "nLocal=(%.6g,%.6g,%.6g) nT=(%.6g,%.6g,%.6g) "
                        "packed=(%.6g,%.6g,%.6g)\n",
                        unsigned(pixel_row.uv_primitive_index),
                        pixel_x,
                        pixel_y,
                        tri_positions[0].x,
                        tri_positions[0].y,
                        tri_positions[0].z,
                        tri_positions[1].x,
                        tri_positions[1].y,
                        tri_positions[1].z,
                        tri_positions[2].x,
                        tri_positions[2].y,
                        tri_positions[2].z,
                        tri_uvs[0].x,
                        tri_uvs[0].y,
                        tri_uvs[1].x,
                        tri_uvs[1].y,
                        tri_uvs[2].x,
                        tri_uvs[2].y,
                        bary.x,
                        bary.y,
                        bary.z,
                        pixel_uv.x,
                        pixel_uv.y,
                        screen[0].x,
                        screen[0].y,
                        screen[1].x,
                        screen[1].y,
                        screen[2].x,
                        screen[2].y,
                        det,
                        pixel_screen.x,
                        pixel_screen.y,
                        n_d.x,
                        n_d.y,
                        n_d.z,
                        face_normal.x,
                        face_normal.y,
                        face_normal.z,
                        n_m.x,
                        n_m.y,
                        n_m.z,
                        t_screen.x,
                        t_screen.y,
                        t_screen.z,
                        b_screen.x,
                        b_screen.y,
                        b_screen.z,
                        t_m.x,
                        t_m.y,
                        t_m.z,
                        b_m.x,
                        b_m.y,
                        b_m.z,
                        tri_bitangent_sign,
                        n_local.x,
                        n_local.y,
                        n_local.z,
                        n_t.x,
                        n_t.y,
                        n_t.z,
                        rgb.x,
                        rgb.y,
                        rgb.z);
                  }
                }
                else if (is_color_channel) {
                  rgb = batch_decode_color ? tls.raw_source_colors[i] :
                                             sampler->color(channel, contexts[i], thread_id);
                }
                else {
                  const float value = sampler->scalar(channel, contexts[i], thread_id);
                  rgb = float3(value);
                }
                if (blend_mode == IMB_BLEND_NORMAL_MIX) {
                  /* Keep packed tangent RGB intact; strength lives in alpha as mix t. */
                  tls.paint_pixels[i] = float4(rgb[0], rgb[1], rgb[2], factors[i]);
                }
                else {
                  tls.paint_pixels[i] = float4(rgb[0], rgb[1], rgb[2], 1.0f) * factors[i];
                }
              }
            }
            else if (blend_mode == IMB_BLEND_NORMAL_MIX) {
              /* Keep packed tangent RGB intact; strength lives in alpha as mix t. */
              for (const int i : range.index_range()) {
                tls.paint_pixels[i] = float4(
                    brush_color[0], brush_color[1], brush_color[2], factors[i]);
              }
            }
            else {
              calc_brush_colors(tls.paint_pixels, factors, brush_color);
            }

#ifdef PBR_PAINT_IMAGE_PROFILE
            ChannelWriteProfile &wprof = g_channel_write_profile[channel];
            wprof.sample_seconds.fetch_add(BLI_time_now_seconds() - sample_phase_start);
            wprof.noop_processor.store(processors->is_noop ? 1 : 0);
            const double read_start = BLI_time_now_seconds();
#endif
            if (!float_buffer.is_empty()) {
              tls.scene_linear_pixels = read_image_pixels(
                  float_buffer, *processors, pixel_row, range, image_buffer->x);
            }
            else {
              tls.scene_linear_pixels = read_image_pixels(byte_buffer,
                                                          *processors,
                                                          pixel_row,
                                                          range,
                                                          image_buffer->x,
                                                          tls.byte_to_float_pixels);
            }

#ifdef DEBUG_PIXEL_NODES
            apply_debug_color(scene_linear_pixels, pixel_row);
#endif

#ifdef PBR_PAINT_IMAGE_PROFILE
            const double blend_start = BLI_time_now_seconds();
            wprof.read_seconds.fetch_add(blend_start - read_start);
#endif
            blend_colors(tls.paint_pixels,
                         tls.scene_linear_pixels,
                         brush,
                         blend_mode,
                         !float_buffer.is_empty());

#ifdef PBR_PAINT_IMAGE_PROFILE
            const double write_start = BLI_time_now_seconds();
            wprof.blend_seconds.fetch_add(write_start - blend_start);
#endif
            if (!float_buffer.is_empty()) {
              write_image_pixels(
                  tls.paint_pixels, float_buffer, *processors, pixel_row, range, image_buffer->x);
            }
            else {
              write_image_pixels(
                  tls.paint_pixels, byte_buffer, *processors, pixel_row, range, image_buffer->x);
            }
#ifdef PBR_PAINT_IMAGE_PROFILE
            wprof.write_seconds.fetch_add(BLI_time_now_seconds() - write_start);
            wprof.pixel_num.fetch_add(range.size());
#endif
          });
        },
        exec_mode::grain_size(2));

    if (!tile_cache.dirty_bounds.is_empty()) {
      tile_data.mark_dirty(tile_cache.dirty_bounds);
    }

    if (tile_data.flags.dirty) {
      BKE_image_mark_dirty(image_data.image, image_buffer);
    }
    pixels_updated |= tile_data.flags.dirty;
  }

  pixel_node.flags.dirty |= pixels_updated;

#ifdef PBR_PAINT_IMAGE_PROFILE
  const int64_t pixels = channel_wprof.pixel_num.load();
  if (pixels > 0) {
    const double sample_ms = channel_wprof.sample_seconds.load() * 1000.0;
    const double read_ms = channel_wprof.read_seconds.load() * 1000.0;
    const double blend_ms = channel_wprof.blend_seconds.load() * 1000.0;
    const double write_ms = channel_wprof.write_seconds.load() * 1000.0;
    printf(
        "[pbr_paint] apply_paint_channel channel=%d pixels=%lld noop_colorspace=%d | "
        "sample=%.3fms read=%.3fms blend=%.3fms write=%.3fms total=%.3fms | "
        "per_1k_px sample=%.4fms read=%.4fms blend=%.4fms write=%.4fms\n",
        int(channel),
        static_cast<long long>(pixels),
        channel_wprof.noop_processor.load(),
        sample_ms,
        read_ms,
        blend_ms,
        write_ms,
        (BLI_time_now_seconds() - apply_start) * 1000.0,
        sample_ms / (double(pixels) / 1000.0),
        read_ms / (double(pixels) / 1000.0),
        blend_ms / (double(pixels) / 1000.0),
        write_ms / (double(pixels) / 1000.0));
  }
#endif
}

static void undo_region_tiles(
    ImBuf *ibuf, int x, int y, int w, int h, int *tx, int *ty, int *tw, int *th)
{
  int srcx = 0, srcy = 0;
  IMB_rectclip(ibuf, nullptr, &x, &y, &srcx, &srcy, &w, &h);
  *tw = ((x + w - 1) >> ED_IMAGE_UNDO_TILE_BITS);
  *th = ((y + h - 1) >> ED_IMAGE_UNDO_TILE_BITS);
  *tx = (x >> ED_IMAGE_UNDO_TILE_BITS);
  *ty = (y >> ED_IMAGE_UNDO_TILE_BITS);
}

static void push_undo(const PixelNode &node_data,
                      Image &image,
                      ImageUser &image_user,
                      const TileNumber tile_number,
                      ImBuf &image_buffer)
{
  for (const UDIMTileUndo &tile_undo : node_data.undo_regions) {
    if (tile_undo.tile_number != tile_number) {
      continue;
    }
    int tilex, tiley, tilew, tileh;
    PaintTileMap *undo_tiles = ED_image_paint_tile_map_get();
    undo_region_tiles(&image_buffer,
                      tile_undo.region.xmin,
                      tile_undo.region.ymin,
                      BLI_rcti_size_x(&tile_undo.region),
                      BLI_rcti_size_y(&tile_undo.region),
                      &tilex,
                      &tiley,
                      &tilew,
                      &tileh);
    for (int ty = tiley; ty <= tileh; ty++) {
      for (int tx = tilex; tx <= tilew; tx++) {
        ED_image_paint_tile_push(
            undo_tiles, &image, &image_buffer, &image_user, tx, ty, nullptr, nullptr, true, true);
      }
    }
  }
}

static void do_push_undo_tile(ImageData &image_data,
                              bke::pbvh::Node & /*node*/,
                              PixelNode &pixel_node)
{
  PRF_scope(ProfileCategory::Editor);
  for (const UDIMTilePixels &tile : pixel_node.tiles) {
    ImBuf *buffer = image_data.buffers.lookup_default(tile.tile_number, nullptr);
    if (buffer == nullptr) {
      continue;
    }

    push_undo(pixel_node, *image_data.image, *image_data.image_user, tile.tile_number, *buffer);
  }
}

/* -------------------------------------------------------------------- */

/** \name Fix non-manifold edge bleeding.
 * \{ */

static Vector<image::TileNumber> collect_dirty_tiles(MutableSpan<PixelNode> nodes,
                                                     const IndexMask &node_mask)
{
  Vector<image::TileNumber> dirty_tiles;
  node_mask.foreach_index(
      [&](const int i) { bke::pbvh::pixels::collect_dirty_tiles(nodes[i], dirty_tiles); });
  return dirty_tiles;
}
static void fix_non_manifold_seam_bleeding(bke::pbvh::Tree &pbvh,
                                           Map<paint::image::TileNumber, ImBuf *> &buffers,
                                           Span<TileNumber> tile_numbers_to_fix)
{
  PRF_scope(ProfileCategory::Editor);
  for (image::TileNumber tile_number : tile_numbers_to_fix) {
    bke::pbvh::pixels::copy_pixels(pbvh, buffers, tile_number);
  }
}

static void fix_non_manifold_seam_bleeding(Object &ob,
                                           ImageData &image_data,
                                           MutableSpan<bke::pbvh::MeshNode> /*nodes*/,
                                           MutableSpan<PixelNode> pixel_nodes,
                                           const IndexMask &node_mask)
{
  Vector<image::TileNumber> dirty_tiles = collect_dirty_tiles(pixel_nodes, node_mask);
  fix_non_manifold_seam_bleeding(*bke::object::pbvh_get(ob), image_data.buffers, dirty_tiles);
}

/** \} */

}  // namespace ed::sculpt_paint::paint::image

using namespace blender::ed::sculpt_paint::paint::image;

bool SCULPT_use_image_paint_brush(PaintModeSettings &settings, Object &ob, const Brush *brush)
{
  if (ob.type != OB_MESH) {
    return false;
  }
  switch (settings.canvas_source) {
    case PAINT_CANVAS_SOURCE_MATERIAL: {
      /* Multi-channel Principled maps; empty target list is a no-op (no texpaint fallback). */
      const BrushMaterialPaint *brush_paint = brush ? brush->material_paint : nullptr;
      return !BKE_paint_material_image_targets_get(ob, settings, brush_paint).is_empty();
    }
    case PAINT_CANVAS_SOURCE_MATERIAL_PAINT:
    case PAINT_CANVAS_SOURCE_COLOR_ATTRIBUTE:
      return false;
    case PAINT_CANVAS_SOURCE_IMAGE: {
      Image *image;
      ImageUser *image_user;
      return BKE_paint_canvas_image_get(&settings, &ob, &image, &image_user);
    }
  }
  return false;
}

void SCULPT_do_paint_brush_image(const Depsgraph &depsgraph,
                                 PaintModeSettings &paint_mode_settings,
                                 const Sculpt &sd,
                                 Object &ob,
                                 const IndexMask &node_mask)
{
  PRF_scope(ProfileCategory::Editor);
  const Brush *brush = BKE_paint_brush_for_read(&sd.paint);
  ed::sculpt_paint::StrokeCache &cache = *ob.runtime->sculpt_session->cache;

  if (cache.image_paint_targets.is_empty()) {
    return;
  }

  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();

  const bool material_canvas_mode = paint_mode_settings.canvas_source ==
                                    PAINT_CANVAS_SOURCE_MATERIAL;
  const MaterialPaintFilter material_filter = MaterialPaintFilter::from_object(
      ob, material_canvas_mode);

  const float4 brush_color_default = float4(cache.toggle_settings.invert ?
                                                BKE_brush_secondary_color_get(&sd.paint, brush) :
                                                BKE_brush_color_get(&sd.paint, brush),
                                            1.0f);

  /* Erasing pulls the channel back to its neutral value and must not read a source texture. */
  const ed::sculpt_paint::material::ChannelSourceSampler *sampler =
      cache.toggle_settings.invert ? nullptr : cache.material_source_sampler.get();
  const ed::sculpt_paint::material::ChannelSourceSampler *active_sampler = (sampler != nullptr &&
                                                                            sampler->is_active()) ?
                                                                               sampler :
                                                                               nullptr;

  SculptSession &ss = *ob.runtime->sculpt_session;
  const Span<float3> positions = bke::pbvh::vert_positions_eval(depsgraph, ob);
  const Span<float3> vert_normals = bke::pbvh::vert_normals_eval(depsgraph, ob);

  /* Brush falloff/hardness/strength/texture factors are channel-independent: computed once per
   * dab against the shared pixel-node encoding and reused by every enabled Material channel,
   * instead of being re-evaluated per channel (Base Color, Normal, Roughness, ...). Recomputed
   * whenever the pixel encoding itself is rebuilt below. */
  Array<Array<RowFactorCache>> node_factor_caches(nodes.size());
  bool factor_caches_valid = false;

  int target_index = 0;
  for (ImagePaintTarget &target : cache.image_paint_targets) {
    ImageData &image_data = *target.data;
    ImageUser tile_user = *image_data.image_user;
    ImBuf *tile_buffer = BKE_image_acquire_ibuf(image_data.image, &tile_user, nullptr);
    const int res_x = tile_buffer ? tile_buffer->x : 0;
    const int res_y = tile_buffer ? tile_buffer->y : 0;
    if (tile_buffer) {
      BKE_image_release_ibuf(image_data.image, tile_buffer, nullptr);
    }

    paint_material_channel_perf::target_begin(target_index, target.channel_name, res_x, res_y);
    PAINT_CHANNEL_PERF_SCOPE(TargetTotal);

    /* Base Color and scalars honor invert each dab; Normal erase blends toward flat tangent.
     * The mode is per-channel, so it is resolved once here rather than taken from #Brush.blend. */
    float4 brush_color_storage;
    const float4 *brush_color_ptr;
    BLI_assert(brush->material_paint != nullptr);
    const IMB_BlendMode blend_mode = IMB_BlendMode(BKE_paint_material_channel_blend_mode(
        *brush->material_paint, target.channel, cache.toggle_settings.invert));
    if (target.is_color_channel) {
      BLI_assert(brush->material_paint != nullptr);
      const float3 rgb = BKE_paint_material_base_color_get(
          *brush->material_paint, sd.paint, *brush, cache.toggle_settings.invert);
      brush_color_storage = float4(rgb, 1.0f);
      brush_color_ptr = &brush_color_storage;
    }
    else if (target.is_normal_channel) {
      float3 tangent(0.0f, 0.0f, 1.0f);
      if (!cache.toggle_settings.invert && target.color_override) {
        /* color_override stores packed 0..1; unpack to tangent then re-pack after invert branch.
         */
        tangent[0] = (*target.color_override)[0] * 2.0f - 1.0f;
        tangent[1] = (*target.color_override)[1] * 2.0f - 1.0f;
        tangent[2] = (*target.color_override)[2] * 2.0f - 1.0f;
      }
      float packed[3];
      BKE_pbr_normal_pack(tangent, false, packed);
      brush_color_storage = float4(packed[0], packed[1], packed[2], 1.0f);
      brush_color_ptr = &brush_color_storage;
    }
    else if (target.color_override) {
      if (cache.toggle_settings.invert) {
        const float v = BKE_paint_material_channel_default_value(target.channel);
        brush_color_storage = float4(v, v, v, 1.0f);
        brush_color_ptr = &brush_color_storage;
      }
      else {
        brush_color_ptr = &(*target.color_override);
      }
    }
    else {
      brush_color_ptr = &brush_color_default;
    }
    const float4 &brush_color = *brush_color_ptr;

    /* Rebuild UV pixel encoding only when tile layout differs from what is
     * already cached on the PBVH (resolution / UDIM / seam margin). Same-sized
     * Material maps reuse the previous encoding. */
    const StringRef uv_map_name =
        BKE_paint_canvas_uvmap_name_get(&paint_mode_settings, &ob).value_or("");
    const std::string layout_key = BKE_paint_pixels_layout_key_get(
        *image_data.image, *image_data.image_user, uv_map_name);
    const bool need_rebuild = pbvh.pixels_ == nullptr || pbvh.pixels_->flags.dirty ||
                              pbvh.pixels_->layout_key != layout_key;
    if (need_rebuild) {
      PAINT_CHANNEL_PERF_SCOPE(BuildPixelsTotal);
      const bool rebuilt = bke::pbvh::build_pixels(
          depsgraph, ob, *image_data.image, *image_data.image_user, uv_map_name);
      if (!rebuilt || pbvh.pixels_ == nullptr || pbvh.pixels_->flags.dirty) {
        continue;
      }
      /* The pixel-node encoding changed, so any cached factors from a previous channel this
       * dab no longer line up with it. */
      factor_caches_valid = false;
    }
#ifdef PAINT_MATERIAL_CHANNEL_PERF_DEBUG
    else {
      paint_material_channel_perf::set_build_pixels_leaf_nodes_updated(0);
    }
#endif

    PixelData &pixel_data = *pbvh.pixels_;
    MutableSpan<PixelNode> pixel_nodes = pixel_data.nodes;

    {
      PAINT_CHANNEL_PERF_SCOPE(FetchBuffers);
      node_mask.foreach_index(
          [&](const int i) { fetch_image_buffers(image_data, nodes[i], pixel_nodes[i]); });
    }
    {
      PAINT_CHANNEL_PERF_SCOPE(UndoPush);
      node_mask.foreach_index(
          [&](const int i) { do_push_undo_tile(image_data, nodes[i], pixel_nodes[i]); },
          exec_mode::grain_size(1));
    }
    if (!factor_caches_valid) {
      PAINT_CHANNEL_PERF_SCOPE(PaintFactors);
      node_mask.foreach_index(
          [&](const int i) {
            node_factor_caches[i] = compute_paint_row_factors(ss,
                                                              pixel_data,
                                                              positions,
                                                              vert_normals,
                                                              *brush,
                                                              material_filter,
                                                              active_sampler,
                                                              pixel_nodes[i]);
          },
          exec_mode::grain_size(1));
      factor_caches_valid = true;
    }
    {
      PAINT_CHANNEL_PERF_SCOPE(PaintPixels);
      node_mask.foreach_index(
          [&](const int i) {
            apply_paint_channel(image_data,
                                *brush,
                                brush_color,
                                blend_mode,
                                pixel_nodes[i],
                                node_factor_caches[i],
                                active_sampler,
                                target.channel,
                                target.is_color_channel,
                                target.is_normal_channel,
                                cache.view_right,
                                cache.view_up,
                                cache.vc->region,
                                cache.projection_mat);
          },
          exec_mode::grain_size(1));
    }

    {
      PAINT_CHANNEL_PERF_SCOPE(SeamFix);
      fix_non_manifold_seam_bleeding(ob, image_data, nodes, pixel_nodes, node_mask);
    }

    {
      PAINT_CHANNEL_PERF_SCOPE(MarkDirty);
      node_mask.foreach_index([&](const int i) {
        bke::pbvh::pixels::mark_image_dirty(
            nodes[i], pixel_nodes[i], *image_data.image, image_data.buffers);
      });
    }

    target_index++;
  }
}

}  // namespace blender
