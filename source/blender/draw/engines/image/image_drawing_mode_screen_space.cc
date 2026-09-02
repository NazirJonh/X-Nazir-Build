/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "draw_view_data.hh"

#include "image_drawing_mode_screen_space.hh"
#include "image_instance.hh"
#include "image_shader.hh"

#include "BLI_memory_utils.hh"

#include "BKE_image.hh"
#include "BKE_image_partial_update.hh"
#include "BKE_paint_material_channel_perf_debug.hh"

namespace blender::image_engine {

void ScreenSpaceDrawingMode::add_shgroups() const
{
  PassSimple &pass = instance_.state.image_ps;
  gpu::Shader *shader = ShaderModule::module_get().color.get();
  const ShaderParameters &sh_params = instance_.state.sh_params;
  DefaultTextureList *dtxl = DRW_context_get()->viewport_texture_list_get();

  pass.shader_set(shader);
  pass.push_constant("far_near_distances", sh_params.far_near);
  pass.push_constant("shuffle", sh_params.shuffle);
  pass.push_constant("draw_flags", int32_t(sh_params.flags));
  pass.push_constant("is_image_premultiplied", sh_params.use_premul_alpha);
  pass.bind_texture("depth_tx", dtxl->depth);

  float4x4 image_mat = float4x4::identity();
  ResourceHandleRange handle = instance_.manager->resource_handle(image_mat);
  for (const TextureInfo &info : instance_.state.texture_infos) {
    PassSimple::Sub &sub = pass.sub("Texture");
    sub.push_constant("offset", info.offset());
    sub.bind_texture("image_tx", info.texture);
    sub.draw(info.batch, handle);
  }
}

void ScreenSpaceDrawingMode::add_depth_shgroups(blender::Image *image, ImageUser *image_user) const
{
  PassSimple &pass = instance_.state.depth_ps;
  gpu::Shader *shader = ShaderModule::module_get().depth.get();
  pass.shader_set(shader);

  float4x4 image_mat = float4x4::identity();
  ResourceHandleRange handle = instance_.manager->resource_handle(image_mat);

  ImageUser tile_user = {nullptr};
  if (image_user) {
    tile_user = *image_user;
  }

  for (const TextureInfo &info : instance_.state.texture_infos) {
    for (ImageTile &image_tile_ptr : image->tiles) {
      const ImageTileWrapper image_tile(&image_tile_ptr);
      const int tile_x = image_tile.get_tile_x_offset();
      const int tile_y = image_tile.get_tile_y_offset();
      tile_user.tile = image_tile.get_tile_number();

      /* NOTE: `BKE_image_has_ibuf` doesn't work as it fails for render results. That could be a
       * bug or a feature. For now we just acquire to determine if there is a texture. */
      void *lock;
      ImBuf *tile_buffer = BKE_image_acquire_ibuf(image, &tile_user, &lock);
      if (tile_buffer != nullptr) {
        instance_.state.float_buffers.mark_used(tile_buffer);
        PassSimple::Sub &sub = pass.sub("Tile");
        float4 min_max_uv(tile_x, tile_y, tile_x + 1, tile_y + 1);
        sub.push_constant("min_max_uv", min_max_uv);
        sub.draw(info.batch, handle);
      }
      BKE_image_release_ibuf(image, tile_buffer, lock);
    }
  }
}

void ScreenSpaceDrawingMode::update_textures(blender::Image *image,
                                             ImageUser *image_user,
                                             ImBuf *override_buffer) const
{
  State &state = instance_.state;

  /* The partial update checker reports changes to the image's own pixels, which say nothing about
   * a display override composited from several images. What changed in one was worked out from the
   * override revision in #image_sync instead, and left in the two fields read here. */
  if (state.has_display_override) {
    const rcti changed_region = state.display_override_changed_region;
    /* Consumed: it describes one revision step and must not be replayed on a later frame. */
    BLI_rcti_init(&state.display_override_changed_region, 0, 0, 0, 0);

    for (TextureInfo &info : state.texture_infos) {
      if (info.need_full_update) {
        do_full_update_gpu_texture(info, image_user, override_buffer);
        continue;
      }
      /* A slot that is already current except for a known rectangle takes only that rectangle.
       * The whole point of the exercise: a dab on a large canvas used to re-transform and
       * re-upload the entire region-sized texture, which costs the same whatever the dab was. */
      if (!BLI_rcti_is_empty(&changed_region) && override_buffer != nullptr) {
        do_partial_update_texture_slot(info, *override_buffer, float2(0.0f), changed_region);
      }
    }
    return;
  }

  PartialUpdateChecker<ImageTileData> checker(image, image_user, state.partial_update.user);
  PartialUpdateChecker<ImageTileData>::CollectResult changes = checker.collect_changes();

  switch (changes.get_result_code()) {
    case ePartialUpdateCollectResult::FullUpdateNeeded:
      state.mark_all_texture_slots_dirty();
      state.float_buffers.clear();
      break;
    case ePartialUpdateCollectResult::NoChangesDetected:
      break;
    case ePartialUpdateCollectResult::PartialChangesDetected:
      /* Partial update when wrap repeat is enabled is not supported, and neither is a rotated
       * canvas: the changed region is mapped to the texture as an axis-aligned rectangle, which
       * a rotation makes meaningless. */
      if (state.flags.do_tile_drawing || instance_.space().get_canvas_rotation() != 0.0f) {
        state.float_buffers.clear();
        state.mark_all_texture_slots_dirty();
      }
      else {
        do_partial_update(changes);
      }
      break;
  }
  /* Null on this path by construction: it is only reached when the space produced no override. */
  do_full_update_for_dirty_textures(image_user, override_buffer);
}

void ScreenSpaceDrawingMode::do_partial_update_float_buffer(
    ImBuf *float_buffer, PartialUpdateChecker<ImageTileData>::CollectResult &iterator) const
{
  ImBuf *src = iterator.tile_data.tile_buffer;
  BLI_assert(float_buffer->float_data() != nullptr);
  BLI_assert(float_buffer->byte_data() == nullptr);
  BLI_assert(src->float_data() == nullptr);
  BLI_assert(src->byte_data() != nullptr);

  /* Calculate the overlap between the updated region and the buffer size. Partial Update Checker
   * always returns a tile (256x256). Which could lay partially outside the buffer when using
   * different resolutions.
   */
  rcti buffer_rect;
  BLI_rcti_init(&buffer_rect, 0, float_buffer->x, 0, float_buffer->y);
  rcti clipped_update_region;
  const bool has_overlap = BLI_rcti_isect(
      &buffer_rect, &iterator.changed_region.region, &clipped_update_region);
  if (!has_overlap) {
    return;
  }

  IMB_float_from_byte_ex(float_buffer, src, &clipped_update_region);
}

void ScreenSpaceDrawingMode::do_partial_update_texture_slot(const TextureInfo &info,
                                                            const ImBuf &source,
                                                            const float2 tile_offset,
                                                            const rcti &changed_region) const
{
  /* The extraction below samples float pixels directly. Every caller already arranges for that --
   * the tile path converts through #FloatBufferCache, and a display override that reports a
   * changed region at all is the float Combined preview -- but a byte buffer arriving here would
   * be read as garbage rather than refused, so it is checked rather than assumed. */
  BLI_assert(source.float_data() != nullptr);
  if (source.float_data() == nullptr) {
    return;
  }

  gpu::Texture *texture = info.texture;
  const float texture_width = GPU_texture_width(texture);
  const float texture_height = GPU_texture_height(texture);

  rctf changed_region_in_uv_space;
  BLI_rctf_init(&changed_region_in_uv_space,
                float(changed_region.xmin) / float(source.x) + tile_offset.x,
                float(changed_region.xmax) / float(source.x) + tile_offset.x,
                float(changed_region.ymin) / float(source.y) + tile_offset.y,
                float(changed_region.ymax) / float(source.y) + tile_offset.y);
  rctf changed_overlapping_region_in_uv_space;
  if (!BLI_rctf_isect(&info.clipping_uv_bounds,
                      &changed_region_in_uv_space,
                      &changed_overlapping_region_in_uv_space))
  {
    return;
  }

  /* Convert the overlapping region to texel space and to ss_pixel space...
   * TODO: first convert to ss_pixel space as integer based. and from there go back to texel
   * space. But perhaps this isn't needed and we could use an extraction offset somehow. */
  rcti gpu_texture_region_to_update;
  BLI_rcti_init(
      &gpu_texture_region_to_update,
      floor((changed_overlapping_region_in_uv_space.xmin - info.clipping_uv_bounds.xmin) *
            texture_width / BLI_rctf_size_x(&info.clipping_uv_bounds)),
      floor((changed_overlapping_region_in_uv_space.xmax - info.clipping_uv_bounds.xmin) *
            texture_width / BLI_rctf_size_x(&info.clipping_uv_bounds)),
      ceil((changed_overlapping_region_in_uv_space.ymin - info.clipping_uv_bounds.ymin) *
           texture_height / BLI_rctf_size_y(&info.clipping_uv_bounds)),
      ceil((changed_overlapping_region_in_uv_space.ymax - info.clipping_uv_bounds.ymin) *
           texture_height / BLI_rctf_size_y(&info.clipping_uv_bounds)));
  gpu_texture_region_to_update.xmax = min_ii(gpu_texture_region_to_update.xmax,
                                             info.clipping_bounds.xmax);
  gpu_texture_region_to_update.ymax = min_ii(gpu_texture_region_to_update.ymax,
                                             info.clipping_bounds.ymax);
  /* A change that rounds away to nothing on screen still has to leave the texture alone rather
   * than upload a zero-sized rectangle. */
  if (BLI_rcti_is_empty(&gpu_texture_region_to_update)) {
    return;
  }

  /* Create an image buffer with a size.
   * Extract and scale into an imbuf. */
  const int texture_region_width = BLI_rcti_size_x(&gpu_texture_region_to_update);
  const int texture_region_height = BLI_rcti_size_y(&gpu_texture_region_to_update);

  ImBuf extracted_buffer;
  IMB_initImBuf(
      &extracted_buffer, texture_region_width, texture_region_height, ImBufFlags::FloatData);

  int offset = 0;
  float *float_data = extracted_buffer.float_data_for_write();
  for (int y = gpu_texture_region_to_update.ymin; y < gpu_texture_region_to_update.ymax; y++) {
    float yf = y / float(texture_height);
    float v = info.clipping_uv_bounds.ymax * yf + info.clipping_uv_bounds.ymin * (1.0 - yf) -
              tile_offset.y;
    for (int x = gpu_texture_region_to_update.xmin; x < gpu_texture_region_to_update.xmax; x++) {
      float xf = x / float(texture_width);
      float u = info.clipping_uv_bounds.xmax * xf + info.clipping_uv_bounds.xmin * (1.0 - xf) -
                tile_offset.x;
      imbuf::interpolate_nearest_border_fl(
          &source, &float_data[offset * 4], u * source.x, v * source.y);
      offset++;
    }
  }
  IMB_gpu_clamp_half_float(&extracted_buffer);

  GPU_texture_update_sub(texture,
                         GPU_DATA_FLOAT,
                         float_data,
                         gpu_texture_region_to_update.xmin,
                         gpu_texture_region_to_update.ymin,
                         0,
                         extracted_buffer.x,
                         extracted_buffer.y,
                         0);
  IMB_free_all_data(&extracted_buffer);
}

void ScreenSpaceDrawingMode::do_partial_update(
    PartialUpdateChecker<ImageTileData>::CollectResult &iterator) const
{
  while (iterator.get_next_change() == ePartialUpdateIterResult::ChangeAvailable) {
    /* Quick exit when tile_buffer isn't available. */
    if (iterator.tile_data.tile_buffer == nullptr) {
      continue;
    }
    ImBuf *tile_buffer = instance_.state.float_buffers.cached_float_buffer(
        iterator.tile_data.tile_buffer);
    if (tile_buffer != iterator.tile_data.tile_buffer) {
      do_partial_update_float_buffer(tile_buffer, iterator);
    }

    const ImageTileWrapper tile_accessor(iterator.tile_data.tile);
    const float2 tile_offset(float(tile_accessor.get_tile_x_offset()),
                             float(tile_accessor.get_tile_y_offset()));

    for (const TextureInfo &info : instance_.state.texture_infos) {
      /* Dirty images will receive a full update. No need to do a partial one now. */
      if (info.need_full_update) {
        continue;
      }
      do_partial_update_texture_slot(
          info, *tile_buffer, tile_offset, iterator.changed_region.region);
    }
  }
}

void ScreenSpaceDrawingMode::do_full_update_for_dirty_textures(const ImageUser *image_user,
                                                               ImBuf *override_buffer) const
{
  for (TextureInfo &info : instance_.state.texture_infos) {
    if (!info.need_full_update) {
      continue;
    }
    do_full_update_gpu_texture(info, image_user, override_buffer);
  }
}

void ScreenSpaceDrawingMode::do_full_update_gpu_texture(TextureInfo &info,
                                                        const ImageUser *image_user,
                                                        ImBuf *override_buffer) const
{
  ImBuf texture_buffer;
  const int texture_width = GPU_texture_width(info.texture);
  const int texture_height = GPU_texture_height(info.texture);
  IMB_initImBuf(&texture_buffer, texture_width, texture_height, ImBufFlags::FloatData);
  ImageUser tile_user = {nullptr};
  if (image_user) {
    tile_user = *image_user;
  }

  void *lock;

  blender::Image *image = instance_.state.image;

  /* A display override replaces the pixels of every tile: it is a single buffer covering the
   * image, and a stack it could have been composited from cannot be tiled in the first place.
   *
   * Resolved by #image_sync and passed in, never acquired here: an acquisition runs the whole
   * gather -- a graph walk and a composite refresh for each of eight channels -- and this function
   * runs once per dirty texture slot. */
  if (override_buffer != nullptr) {
    const ImageTileWrapper image_tile(static_cast<ImageTile *>(image->tiles.first));
    do_full_update_texture_slot(info, texture_buffer, *override_buffer, image_tile);
    IMB_gpu_clamp_half_float(&texture_buffer);
    GPU_texture_update(info.texture, GPU_DATA_FLOAT, texture_buffer.float_data());
    IMB_free_all_data(&texture_buffer);
    return;
  }

  for (ImageTile &image_tile_ptr : image->tiles) {
    const ImageTileWrapper image_tile(&image_tile_ptr);
    tile_user.tile = image_tile.get_tile_number();

    ImBuf *tile_buffer = BKE_image_acquire_ibuf(image, &tile_user, &lock);
    if (tile_buffer != nullptr) {
      do_full_update_texture_slot(info, texture_buffer, *tile_buffer, image_tile);
    }
    BKE_image_release_ibuf(image, tile_buffer, lock);
  }
  IMB_gpu_clamp_half_float(&texture_buffer);
  GPU_texture_update(info.texture, GPU_DATA_FLOAT, texture_buffer.float_data());
  IMB_free_all_data(&texture_buffer);
}

void ScreenSpaceDrawingMode::do_full_update_texture_slot(const TextureInfo &texture_info,
                                                         ImBuf &texture_buffer,
                                                         ImBuf &tile_buffer,
                                                         const ImageTileWrapper &image_tile) const
{
  ImBuf *float_tile_buffer = instance_.state.float_buffers.cached_float_buffer(&tile_buffer);

  /* Destination texel to source pixel, which is what #IMB_transform maps with.
   *
   * Built from #State.ss_to_texture rather than by fitting the two rectangles to each other: the
   * region-to-image mapping is not a plain rectangle fit once the canvas is rotated, and a
   * rect-to-rect fit silently drops that rotation -- which is what used to leave a rotated canvas
   * drawn straight in this drawing mode.
   *
   * A texture covers its #TextureInfo.clipping_bounds of the region at one texel per pixel, so a
   * destination texel becomes a region pixel by that offset and a normalized screen coordinate by
   * the region size. Taking the offset from the texture info rather than assuming the whole region
   * is what keeps this correct for a method that splits the region into several textures. */
  const float2 region_size = float2(instance_.region->winx, instance_.region->winy);
  const float3x3 texel_to_screen_uv = math::from_scale<float3x3, 2>(1.0f / region_size) *
                                      math::from_location<float3x3>(
                                          float2(texture_info.clipping_bounds.xmin,
                                                 texture_info.clipping_bounds.ymin));
  const float3x3 image_uv_to_tile_texel =
      math::from_scale<float3x3, 2>(float2(tile_buffer.x, tile_buffer.y)) *
      math::from_location<float3x3>(
          float2(-image_tile.get_tile_x_offset(), -image_tile.get_tile_y_offset()));
  const float3x3 uv_to_texel = image_uv_to_tile_texel * instance_.state.ss_to_texture *
                               texel_to_screen_uv;

  rctf crop_rect;
  const rctf *crop_rect_ptr = nullptr;
  eIMBTransformMode transform_mode;
  if (instance_.state.flags.do_tile_drawing) {
    transform_mode = IMB_TRANSFORM_MODE_WRAP_REPEAT;
  }
  else {
    BLI_rctf_init(&crop_rect, 0.0, tile_buffer.x, 0.0, tile_buffer.y);
    crop_rect_ptr = &crop_rect;
    transform_mode = IMB_TRANSFORM_MODE_CROP_SRC;
  }

  IMB_transform(float_tile_buffer,
                &texture_buffer,
                transform_mode,
                IMB_FILTER_NEAREST,
                uv_to_texel,
                crop_rect_ptr);
}

void ScreenSpaceDrawingMode::begin_sync() const
{
  {
    DefaultTextureList *dtxl = DRW_context_get()->viewport_texture_list_get();
    instance_.state.depth_fb.ensure(GPU_ATTACHMENT_TEXTURE(dtxl->depth));
    instance_.state.color_fb.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(dtxl->color));
  }
  {
    PassSimple &pass = instance_.state.image_ps;
    pass.init();
    pass.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_ALWAYS | DRW_STATE_BLEND_ALPHA_PREMUL);
  }
  {
    PassSimple &pass = instance_.state.depth_ps;
    pass.init();
    pass.state_set(DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS_EQUAL);
  }
}

void ScreenSpaceDrawingMode::image_sync(blender::Image *image, ImageUser *iuser) const
{
  State &state = instance_.state;

  state.partial_update.ensure_image(image);
  state.clear_need_full_update_flag();

  /* Step: Find out which screen space textures are needed to draw on the screen. Recycle
   * textures that are not on screen anymore. */
  OneTexture method(&state);
  method.ensure_texture_infos();
  method.update_bounds(instance_.region);

  /* Step: Check for changes in the image user compared to the last time. */
  state.update_image_usage(iuser);

  /* Step: A display override brings its own notion of "changed": its pixels are composited from
   * several images, so the image's own partial-update log does not describe them.
   *
   * This is the one place per frame that resolves the override, and the rest of the frame reads
   * #State.has_display_override. The distinction matters: the space being *set* to show a
   * composite is not the same as one having been produced -- a material that is not a layer stack
   * falls back to the image, and the paths below must then treat it as an ordinary image again. */
  uint64_t override_revision = 0;
  rcti override_changed_region;
  BLI_rcti_init(&override_changed_region, 0, 0, 0, 0);
  ImBuf *override_buffer = instance_.space().has_display_override() ?
                               instance_.space().acquire_display_override_buffer(
                                   instance_.main(),
                                   &override_revision,
                                   &override_changed_region) :
                               nullptr;
  state.has_display_override = override_buffer != nullptr;
  /* Held until the textures have been filled from it, rather than released here and acquired again
   * per dirty texture slot: an acquisition is a full gather, and the pixels it produces are the
   * same ones the upload below needs. */
  BLI_SCOPED_DEFER([&]() {
    if (override_buffer != nullptr) {
      instance_.space().release_display_override_buffer(override_buffer);
    }
  });
  if (override_revision != state.display_override_revision) {
    state.display_override_revision = override_revision;

    /* A rectangle is only usable while the image-to-texture mapping is an axis-aligned rectangle
     * itself, which is the same restriction the image's own partial-update path works under. An
     * override that could not say what changed, or one that went away and left its pixels in the
     * textures, falls back to the full re-upload this always did -- a zero revision is what "no
     * override" reads as. */
    const bool can_update_part = !BLI_rcti_is_empty(&override_changed_region) &&
                                 override_buffer != nullptr && !state.flags.do_tile_drawing &&
                                 instance_.space().get_canvas_rotation() == 0.0f;
    if (can_update_part) {
      state.display_override_changed_region = override_changed_region;
    }
    else {
      BLI_rcti_init(&state.display_override_changed_region, 0, 0, 0, 0);
      state.mark_all_texture_slots_dirty();
      state.float_buffers.clear();
    }
  }

  /* Step: Update the GPU textures based on the changes in the image. */
  {
    /* Stage (d) of the Combined preview budget: a float RGBA canvas is four times the bytes of an
     * ordinary one, so the upload is worth measuring apart from the shading that produced it. */
    PAINT_CHANNEL_PERF_COMBINED_SCOPE(TextureUpload);
    method.ensure_gpu_textures_allocation();
    update_textures(image, iuser, override_buffer);
  }

  /* Step: Add the GPU textures to the shgroup. */
  state.update_batches();
  if (!state.flags.do_tile_drawing) {
    add_depth_shgroups(image, iuser);
  }
  add_shgroups();
}

void ScreenSpaceDrawingMode::draw_viewport() const
{
  float clear_depth = instance_.state.flags.do_tile_drawing ? 0.75 : 1.0f;
  GPU_framebuffer_bind(instance_.state.depth_fb);
  instance_.state.depth_fb.clear_depth(clear_depth);
  instance_.manager->submit(instance_.state.depth_ps, instance_.state.view);

  GPU_framebuffer_bind(instance_.state.color_fb);
  GPU_framebuffer_clear_color(instance_.state.color_fb, double4(0.0));
  instance_.manager->submit(instance_.state.image_ps, instance_.state.view);
}

}  // namespace blender::image_engine
