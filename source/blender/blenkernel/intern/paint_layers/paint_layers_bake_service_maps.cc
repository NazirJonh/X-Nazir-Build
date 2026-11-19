/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The service maps of a row's bake, extracted from `paint_layers_bake.cc`: the maps the generator
 * and the CPU substitute write into (#bake_service_image), the channel's float-or-byte choice, and
 * the pixel writers that store a rendered value into a map. The planner and the worker job reach
 * them through `paint_layers_bake_intern.hh`.
 */

#include <cstring>

#include "BLI_math_base.h"
#include "BLI_rect.h"
#include "BLI_string.h"
#include "BLI_vector.hh"

#include "BKE_image.hh"
#include "BKE_image_partial_update.hh"
#include "BKE_lib_id.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_target.hh"
#include "BKE_paint_layers_tile.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf_types.hh"

#include "paint_layers_bake_intern.hh"
#include "paint_layers_intern.hh"
#include "paint_layers_kind.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Service maps
 * \{ */

/** The service map of \a layer: its \a channel bake, or the coverage one for a negative channel. */
static Image **bake_service_slot(MaterialPaintLayer &layer, const int channel)
{
  return (channel >= 0) ? &layer.bake->images[channel] : &layer.bake->coverage;
}

/** Whether \a image's generated tile holds a float buffer rather than a byte one. */
static bool bake_image_is_float(Image &image)
{
  const ImageTile *tile = BKE_image_get_tile(&image, 0);
  return tile != nullptr && (tile->gen_flag & IMA_GEN_FLOAT) != 0;
}

/**
 * Whether a bake map of \a layer in \a info's channel is kept as float.
 *
 * A generated row qualifies: its pixels are a formula the shader evaluates in float, so a scalar
 * channel stored as bytes reads back as terraces. Height qualifies everywhere: it is a continuous
 * field, and its painted map is a float buffer too (see
 * #BKE_paint_layers_target_ensure_writable), so a byte bake would leave the merged correction in a
 * different format from the row it replaces. Every other row bakes a map it already holds (or a
 * flat value), which is a byte texture like the maps around it.
 */
bool bake_channel_needs_float(const MaterialPaintLayer &layer, const MaterialPaintChannelInfo &info)
{
  if (info.map_float) {
    return true;
  }
  return info.bake_float && paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_GENERATES);
}

/**
 * The map of \a layer in \a channel at \a size, made when it is missing or of another size or
 * format.
 *
 * \param floatbuf: keep the values as float. A map of the wrong format cannot be reused: the pixel
 *                  writers index one buffer or the other, and a byte map read as float (or back)
 *                  would be a different picture.
 */
Image *bake_service_image(Main &bmain,
                          MaterialPaintLayer &layer,
                          const int channel,
                          const int size,
                          const bool is_color,
                          const bool floatbuf,
                          const int object_num,
                          const Span<int> tile_numbers,
                          const int height)
{
  const bool tiled = object_num > 0;
  /* The shape the map is made at: a square `size` for every caller that asks no height, the
   * caller's canvas for a non-square one (a canvas Material row's). */
  const int map_height = (height > 0) ? height : size;
  /* The tiles a tiled map has to hold: the blocks' tiles, or one per object in the single-tile
   * scheme. The first one is created by the image itself and is always 1001 (the first block sits
   * at the origin of the map). */
  const auto tile_number_at = [&](const int index) {
    return tile_numbers.is_empty() ? PAINT_TILE_NUMBER_BASE + index : tile_numbers[index];
  };
  const int tile_num = tile_numbers.is_empty() ? object_num : int(tile_numbers.size());
  Image **slot = bake_service_slot(layer, channel);
  if (*slot != nullptr) {
    int slot_width = 0;
    int slot_height = 0;
    BKE_image_get_size(*slot, nullptr, &slot_width, &slot_height);
    const bool tiled_ok = ((*slot)->source == IMA_SRC_TILED) == tiled;
    if (slot_width == size && slot_height == map_height &&
        bake_image_is_float(**slot) == floatbuf && tiled_ok)
    {
      if (!tiled) {
        return *slot;
      }
      /* A precreated per-object map has tile 1001 only: grow it to the number of tiles the bake
       * resolved instead of throwing the canvas away. */
      bool all_tiles = true;
      for (int index = 0; index < tile_num; index++) {
        if (!BKE_paint_layers_map_coverage_tile_ensure(**slot, tile_number_at(index))) {
          all_tiles = false;
          break;
        }
      }
      if (all_tiles) {
        return *slot;
      }
    }
    /* The map was created at another size (the row's `bake_size` changed) or in another format (a
     * generated row's channel moved to float): the pixel writers index the render buffer by the
     * image's own width, so neither can be reused. The creation reference is the bake's alone, so
     * the old map goes with it. */
    Image *stale = *slot;
    *slot = nullptr;
    if (stale->id.us > 0) {
      id_us_min(&stale->id);
    }
    if (stale->id.us == 0 && ID_FAKE_USERS(&stale->id) == 0) {
      BKE_id_free(&bmain, &stale->id);
    }
  }
  char name[192];
  if (channel >= 0) {
    const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
        eMaterialPaintChannel(channel));
    SNPRINTF(name, "%s %s Bake", layer.name[0] != '\0' ? layer.name : "Layer", info.ui_name);
  }
  else {
    SNPRINTF(name, "%s Coverage Bake", layer.name[0] != '\0' ? layer.name : "Layer");
  }
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  /* The creation reference is the one the row holds: unlike #BKE_paint_layers_bake_set_map this
   * takes no user of its own. */
  Image *image = BKE_image_add_generated(&bmain,
                                         size,
                                         map_height,
                                         name,
                                         32,
                                         floatbuf,
                                         IMA_GENTYPE_BLANK,
                                         black,
                                         false,
                                         !is_color,
                                         tiled);
  if (image != nullptr) {
    image->alpha_mode = IMA_ALPHA_STRAIGHT;
    if (tiled) {
      /* The first tile is created by the image itself; the rest carry the same canvas and format,
       * because a UDIM image requires every tile to have the same resolution. */
      for (int index = 0; index < tile_num; index++) {
        if (tile_number_at(index) != PAINT_TILE_NUMBER_BASE) {
          BKE_paint_layers_map_coverage_tile_ensure(*image, tile_number_at(index));
        }
      }
    }
    /* A bake is the stack's own read of a row, so the browsers hide it like every other map. */
    BKE_paint_layers_image_mark_internal(*image);
  }
  *slot = image;
  return image;
}

/**
 * Tell the image system that \a ibuf's pixels were rewritten behind its back. Without this a map
 * that already reached the GPU keeps showing its previous bake, since nothing else invalidates the
 * texture.
 *
 * The invalidation stays at the tile the write landed in: the region walk marks that tile's rect
 * and the GPU texture of it is refreshed in place, so a bake that fills its tiles one object at a
 * time does not drop every other tile's texture along the way. A tile the map does not know (a
 * write through the base buffer of a non-tiled map that lost its first tile) falls back to the
 * whole-image invalidation.
 */
void bake_image_pixels_changed(Image &image,
                               ImBuf &ibuf,
                               const int tile_number,
                               const int *rect)
{
  ibuf.userflags |= IB_DISPLAY_BUFFER_INVALID;
  BKE_image_mark_dirty(&image, &ibuf);
  ImageTile *tile = BKE_image_get_tile(&image, tile_number);
  if (tile != nullptr) {
    /* Only the rows and columns a partial write touched are marked, clamped to the buffer the way
     * the writers clamp them. */
    const int x0 = rect != nullptr ? clamp_i(rect[0], 0, ibuf.x) : 0;
    const int x1 = rect != nullptr ? clamp_i(rect[2], 0, ibuf.x) : ibuf.x;
    const int y0 = rect != nullptr ? clamp_i(rect[1], 0, ibuf.y) : 0;
    const int y1 = rect != nullptr ? clamp_i(rect[3], 0, ibuf.y) : ibuf.y;
    if (x1 <= x0 || y1 <= y0) {
      return;
    }
    rcti region;
    BLI_rcti_init(&region, x0, x1, y0, y1);
    BKE_image_partial_update_mark_region(&image, tile, &ibuf, &region);
    BKE_image_update_gputexture_delayed(&image, tile, &ibuf, x0, y0, x1 - x0, y1 - y0);
  }
  else {
    BKE_image_partial_update_mark_full_update(&image);
    BKE_image_free_gputextures(&image);
  }
}



/**
 * The float map's storage of one straight-RGBA value: premultiplied by its own alpha, which is what
 * the composite and the renderer both read.
 */
void bake_pixel_store_float(const float rgba[4], float r_store[4])
{
  const float alpha = clamp_f(rgba[3], 0.0f, 1.0f);
  r_store[0] = rgba[0] * alpha;
  r_store[1] = rgba[1] * alpha;
  r_store[2] = rgba[2] * alpha;
  r_store[3] = alpha;
}

/**
 * The byte map's storage of one straight-RGBA value: the colour encoded into the map's colorspace
 * and quantized, the alpha clamped. Only a colour channel is encoded; a data channel keeps the
 * scene-linear value quantized as it came.
 */
void bake_pixel_store_byte(const float rgba[4],
                           const bool is_color,
                           const ColorSpace *colorspace,
                           uchar r_store[4])
{
  float encoded[4] = {rgba[0], rgba[1], rgba[2], clamp_f(rgba[3], 0.0f, 1.0f)};
  if (is_color && colorspace != nullptr) {
    IMB_colormanagement_scene_linear_to_colorspace_v3(encoded, colorspace);
  }
  r_store[0] = uchar(clamp_i(int(encoded[0] * 255.0f + 0.5f), 0, 255));
  r_store[1] = uchar(clamp_i(int(encoded[1] * 255.0f + 0.5f), 0, 255));
  r_store[2] = uchar(clamp_i(int(encoded[2] * 255.0f + 0.5f), 0, 255));
  r_store[3] = uchar(clamp_i(int(encoded[3] * 255.0f + 0.5f), 0, 255));
}

/**
 * Write \a image, or only \a rect (x0, y0, x1, y1 in image pixels) when given, from a source that
 * fills one row at a time: \a fill_row is called per row with the row's \a y and the buffer's
 * width, and fills \a width straight RGBA floats into \a r_row. A row scratch is all the source
 * needs -- a map's width wide, nothing next to the map itself -- so a source that is sampled as it
 * is written never holds a second copy of the whole picture.
 *
 * The map's own buffer decides the format: a byte map stores the value encoded into its colorspace,
 * a float map stores it as it came, which is what a precise channel of a generated row is for.
 * Shared with the bake tests, which hold the worker's stored-buffer write to this same encoding.
 */
void bake_write_image_rows(
    Image &image,
    const bool is_color,
    const int *rect,
    const int tile_number,
    const FunctionRef<void(const int y, const int width, float *r_row)> fill_row)
{
  ImageUser iuser{};
  iuser.tile = tile_number;
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(&image, tile_number > 0 ? &iuser : nullptr, &lock);
  if (ibuf == nullptr) {
    return;
  }
  const int width = ibuf->x;
  const int x0 = rect != nullptr ? clamp_i(rect[0], 0, width) : 0;
  const int x1 = rect != nullptr ? clamp_i(rect[2], 0, width) : width;
  const int y0 = rect != nullptr ? clamp_i(rect[1], 0, ibuf->y) : 0;
  const int y1 = rect != nullptr ? clamp_i(rect[3], 0, ibuf->y) : ibuf->y;
  /* The buffer that exists decides, which is the rule `composite_decode_tile` reads a map by: a
   * byte buffer wins over a float one there, so writing the other here would leave the map showing
   * one picture to the renderer and another to the composite. */
  float *floats = (ibuf->byte_buffer.data == nullptr && ibuf->float_buffer.data != nullptr) ?
                      ibuf->float_data_for_write() :
                      nullptr;
  if (floats != nullptr) {
    /* A float buffer is premultiplied -- the composite and the renderer both read it that way -- so
     * the straight colour it is given is stored multiplied by the alpha that travels with it. */
    Vector<float> row(int64_t(width) * 4);
    for (int y = y0; y < y1; y++) {
      fill_row(y, width, row.data());
      for (int x = x0; x < x1; x++) {
        const int64_t i = int64_t(y) * width + x;
        bake_pixel_store_float(&row[x * 4], &floats[i * 4]);
      }
    }
  }
  else if (uchar *pixels = ibuf->byte_data_for_write()) {
    const auto *colorspace = ibuf->byte_buffer.colorspace;
    Vector<float> row(int64_t(width) * 4);
    for (int y = y0; y < y1; y++) {
      fill_row(y, width, row.data());
      for (int x = x0; x < x1; x++) {
        const int64_t i = int64_t(y) * width + x;
        bake_pixel_store_byte(&row[x * 4], is_color, colorspace, &pixels[i * 4]);
      }
    }
  }
  bake_image_pixels_changed(image, *ibuf, tile_number, rect);
  BKE_image_release_ibuf(&image, ibuf, lock);
}

/**
 * Write \a values into \a image, or only into \a rect (x0, y0, x1, y1 in image pixels) when given.
 *
 * The map's own buffer decides the format: a byte map stores the value encoded into its colorspace,
 * a float map stores it as it came, which is what a precise channel of a generated row is for.
 * Shared with the bake tests, which hold the worker's stored-buffer write to this same encoding.
 */
void bake_write_image(Image &image,
                      const float *values,
                      const bool is_color,
                      const int *rect,
                      const int tile_number /* = 0 */)
{
  bake_write_image_rows(image, is_color, rect, tile_number,
                        [&](const int y, const int width, float *r_row) {
    memcpy(r_row, values + int64_t(y) * width * 4, sizeof(float[4]) * size_t(width));
  });
}

/**
 * Write a stored buffer -- the per-pixel format #bake_pixel_store_float or #bake_pixel_store_byte
 * produce, which a worker job holds instead of its straight-RGBA render -- into \a image, or only
 * into \a rect. No conversion happens here: the values are already the map's own.
 */
void bake_write_stored_image(Image &image,
                             const bool stored_float,
                             const void *stored,
                             const int *rect,
                             const int tile_number)
{
  ImageUser iuser{};
  iuser.tile = tile_number;
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(&image, tile_number > 0 ? &iuser : nullptr, &lock);
  if (ibuf == nullptr) {
    return;
  }
  const int width = ibuf->x;
  const int x0 = rect != nullptr ? clamp_i(rect[0], 0, width) : 0;
  const int x1 = rect != nullptr ? clamp_i(rect[2], 0, width) : width;
  const int y0 = rect != nullptr ? clamp_i(rect[1], 0, ibuf->y) : 0;
  const int y1 = rect != nullptr ? clamp_i(rect[3], 0, ibuf->y) : ibuf->y;
  if (stored_float) {
    if (float *floats = (ibuf->byte_buffer.data == nullptr && ibuf->float_buffer.data != nullptr) ?
                            ibuf->float_data_for_write() :
                            nullptr)
    {
      const float *values = static_cast<const float *>(stored);
      for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
          const int64_t i = int64_t(y) * width + x;
          floats[i * 4 + 0] = values[i * 4 + 0];
          floats[i * 4 + 1] = values[i * 4 + 1];
          floats[i * 4 + 2] = values[i * 4 + 2];
          floats[i * 4 + 3] = values[i * 4 + 3];
        }
      }
    }
  }
  else if (uchar *pixels = ibuf->byte_data_for_write()) {
    const uchar *values = static_cast<const uchar *>(stored);
    for (int y = y0; y < y1; y++) {
      for (int x = x0; x < x1; x++) {
        const int64_t i = int64_t(y) * width + x;
        pixels[i * 4 + 0] = values[i * 4 + 0];
        pixels[i * 4 + 1] = values[i * 4 + 1];
        pixels[i * 4 + 2] = values[i * 4 + 2];
        pixels[i * 4 + 3] = values[i * 4 + 3];
      }
    }
  }
  bake_image_pixels_changed(image, *ibuf, tile_number, rect);
  BKE_image_release_ibuf(&image, ibuf, lock);
}

/** The coverage map keeps its grey in all three colour channels and is opaque. */
void bake_write_coverage_image(Image &image,
                               const float *coverage,
                               const int *rect,
                               const int tile_number /* = 0 */)
{
  ImageUser iuser{};
  iuser.tile = tile_number;
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(&image, tile_number > 0 ? &iuser : nullptr, &lock);
  if (ibuf == nullptr) {
    return;
  }
  uchar *pixels = ibuf->byte_data_for_write();
  const int width = ibuf->x;
  const int x0 = rect != nullptr ? clamp_i(rect[0], 0, width) : 0;
  const int x1 = rect != nullptr ? clamp_i(rect[2], 0, width) : width;
  const int y0 = rect != nullptr ? clamp_i(rect[1], 0, ibuf->y) : 0;
  const int y1 = rect != nullptr ? clamp_i(rect[3], 0, ibuf->y) : ibuf->y;
  if (pixels != nullptr) {
    for (int y = y0; y < y1; y++) {
      for (int x = x0; x < x1; x++) {
        const int64_t i = int64_t(y) * width + x;
        const uchar gray = uchar(
            clamp_i(int(clamp_f(coverage[i], 0.0f, 1.0f) * 255.0f + 0.5f), 0, 255));
        pixels[i * 4 + 0] = gray;
        pixels[i * 4 + 1] = gray;
        pixels[i * 4 + 2] = gray;
        pixels[i * 4 + 3] = 255;
      }
    }
  }
  bake_image_pixels_changed(image, *ibuf, tile_number, rect);
  BKE_image_release_ibuf(&image, ibuf, lock);
}

/** \} */

}  // namespace blender
