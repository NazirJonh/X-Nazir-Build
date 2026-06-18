/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include <cmath>

#include "MEM_guardedalloc.h"

#include "BLI_math_base.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_rand_c.hh"
#include "BLI_rect.hh"
#include "BLI_vector.hh"

#include "BKE_context.hh"
#include "BKE_brush.hh"

#include "DNA_brush_enums.h"
#include "DNA_brush_types.h"
#include "DNA_texture_types.h"

#include "GPU_immediate.hh"
#include "GPU_shader_builtin.hh"
#include "GPU_state.hh"

#include "ED_screen.hh"

#include "brush/brush_texture_preview.h"
#include "brush/brush_texture_preview_api.h"

#include "interface_template_preview_brush.hh"

using namespace blender;

/* Per-stroke transformation derived from the brush settings. */
struct BrushTransform {
  float scale_x;
  float scale_y;
  float rotation;
  float pattern_spacing;
  bool use_random;
  float random_angle;
};

/* A single brush stamp along the previewed stroke. */
struct PatternElement {
  float x, y;
  float rotation;
};

/* Generate a sinusoidal stroke of stamps. The result is deterministic (fixed RNG seed) so the
 * preview does not flicker between redraws. */
static blender::Vector<PatternElement> generate_brush_pattern(const blender::Brush *brush,
                                                              const BrushTransform *transform,
                                                              float center_x,
                                                              float center_y,
                                                              float preview_size,
                                                              float base_angle)
{
  const float preview_width = preview_size * 2.0f;
  const float preview_height = preview_size;
  float footprint_size = min_ff(brush->size, preview_height * 0.8f);
  footprint_size = max_ff(footprint_size, 16.0f);

  /* Map spacing percentage to a stamp count: smaller spacing -> more stamps. */
  float spacing_clamped = max_ff(transform->pattern_spacing, 1.0f);
  spacing_clamped = min_ff(spacing_clamped, 500.0f);
  const float spacing_normalized = (spacing_clamped - 1.0f) / (500.0f - 1.0f);
  int max_elements = int(30.0f - (30.0f - 3.0f) * spacing_normalized);
  max_elements = max_ii(max_elements, 3);
  max_elements = min_ii(max_elements, 30);

  /* Distribute stamps evenly across the preview width. */
  float step_distance = footprint_size * (spacing_clamped / 100.0f);
  step_distance = max_ff(step_distance, footprint_size * 0.1f);
  if (max_elements > 1) {
    step_distance = (preview_width * 0.8f) / float(max_elements - 1);
  }

  const float start_x = center_x - (preview_width * 0.4f);
  const float base_amplitude = preview_height * 0.15f;
  const float max_amplitude = preview_height * 0.4f;
  const float periods = 1.5f;

  RNG *rng = BLI_rng_new(0);

  const bool use_jitter = (brush->flag & BRUSH_ABSOLUTE_JITTER) ? (brush->jitter_absolute != 0) :
                                                                  (brush->jitter != 0.0f);
  const float jitter_amount = use_jitter ? ((brush->flag & BRUSH_ABSOLUTE_JITTER) ?
                                                float(brush->jitter_absolute) :
                                                brush->jitter) :
                                           0.0f;

  blender::Vector<PatternElement> elements;
  elements.reserve(max_elements);

  for (int i = 0; i < max_elements; i++) {
    float x = start_x + float(i) * step_distance;
    const float progress = (max_elements > 1) ? float(i) / float(max_elements - 1) : 0.0f;
    const float sine_phase = progress * periods * 2.0f * float(M_PI);
    const float current_amplitude = base_amplitude + (max_amplitude - base_amplitude) * progress;
    float y = center_y + current_amplitude * sinf(sine_phase);

    if (use_jitter) {
      /* Random offset within a circle (same idea as #BKE_brush_jitter_pos). */
      float rand_pos[2];
      do {
        rand_pos[0] = BLI_rng_get_float(rng) - 0.5f;
        rand_pos[1] = BLI_rng_get_float(rng) - 0.5f;
      } while ((rand_pos[0] * rand_pos[0] + rand_pos[1] * rand_pos[1]) > 0.25f);

      const float jitter_radius = (brush->flag & BRUSH_ABSOLUTE_JITTER) ?
                                      jitter_amount :
                                      preview_size * 0.1f * jitter_amount;
      x += 2.0f * rand_pos[0] * jitter_radius;
      y += 2.0f * rand_pos[1] * jitter_radius;
    }

    float element_angle = base_angle;
    if (transform->use_random && transform->random_angle > 0.0f) {
      const float random_factor = BLI_rng_get_float(rng);
      element_angle += -transform->random_angle / 2.0f + transform->random_angle * random_factor;
    }

    elements.append(PatternElement{x, y, element_angle});
  }

  BLI_rng_free(rng);
  return elements;
}

void ED_brush_stroke_preview_draw(
    const blender::bContext *C, void *brush_data, float angle, float spacing, blender::rcti *rect)
{
  if (!brush_data || !rect) {
    return;
  }

  blender::Brush *brush = static_cast<blender::Brush *>(brush_data);

  BrushTransform transform = {};
  transform.scale_x = 1.0f;
  transform.scale_y = 1.0f;
  transform.rotation = angle;
  transform.pattern_spacing = spacing;
  transform.use_random = (brush->mtex.brush_angle_mode & MTEX_ANGLE_RANDOM) != 0;
  transform.random_angle = brush->mtex.random_angle;

  /* Scale the stamp size with the brush size. */
  const float size_factor = brush->size / 100.0f;
  transform.scale_x *= size_factor;
  transform.scale_y *= size_factor;

  /* Stamp color depends on the brush mode. */
  float color[4] = {0.5f, 0.5f, 0.5f, 1.0f};
  if (brush->sculpt_brush_type != 0) {
    color[0] = 0.2f;
    color[1] = 0.6f;
    color[2] = 1.0f;
  }
  else if (brush->vertex_brush_type != 0) {
    color[0] = 0.2f;
    color[1] = 1.0f;
    color[2] = 0.2f;
  }
  else if (brush->weight_brush_type != 0) {
    color[0] = 1.0f;
    color[1] = 1.0f;
    color[2] = 0.2f;
  }
  else if (brush->image_brush_type != 0) {
    color[0] = 1.0f;
    color[1] = 0.2f;
    color[2] = 0.2f;
  }
  else if (brush->gpencil_brush_type != 0) {
    color[0] = 0.8f;
    color[1] = 0.2f;
    color[2] = 0.8f;
  }
  else if (brush->curves_sculpt_brush_type != 0) {
    color[0] = 0.2f;
    color[1] = 0.8f;
    color[2] = 0.8f;
  }

  const float preview_size = min_ff(rect->xmax - rect->xmin, rect->ymax - rect->ymin) * 0.7f;
  const float center_x = (rect->xmin + rect->xmax) * 0.5f;
  const float center_y = (rect->ymin + rect->ymax) * 0.5f;

  /* Prefer the GPU texture preview when the brush has a usable texture. It renders its own stamp
   * pattern into an off-screen target and blits it into the widget rectangle. */
  const blender::ARegion *region = CTX_wm_region(C);
  if (region && brush->mtex.tex) {
    const blender::int2 preview_px(rect->xmax - rect->xmin, rect->ymax - rect->ymin);
    const blender::Brush *brush_const = static_cast<const blender::Brush *>(brush_data);
    blender::ed::interface::BrushTexturePreview *texture_preview =
        blender::ed::interface::BKE_brush_texture_preview_ensure(
            region, brush_const, preview_px, angle, spacing);
    if (texture_preview &&
        blender::ed::interface::BKE_brush_texture_preview_render(texture_preview) &&
        blender::ed::interface::BKE_brush_texture_preview_draw_ui(texture_preview, rect, 1.0f))
    {
      return;
    }
  }

  /* Fallback: draw colored stamps along the previewed stroke. */
  const blender::Vector<PatternElement> pattern = generate_brush_pattern(
      brush, &transform, center_x, center_y, preview_size, angle);
  const int pattern_count = pattern.size();

  const float base_element_size = (preview_size * transform.scale_x) * 0.3f;
  const float max_element_size = (preview_size * transform.scale_x) * 0.8f;
  const float min_element_size = max_ff(base_element_size * 0.5f, 15.0f);

  GPU_blend(GPU_BLEND_ALPHA);

  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(
      format, "pos", blender::gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  /* Background. */
  immUniformColor4f(0.1f, 0.1f, 0.1f, 1.0f);
  immBegin(GPU_PRIM_TRI_FAN, 4);
  immVertex2f(pos, rect->xmin, rect->ymin);
  immVertex2f(pos, rect->xmax, rect->ymin);
  immVertex2f(pos, rect->xmax, rect->ymax);
  immVertex2f(pos, rect->xmin, rect->ymax);
  immEnd();

  /* Stamps as rotated quads with growing size. */
  immUniformColor4fv(color);
  for (int p = 0; p < pattern_count; p++) {
    const float progress = (pattern_count > 1) ? float(p) / float(pattern_count - 1) : 0.0f;
    float element_size = base_element_size + (max_element_size - base_element_size) * progress;
    element_size = max_ff(element_size, min_element_size);
    const float half_size = element_size * 0.5f;

    const float cx = pattern[p].x;
    const float cy = pattern[p].y;
    const float cos_a = cosf(pattern[p].rotation);
    const float sin_a = sinf(pattern[p].rotation);
    const float corners[4][2] = {
        {-half_size, -half_size},
        {half_size, -half_size},
        {half_size, half_size},
        {-half_size, half_size},
    };

    immBegin(GPU_PRIM_TRI_FAN, 6);
    immVertex2f(pos, cx, cy);
    for (int i = 0; i < 5; i++) {
      const int c = i % 4;
      const float rx = corners[c][0] * cos_a - corners[c][1] * sin_a;
      const float ry = corners[c][0] * sin_a + corners[c][1] * cos_a;
      immVertex2f(pos, cx + rx, cy + ry);
    }
    immEnd();
  }
  immUnbindProgram();

  /* Rotation indicator. */
  if (transform.rotation != 0.0f) {
    const float indicator_radius = preview_size * 0.15f;
    GPUVertFormat *line_format = immVertexFormat();
    const uint line_pos = GPU_vertformat_attr_add(
        line_format, "pos", blender::gpu::VertAttrType::SFLOAT_32_32);
    immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
    immUniformColor4f(1.0f, 1.0f, 1.0f, 0.8f);
    immBegin(GPU_PRIM_LINES, 2);
    immVertex2f(line_pos, center_x, center_y);
    immVertex2f(line_pos,
                center_x + indicator_radius * cosf(transform.rotation),
                center_y + indicator_radius * sinf(transform.rotation));
    immEnd();
    immUnbindProgram();
  }

  GPU_blend(GPU_BLEND_NONE);
}
