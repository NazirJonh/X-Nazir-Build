/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Mask Source texture preview inside a shape; see `paint_shape_texture_preview.hh`.
 */

#include "paint_shape_texture_preview.hh"

#include <algorithm>
#include <optional>

#include "BLI_array.hh"
#include "BLI_math_base.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_task.h"
#include "BLI_task.hh"
#include "BLI_utildefines.h"

#include "DNA_brush_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"

#include "BKE_context.hh"
#include "BKE_paint.hh"

#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_shader.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"

#include "IMB_imbuf_types.hh"

#include "ED_paint.hh"

#include "../shapes/paint_shape.hh"
#include "../shapes/paint_shape_texture.hh"
#include "paint_material_source.hh"

namespace blender::ed::sculpt_paint::shape {

const PaintShapeSettings &shape_texture_preview_settings_get(const bContext *C)
{
  /* A live Vector session owns its private settings copy (see #paint_shape_settings in Python);
   * the preview must read the same copy the header edits. */
  if (SpaceImage *sima = CTX_wm_space_image(C)) {
    if (PaintShapeSettings *session = ED_image_shape_session_settings_get(sima)) {
      return *session;
    }
  }
  if (Object *ob = CTX_data_active_object(C)) {
    if (PaintShapeSettings *session = ED_paint_shape_session_settings_get(*ob)) {
      return *session;
    }
  }
  return BKE_paint_shape_settings_get(*CTX_data_scene(C)->toolsettings);
}

ShapeTexturePreview::ShapeTexturePreview() = default;

ShapeTexturePreview::~ShapeTexturePreview()
{
  this->clear();
}

void ShapeTexturePreview::clear()
{
  positions_.clear();
  uvs_.clear();
  opacity_ = 0.0f;
  if (texture_ != nullptr) {
    GPU_texture_free(texture_);
    texture_ = nullptr;
  }
  texture_signature_ = 0;
  source_key_ = 0;
  source_.reset();
}

/** Build the grayscale coverage texture the preview draws, from the resolved source image. */
static gpu::Texture *preview_texture_build(const ShapeMaskSource &source)
{
  const std::optional<eMaterialPaintChannel> channel = material::mask_source_channel_get(
      *source.brush_paint);
  if (!channel) {
    return nullptr;
  }
  const material::ChannelSourceSet::ChannelSource &src = source.sources.source(*channel);
  const ImBuf *ibuf = src.ibuf;
  if (ibuf == nullptr || ibuf->x <= 0 || ibuf->y <= 0) {
    return nullptr;
  }
  /* Only a preview: a large image is sampled on a coarser grid (at most #PREVIEW_MAX_SIZE texels
   * on the longer side), or building it on the first drag frame would stall the redraw. */
  constexpr int PREVIEW_MAX_SIZE = 1024;
  const int src_width = ibuf->x;
  const int src_height = ibuf->y;
  const int step = std::max(1, (std::max(src_width, src_height) + PREVIEW_MAX_SIZE - 1) /
                                   PREVIEW_MAX_SIZE);
  const int width = (src_width + step - 1) / step;
  const int height = (src_height + step - 1) / step;

  /* Sampled through the same function the commit uses, so the colorspace handling, the Alpha rule
   * and Source Invert cannot drift from the painted coverage. */
  Array<uchar> pixels(int64_t(width) * int64_t(height) * 4);
  threading::parallel_for(IndexRange(height), 16, [&](const IndexRange rows) {
    const int thread = BLI_task_parallel_thread_id(nullptr);
    for (const int y : rows) {
      for (const int x : IndexRange(width)) {
        const float2 uv((float(x) + 0.5f) / float(width), (float(y) + 0.5f) / float(height));
        const float coverage = material::mask_source_coverage_at_uv(
            source.sampler, *source.brush_paint, uv, thread);
        const uchar value = uchar(int(math::clamp(coverage, 0.0f, 1.0f) * 255.0f + 0.5f));
        /* Premultiplied white: the draw color's opacity then scales coverage and alpha alike. */
        const int64_t dst_index = (int64_t(y) * int64_t(width) + int64_t(x)) * 4;
        pixels[dst_index + 0] = value;
        pixels[dst_index + 1] = value;
        pixels[dst_index + 2] = value;
        pixels[dst_index + 3] = value;
      }
    }
  });

  gpu::Texture *texture = GPU_texture_create_2d("shape_mask_preview",
                                                width,
                                                height,
                                                1,
                                                gpu::TextureFormat::UNORM_8_8_8_8,
                                                /* Same usage as the brush texture overlay. */
                                                GPU_TEXTURE_USAGE_SHADER_READ |
                                                    GPU_TEXTURE_USAGE_ATTACHMENT,
                                                nullptr);
  if (texture != nullptr) {
    GPU_texture_update(texture, GPU_DATA_UBYTE, pixels.data());
    GPU_texture_filter_mode(texture, true);
  }
  return texture;
}

static void preview_append_arc(Vector<float2> &points,
                               const float2 &center,
                               const float2 &radius,
                               const float angle_start,
                               const float angle_end,
                               const int segments)
{
  for (int i = 0; i <= segments; i++) {
    const float t = float(i) / float(segments);
    const float angle = angle_start + (angle_end - angle_start) * t;
    points.append(center + radius * float2(math::cos(angle), math::sin(angle)));
  }
}

/** Shape outline in #ShapeSample::shape_uv coordinates ([0, 1]^2). */
static void preview_outline(const PaintShape &shape, Vector<float2> &r_points)
{
  r_points.clear();
  const float2 half = math::max(shape.half_size, float2(1e-6f));
  if (shape.type == PAINT_SHAPE_ELLIPSE) {
    const int segments = 64;
    for (int i = 0; i < segments; i++) {
      const float angle = (float(i) / float(segments)) * 2.0f * float(M_PI);
      r_points.append(float2(0.5f + 0.5f * math::cos(angle), 0.5f + 0.5f * math::sin(angle)));
    }
    return;
  }
  /* Rectangle with per-corner radii (TL, TR, BR, BL), converted from pixels to frame fractions. */
  const auto radius = [&](const float value, const float extent) {
    return math::clamp(value / (2.0f * extent), 0.0f, 0.5f);
  };
  const float4 corner = shape.corner_radius;
  const float2 tl = float2(radius(corner.x, half.x), radius(corner.x, half.y));
  const float2 tr = float2(radius(corner.y, half.x), radius(corner.y, half.y));
  const float2 br = float2(radius(corner.z, half.x), radius(corner.z, half.y));
  const float2 bl = float2(radius(corner.w, half.x), radius(corner.w, half.y));
  constexpr int segments = 8;
  preview_append_arc(r_points, float2(1.0f - br.x, br.y), br, -0.5f * float(M_PI), 0.0f, segments);
  preview_append_arc(
      r_points, float2(1.0f - tr.x, 1.0f - tr.y), tr, 0.0f, 0.5f * float(M_PI), segments);
  preview_append_arc(
      r_points, float2(tl.x, 1.0f - tl.y), tl, 0.5f * float(M_PI), float(M_PI), segments);
  preview_append_arc(r_points, float2(bl.x, bl.y), bl, float(M_PI), 1.5f * float(M_PI), segments);
}

/** Sutherland-Hodgman clip of \a poly against one axis-aligned half-plane. */
static void preview_clip_axis(Vector<float2> &poly,
                              const int axis,
                              const float bound,
                              const bool keep_greater)
{
  if (poly.is_empty()) {
    return;
  }
  const auto inside = [&](const float2 &p) {
    return keep_greater ? p[axis] >= bound : p[axis] <= bound;
  };
  Vector<float2> result;
  result.reserve(poly.size() + 4);
  float2 previous = poly.last();
  bool previous_inside = inside(previous);
  for (const float2 &current : poly) {
    const bool current_inside = inside(current);
    if (current_inside != previous_inside) {
      const float denominator = current[axis] - previous[axis];
      if (denominator != 0.0f) {
        const float t = (bound - previous[axis]) / denominator;
        result.append(previous + (current - previous) * t);
      }
    }
    if (current_inside) {
      result.append(current);
    }
    previous = current;
    previous_inside = current_inside;
  }
  poly = std::move(result);
}

bool ShapeTexturePreview::update(const bContext *C,
                                 const PaintShape &shape,
                                 const PaintShapeSettings &settings,
                                 const bool is_3d,
                                 const int shape_index)
{
  /* Keep the resolved source and its texture across redraws; only the geometry is rebuilt. */
  positions_.clear();
  uvs_.clear();
  opacity_ = 0.0f;

  const Scene *scene = CTX_data_scene(C);
  if (scene == nullptr || scene->toolsettings == nullptr) {
    this->clear();
    return false;
  }
  PaintModeSettings &paint_mode = scene->toolsettings->paint_mode;
  /* Nothing to show outside Mask mode, with the preview switched off or for any other shape. */
  if (paint_mode.layer_target_mode != PAINT_LAYER_TARGET_MASK ||
      (settings.flag & PAINT_SHAPE_TEXTURE_PREVIEW) == 0 ||
      settings.texture_preview_opacity <= 0.0f ||
      !ELEM(shape.type, PAINT_SHAPE_RECT, PAINT_SHAPE_ELLIPSE))
  {
    this->clear();
    return false;
  }

  /* The context's active paint can be unresolved while an overlay draws; fall back to the paint
   * the shape bake itself resolves its Mask Source from. */
  Paint *paint = BKE_paint_get_active_from_context(C);
  if (paint == nullptr) {
    ToolSettings &ts = *scene->toolsettings;
    paint = is_3d ? (ts.sculpt != nullptr ? &ts.sculpt->paint : nullptr) : &ts.imapaint.paint;
  }
  const Brush *brush = paint != nullptr ? BKE_paint_brush_for_read(paint) : nullptr;
  if (brush == nullptr || brush->material_paint == nullptr) {
    this->clear();
    return false;
  }
  BrushMaterialPaint &brush_paint = *brush->material_paint;

  /* Reopening the source images is the expensive part; reuse the resolved source while the brush
   * inputs and the placement are unchanged. */
  const uint64_t source_key = mask_source_key(brush_paint, *brush, paint_mode, *paint);
  if (!(source_key != 0 && source_key == source_key_ && source_)) {
    source_ = std::make_shared<ShapeMaskSource>(
        brush_paint, paint_mode, *paint, *brush, paint->visible_material_channels);
    source_key_ = source_key;
    texture_signature_ = 0;
  }
  if (!source_ || !source_->has_texture()) {
    /* A flat value or a missing image does not modulate the coverage. */
    this->clear();
    return false;
  }

  const ShapeMaskSource &source = *source_;
  if (texture_ == nullptr || texture_signature_ != source.signature) {
    if (texture_ != nullptr) {
      GPU_texture_free(texture_);
    }
    texture_ = preview_texture_build(source);
    texture_signature_ = source.signature;
  }
  if (texture_ == nullptr) {
    return false;
  }

  /* Build the outline in shape UV, compute its texture UVs with the same matrix the painted
   * coverage uses, clip to the texture (scale < 1 leaves the outside empty, no tiling), and map
   * the clipped vertices back to the unit frame. */
  Vector<float2> outline;
  preview_outline(shape, outline);
  const float4x4 uv_matrix = shape_mask_source_uv_matrix(source.mapping, shape_index);

  Vector<float2> uv_polygon;
  uv_polygon.reserve(outline.size());
  for (const float2 &p : outline) {
    const float3 uv = math::transform_point(uv_matrix, float3(p.x, p.y, 0.0f));
    uv_polygon.append(float2(uv.x, uv.y));
  }
  preview_clip_axis(uv_polygon, 0, 0.0f, true);
  preview_clip_axis(uv_polygon, 0, 1.0f, false);
  preview_clip_axis(uv_polygon, 1, 0.0f, true);
  preview_clip_axis(uv_polygon, 1, 1.0f, false);
  if (uv_polygon.size() < 3) {
    /* The placement leaves nothing of the texture inside the shape. */
    return false;
  }

  const float4x4 uv_inverse = math::invert(uv_matrix);
  positions_.reserve(uv_polygon.size());
  uvs_.reserve(uv_polygon.size());
  for (const float2 &uv : uv_polygon) {
    const float3 local = math::transform_point(uv_inverse, float3(uv.x, uv.y, 0.0f));
    positions_.append(float2(local.x, local.y) - float2(0.5f));
    uvs_.append(uv);
  }
  opacity_ = settings.texture_preview_opacity;
  return true;
}

void ShapeTexturePreview::draw_pixel_shape(const bContext *C,
                                           const PaintShape &shape,
                                           const FunctionRef<float2(const float2 &)> to_region,
                                           const bool is_3d)
{
  /* A Pixel shape is a single, fresh shape: index 0 in both backends. */
  if (!this->update(C, shape, shape_texture_preview_settings_get(C), is_3d, 0)) {
    return;
  }
  /* Unit frame [-0.5, 0.5]^2 -> the shape's rotated frame in shape pixels -> region pixels. */
  const float2 half = shape.half_size;
  const float c = math::cos(shape.rotation);
  const float s = math::sin(shape.rotation);
  const auto to_screen = [&](const float2 &unit) {
    const float lx = unit.x * 2.0f * half.x;
    const float ly = unit.y * 2.0f * half.y;
    return to_region(shape.center + float2(lx * c - ly * s, lx * s + ly * c));
  };

  /* The polygon is convex, so rings scaled about its centroid cover it. Every vertex is mapped
   * on its own and the UVs stay linear in the unit frame (the texture matrix is affine), so a
   * perspective mapping (Surface space) bends the texture with the shape; one affine frame
   * would shear it. Eight rings keep the bend smooth for a large shape. */
  constexpr int rings = 8;
  const int num = int(positions_.size());
  float2 pos_center(0.0f);
  float2 uv_center(0.0f);
  for (const int i : positions_.index_range()) {
    pos_center += positions_[i];
    uv_center += uvs_[i];
  }
  pos_center /= float(num);
  uv_center /= float(num);

  struct Vertex {
    float2 pos;
    float2 uv;
  };
  const auto vertex_at = [&](const int ring, const int i) {
    const float t = float(ring) / float(rings);
    return Vertex{to_screen(pos_center + (positions_[i] - pos_center) * t),
                  uv_center + (uvs_[i] - uv_center) * t};
  };
  const Vertex center_vertex{to_screen(pos_center), uv_center};

  Vector<Vertex> grid;
  grid.reserve(int64_t(rings) * num);
  for (const int ring : IndexRange(1, rings)) {
    for (const int i : IndexRange(num)) {
      grid.append(vertex_at(ring, i));
    }
  }
  const auto grid_at = [&](const int ring, const int i) -> const Vertex & {
    return grid[(ring - 1) * num + (i % num)];
  };
  GPU_blend(GPU_BLEND_ALPHA_PREMULT);
  GPU_depth_test(GPU_DEPTH_NONE);

  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  const uint tex_coord = GPU_vertformat_attr_add(
      format, "texCoord", gpu::VertAttrType::SFLOAT_32_32);

  immBindBuiltinProgram(GPU_SHADER_3D_IMAGE_COLOR);
  immUniformColor4f(1.0f, 1.0f, 1.0f, opacity_);
  immBindTextureSampler("image",
                        texture_,
                        {GPU_SAMPLER_FILTERING_LINEAR,
                         GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER,
                         GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER});
  const auto emit = [&](const Vertex &v) {
    immAttr2f(tex_coord, v.uv.x, v.uv.y);
    immVertex3f(pos, v.pos.x, v.pos.y, 0.0f);
  };
  /* The first ring is a fan around the centroid, every further ring a strip of two triangles per
   * edge. */
  immBegin(GPU_PRIM_TRIS, uint(num * 3 + (rings - 1) * num * 6));
  for (const int i : IndexRange(num)) {
    emit(center_vertex);
    emit(grid_at(1, i));
    emit(grid_at(1, i + 1));
  }
  for (const int ring : IndexRange(1, rings - 1)) {
    for (const int i : IndexRange(num)) {
      const Vertex &a = grid_at(ring, i);
      const Vertex &b = grid_at(ring, i + 1);
      const Vertex &c_out = grid_at(ring + 1, i);
      const Vertex &d_out = grid_at(ring + 1, i + 1);
      emit(a);
      emit(c_out);
      emit(b);
      emit(b);
      emit(c_out);
      emit(d_out);
    }
  }
  immEnd();

  immUnbindProgram();
  GPU_texture_unbind(texture_);
  GPU_blend(GPU_BLEND_NONE);
}

}  // namespace blender::ed::sculpt_paint::shape
