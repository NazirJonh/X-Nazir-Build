/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * See #BKE_paint_layers_target.hh: the paint target of a layered material, and growing the
 * description on the first stroke into a channel the active row has no map for yet.
 */

#include "BKE_paint_layers_target.hh"

#include "BLI_index_range.hh"
#include "BLI_listbase_iterator.hh"
#include "BLI_math_base.h"
#include "BLI_math_vector.h"
#include "BLI_string.h"

#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_material.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_object_mask.hh"
#include "BKE_paint_material_composite.hh"

#include "paint_layers_intern.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

namespace blender {

/** Encode a scene-linear colour into \a image's stored colorspace; data colorspaces stay as is. */
static void paint_layers_image_encode(const Image &image, float rgba[4])
{
  if (image.colorspace_settings.name[0] == '\0') {
    return;
  }
  const ColorSpace *colorspace = IMB_colormanagement_space_get_named(image.colorspace_settings.name);
  if (!BKE_paint_material_composite_colorspace_is_converted(colorspace)) {
    return;
  }
  IMB_colormanagement_scene_linear_to_colorspace(rgba, 1, 1, 4, colorspace);
}

/** Write one solid colour into \a image's byte buffer, encoded into the image's colorspace. */
static void paint_layers_image_fill(Image &image, const bool is_color, const float rgba[4])
{
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(&image, nullptr, &lock);
  if (ibuf == nullptr) {
    return;
  }
  uchar *pixels = ibuf->byte_data_for_write();
  if (pixels != nullptr) {
    float encoded[4];
    copy_v4_v4(encoded, rgba);
    if (is_color) {
      /* The constant is scene linear; the byte buffer stores the map's colorspace. */
      paint_layers_image_encode(image, encoded);
    }
    const uchar r = uchar(clamp_f(encoded[0], 0.0f, 1.0f) * 255.0f + 0.5f);
    const uchar g = uchar(clamp_f(encoded[1], 0.0f, 1.0f) * 255.0f + 0.5f);
    const uchar b = uchar(clamp_f(encoded[2], 0.0f, 1.0f) * 255.0f + 0.5f);
    const uchar a = uchar(clamp_f(encoded[3], 0.0f, 1.0f) * 255.0f + 0.5f);
    for (const int64_t i : IndexRange(int64_t(ibuf->x) * ibuf->y)) {
      pixels[i * 4 + 0] = r;
      pixels[i * 4 + 1] = g;
      pixels[i * 4 + 2] = b;
      pixels[i * 4 + 3] = a;
    }
  }
  BKE_image_release_ibuf(&image, ibuf, lock);
}

static Image *paint_layers_map_create(Main &bmain,
                                      const char *name,
                                      const int size,
                                      const bool is_color,
                                      const float rgba[4],
                                      const bool tiled = false)
{
  /* 32 planes, not 24: the buffer's color mode decides how many channels a PNG pack writes, and a
   * map's alpha is its coverage, so a 24-plane map would reopen fully opaque. */
  Image *image = BKE_image_add_generated(
      &bmain, size, size, name, 32, false, IMA_GENTYPE_BLANK, rgba, false, !is_color, tiled);
  if (image == nullptr) {
    return nullptr;
  }
  /* One convention for every paint-layer map: the byte buffer stays straight, so the generated
   * graph's Image Texture alpha is the map's own coverage and the CPU composite reads it unchanged. */
  image->alpha_mode = IMA_ALPHA_STRAIGHT;
  image->flag |= IMA_PAINT_CANVAS;
  paint_layers_image_fill(*image, is_color, rgba);
  return image;
}

bool BKE_paint_layers_target_get(Material &ma, const int channel, PaintLayersTarget &r_target)
{
  return BKE_paint_layers_target_get(ma, channel, PaintLayersTargetMode::Content, r_target);
}

PaintLayersTargetMode BKE_paint_layers_target_mode(const PaintModeSettings *settings)
{
  return (settings != nullptr && settings->layer_target_mode == PAINT_LAYER_TARGET_MASK) ?
             PaintLayersTargetMode::Mask :
             PaintLayersTargetMode::Content;
}

bool BKE_paint_layers_target_get(Material &ma,
                                 const int channel,
                                 const PaintLayersTargetMode mode,
                                 PaintLayersTarget &r_target)
{
  return BKE_paint_layers_target_resolve(ma, channel, mode, r_target) ==
         PaintLayersTargetStatus::Ok;
}

PaintLayersTargetStatus BKE_paint_layers_target_resolve(Material &ma,
                                                        const int channel,
                                                        const PaintLayersTargetMode mode,
                                                        PaintLayersTarget &r_target)
{
  if (!BKE_paint_layers_is_layered(ma)) {
    return PaintLayersTargetStatus::NotLayered;
  }
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
    return PaintLayersTargetStatus::InvalidChannel;
  }
  MaterialPaintLayer *layer = BKE_paint_layers_active_layer_get(ma);
  if (layer == nullptr) {
    return PaintLayersTargetStatus::NoActiveRow;
  }
  r_target = {};
  r_target.material = &ma;
  r_target.layer = layer;
  r_target.channel = channel;
  r_target.mode = mode;
  if (mode == PaintLayersTargetMode::Content) {
    /* A filter's records only say which channels it works in: there is no map to write into, and
     * a stroke must not make one. The mask of its row is still paintable. */
    if (paint_layer_effect_is_filter(*layer)) {
      return PaintLayersTargetStatus::RowTakesNoStrokes;
    }
    /* Likewise a row that reads a material: it owns no map for a stroke to write into. */
    if (paint_layer_kind_has_flag(*layer, PAINT_LAYER_KIND_READS_SOURCE) ||
        paint_layer_kind_reads_anchor_point(*layer) || paint_layer_kind_is_anchor_point_marker(*layer))
    {
      return PaintLayersTargetStatus::RowTakesNoStrokes;
    }
    /* Cannot paint into a channel that is not in the material's channel set. */
    if (!BKE_paint_layers_channel_in_set(ma, eMaterialPaintChannel(channel))) {
      return PaintLayersTargetStatus::OutsideChannelSet;
    }
    r_target.channel_record = paint_layer_channel_find(*layer, channel);
    return PaintLayersTargetStatus::Ok;
  }
  /* A mask is one grey map in the item's Base Color record: a stroke's other channels have nothing
   * to write into, and a row without a mask has nothing to paint (the mode switch adds one). */
  if (channel != PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
    return PaintLayersTargetStatus::ChannelNotInMode;
  }
  /* The correction being worked on, when it is one of this row's; the base mask otherwise. */
  MaterialPaintLayer *active_item = BKE_paint_layers_active_mask_item_get(ma);
  const bool item_of_layer = active_item != nullptr &&
                             BKE_paint_layers_mask_item_owner(ma, *active_item) == layer;
  r_target.mask_item = item_of_layer ? active_item : BKE_paint_layers_mask_base(*layer);
  if (r_target.mask_item == nullptr) {
    return PaintLayersTargetStatus::NoMask;
  }
  /* A map-coverage item whose kind is reference-only (Color ID: an external ID Map) is not a stroke
   * target; editability is a trait of the kind, so the user cannot pick it by accident. A paintable
   * one (UV Island) is written by its capture, not by a stroke, but is still a valid target image. */
  if (paint_layer_kind_is_map_coverage(*r_target.mask_item) &&
      !paint_layer_kind_map_paintable(*r_target.mask_item))
  {
    return PaintLayersTargetStatus::NoMask;
  }
  r_target.channel_record = paint_layer_channel_find(*r_target.mask_item, channel);
  return PaintLayersTargetStatus::Ok;
}

Material *BKE_paint_layers_active_material_get(Object *ob)
{
  if (ob == nullptr) {
    return nullptr;
  }
  Material *ma = BKE_object_material_get(ob, ob->actcol);
  return (ma != nullptr && BKE_paint_layers_is_layered(*ma)) ? ma : nullptr;
}

bool BKE_paint_layers_find_image_use(Material &ma,
                                     const Image &image,
                                     PaintLayersImageUse &r_use)
{
  bool found = false;
  BKE_paint_layers_foreach(
      ma, [&](MaterialPaintLayer &layer, MaterialPaintLayer * /*parent*/) -> bool {
        for (const int i : IndexRange(layer.channels_num)) {
          if (layer.channels[i].image == &image) {
            r_use.layer = &layer;
            r_use.channel = layer.channels[i].channel;
            r_use.is_mask = false;
            found = true;
            return false;
          }
        }
        /* A correction's map: reported as the correction, so the caller reads its channels like a
         * row's. */
        for (MaterialPaintLayer &effect : layer.effects) {
          for (const int i : IndexRange(effect.channels_num)) {
            if (effect.channels[i].image == &image) {
              r_use.layer = &effect;
              r_use.channel = effect.channels[i].channel;
              r_use.is_mask = false;
              found = true;
              return false;
            }
          }
        }
        /* A mask item's map: reported as the item, so the caller reads its channels like a row's. */
        for (MaterialPaintLayer &item : layer.mask_stack) {
          for (const int i : IndexRange(item.channels_num)) {
            if (item.channels[i].image == &image) {
              r_use.layer = &item;
              r_use.channel = item.channels[i].channel;
              r_use.is_mask = true;
              found = true;
              return false;
            }
          }
        }
        return true;
      });
  return found;
}

/**
 * What the object mask says about the resolved row on \a ob. Tile 0 asks the Mesh answer alone,
 * folders included; tiles are answered per pixel through the returned bits.
 */
static void object_mask_resolve(const Object &ob, const Material &ma, PaintLayersTarget &r_target)
{
  if (r_target.layer == nullptr) {
    return;
  }
  r_target.object_masked = !BKE_paint_layer_object_mask_visible(ob, ma, *r_target.layer, 0);
  r_target.excluded_tiles = BKE_paint_layer_object_mask_excluded_tiles(ob, ma, *r_target.layer);
}

bool BKE_paint_layers_target_get(
    Object &ob, const int material_slot, const int channel, PaintLayersTarget &r_target)
{
  /* The active slot goes through the one helper that names the active layered material; an
   * explicit slot resolves its own material here. */
  Material *ma = (material_slot < 0) ?
                     BKE_paint_layers_active_material_get(&ob) :
                     BKE_object_material_get(&ob, short(material_slot + 1));
  if (ma == nullptr) {
    return false;
  }
  if (!BKE_paint_layers_target_get(*ma, channel, r_target)) {
    return false;
  }
  object_mask_resolve(ob, *ma, r_target);
  return true;
}

PaintLayersTargetStatus BKE_paint_layers_target_resolve(Object &ob,
                                                        const int material_slot,
                                                        const int channel,
                                                        const PaintLayersTargetMode mode,
                                                        PaintLayersTarget &r_target)
{
  Material *ma = (material_slot < 0) ?
                     BKE_paint_layers_active_material_get(&ob) :
                     BKE_object_material_get(&ob, short(material_slot + 1));
  if (ma == nullptr) {
    return PaintLayersTargetStatus::NotLayered;
  }
  const PaintLayersTargetStatus status = BKE_paint_layers_target_resolve(
      *ma, channel, mode, r_target);
  if (status == PaintLayersTargetStatus::Ok) {
    object_mask_resolve(ob, *ma, r_target);
  }
  return status;
}

bool BKE_paint_layers_target_get(Object &ob,
                                 const int material_slot,
                                 const int channel,
                                 const PaintLayersTargetMode mode,
                                 PaintLayersTarget &r_target)
{
  return BKE_paint_layers_target_resolve(ob, material_slot, channel, mode, r_target) ==
         PaintLayersTargetStatus::Ok;
}

bool BKE_paint_layers_target_is_live(const PaintLayersTarget &target)
{
  if (target.layer == nullptr || !paint_layer_channel_live(target.channel_record)) {
    return false;
  }
  /* A switched-off mask is not a paint target: it hides nothing, so a stroke into it would be
   * invisible. */
  return target.mode != PaintLayersTargetMode::Mask ||
         (target.mask_item != nullptr && (target.mask_item->flag & MA_PAINT_LAYER_ENABLED) != 0);
}

Image *BKE_paint_layers_target_image(const PaintLayersTarget &target)
{
  if (!BKE_paint_layers_target_is_live(target)) {
    return nullptr;
  }
  return target.channel_record->image;
}

/**
 * The mask counterpart of #BKE_paint_layers_target_ensure_writable: the base mask item gets a map
 * the first time a stroke lands on it. The map is filled with the constant it replaces, so the row
 * looks the same before and after the first stroke; its colorspace is Non-Color, as a mask is a
 * number and not a colour.
 */
static Image *paint_layers_mask_ensure_writable(Main &bmain,
                                                PaintLayersTarget &target,
                                                const int image_size)
{
  MaterialPaintLayer *item = target.mask_item;
  if (item == nullptr || (item->flag & MA_PAINT_LAYER_ENABLED) == 0) {
    return nullptr;
  }
  MaterialPaintLayerChannel *record = paint_layer_channel_find(*item,
                                                               PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  /* A switched-off record is refused rather than re-enabled, as for any other channel. */
  if (record == nullptr || !paint_layer_channel_live(record)) {
    return nullptr;
  }
  target.channel_record = record;
  if (record->image != nullptr) {
    return record->image;
  }

  char name[192];
  BLI_snprintf(name,
               sizeof(name),
               "%s %s",
               target.layer->name[0] != '\0' ? target.layer->name : "Layer",
               item->name[0] != '\0' ? item->name : "Mask");
  /* A map-coverage map (UV Island) is tiled, so islands of other UDIM tiles can be written. */
  Image *image = paint_layers_map_create(bmain,
                                         name,
                                         image_size,
                                         /*is_color=*/false,
                                         record->value,
                                         paint_layer_kind_is_map_coverage(*item));
  if (image == nullptr) {
    return nullptr;
  }
  if (!BKE_paint_layers_channel_set_image(
          *target.material, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, image))
  {
    BKE_id_free(&bmain, image);
    return nullptr;
  }
  /* The record took its own user; the one the image was born with goes, or the map could never
   * become an orphan once its mask is removed. */
  id_us_min(&image->id);
  return record->image;
}

Image *BKE_paint_layers_map_coverage_map_ensure(Main &bmain,
                                                Material &ma,
                                                MaterialPaintLayer &item,
                                                const int image_size)
{
  MaterialPaintLayer *layer = BKE_paint_layers_mask_item_owner(ma, item);
  if (layer == nullptr || !paint_layer_kind_is_map_coverage(item) ||
      !paint_layer_kind_map_paintable(item))
  {
    return nullptr;
  }
  MaterialPaintLayerChannel *record = paint_layer_channel_find(item,
                                                               PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  if (record != nullptr && record->image != nullptr) {
    return record->image;
  }
  /* The material owns the size, so every map of its stack agrees; the caller's is the fallback. */
  const int map_size = ma.paint_layers_image_size > 0 ? int(ma.paint_layers_image_size) :
                                                        image_size;
  /* Zero-filled: the operator restricts everything until a capture or a stroke raises it. */
  /* Opaque black: the scalar is the RGB, and an opaque map reads the same on every path. */
  const float zero[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  char name[192];
  BLI_snprintf(name,
               sizeof(name),
               "%s %s",
               layer->name[0] != '\0' ? layer->name : "Layer",
               item.name[0] != '\0' ? item.name : "UV Island");
  /* Tiled, so a UV island on another UDIM tile can get its own tile on demand. */
  Image *image = paint_layers_map_create(
      bmain, name, map_size, /*is_color=*/false, zero, /*tiled=*/true);
  if (image == nullptr) {
    return nullptr;
  }
  if (!BKE_paint_layers_channel_set_image(ma, &item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, image)) {
    BKE_id_free(&bmain, image);
    return nullptr;
  }
  /* The record took its own user; the one the new image was born with goes, so the map can become
   * an orphan once its item is removed. */
  id_us_min(&image->id);
  /* #BKE_paint_layers_channel_set_image does not reallocate the records, so \a record stays valid,
   * but the item may have had none: read the image back through the record the setter used. */
  return paint_layer_channel_find(item, PAINT_MATERIAL_CHANNEL_BASE_COLOR)->image;
}

bool BKE_paint_layers_map_coverage_tile_ensure(Image &image, const int tile_number)
{
  if (image.source != IMA_SRC_TILED) {
    return false;
  }
  if (BKE_image_get_tile(&image, tile_number) != nullptr) {
    return true;
  }
  const ImageTile *base = static_cast<const ImageTile *>(image.tiles.first);
  if (base == nullptr) {
    return false;
  }
  /* #BKE_image_add_tile keeps the list sorted only when it already is. */
  BKE_image_sort_tiles(&image);
  ImageTile *tile = BKE_image_add_tile(&image, tile_number, nullptr);
  if (tile == nullptr) {
    return false;
  }
  /* The new tile is generated like the first one: same size, depth and zero-filled data colour. */
  tile->gen_x = base->gen_x;
  tile->gen_y = base->gen_y;
  tile->gen_type = base->gen_type;
  tile->gen_flag = base->gen_flag;
  tile->gen_depth = base->gen_depth;
  copy_v4_v4(tile->gen_color, base->gen_color);
  if (!BKE_image_fill_tile(&image, tile)) {
    BKE_image_remove_tile(&image, tile);
    return false;
  }
  return true;
}

Image *BKE_paint_layers_target_ensure_writable(Main &bmain,
                                               PaintLayersTarget &target,
                                               const int image_size,
                                               const float *new_channel_fill_linear)
{
  if (target.material == nullptr || target.layer == nullptr) {
    return nullptr;
  }
  /* The material owns the size so that every map of its stack agrees, whatever the caller's
   * scene setting says now. */
  const int map_size = target.material->paint_layers_image_size > 0 ?
                           int(target.material->paint_layers_image_size) :
                           image_size;
  if (target.mode == PaintLayersTargetMode::Mask) {
    return paint_layers_mask_ensure_writable(bmain, target, map_size);
  }
  const eMaterialPaintChannel channel = eMaterialPaintChannel(target.channel);
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(channel);

  MaterialPaintLayerChannel *record = paint_layer_channel_find(*target.layer, target.channel);
  /* A disabled record refuses writability rather than being switched back on behind the user's
   * back: the stroke must not silently re-enable a channel the user turned off. */
  if (record != nullptr && !paint_layer_channel_live(record)) {
    return nullptr;
  }
  const bool had_record = record != nullptr;
  if (!had_record) {
    record = BKE_paint_layers_channel_add(*target.material, target.layer, channel);
    if (record == nullptr) {
      return nullptr;
    }
    if (new_channel_fill_linear != nullptr) {
      copy_v4_v4(record->value, new_channel_fill_linear);
    }
  }
  target.channel_record = record;
  if (record->image != nullptr) {
    return record->image;
  }

  /* A record the row already had carries a flat value, and the first map keeps that look; one this
   * call had to add starts from the caller's colour or the channel's neutral. */
  float fill[4];
  if (had_record) {
    copy_v4_v4(fill, record->value);
  }
  else if (new_channel_fill_linear != nullptr) {
    copy_v4_v4(fill, new_channel_fill_linear);
  }
  else {
    /* The same table the composite starts from, so a first stroke on Specular or Roughness fills
     * the rest of the map with the neutral mid value instead of a zero that darkens the whole mesh. */
    BKE_paint_layers_channel_bottom_color(channel, fill);
  }
  /* A transparent texel still takes part in bilinear filtering: the GPU texture holds straight
   * bytes, so across a stroke's edge the colour is interpolated on its own and a black background
   * decodes to a tilted normal. The empty texels of a Normal map therefore hold the flat tangent,
   * which is what interpolates to nothing. */
  if (channel == PAINT_MATERIAL_CHANNEL_NORMAL && fill[3] <= 0.0f) {
    fill[0] = 0.5f;
    fill[1] = 0.5f;
    fill[2] = 1.0f;
  }
  /* A Fill correction's first map in a channel starts as the colour it laid there: opaque, because a
   * Fill covers by its opacity alone and its alpha was never part of the look. It laid nothing in
   * Normal, where the map therefore starts transparent. The channels it has no map in keep their
   * flat colour. */
  const bool fill_correction = target.layer->role == MA_PAINT_LAYER_ROLE_EFFECT &&
                               paint_layer_kind_has_flag(*target.layer,
                                                         PAINT_LAYER_KIND_FLAT_VALUE);
  if (fill_correction) {
    if (channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
      fill[0] = 0.5f;
      fill[1] = 0.5f;
      fill[2] = 1.0f;
      fill[3] = 0.0f;
    }
    else {
      fill[3] = 1.0f;
    }
  }

  /* `"<layer> <channel>"`, the naming a fresh channel map gets. Never translated. */
  char name[192];
  BLI_snprintf(name,
               sizeof(name),
               "%s %s",
               target.layer->name[0] != '\0' ? target.layer->name : "Layer",
               info.ui_name);
  Image *image = paint_layers_map_create(bmain, name, map_size, info.is_color, fill);
  if (image == nullptr) {
    return nullptr;
  }
  if (!BKE_paint_layers_channel_set_image(*target.material, target.layer, channel, image)) {
    BKE_id_free(&bmain, image);
    return nullptr;
  }
  /* The channel took its own user; the one the new image was born with was only ever the first
   * reference's, so it goes, or the map could never become an orphan once its row is removed. */
  id_us_min(&image->id);

  /* #BKE_paint_layers_channel_set_image does not reallocate the records, so \a record stays valid. */
  return record->image;
}

int BKE_paint_layers_active_maps_ensure(Main &bmain, Material &ma, const int image_size)
{
  const uint16_t channel_set = BKE_paint_layers_channel_set_mask_get(ma);
  int created = 0;
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    if (!info.supports_image_paint || (channel_set & (uint16_t(1) << int(info.channel))) == 0) {
      continue;
    }
    PaintLayersTarget target;
    if (!BKE_paint_layers_target_get(
            ma, int(info.channel), PaintLayersTargetMode::Content, target) ||
        BKE_paint_layers_target_image(target) != nullptr)
    {
      continue;
    }
    /* A switched-off record is the user's choice and stays off; a row with no record at all takes
     * part in the set, so the call adds it together with its map. */
    if (target.channel_record != nullptr && !BKE_paint_layers_target_is_live(target)) {
      continue;
    }
    if (BKE_paint_layers_target_ensure_writable(bmain, target, image_size) != nullptr) {
      created++;
    }
  }
  return created;
}

}  // namespace blender
