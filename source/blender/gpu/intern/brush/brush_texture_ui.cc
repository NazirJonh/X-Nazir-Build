/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 * \brief UI integration for brush texture preview system.
 */

#include "brush_texture_preview_api.h"
#include "brush_texture_shaders.h"

#include "DNA_brush_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "BKE_brush.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
//#include "BKE_material.h"
#include "BKE_paint.hh"
#include "BKE_scene.hh"
#include "BKE_texture.h"

#include "BLI_fileops.hh"
#include "BLI_hash.hh"
#include "BLI_listbase.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_path_utils.hh"
#include "BLI_rect.hh"
#include "BLI_string.hh"
#include "BLI_threads.hh"



#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "RNA_types.hh"

#include "MEM_guardedalloc.h"

namespace blender::ed::interface {

/* -------------------------------------------------------------------- */
/** \name UI Template Registration
 * \{ */

/* UI template for brush texture preview */
struct BrushTexturePreviewTemplate {
  BrushTexturePreview *preview;
  int2 last_size;
  bool needs_update;
  float zoom_factor;
  float2 pan_offset;
  bool show_pattern_overlay;
  bool show_mask_overlay;
  bool interactive_mode;
};

static void brush_texture_preview_template_free(BrushTexturePreviewTemplate *template_data)
{
  if (!template_data) {
    return;
  }
  
  if (template_data->preview) {
    BKE_brush_texture_preview_free(template_data->preview);
  }
  
  MEM_delete(template_data);
}

static BrushTexturePreviewTemplate *brush_texture_preview_template_create(const Brush *brush, int2 size)
{
  BrushTexturePreviewTemplate *template_data = MEM_new<BrushTexturePreviewTemplate>("BrushTexturePreviewTemplate");
  
  template_data->preview = BKE_brush_texture_preview_create(brush, size);
  template_data->last_size = size;
  template_data->needs_update = true;
  template_data->zoom_factor = 1.0f;
  template_data->pan_offset = float2(0.0f, 0.0f);
  template_data->show_pattern_overlay = true;
  template_data->show_mask_overlay = false;
  template_data->interactive_mode = false;
  
  return template_data;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name UI Drawing Functions
 * \{ */

static void draw_brush_texture_preview_background(const rcti *rect)
{
  /* Draw checkerboard background for transparency */
  float checker_colors[2][4] = {
    {0.8f, 0.8f, 0.8f, 1.0f}, /* Light gray */
    {0.6f, 0.6f, 0.6f, 1.0f}  /* Dark gray */
  };
  
  int checker_size = 8;
  int width = BLI_rcti_size_x(rect);
  int height = BLI_rcti_size_y(rect);
  
  GPU_blend(GPU_BLEND_NONE);
  
  GPUVertFormat *format = immVertexFormat();
  uint pos_attr = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);
  uint col_attr = GPU_vertformat_attr_add(format, "color", gpu::VertAttrType::SFLOAT_32_32_32_32);
  
  immBindBuiltinProgram(GPU_SHADER_3D_FLAT_COLOR);
  immBegin(GPU_PRIM_TRIS, (width / checker_size + 1) * (height / checker_size + 1) * 6);
  
  for (int y = 0; y < height; y += checker_size) {
    for (int x = 0; x < width; x += checker_size) {
      int checker_idx = ((x / checker_size) + (y / checker_size)) % 2;
      float *color = checker_colors[checker_idx];
      
      float x1 = rect->xmin + x;
      float y1 = rect->ymin + y;
      float x2 = min_ff(rect->xmin + x + checker_size, rect->xmax);
      float y2 = min_ff(rect->ymin + y + checker_size, rect->ymax);
      
      /* Draw quad as two triangles */
      immAttr4fv(col_attr, color);
      immVertex2f(pos_attr, x1, y1);
      immAttr4fv(col_attr, color);
      immVertex2f(pos_attr, x2, y1);
      immAttr4fv(col_attr, color);
      immVertex2f(pos_attr, x2, y2);
      
      immAttr4fv(col_attr, color);
      immVertex2f(pos_attr, x1, y1);
      immAttr4fv(col_attr, color);
      immVertex2f(pos_attr, x2, y2);
      immAttr4fv(col_attr, color);
      immVertex2f(pos_attr, x1, y2);
    }
  }
  
  immEnd();
  immUnbindProgram();
}

static void draw_brush_texture_preview_border(const rcti *rect)
{
  /* Draw border around preview */
  float border_color[4] = {0.3f, 0.3f, 0.3f, 1.0f};
  
  GPU_line_width(1.0f);
  
  GPUVertFormat *format = immVertexFormat();
  uint pos_attr = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);
  
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
  immUniformColor4fv(border_color);
  
  immBegin(GPU_PRIM_LINE_LOOP, 4);
  immVertex2f(pos_attr, rect->xmin, rect->ymin);
  immVertex2f(pos_attr, rect->xmax, rect->ymin);
  immVertex2f(pos_attr, rect->xmax, rect->ymax);
  immVertex2f(pos_attr, rect->xmin, rect->ymax);
  immEnd();
  
  immUnbindProgram();
}

static void draw_brush_texture_preview_overlay(const BrushTexturePreviewTemplate *template_data, const rcti *rect)
{
  if (!template_data->show_pattern_overlay && !template_data->show_mask_overlay) {
    return;
  }
  
  const BrushTexturePreview *preview = template_data->preview;
  if (!preview) {
    return;
  }
  
  GPU_blend(GPU_BLEND_ALPHA);
  
  GPUVertFormat *format = immVertexFormat();
  uint pos_attr = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);
  uint col_attr = GPU_vertformat_attr_add(format, "color", gpu::VertAttrType::SFLOAT_32_32_32_32);
  
  immBindBuiltinProgram(GPU_SHADER_3D_FLAT_COLOR);
  
  /* Draw pattern elements as overlay */
  if (template_data->show_pattern_overlay) {
    float overlay_color[4] = {1.0f, 1.0f, 0.0f, 0.3f}; /* Yellow overlay */
    
    for (const TextureElement &element : preview->pattern_elements) {
      /* Convert element position to screen coordinates */
      float center_x = rect->xmin + (element.position.x + 1.0f) * 0.5f * BLI_rcti_size_x(rect);
      float center_y = rect->ymin + (element.position.y + 1.0f) * 0.5f * BLI_rcti_size_y(rect);
      
      float size_x = element.size.x * BLI_rcti_size_x(rect) * 0.5f;
      float size_y = element.size.y * BLI_rcti_size_y(rect) * 0.5f;
      
      /* Draw element bounds */
      immBegin(GPU_PRIM_LINE_LOOP, 4);
      immAttr4fv(col_attr, overlay_color);
      immVertex2f(pos_attr, center_x - size_x, center_y - size_y);
      immAttr4fv(col_attr, overlay_color);
      immVertex2f(pos_attr, center_x + size_x, center_y - size_y);
      immAttr4fv(col_attr, overlay_color);
      immVertex2f(pos_attr, center_x + size_x, center_y + size_y);
      immAttr4fv(col_attr, overlay_color);
      immVertex2f(pos_attr, center_x - size_x, center_y + size_y);
      immEnd();
    }
  }
  
  immUnbindProgram();
  GPU_blend(GPU_BLEND_NONE);
}

static void draw_brush_texture_preview_info(const BrushTexturePreviewTemplate *template_data, [[maybe_unused]] const rcti *rect)
{
  const BrushTexturePreview *preview = template_data->preview;
  if (!preview) {
    return;
  }
  
  /* Draw info text overlay */
  char info_text[256];
  BLI_snprintf(info_text, sizeof(info_text), 
               "Elements: %d | Zoom: %.1fx | %s",
               int(preview->pattern_elements.size()),
               template_data->zoom_factor,
               template_data->interactive_mode ? "Interactive" : "Static");
  
  /* This would use Blender's text drawing API */
  /* For now, we'll skip the actual text rendering implementation */
  (void)info_text; /* Suppress unused warning */
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name UI Event Handling
 * \{ */

static int brush_texture_preview_handle_mouse(BrushTexturePreviewTemplate *template_data,
                                             const rcti *rect,
                                             const wmEvent *event)
{
  if (!template_data || !template_data->interactive_mode) {
    return WM_UI_HANDLER_CONTINUE;
  }
  
  int width = BLI_rcti_size_x(rect);
  int height = BLI_rcti_size_y(rect);
  
  /* Convert mouse coordinates to UV space */
  float2 mouse_uv = float2(
      float(event->xy[0] - rect->xmin) / width,
      float(event->xy[1] - rect->ymin) / height
  );
  
  switch (event->type) {
    case MOUSEMOVE:
      if (event->modifier & KM_SHIFT) {
        /* Pan mode */
        static float2 last_mouse = mouse_uv;
        template_data->pan_offset += (mouse_uv - last_mouse) * 2.0f;
        template_data->needs_update = true;
        last_mouse = mouse_uv;
        return WM_UI_HANDLER_BREAK;
      }
      break;
      
    case WHEELUPMOUSE:
      /* Zoom in */
      template_data->zoom_factor = min_ff(template_data->zoom_factor * 1.1f, 10.0f);
      template_data->needs_update = true;
      return WM_UI_HANDLER_BREAK;
      
    case WHEELDOWNMOUSE:
      /* Zoom out */
      template_data->zoom_factor = max_ff(template_data->zoom_factor * 0.9f, 0.1f);
      template_data->needs_update = true;
      return WM_UI_HANDLER_BREAK;
      
    case LEFTMOUSE:
      if (event->val == KM_PRESS) {
        /* Sample texture at mouse position */
        (void)mouse_uv; /* TODO: Implement pixel info sampling */
        return WM_UI_HANDLER_BREAK;
      }
      break;
      
    case EVT_RKEY:
      if (event->val == KM_PRESS) {
        /* Reset view */
        template_data->zoom_factor = 1.0f;
        template_data->pan_offset = float2(0.0f, 0.0f);
        template_data->needs_update = true;
        return WM_UI_HANDLER_BREAK;
      }
      break;
      
    default:
      /* Ignore all other event types */
      break;
  }
  
  return WM_UI_HANDLER_CONTINUE;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Registration and Cleanup
 * \{ */

void ED_brush_texture_preview_ui_register()
{
  /* Register UI templates with Blender's UI system */
  /* This would be called during editor initialization */
}

void ED_brush_texture_preview_ui_unregister()
{
  /* Cleanup UI templates */
  /* This would be called during editor cleanup */
}

/** \} */

} // namespace blender::ed::interface
