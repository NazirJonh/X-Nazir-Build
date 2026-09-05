/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw_engine
 */

#pragma once

#include "BLI_math_matrix_types.hh"

#include "BKE_image_wrappers.hh"

#include "image_batches.hh"
#include "image_buffer_cache.hh"
#include "image_partial_updater.hh"
#include "image_private.hh"
#include "image_shader_params.hh"
#include "image_texture_info.hh"
#include "image_usage.hh"

#include "DRW_render.hh"
#include "draw_command.hh"
#include "draw_manager.hh"
#include "draw_pass.hh"

namespace blender::image_engine {
using namespace blender::draw;

struct State {
  blender::Image *image = nullptr;
  /** Usage data of the previous time, to identify changes that require a full update. */
  ImageUsage last_usage;
  /**
   * Revision of the display override buffer the textures were last uploaded from. Zero when the
   * space has no override, which is every space but an Image Editor showing a composite.
   */
  uint64_t display_override_revision = 0;
  /**
   * The part of the override that changed with #display_override_revision, in image coordinates,
   * or empty when the whole texture has to be rebuilt from it.
   *
   * Empty is the conservative answer and covers three different situations: nothing changed, the
   * override could not say what changed, and the mapping to the texture is one an axis-aligned
   * rectangle cannot express -- a rotated canvas or tile drawing. Only a non-empty rectangle is
   * ever a licence to refresh less than everything.
   *
   * Consumed by #ScreenSpaceDrawingMode::update_textures in the same frame it is set.
   */
  rcti display_override_changed_region = {0, 0, 0, 0};
  /**
   * Whether an override buffer was really produced this frame, as opposed to the space merely
   * being set to want one.
   *
   * Resolved once, in #ScreenSpaceDrawingMode::image_sync, and read by the paths that would
   * otherwise each pay for the composite again. False also covers a space that asked for a
   * composite its material cannot supply, which then has to be drawn as an ordinary image.
   */
  bool has_display_override = false;

  PartialImageUpdater partial_update = {};

  View view = {"Image.View"};
  ShaderParameters sh_params;
  struct {
    /**
     * \brief should we perform tiled drawing (wrap repeat).
     *
     * Option is true when image is capable of tile drawing (image is not tile) and the tiled
     * option is set in the space.
     */
    bool do_tile_drawing : 1;
  } flags;

  Framebuffer depth_fb = {"Image.Depth"};
  Framebuffer color_fb = {"Image.Color"};

  PassSimple depth_ps = {"Image.Depth"};
  PassSimple image_ps = {"Image.Color"};

  /**
   * Cache containing the float buffers when drawing byte images.
   */
  FloatBufferCache float_buffers;

  /** \brief Transform matrix to convert a normalized screen space coordinates to texture space. */
  float3x3 ss_to_texture;

  Vector<TextureInfo> texture_infos;

 public:
  virtual ~State() = default;

  void clear_need_full_update_flag()
  {
    reset_need_full_update(false);
  }
  void mark_all_texture_slots_dirty()
  {
    reset_need_full_update(true);
  }

  void update_batches()
  {
    for (TextureInfo &info : texture_infos) {
      BatchUpdater batch_updater(info);
      batch_updater.update_batch();
    }
  }

  void update_image_usage(const ImageUser *image_user)
  {
    ImageUsage usage(image, image_user, flags.do_tile_drawing);
    if (last_usage != usage) {
      last_usage = usage;
      reset_need_full_update(true);
      float_buffers.clear();
    }
  }

 private:
  /** \brief Set dirty flag of all texture slots to the given value. */
  void reset_need_full_update(bool new_value)
  {
    for (TextureInfo &info : texture_infos) {
      info.need_full_update = new_value;
    }
  }
};

}  // namespace blender::image_engine
