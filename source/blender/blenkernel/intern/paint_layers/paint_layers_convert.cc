/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Converting a foreign map into the maps the stack's channels read. The specular converter is the
 * one: a specular-glossiness texture set says "reflectance colour and glossiness", while the
 * channel it lands on feeds the Principled BSDF's "Specular IOR Level" -- a scalar multiplier, not
 * a colour -- so the map cannot be slotted as it is.
 */

#include <algorithm>
#include <memory>

#include "BKE_paint_layers.hh"

#include "BKE_image.hh"
#include "BKE_image_tile_buffer.hh"
#include "BKE_lib_id.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "BLI_math_base.h"
#include "BLI_math_vector.hh"
#include "BLI_string.h"

#include "DNA_image_types.h"
#include "DNA_material_types.h"

#include "paint_layers_intern.hh"

namespace blender {

namespace {

/** A stored byte level as the map's readers see it: the byte over 255, no colorspace in the
 * way. */
float specular_stored_level(const uint8_t byte)
{
  return float(byte) / 255.0f;
}

/** A level clamped into the byte a grey map stores it in. */
uint8_t specular_level_byte(const float level)
{
  return uint8_t(clamp_f(level, 0.0f, 1.0f) * 255.0f + 0.5f);
}

/**
 * An empty byte ImBuf of \a source's size, ready for the converted grey: four channels, as every
 * byte map of the stack is. Null when the allocation is refused.
 */
ImBuf *specular_convert_buf_alloc(const ImBuf &source)
{
  ImBuf *buf = IMB_allocImBuf(uint(source.x), uint(source.y), ImBufFlags::ByteData);
  if (buf != nullptr && buf->byte_data_for_write() == nullptr) {
    IMB_freeImBuf(buf);
    return nullptr;
  }
  return buf;
}

/** Name the converted map after its source, with \a suffix saying which one it is. */
void specular_convert_name(const Image &source, const char *suffix, char r_name[MAX_NAME])
{
  BLI_snprintf(r_name, MAX_NAME, "%s%s", source.id.name + 2, suffix);
}

/**
 * Turn the filled grey \a buf into an Image of \a bmain, read as the data map it is. The buffer's
 * one reference moves to the image, the way #BKE_image_add_from_imbuf's callers hand it over.
 */
Image *specular_convert_image_make(Main &bmain, ImBuf *buf, const char *name)
{
  IMB_colormanagement_assign_byte_colorspace(
      buf, IMB_colormanagement_role_colorspace_name_get(COLOR_ROLE_DATA));
  Image *image = BKE_image_add_from_imbuf(&bmain, buf, name);
  IMB_freeImBuf(buf);
  return image;
}

/**
 * An empty tiled Image in the data colorspace, its first tile sized like \a first: a tiled map
 * converts tile by tile, so the grey maps mirror its tiling rather than collapsing it into one
 * buffer, which would leave every other tile's content out.
 */
Image *specular_convert_image_tiled_make(Main &bmain, const ImBuf &first, const char *name)
{
  const float blank[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  Image *image = BKE_image_add_generated(&bmain,
                                         uint(first.x),
                                         uint(first.y),
                                         name,
                                         32,
                                         false,
                                         IMA_GENTYPE_BLANK,
                                         blank,
                                         false,
                                         /*is_data=*/true,
                                         /*tiled=*/true);
  return image;
}

/**
 * Put \a buf's grey into \a image's tile \a tile_number, adding the tile when the image does not
 * have it yet and generating it blank from \a buf's size first. False when the tile could not be
 * made or does not match the buffer.
 */
bool specular_convert_tile_write(Image &image, const int tile_number, const ImBuf &buf)
{
  /* The tile generates like the first one does, but at \a buf's size: a source whose tiles
   * differ in resolution still reads back faithfully. */
  if (!BKE_paint_layers_map_coverage_tile_ensure(image, tile_number, buf.x, buf.y)) {
    return false;
  }
  ImageUser tile_user;
  BKE_imageuser_default(&tile_user);
  tile_user.tile = tile_number;
  ImageTileBufferScope dst(image, &tile_user);
  if (dst == nullptr || dst->x != buf.x || dst->y != buf.y) {
    return false;
  }
  uchar *dst_pixels = dst->byte_data_for_write();
  if (dst_pixels == nullptr) {
    return false;
  }
  memcpy(dst_pixels, buf.byte_data(), size_t(buf.x) * size_t(buf.y) * 4);
  /* Painted pixels exist only in this buffer, so it has to count as unsaved work. */
  dst->userflags |= IB_DISPLAY_BUFFER_INVALID;
  BKE_image_mark_dirty(&image, dst.get());
  return true;
}

/** The channels a float buffer stores per pixel: zero stands for the four-channel default. */
int specular_source_channels(const ImBuf &source)
{
  return (source.channels > 0) ? source.channels : 4;
}

/** Convert one buffer's pixels into the grey the channels read: the specular level from the
 * colour, and -- when \a rough_buf is given -- the roughness from the alpha. Both outputs are byte
 * buffers of \a source's size, as every maker of a grey map fills them. */
void specular_convert_buf_fill(const ImBuf &source,
                               const PaintSpecularMapKind kind,
                               ImBuf *level_buf,
                               ImBuf *rough_buf)
{
  const size_t pixel_num = size_t(source.x) * size_t(source.y);
  uint8_t *level_out = level_buf->byte_data_for_write();
  uint8_t *rough_out = (rough_buf != nullptr) ? rough_buf->byte_data_for_write() : nullptr;
  const float *source_float = source.float_data();
  const uint8_t *source_byte = source.byte_data();
  const ColorSpace *source_colorspace = source.byte_buffer.colorspace;
  const int source_channels = (source_float != nullptr) ? specular_source_channels(source) : 4;

  for (size_t i = 0; i < pixel_num; i++) {
    /* The specular colour as scene linear, the way a colour map's reader decodes it; a level map
     * is data, so its stored value goes over as it is. */
    float3 color;
    float glossiness = 0.0f;
    if (source_float != nullptr) {
      const float *px = source_float + i * source_channels;
      if (source_channels == 1) {
        color = float3(px[0]);
      }
      else {
        color = float3(px[0], px[1], px[2]);
      }
      glossiness = (source_channels >= 4) ? px[3] : 0.0f;
    }
    else {
      const uint8_t *px = source_byte + i * 4;
      color = float3(specular_stored_level(px[0]),
                     specular_stored_level(px[1]),
                     specular_stored_level(px[2]));
      if (kind != PaintSpecularMapKind::Level && source_colorspace != nullptr) {
        IMB_colormanagement_colorspace_to_scene_linear_v3(color, source_colorspace);
      }
      glossiness = specular_stored_level(px[3]);
    }

    const float level = (kind == PaintSpecularMapKind::Level) ?
                            paint_layer_map_luminance(color) :
                            paint_layer_map_luminance(color) / SPECULAR_LEVEL_F0_FULL;
    const uint8_t level_byte = specular_level_byte(level);
    level_out[i * 4 + 0] = level_byte;
    level_out[i * 4 + 1] = level_byte;
    level_out[i * 4 + 2] = level_byte;
    level_out[i * 4 + 3] = 255;

    if (rough_out != nullptr) {
      const uint8_t rough_byte = specular_level_byte(1.0f - glossiness);
      rough_out[i * 4 + 0] = rough_byte;
      rough_out[i * 4 + 1] = rough_byte;
      rough_out[i * 4 + 2] = rough_byte;
      rough_out[i * 4 + 3] = 255;
    }
  }
}

/** Whether the source holds a colour to read and, separately, the alpha glossiness needs. */
struct SpecularSourceShape {
  bool has_color = false;
  bool has_alpha = false;
};

SpecularSourceShape specular_source_shape(const ImBuf &source)
{
  SpecularSourceShape shape;
  if (source.float_data() != nullptr) {
    /* A float buffer carries its channels in #ImBuf::channels; a byte one is always four. */
    const int channels = specular_source_channels(source);
    shape.has_color = channels >= 3 || channels == 1;
    shape.has_alpha = channels >= 4;
  }
  else if (source.byte_data() != nullptr) {
    shape.has_color = true;
    shape.has_alpha = true;
  }
  return shape;
}

/**
 * Whether the map's alpha is not one flat value: the one thing the no-bake spec-gloss set needs
 * the pixels for. A constant alpha carries no glossiness, so a roughness record pointed at it
 * would read the roughness flat -- the whole map at 1 - constant.
 */
bool specular_alpha_varies(const ImBuf &source)
{
  const size_t pixel_num = size_t(source.x) * size_t(source.y);
  if (source.float_data() != nullptr) {
    const int channels = specular_source_channels(source);
    if (channels < 4) {
      return false;
    }
    const float *px = source.float_data() + 3;
    float lo = px[0];
    float hi = px[0];
    for (size_t i = 0; i < pixel_num; i++) {
      const float a = px[i * channels];
      lo = std::min(lo, a);
      hi = std::max(hi, a);
    }
    return lo != hi;
  }
  if (source.byte_data() != nullptr) {
    const uint8_t *px = source.byte_data() + 3;
    uint8_t lo = px[0];
    uint8_t hi = px[0];
    for (size_t i = 0; i < pixel_num; i++) {
      const uint8_t a = px[i * 4];
      lo = std::min(lo, a);
      hi = std::max(hi, a);
    }
    return lo != hi;
  }
  return false;
}

/** Whether any buffer the map holds carries an alpha that is not one flat value; a tiled map's
 * glossiness may sit on any tile, so the walk goes over them all, and an ordinary map is its
 * one-buffer case. A tile without a buffer is skipped: nothing was stored in it that a reader
 * could see. */
bool specular_alpha_varies_map(Image &spec_map)
{
  bool varies = false;
  BKE_image_tile_buffers_foreach(spec_map, [&](const int /*tile_number*/, ImBuf &ibuf) {
    if (specular_alpha_varies(ibuf)) {
      varies = true;
      return false;
    }
    return true;
  });
  return varies;
}

/**
 * The no-bake spec-gloss set (#PaintSpecularMapKind::SpecGlossSet): the map keeps its pixels, the
 * Specular record reads it as the F0 colour with the glossiness in its alpha, and -- when \a
 * make_roughness is set -- the Roughness record reads `1 - alpha` of the same image, which then
 * carries a second user. No image is made and none deleted: undoing the two encodings turns the
 * set back into a plain map.
 */
bool specular_spec_gloss_set(Material &ma,
                             MaterialPaintLayer &layer,
                             Image &spec_map,
                             const bool make_roughness,
                             const char **r_reason)
{
  const auto refuse = [&](const char *reason) {
    if (r_reason != nullptr) {
      *r_reason = reason;
    }
    return false;
  };
  /* The map first: nothing is created or changed for one that cannot be used. */
  void *source_lock = nullptr;
  ImBuf *source = BKE_image_acquire_ibuf(&spec_map, nullptr, &source_lock);
  if (source == nullptr || source->x <= 0 || source->y <= 0) {
    BKE_image_release_ibuf(&spec_map, source, source_lock);
    return refuse("The map cannot be read");
  }
  const SpecularSourceShape shape = specular_source_shape(*source);
  bool alpha_varies = false;
  if (shape.has_alpha && make_roughness) {
    /* A tiled map's glossiness may sit on any tile, so the walk goes over them all. */
    alpha_varies = specular_alpha_varies_map(spec_map);
  }
  BKE_image_release_ibuf(&spec_map, source, source_lock);
  if (!shape.has_color) {
    return refuse("The map has no colour to read as the specular");
  }
  if (make_roughness && !shape.has_alpha) {
    return refuse("The map has no alpha to read the glossiness from");
  }
  if (make_roughness && !alpha_varies) {
    return refuse("The map's alpha is constant, so it carries no glossiness to read");
  }
  /* The two records have to read the same image: the Specular half reads the colour, the Roughness
   * half the alpha of it, so a record pointing elsewhere would make the pair lie. */
  MaterialPaintLayerChannel *spec_record = paint_layer_channel_find(
      layer, PAINT_MATERIAL_CHANNEL_SPECULAR);
  if (spec_record == nullptr || spec_record->image != &spec_map) {
    return refuse("The row's Specular record does not read this map");
  }
  if (make_roughness) {
    const MaterialPaintLayerChannel *rough_record = paint_layer_channel_find(
        layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
    if (rough_record != nullptr && rough_record->image != nullptr) {
      return refuse("The row already has a roughness map; remove it first");
    }
  }

  /* The Specular half: the map stays as it is, the record reads its colour as the F0. */
  const eMaterialPaintLayerMapEncoding spec_previous = paint_layer_map_encoding_get(*spec_record);
  if (!BKE_paint_layers_channel_set_map_encoding(ma,
                                                 &layer,
                                                 PAINT_MATERIAL_CHANNEL_SPECULAR,
                                                 MA_PAINT_LAYER_MAP_ENCODING_F0_GLOSSINESS_ALPHA))
  {
    return refuse("The Specular record refuses the encoding");
  }
  if (!make_roughness) {
    return true;
  }
  /* The Roughness half: the same image under the roughness record, reading its alpha -- the
   * second user keeps the map alive while either record points at it. */
  if (BKE_paint_layers_channel_add(ma, &layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS) == nullptr ||
      !BKE_paint_layers_channel_set_image(
          ma, &layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS, &spec_map) ||
      !BKE_paint_layers_channel_set_map_encoding(ma,
                                                 &layer,
                                                 PAINT_MATERIAL_CHANNEL_ROUGHNESS,
                                                 MA_PAINT_LAYER_MAP_ENCODING_GLOSSINESS_ALPHA))
  {
    /* Nothing assigned, as promised: the Specular half goes back to what it read. */
    BKE_paint_layers_channel_set_image(ma, &layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS, nullptr);
    BKE_paint_layers_channel_set_map_encoding(
        ma, &layer, PAINT_MATERIAL_CHANNEL_SPECULAR, spec_previous);
    return refuse("The roughness record refused the map");
  }
  return true;
}

/** An ImBuf owned for a scope: the tile buffers of the conversion walk die with it, so the walk
 * frees nothing by hand. */
struct ImBufDeleter {
  void operator()(ImBuf *buf) const
  {
    IMB_freeImBuf(buf);
  }
};
using ImBufHandle = std::unique_ptr<ImBuf, ImBufDeleter>;

/** A half conversion must not read as one: the maps made so far go away again. */
void specular_convert_discard(Main &bmain, Image *level_image, Image *rough_image)
{
  if (level_image != nullptr) {
    BKE_id_delete(&bmain, &level_image->id);
  }
  if (rough_image != nullptr) {
    BKE_id_delete(&bmain, &rough_image->id);
  }
}

}  // namespace

bool BKE_paint_layers_specular_map_convert(Main &bmain,
                                           Material &ma,
                                           MaterialPaintLayer &layer,
                                           Image &spec_map,
                                           const PaintSpecularMapKind kind,
                                           const bool make_roughness,
                                           Image **r_level_map,
                                           Image **r_rough_map,
                                           const char **r_reason)
{
  if (r_level_map != nullptr) {
    *r_level_map = nullptr;
  }
  if (r_rough_map != nullptr) {
    *r_rough_map = nullptr;
  }
  if (r_reason != nullptr) {
    *r_reason = nullptr;
  }
  /* Every refusal names itself, the way the no-bake set's does: the caller's report shows the
   * reason rather than a bare cancellation. */
  const auto refuse = [&](const char *reason) {
    if (r_reason != nullptr) {
      *r_reason = reason;
    }
    return false;
  };
  /* A Fill row of the stack and a Fill correction hanging off one both take maps of their own. */
  if (!paint_layer_in_stack(ma, &layer) &&
      BKE_paint_layers_correction_owner(ma, layer) == nullptr)
  {
    return refuse("The row is not part of the material's stack");
  }
  /* The same rule the drop gate applies: only a Fill row takes maps of its own. */
  if (BKE_paint_layers_row_image_map_issue(layer) != PaintLayerImageMapIssue::None) {
    return refuse("Only a Fill row takes maps of its own");
  }
  /* The no-bake set: the map keeps its pixels, the two records read it where it lies. */
  if (kind == PaintSpecularMapKind::SpecGlossSet) {
    return specular_spec_gloss_set(ma, layer, spec_map, make_roughness, r_reason);
  }
  /* Glossiness exists only in the specular-glossiness packing; every other kind has none to
   * invert, and a record asked for anyway would read as the map lying about its alpha. */
  const bool want_roughness = make_roughness && kind == PaintSpecularMapKind::ColorGlossiness;
  /* Non-destructive: the roughness map the row already carries is not replaced; the caller
   * removes it first. The Specular map being converted is the record's own, so replacing it is
   * the point. */
  if (want_roughness) {
    const MaterialPaintLayerChannel *rough_record = paint_layer_channel_find(
        layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
    if (rough_record != nullptr && rough_record->image != nullptr) {
      return refuse("The row already has a roughness map; remove it first");
    }
  }
  /* What the Specular record points at now, so a refusal in the roughness half can put it back. */
  MaterialPaintLayerChannel *spec_record = paint_layer_channel_find(
      layer, PAINT_MATERIAL_CHANNEL_SPECULAR);
  Image *spec_previous = (spec_record != nullptr) ? spec_record->image : nullptr;

  void *source_lock = nullptr;
  ImBuf *source = BKE_image_acquire_ibuf(&spec_map, nullptr, &source_lock);
  if (source == nullptr || source->x <= 0 || source->y <= 0) {
    BKE_image_release_ibuf(&spec_map, source, source_lock);
    return refuse("The map cannot be read");
  }
  const SpecularSourceShape shape = specular_source_shape(*source);
  if (!shape.has_color) {
    BKE_image_release_ibuf(&spec_map, source, source_lock);
    return refuse("The map has no colour to read as the specular");
  }
  if (want_roughness && !shape.has_alpha) {
    BKE_image_release_ibuf(&spec_map, source, source_lock);
    return refuse("The map has no alpha to read the glossiness from");
  }

  /* The grey maps mirror the source's tiling: a tiled map converts tile by tile, so every tile's
   * pixels land in the grey map's own tile, and an ordinary map is the one-tile list of the same
   * walk, its greys landing in the plain generated images. The per-tile buffers die with their
   * scope, so neither walk holds a second tile's pixels nor frees anything by hand. */
  const bool tiled = spec_map.source == IMA_SRC_TILED;
  Image *level_image = nullptr;
  Image *rough_image = nullptr;
  char name[MAX_NAME];
  if (tiled) {
    specular_convert_name(spec_map, " Specular", name);
    level_image = specular_convert_image_tiled_make(bmain, *source, name);
    if (want_roughness) {
      specular_convert_name(spec_map, " Roughness", name);
      rough_image = specular_convert_image_tiled_make(bmain, *source, name);
    }
    if (level_image == nullptr || (want_roughness && rough_image == nullptr)) {
      BKE_image_release_ibuf(&spec_map, source, source_lock);
      specular_convert_discard(bmain, level_image, rough_image);
      return refuse("The converted maps could not be created");
    }
  }

  /* One walk over the source's tiles: a tiled map over its own, an ordinary map over the single
   * buffer acquired above. A tile that cannot be read or written would silently drop that tile's
   * specular, so the conversion is refused whole, the way any other half-done result is. */
  const char *fail = nullptr;
  const auto convert_tile = [&](const int tile_number, const ImBuf &tile_buf) {
    if (tile_buf.x <= 0 || tile_buf.y <= 0) {
      fail = "A tile of the map cannot be read";
      return false;
    }
    ImBufHandle level_buf(specular_convert_buf_alloc(tile_buf));
    ImBufHandle rough_buf(want_roughness ? specular_convert_buf_alloc(tile_buf) : nullptr);
    if (level_buf == nullptr || (want_roughness && rough_buf == nullptr)) {
      fail = "The converted maps could not be allocated";
      return false;
    }
    specular_convert_buf_fill(tile_buf, kind, level_buf.get(), rough_buf.get());
    if (tiled) {
      if (!specular_convert_tile_write(*level_image, tile_number, *level_buf) ||
          (rough_buf != nullptr &&
           !specular_convert_tile_write(*rough_image, tile_number, *rough_buf)))
      {
        fail = "A tile of the converted maps could not be written";
        return false;
      }
      return true;
    }
    /* The one tile of an ordinary map becomes the plain image whole, buffer and all. */
    specular_convert_name(spec_map, " Specular", name);
    level_image = specular_convert_image_make(bmain, level_buf.release(), name);
    if (rough_buf != nullptr) {
      specular_convert_name(spec_map, " Roughness", name);
      rough_image = specular_convert_image_make(bmain, rough_buf.release(), name);
    }
    if (level_image == nullptr || (want_roughness && rough_image == nullptr)) {
      fail = "The converted maps could not be created";
      return false;
    }
    return true;
  };
  if (tiled) {
    for (const ImageTile &tile : spec_map.tiles) {
      ImageUser tile_user;
      BKE_imageuser_default(&tile_user);
      tile_user.tile = tile.tile_number;
      ImageTileBufferScope tile_buf(spec_map, &tile_user);
      if (tile_buf == nullptr || !convert_tile(tile.tile_number, *tile_buf)) {
        break;
      }
    }
  }
  else {
    const ImageTile *only = static_cast<const ImageTile *>(spec_map.tiles.first);
    convert_tile((only != nullptr) ? only->tile_number : PAINT_TILE_NUMBER_BASE, *source);
  }
  BKE_image_release_ibuf(&spec_map, source, source_lock);
  if (fail != nullptr) {
    specular_convert_discard(bmain, level_image, rough_image);
    return refuse(fail);
  }

  if (BKE_paint_layers_channel_add(ma, &layer, PAINT_MATERIAL_CHANNEL_SPECULAR) == nullptr ||
      !BKE_paint_layers_channel_set_image(
          ma, &layer, PAINT_MATERIAL_CHANNEL_SPECULAR, level_image))
  {
    BKE_id_delete(&bmain, &level_image->id);
    if (rough_image != nullptr) {
      BKE_id_delete(&bmain, &rough_image->id);
    }
    return refuse("The Specular record refused the map");
  }
  if (rough_image != nullptr) {
    if (BKE_paint_layers_channel_add(ma, &layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS) == nullptr ||
        !BKE_paint_layers_channel_set_image(
            ma, &layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS, rough_image))
    {
      /* Nothing assigned, as promised: the Specular record goes back to what it pointed at. */
      BKE_paint_layers_channel_set_image(
          ma, &layer, PAINT_MATERIAL_CHANNEL_SPECULAR, spec_previous);
      BKE_id_delete(&bmain, &rough_image->id);
      BKE_id_delete(&bmain, &level_image->id);
      return refuse("The roughness record refused the map");
    }
  }

  if (r_level_map != nullptr) {
    *r_level_map = level_image;
  }
  if (r_rough_map != nullptr) {
    *r_rough_map = rough_image;
  }
  return true;
}

}  // namespace blender
