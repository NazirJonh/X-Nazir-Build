/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 * \brief Off-screen GPU rendering of the brush-texture stroke preview widget.
 */

#include "interface_brush_texture_preview.hh"
#include "interface_template_preview_brush.hh"

#include <algorithm>
#include <cmath>
#include <memory>

#include "DNA_brush_enums.h"
#include "DNA_brush_types.h"
#include "DNA_image_types.h"
#include "DNA_node_types.h"
#include "DNA_texture_types.h"

#include "BKE_image.hh"

#include "BLI_map.hh"
#include "BLI_math_base.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_rect.hh"
#include "BLI_utildefines.hh"
#include "BLI_vector.hh"

#include "GPU_batch.hh"
#include "GPU_framebuffer.hh"
#include "GPU_immediate.hh"
#include "GPU_shader.hh"
#include "GPU_shader_builtin.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"

#include "IMB_imbuf_types.hh"

#include "RE_texture.h"

#include "NOD_texture.h"

#include "MEM_guardedalloc.h"

namespace blender::ed::interface {

/* Preview blend modes. Kept local to the editor because they only drive the preview shader; they
 * are not persisted and must not leak into DNA. */
enum StrokeBlendMode {
  STROKE_BLEND_MIX = 0,
  STROKE_BLEND_MULTIPLY = 1,
  STROKE_BLEND_SCREEN = 2,
  STROKE_BLEND_OVERLAY = 3,
  STROKE_BLEND_ADD = 4,
  STROKE_BLEND_SUBTRACT = 5,
};

/* Registry of live previews keyed by an opaque owner (the UI region). Keeping one preview per owner
 * avoids recreating GPU resources when several regions show different brushes, and lets all
 * resources be released together from #BrushStrokePreview::free_all() at GPU shutdown. */
static Map<const void *, std::unique_ptr<BrushStrokePreview>> g_registry;

/* The preview shader is shared by every region: created lazily on first use and freed together with
 * the registry in #free_all(). */
static gpu::Shader *g_stroke_preview_shader = nullptr;

static gpu::Shader *stroke_preview_shader_ensure()
{
  if (g_stroke_preview_shader == nullptr) {
    g_stroke_preview_shader = GPU_shader_create_from_info_name("interface_brush_stroke_preview");
  }
  return g_stroke_preview_shader;
}

/* -------------------------------------------------------------------- */
/** \name Texture Evaluation Helpers
 * \{ */

static int convert_blend_mode(int mtex_blend)
{
  switch (mtex_blend) {
    case MTEX_BLEND:
      return STROKE_BLEND_MIX;
    case MTEX_MUL:
      return STROKE_BLEND_MULTIPLY;
    case MTEX_SCREEN:
      return STROKE_BLEND_SCREEN;
    case MTEX_OVERLAY:
      return STROKE_BLEND_OVERLAY;
    case MTEX_ADD:
      return STROKE_BLEND_ADD;
    case MTEX_SUB:
      return STROKE_BLEND_SUBTRACT;
    default:
      return STROKE_BLEND_MIX;
  }
}

static int2 texture_get_resolution(const MTex *mtex)
{
  if (!mtex || !mtex->tex) {
    return int2(256, 256);
  }

  if (mtex->tex->type == TEX_IMAGE && mtex->tex->ima) {
    ImBuf *ibuf = BKE_image_acquire_ibuf(mtex->tex->ima, nullptr, nullptr);
    if (ibuf) {
      int2 resolution = int2(ibuf->x, ibuf->y);
      BKE_image_release_ibuf(mtex->tex->ima, ibuf, nullptr);
      return resolution;
    }
  }

  return int2(256, 256);
}

static uint64_t texture_get_update_time(const MTex *mtex)
{
  if (!mtex || !mtex->tex) {
    return 0;
  }

  /* For image textures, use image modification time. */
  if (mtex->tex->type == TEX_IMAGE && mtex->tex->ima) {
    return mtex->tex->ima->id.recalc;
  }

  /* For procedural textures, use texture modification time. */
  return mtex->tex->id.recalc;
}

/* Map pixel coordinates to normalized device coordinates. */
static float2 pixel_to_ndc(float x, float y, int width, int height)
{
  return float2(2.0f * x / float(width) - 1.0f, 2.0f * y / float(height) - 1.0f);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Texture Cache
 * \{ */

bool BrushStrokePreview::cache_is_valid(const TextureCache &cache, const MTex *mtex)
{
  if (!mtex || !cache.is_valid) {
    return false;
  }

  /* Check if source texture has changed. */
  if (cache.source_texture != mtex->tex) {
    return false;
  }

  const uint64_t current_time = texture_get_update_time(mtex);
  return current_time == cache.last_update;
}

bool BrushStrokePreview::cache_init(TextureCache &cache, const MTex *mtex)
{
  if (!mtex || !mtex->tex) {
    return false;
  }

  cache.gpu_texture = nullptr;
  cache.source_texture = mtex->tex;
  cache.source_image = nullptr;
  cache.last_update = 0;
  cache.resolution = int2(256, 256);
  cache.is_valid = false;

  if (mtex->tex->type == TEX_IMAGE && mtex->tex->ima) {
    cache.source_image = mtex->tex->ima;
    cache.resolution = texture_get_resolution(mtex);
  }

  return true;
}

bool BrushStrokePreview::cache_update(TextureCache &cache, const MTex *mtex)
{
  if (!mtex) {
    return false;
  }

  /* `mtex->tex` may be cleared between init and update; guard all dereferences. */
  if (!mtex->tex) {
    cache.is_valid = false;
    return false;
  }

  if (cache_is_valid(cache, mtex)) {
    return true;
  }

  if (cache.gpu_texture) {
    GPU_texture_free(cache.gpu_texture);
    cache.gpu_texture = nullptr;
  }

  cache.source_texture = mtex->tex;
  cache.last_update = texture_get_update_time(mtex);

  const int size = std::max(256, std::max(cache.resolution.x, cache.resolution.y));
  cache.resolution = int2(size, size);

  uchar *buffer = MEM_new_array_uninitialized<uchar>(size * size * 4, "brush_preview_tex");
  if (!buffer) {
    cache.is_valid = false;
    return false;
  }

  ImagePool *pool = BKE_image_pool_new();
  const float rotation = -mtex->rot;

  bNodeTreeExec *tex_exec = nullptr;
  if (mtex->tex->nodetree) {
    tex_exec = ntreeTexBeginExecTree(mtex->tex->nodetree);
  }

  for (int j = 0; j < size; j++) {
    for (int i = 0; i < size; i++) {
      const int index = (j * size + i) * 4;
      float x = (float(i) / float(size) - 0.5f) * 2.0f;
      float y = (float(j) / float(size) - 0.5f) * 2.0f;
      const float len = sqrtf(x * x + y * y);

      if (len <= 1.0f) {
        if (fabsf(rotation) > 0.001f) {
          const float angle = atan2f(y, x) + rotation;
          x = len * cosf(angle);
          y = len * sinf(angle);
        }

        const float co[3] = {x, y, 0.0f};
        float intensity;
        float rgba[4];
        const bool has_rgb = RE_texture_evaluate(
            mtex, co, 0, pool, false, false, &intensity, rgba);

        if (has_rgb) {
          buffer[index] = uchar(clamp_f(rgba[0], 0.0f, 1.0f) * 255.0f);
          buffer[index + 1] = uchar(clamp_f(rgba[1], 0.0f, 1.0f) * 255.0f);
          buffer[index + 2] = uchar(clamp_f(rgba[2], 0.0f, 1.0f) * 255.0f);
          buffer[index + 3] = uchar(clamp_f(rgba[3], 0.0f, 1.0f) * 255.0f);
        }
        else {
          /* Alpha/strength textures: match paint cursor inversion. */
          CLAMP(intensity, 0.0f, 1.0f);
          const uchar val = uchar(255.0f - intensity * 255.0f);
          buffer[index] = val;
          buffer[index + 1] = val;
          buffer[index + 2] = val;
          buffer[index + 3] = 255;
        }
      }
      else {
        buffer[index] = 0;
        buffer[index + 1] = 0;
        buffer[index + 2] = 0;
        buffer[index + 3] = 0;
      }
    }
  }

  if (tex_exec) {
    ntreeTexEndExecTree(tex_exec);
  }
  if (pool) {
    BKE_image_pool_free(pool);
  }

  cache.gpu_texture = GPU_texture_create_2d("brush_texture_cache",
                                            size,
                                            size,
                                            1,
                                            gpu::TextureFormat::UNORM_8_8_8_8,
                                            GPU_TEXTURE_USAGE_SHADER_READ,
                                            nullptr);
  if (cache.gpu_texture) {
    GPU_texture_update(cache.gpu_texture, GPU_DATA_UBYTE, buffer);
  }

  MEM_delete(buffer);

  cache.is_valid = (cache.gpu_texture != nullptr);
  return cache.is_valid;
}

void BrushStrokePreview::cache_free(TextureCache &cache)
{
  if (cache.gpu_texture) {
    GPU_texture_free(cache.gpu_texture);
    cache.gpu_texture = nullptr;
  }
  cache.is_valid = false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name GPU Context
 * \{ */

bool BrushStrokePreview::gpu_context_init()
{
  /* The shared shader is required for rendering; bail out if it cannot be created. */
  if (!stroke_preview_shader_ensure()) {
    return false;
  }

  /* Create quad batch. Attribute names must match the shader: `pos` / `texcoord`. */
  gpu::VertBuf *vbo = GPU_vertbuf_calloc();
  GPUVertFormat format;
  GPU_vertformat_clear(&format);
  GPU_vertformat_attr_add(&format, "pos", gpu::VertAttrType::SFLOAT_32_32);
  GPU_vertformat_attr_add(&format, "texcoord", gpu::VertAttrType::SFLOAT_32_32);
  GPU_vertbuf_init_with_format(*vbo, format);
  GPU_vertbuf_data_alloc(*vbo, 4);

  /* Quad vertices: position (-1..1) and matching texture coordinates. */
  float vertices[4][4] = {
      {-1.0f, -1.0f, 0.0f, 0.0f}, /* Bottom-left. */
      {1.0f, -1.0f, 1.0f, 0.0f},  /* Bottom-right. */
      {1.0f, 1.0f, 1.0f, 1.0f},   /* Top-right. */
      {-1.0f, 1.0f, 0.0f, 1.0f},  /* Top-left. */
  };

  for (int i = 0; i < 4; i++) {
    GPU_vertbuf_vert_set(vbo, i, vertices[i]);
  }

  gpu::IndexBuf *ibuf = GPU_indexbuf_calloc();
  GPUIndexBufBuilder builder;
  GPU_indexbuf_init(&builder, GPU_PRIM_TRIS, 2, 4);
  GPU_indexbuf_add_tri_verts(&builder, 0, 1, 2);
  GPU_indexbuf_add_tri_verts(&builder, 0, 2, 3);
  GPU_indexbuf_build_in_place(&builder, ibuf);

  quad_batch_ = GPU_batch_create_ex(
      GPU_PRIM_TRIS, vbo, ibuf, GPU_BATCH_OWNS_VBO | GPU_BATCH_OWNS_INDEX);

  render_target_ = GPU_texture_create_2d(
      "brush_preview_target",
      preview_size_.x,
      preview_size_.y,
      1,
      gpu::TextureFormat::UNORM_8_8_8_8,
      GPU_TEXTURE_USAGE_ATTACHMENT | GPU_TEXTURE_USAGE_SHADER_READ,
      nullptr);

  if (render_target_) {
    /* No filtering for pixel-perfect display; clamp to edge. */
    GPU_texture_filter_mode(render_target_, false);
    GPU_texture_extend_mode(render_target_, GPU_SAMPLER_EXTEND_MODE_EXTEND);
  }

  depth_buffer_ = GPU_texture_create_2d("brush_preview_depth",
                                        preview_size_.x,
                                        preview_size_.y,
                                        1,
                                        gpu::TextureFormat::SFLOAT_32_DEPTH_UINT_8,
                                        GPU_TEXTURE_USAGE_ATTACHMENT,
                                        nullptr);

  framebuffer_ = GPU_framebuffer_create("brush_preview_fb");
  GPU_framebuffer_texture_attach(framebuffer_, render_target_, 0, 0);
  GPU_framebuffer_texture_attach(framebuffer_, depth_buffer_, 0, 0);

  if (!GPU_framebuffer_check_valid(framebuffer_, nullptr)) {
    gpu_context_free();
    return false;
  }

  /* 1x1 white texture bound to the mask sampler when the brush has no mask. Owned by this preview
   * so it is freed together with the framebuffer that uses it. */
  {
    float data[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    dummy_mask_texture_ = GPU_texture_create_2d("brush_dummy_mask",
                                                1,
                                                1,
                                                1,
                                                gpu::TextureFormat::UNORM_8_8_8_8,
                                                GPU_TEXTURE_USAGE_SHADER_READ,
                                                data);
  }

  initialized_ = true;
  return true;
}

void BrushStrokePreview::gpu_context_free()
{
  if (quad_batch_) {
    GPU_batch_discard(quad_batch_);
    quad_batch_ = nullptr;
  }
  if (framebuffer_) {
    GPU_framebuffer_free(framebuffer_);
    framebuffer_ = nullptr;
  }
  if (render_target_) {
    GPU_texture_free(render_target_);
    render_target_ = nullptr;
  }
  if (depth_buffer_) {
    GPU_texture_free(depth_buffer_);
    depth_buffer_ = nullptr;
  }
  if (dummy_mask_texture_) {
    GPU_texture_free(dummy_mask_texture_);
    dummy_mask_texture_ = nullptr;
  }

  initialized_ = false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Stroke Layout
 * \{ */

void BrushStrokePreview::generate_stroke_layout()
{
  elements_.clear();

  const Brush *brush = brush_;
  const int width = preview_size_.x;
  const int height = preview_size_.y;
  const float preview_size = min_ff(float(width), float(height)) * 0.7f;
  const float center_x = float(width) * 0.5f;
  const float center_y = float(height) * 0.5f;

  const float size_factor = brush->size / 100.0f;

  /* Share the stamp placement (position/rotation/count) with the immediate-mode fallback. */
  StrokeTransform transform = {};
  transform.scale_x = size_factor;
  transform.scale_y = size_factor;
  transform.rotation = stroke_angle_;
  transform.pattern_spacing = stroke_spacing_;
  transform.use_random = (brush->mtex.brush_angle_mode & MTEX_ANGLE_RANDOM) != 0;
  transform.random_angle = brush->mtex.random_angle;

  const Vector<StrokeStamp> stamps = compute_stroke_stamps(
      brush, transform, center_x, center_y, preview_size, stroke_angle_);

  /* Stamps grow along the stroke; the size progression mirrors the fallback path. */
  const float base_element_size = (preview_size * size_factor) * 0.3f;
  const float max_element_size = (preview_size * size_factor) * 0.8f;
  const float min_element_size = max_ff(base_element_size * 0.5f, 15.0f);

  const int count = stamps.size();
  for (int i = 0; i < count; i++) {
    const float progress = (count > 1) ? float(i) / float(count - 1) : 0.0f;
    float element_size = base_element_size + (max_element_size - base_element_size) * progress;
    element_size = max_ff(element_size, min_element_size);

    Element element;
    element.center = pixel_to_ndc(stamps[i].x, stamps[i].y, width, height);
    element.scale = float2(element_size / float(width) * 2.0f, element_size / float(height) * 2.0f);
    element.rotation = stamps[i].rotation;
    element.color = float3(1.0f, 1.0f, 1.0f);
    element.opacity = brush->alpha;
    element.uv_offset = float2(0.0f, 0.0f);
    element.uv_scale = float2(1.0f, 1.0f);
    element.blend_mode = brush->mtex.tex ? convert_blend_mode(brush->mtex.blendtype) :
                                           STROKE_BLEND_MIX;

    elements_.append(element);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Rendering
 * \{ */

void BrushStrokePreview::render_element(const Element &element)
{
  gpu::Shader *shader = stroke_preview_shader_ensure();
  if (!quad_batch_ || !shader || !main_cache_.gpu_texture) {
    return;
  }

  /* Assign shader to batch so #GPU_batch_draw uses it. */
  GPU_batch_set_shader(quad_batch_, shader);

  /* Setup push-constant uniforms expected by the shader. */
  const float4x4 mvp = float4x4::identity();
  const float4x4 tex_xform = float4x4::identity();

  GPU_shader_uniform_mat4(shader, "ModelViewProjectionMatrix", (const float(*)[4])mvp.ptr());
  GPU_shader_uniform_mat4(shader, "u_texture_transform", (const float(*)[4])tex_xform.ptr());

  GPU_shader_uniform_2f(shader, "u_element_center", element.center.x, element.center.y);
  GPU_shader_uniform_2f(shader, "u_element_scale", element.scale.x, element.scale.y);
  GPU_shader_uniform_1f(shader, "u_element_rotation", element.rotation);
  GPU_shader_uniform_4f(
      shader, "u_color_tint", element.color.x, element.color.y, element.color.z, 1.0f);
  GPU_shader_uniform_1f(shader, "u_opacity", element.opacity);
  GPU_shader_uniform_1i(shader, "u_blend_mode", element.blend_mode);
  GPU_shader_uniform_2f(shader, "u_texture_scale", element.uv_scale.x, element.uv_scale.y);
  GPU_shader_uniform_2f(shader, "u_texture_offset", element.uv_offset.x, element.uv_offset.y);
  GPU_shader_uniform_1f(shader, "u_texture_rotation", 0.0f);

  /* Bind shader first so texture bindings apply to the correct descriptor set. */
  GPU_shader_bind(shader);

  /* Query sampler bindings to avoid binding to nonexistent slots. */
  const int tex_binding = GPU_shader_get_sampler_binding(shader, "u_texture");
  const int mask_binding = GPU_shader_get_sampler_binding(shader, "u_mask_texture");

  if (tex_binding >= 0 && main_cache_.gpu_texture) {
    GPU_texture_bind(main_cache_.gpu_texture, tex_binding);
  }

  /* Fall back to the 1x1 white texture so the mask sampler is always bound. */
  gpu::Texture *mask_tex = mask_cache_.gpu_texture ? mask_cache_.gpu_texture : dummy_mask_texture_;
  if (mask_binding >= 0 && mask_tex) {
    GPU_texture_bind(mask_tex, mask_binding);
  }

  switch (element.blend_mode) {
    case STROKE_BLEND_MIX:
      GPU_blend(GPU_BLEND_ALPHA);
      break;
    case STROKE_BLEND_MULTIPLY:
      GPU_blend(GPU_BLEND_MULTIPLY);
      break;
    case STROKE_BLEND_SCREEN:
      GPU_blend(GPU_BLEND_ADDITIVE);
      break;
    case STROKE_BLEND_ADD:
      GPU_blend(GPU_BLEND_ADDITIVE);
      break;
    case STROKE_BLEND_SUBTRACT:
      GPU_blend(GPU_BLEND_SUBTRACT);
      break;
    default:
      GPU_blend(GPU_BLEND_ALPHA);
      break;
  }

  GPU_batch_draw(quad_batch_);
}

bool BrushStrokePreview::render()
{
  if (!initialized_ || !main_cache_.gpu_texture) {
    return false;
  }

  /* Save caller's framebuffer and viewport so we can restore them afterwards.
   * #GPU_framebuffer_restore() only reverts to the default back-buffer, which breaks Blender's
   * offscreen UI pipeline. */
  gpu::FrameBuffer *prev_fb = GPU_framebuffer_active_get();
  int prev_viewport[4];
  GPU_viewport_size_get_i(prev_viewport);

  GPU_framebuffer_bind(framebuffer_);
  GPU_viewport(0, 0, preview_size_.x, preview_size_.y);

  const double4 clear_color = double4(0.0, 0.0, 0.0, 0.0);
  GPU_framebuffer_clear_color(framebuffer_, clear_color);
  GPU_framebuffer_clear_depth(framebuffer_, 1.0f);

  GPU_blend(GPU_BLEND_ALPHA);
  GPU_depth_test(GPU_DEPTH_NONE);

  for (const Element &element : elements_) {
    render_element(element);
  }

  GPU_blend(GPU_BLEND_NONE);

  /* Restore the caller's framebuffer and viewport before the memory barrier so subsequent UI draw
   * calls go to the correct offscreen buffer with the correct viewport. */
  if (prev_fb) {
    GPU_framebuffer_bind(prev_fb);
  }
  else {
    GPU_framebuffer_restore();
  }
  GPU_viewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);

  /* Ensure written color is visible to subsequent sampling. */
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH | GPU_BARRIER_FRAMEBUFFER);

  return true;
}

bool BrushStrokePreview::draw(const rcti *rect, [[maybe_unused]] float zoom_factor) const
{
  if (!rect || !render_target_) {
    return false;
  }

  const int display_width = BLI_rcti_size_x(rect);
  const int display_height = BLI_rcti_size_y(rect);

  int old_scissor[4] = {0};
  GPU_scissor_get(old_scissor);

  GPU_scissor_test(true);
  GPU_scissor(rect->xmin, rect->ymin, display_width, display_height);

  /* Checkerboard background for transparency. */
  {
    const float checker_colors[2][4] = {
        {0.8f, 0.8f, 0.8f, 1.0f},
        {0.6f, 0.6f, 0.6f, 1.0f},
    };
    const int checker_size = 8;

    GPU_blend(GPU_BLEND_NONE);
    GPUVertFormat *bg_format = immVertexFormat();
    const uint bg_pos = GPU_vertformat_attr_add(
        bg_format, "pos", blender::gpu::VertAttrType::SFLOAT_32_32);
    const uint bg_col = GPU_vertformat_attr_add(
        bg_format, "color", blender::gpu::VertAttrType::SFLOAT_32_32_32_32);

    immBindBuiltinProgram(GPU_SHADER_3D_FLAT_COLOR);
    const int cells_x = (display_width + checker_size - 1) / checker_size;
    const int cells_y = (display_height + checker_size - 1) / checker_size;
    immBegin(GPU_PRIM_TRIS, cells_x * cells_y * 6);

    for (int y = 0; y < display_height; y += checker_size) {
      for (int x = 0; x < display_width; x += checker_size) {
        const int checker_idx = ((x / checker_size) + (y / checker_size)) % 2;
        const float *color = checker_colors[checker_idx];
        const float x1 = float(rect->xmin + x);
        const float y1 = float(rect->ymin + y);
        const float x2 = float(min_ii(rect->xmin + x + checker_size, rect->xmax));
        const float y2 = float(min_ii(rect->ymin + y + checker_size, rect->ymax));

        immAttr4fv(bg_col, color);
        immVertex2f(bg_pos, x1, y1);
        immAttr4fv(bg_col, color);
        immVertex2f(bg_pos, x2, y1);
        immAttr4fv(bg_col, color);
        immVertex2f(bg_pos, x2, y2);

        immAttr4fv(bg_col, color);
        immVertex2f(bg_pos, x1, y1);
        immAttr4fv(bg_col, color);
        immVertex2f(bg_pos, x2, y2);
        immAttr4fv(bg_col, color);
        immVertex2f(bg_pos, x1, y2);
      }
    }
    immEnd();
    immUnbindProgram();
  }

  GPU_depth_test(GPU_DEPTH_NONE);
  GPU_depth_mask(false);
  GPU_face_culling(GPU_CULL_NONE);
  GPU_color_mask(true, true, true, true);

  /* Draw the rendered texture into the widget rect. Use the existing UI matrix (which accounts for
   * scroll offset and maps region pixel coords to NDC). A custom ortho is NOT used here because it
   * would fill the entire viewport rather than the widget area. */
  GPUVertFormat *format = immVertexFormat();
  const uint attr_pos = GPU_vertformat_attr_add(
      format, "pos", blender::gpu::VertAttrType::SFLOAT_32_32);
  const uint attr_tex = GPU_vertformat_attr_add(
      format, "texCoord", blender::gpu::VertAttrType::SFLOAT_32_32);

  immBindBuiltinProgram(GPU_SHADER_3D_IMAGE_COLOR);
  immUniformColor4f(1.0f, 1.0f, 1.0f, 1.0f);

  const GPUSamplerState sampler_state = {GPU_SAMPLER_FILTERING_LINEAR,
                                         GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER,
                                         GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER,
                                         GPU_SAMPLER_CUSTOM_ICON,
                                         GPU_SAMPLER_STATE_TYPE_PARAMETERS};
  immBindTextureSampler("image", render_target_, sampler_state);

  GPU_blend(GPU_BLEND_ALPHA_PREMULT);

  const float x1 = float(rect->xmin);
  const float y1 = float(rect->ymin);
  const float x2 = float(rect->xmax);
  const float y2 = float(rect->ymax);

  immBegin(GPU_PRIM_TRI_FAN, 4);
  immAttr2f(attr_tex, 0.0f, 0.0f);
  immVertex2f(attr_pos, x1, y1);
  immAttr2f(attr_tex, 1.0f, 0.0f);
  immVertex2f(attr_pos, x2, y1);
  immAttr2f(attr_tex, 1.0f, 1.0f);
  immVertex2f(attr_pos, x2, y2);
  immAttr2f(attr_tex, 0.0f, 1.0f);
  immVertex2f(attr_pos, x1, y2);
  immEnd();

  GPU_texture_unbind(render_target_);
  GPU_blend(GPU_BLEND_NONE);
  GPU_depth_mask(true);
  immUnbindProgram();

  GPU_scissor(old_scissor[0], old_scissor[1], old_scissor[2], old_scissor[3]);

  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Lifetime
 * \{ */

BrushStrokePreview::~BrushStrokePreview()
{
  gpu_context_free();
  cache_free(main_cache_);
  cache_free(mask_cache_);
}

bool BrushStrokePreview::update()
{
  if (!brush_) {
    return false;
  }

  /* Re-evaluate the brush textures only when they actually changed (guarded inside #cache_update),
   * then rebuild the deterministic stamp layout for the current stroke parameters. */
  cache_update(main_cache_, &brush_->mtex);
  cache_update(mask_cache_, &brush_->mask_mtex);
  generate_stroke_layout();

  return true;
}

BrushStrokePreview *BrushStrokePreview::ensure(const void *owner,
                                               const Brush *brush,
                                               int2 size,
                                               float stroke_angle,
                                               float stroke_spacing)
{
  if (!owner || !brush) {
    return nullptr;
  }

  size = int2(max_ii(size.x, 1), max_ii(size.y, 1));

  std::unique_ptr<BrushStrokePreview> *entry = g_registry.lookup_ptr(owner);
  BrushStrokePreview *preview = entry ? entry->get() : nullptr;

  /* Recreate GPU resources only when the brush or the size changes. Content changes (texture,
   * layout) are handled by #update() below. */
  if (preview && (preview->brush_ != brush || preview->preview_size_ != size)) {
    g_registry.remove(owner);
    preview = nullptr;
  }

  if (!preview) {
    std::unique_ptr<BrushStrokePreview> new_preview = std::make_unique<BrushStrokePreview>();
    new_preview->brush_ = brush;
    new_preview->preview_size_ = size;

    if (!new_preview->gpu_context_init()) {
      return nullptr;
    }

    cache_init(new_preview->main_cache_, &brush->mtex);
    cache_init(new_preview->mask_cache_, &brush->mask_mtex);

    preview = new_preview.get();
    g_registry.add_overwrite(owner, std::move(new_preview));
  }

  preview->brush_ = brush;
  preview->stroke_angle_ = stroke_angle;
  preview->stroke_spacing_ = stroke_spacing;
  preview->update();

  return preview;
}

void BrushStrokePreview::free_all()
{
  /* Destructors release each preview's GPU resources. */
  g_registry.clear();

  if (g_stroke_preview_shader) {
    GPU_shader_free(g_stroke_preview_shader);
    g_stroke_preview_shader = nullptr;
  }
}

/** \} */

}  // namespace blender::ed::interface
