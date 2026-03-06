/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "image_editor_cursor_target.hh"

#include "DNA_space_types.h"
#include "DNA_image_types.h"

#include "ED_image.hh"
#include "ED_screen.hh"

#include "GPU_immediate.hh"
#include "GPU_immediate_util.hh"
#include "GPU_state.hh"
#include "GPU_matrix.hh"
#include "GPU_texture.hh"

#include "UI_resources.hh"
#include "BLI_math_color.h"
#include "BKE_image.hh"

#include "IMB_imbuf.hh"

#include <numbers>

namespace blender::editors {

/* Initialize static map. */
Map<ARegion *, ImageEditorCursorTarget *> ImageEditorCursorTarget::region_target_map_;

ImageEditorCursorTarget::ImageEditorCursorTarget(SpaceImage *sima, ARegion *region)
    : sima_(sima), region_(region)
{
}

ImageEditorCursorTarget::~ImageEditorCursorTarget() = default;

std::string_view ImageEditorCursorTarget::get_id() const
{
  return "image_editor_cursor";
}

bool ImageEditorCursorTarget::can_display() const
{
  return sima_ != nullptr && region_ != nullptr;
}

PaintCursorTarget::SpaceType ImageEditorCursorTarget::get_space_type() const
{
  return SpaceType::Image;
}

void ImageEditorCursorTarget::update_cursor(const CursorSyncData &data)
{
  if (!data.is_valid) {
    needs_redraw_ = false;
    return;
  }

  current_data_ = data;
  needs_redraw_ = true;

  if (region_) {
    ED_region_tag_redraw(region_);
  }
}

bool ImageEditorCursorTarget::is_compatible_image(Image *image) const
{
  if (sima_ == nullptr) {
    return false;
  }
  return sima_->image == image;
}

void ImageEditorCursorTarget::draw()
{
  if (!needs_redraw_ || !can_display()) {
    return;
  }

  if (!current_data_.is_valid) {
    needs_redraw_ = false;
    return;
  }

  /* Для 2D paint используем image_space_position или UV, если доступна. */
  float2 cursor_pos;
  bool has_cursor_pos = false;

  if (current_data_.image_space_position.has_value()) {
    /* Позиция в image space (0-1) — приоритет для 2D paint. */
    cursor_pos = *current_data_.image_space_position;
    has_cursor_pos = true;
  }
  else if (current_data_.uv_position.has_value()) {
    /* UV координаты — fallback для 3D viewport sync. */
    cursor_pos = *current_data_.uv_position;
    has_cursor_pos = true;
  }

  if (!has_cursor_pos) {
    needs_redraw_ = false;
    return;
  }

  /* Получаем размеры изображения для конвертации UV в pixel coordinates. */
  ImBuf *ibuf = BKE_image_acquire_ibuf(sima_->image, &sima_->iuser, nullptr);
  if (!ibuf) {
    needs_redraw_ = false;
    return;
  }

  const float2 cursor_pixel_pos(cursor_pos.x * ibuf->x, cursor_pos.y * ibuf->y);
  BKE_image_release_ibuf(sima_->image, ibuf, nullptr);

  /* Рисуем круговой курсор. */
  const float radius = current_data_.brush_radius_px;
  if (radius <= 0.0f) {
    needs_redraw_ = false;
    return;
  }

  GPU_matrix_push();

  /* Устанавливаем 2D проекцию как в image editor. */
  float zoomx, zoomy;
  ED_space_image_get_zoom(sima_, region_, &zoomx, &zoomy);

  GPU_matrix_translate_2fv(cursor_pixel_pos);
  GPU_matrix_scale_2f(zoomx, zoomy);

  /* Рисуем внешний круг (белый с прозрачностью). */
  GPUVertFormat *format = immVertexFormat();
  uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  GPU_depth_test(GPU_DEPTH_NONE);
  GPU_blend(GPU_BLEND_ALPHA);
  GPU_line_width(2.0f);

  float white[4] = {1.0f, 1.0f, 1.0f, 0.8f};
  immUniformColor4fv(white);

  /* Рисуем круг через line loop. */
  const int segments = 32;
  immBegin(GPU_PRIM_LINE_LOOP, segments);
  for (int i = 0; i < segments; i++) {
    const float angle = 2.0f * float(M_PI) * float(i) / float(segments);
    const float x = radius * cosf(angle);
    const float y = radius * sinf(angle);
    immVertex2f(pos, x, y);
  }
  immEnd();

  /* Рисуем перекрестие в центре. */
  immUniformColor4f(1.0f, 1.0f, 1.0f, 0.5f);
  immBegin(GPU_PRIM_LINES, 4);
  immVertex2f(pos, -radius * 0.1f, 0.0f);
  immVertex2f(pos, radius * 0.1f, 0.0f);
  immVertex2f(pos, 0.0f, -radius * 0.1f);
  immVertex2f(pos, 0.0f, radius * 0.1f);
  immEnd();

  immUnbindProgram();

  GPU_matrix_pop();

  needs_redraw_ = false;
}

void ImageEditorCursorTarget::register_target_for_region(ARegion *region,
                                                         ImageEditorCursorTarget *target)
{
  region_target_map_.add_overwrite(region, target);
}

void ImageEditorCursorTarget::unregister_target_for_region(ARegion *region)
{
  region_target_map_.remove(region);
}

ImageEditorCursorTarget *ImageEditorCursorTarget::get_target_for_region(ARegion *region)
{
  return region_target_map_.lookup_default(region, nullptr);
}

}  // namespace blender::editors
