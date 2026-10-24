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

#include <cstdio>

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_listbase_iterator.hh"
#include "BLI_math_base.h"
#include "BLI_math_vector.h"
#include "BLI_rect.h"
#include "BLI_string.h"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_layers_object_mask.hh"
#include "BKE_paint_material_composite.hh"
#include "BKE_report.hh"

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
  /* A locked row refuses the stroke before any mode-specific reason: painting it is what the user
   * asked to prevent, whatever the channel or the kind of row would otherwise answer. */
  if (BKE_paint_layers_lock_blocker(ma, *layer) != nullptr) {
    return PaintLayersTargetStatus::Locked;
  }
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
    /* A generator produces its colour from nodes; a stroke has no map to write into it either. Its
     * mask is still paintable, as for a filter. */
    if (paint_layer_kind_has_flag(*layer, PAINT_LAYER_KIND_GENERATES)) {
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
  /* The mask item carries its own lock, and its row's lock was checked above. */
  if (BKE_paint_layers_lock_blocker(ma, *r_target.mask_item) != nullptr) {
    return PaintLayersTargetStatus::Locked;
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

/** Append the maps \a layer holds a user on to \a r_images: its channel maps and its bake's. */
static void paint_layer_maps_collect(const MaterialPaintLayer &layer, Vector<Image *> &r_images)
{
  const auto add = [&](Image *image) {
    if (image != nullptr) {
      r_images.append_non_duplicates(image);
    }
  };
  for (const int i : IndexRange(layer.channels_num)) {
    add(layer.channels[i].image);
  }
  if (layer.bake != nullptr) {
    for (Image *image : layer.bake->images) {
      add(image);
    }
    add(layer.bake->coverage);
  }
}

/**
 * Maps a one-way edit left behind that something still held a user on when it ended: the generated
 * shader tree lets go of its Image Texture nodes only when it is rebuilt, which happens after the
 * edit. They are identified by session UID (a freed ID's address could be reused) and purged by
 * #BKE_paint_layers_orphan_maps_purge once nothing holds them. Main thread only.
 */
struct PendingOrphanMap {
  uint32_t session_uid;
  /** Purge passes left before the map is given up on, so a map that lives on does not stay listed. */
  int tries_left;
};

static Vector<PendingOrphanMap> &pending_orphan_maps()
{
  static Vector<PendingOrphanMap> pending;
  return pending;
}

/**
 * Whether \a image is a map this system may free once nothing uses it: one with no file behind it
 * (a paint canvas, a rendered material map or any generated image), so a copy nothing uses is memory
 * with no way back. A file-backed texture stays, however it was dropped in.
 */
static bool paint_layers_orphan_map_eligible(const Image &image)
{
  ImageMaterialSource source;
  return (image.flag & IMA_PAINT_CANVAS) != 0 || image.source == IMA_SRC_GENERATED ||
         BKE_image_material_source_get(image, source);
}

/** Whether \a image is held by nothing at all: no user, no fake user, not linked. */
static bool paint_layers_orphan_map_unused(const Image &image)
{
  return image.id.us <= 0 && ID_FAKE_USERS(&image.id) == 0 && !ID_IS_LINKED(&image.id);
}

/**
 * Free the maps of \a candidates that nothing uses any more, after a one-way edit: the maps a
 * merge folded into a new one. Such an edit clears the undo history, so nothing can bring the rows
 * back, and a 4K map is 64 MiB of pixels that would otherwise stay in memory as an orphan until the
 * file is saved and reopened or purged by hand.
 *
 * A map something still holds a user on is not freed now but listed, and purged later.
 */
static void paint_layers_orphan_maps_free(Main &bmain, const Span<Image *> candidates)
{
  int freed = 0;
  int deferred = 0;
  for (Image *image : candidates) {
    if (!paint_layers_orphan_map_eligible(*image) || ID_IS_LINKED(&image->id) ||
        ID_FAKE_USERS(&image->id) != 0)
    {
      continue;
    }
    if (paint_layers_orphan_map_unused(*image)) {
      BKE_id_delete(&bmain, image);
      freed++;
    }
    else {
      pending_orphan_maps().append({image->id.session_uid, 8});
      deferred++;
    }
  }
  printf("[paint-layers orphan maps] freed=%d deferred=%d\n", freed, deferred);
}

void BKE_paint_layers_orphan_maps_purge(Main &bmain)
{
  Vector<PendingOrphanMap> &pending = pending_orphan_maps();
  if (pending.is_empty()) {
    return;
  }
  Vector<Image *> to_delete;
  for (Image &image : bmain.images) {
    for (PendingOrphanMap &entry : pending) {
      if (entry.session_uid == image.id.session_uid && paint_layers_orphan_map_unused(image)) {
        to_delete.append(&image);
        entry.tries_left = 0;
      }
    }
  }
  for (Image *image : to_delete) {
    BKE_id_delete(&bmain, image);
  }
  /* Entries that were deleted or ran out of passes are done with; the rest wait for the next. */
  Vector<PendingOrphanMap> kept;
  for (PendingOrphanMap &entry : pending) {
    if (entry.tries_left > 0 && --entry.tries_left > 0) {
      kept.append(entry);
    }
  }
  printf("[paint-layers orphan maps] purged=%d waiting=%d\n", int(to_delete.size()), int(kept.size()));
  pending = std::move(kept);
}
Image *BKE_paint_layers_mask_stack_merge(Main &bmain,
                                         Material &ma,
                                         MaterialPaintLayer &layer,
                                         ReportList *reports)
{
  MaterialPaintLayer *base = BKE_paint_layers_mask_base(layer);
  if (base == nullptr) {
    BKE_report(reports, RPT_ERROR, "The row has no mask to merge");
    return nullptr;
  }
  /* One item is the base alone: nothing lays over it to fold in. */
  if (layer.mask_stack.first == layer.mask_stack.last) {
    BKE_report(reports, RPT_ERROR, "The row has no mask corrections to merge");
    return nullptr;
  }
  /* The merge writes a plain coverage map into the base's Base Color record. A filter -- a mask
   * filter or a map-coverage operator alike -- reads its own kind and params instead, so folding
   * the stack into that record would leave the base showing something other than the merge. A base
   * like that only comes from a pasted mask item. */
  if (base->source == MA_PAINT_LAYER_SOURCE_FILTER) {
    BKE_report(
        reports, RPT_ERROR, "The base mask is a filter or a coverage operator and cannot be merged");
    return nullptr;
  }
  /* The same lock a stroke answers to: the row itself, any of its mask items, or a folder above. */
  if (const MaterialPaintLayer *blocker = BKE_paint_layers_lock_blocker(ma, layer)) {
    BKE_reportf(reports,
                RPT_ERROR,
                "\"%s\" is locked; unlock it before merging",
                blocker->name[0] != '\0' ? blocker->name : "Layer");
    return nullptr;
  }
  for (const MaterialPaintLayer &item : layer.mask_stack) {
    if ((item.flag & MA_PAINT_LAYER_MASK_BASE) != 0) {
      continue;
    }
    /* The row and the folders above it were checked with it; what is left is the item's own lock. */
    if (BKE_paint_layers_is_locked(item)) {
      BKE_reportf(reports,
                  RPT_ERROR,
                  "The mask correction \"%s\" is locked; unlock it before merging",
                  item.name[0] != '\0' ? item.name : "Mask");
      return nullptr;
    }
  }

  const int material_size = (ma.paint_layers_image_size > 0) ? int(ma.paint_layers_image_size) :
                                                               1024;
  int size = material_size;
  /* Prefer the size of an existing mask map: maps made by this material share it, and imported
   * masks may be smaller than the material's default. Refuse a stack that would silently lose
   * maps: an item read at its own size has to be square and to agree with the other such maps,
   * because the sampler keeps only buffers of the one size the merge reads, and a dropped map
   * would be lost from a stack the merge then deletes. */
  bool found_size = false;
  bool found_fallback = false;
  Vector<int> tiles;
  for (const MaterialPaintLayer &item : layer.mask_stack) {
    const MaterialPaintLayerChannel *item_record = paint_layer_channel_find(
        item, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    Image *image = (item_record != nullptr) ? item_record->image : nullptr;
    if (image == nullptr) {
      continue;
    }
    /* The merged map covers what the stack covers: the union of the maps' tiles, with 1001
     * standing in for a map that is not tiled at all. */
    if (image->source == IMA_SRC_TILED) {
      for (const ImageTile &tile : image->tiles) {
        const int number = tile.tile_number == 0 ? 1001 : tile.tile_number;
        if (!tiles.contains(number)) {
          tiles.append(number);
        }
      }
    }
    else if (!tiles.contains(1001)) {
      tiles.append(1001);
    }
    /* A filter owns no map to read, and a coverage operator's map is resampled to the merged size
     * whatever it is (its per-tile `match` is computed from the resampled buffer): neither
     * constrains the size. An Anchor Point reader owns a slice in canvas space, never a map. */
    if (paint_layer_effect_is_filter(item) || paint_layer_kind_is_map_coverage(item) ||
        paint_layer_kind_reads_anchor_point(item))
    {
      continue;
    }
    /* The map's own size, read the way the sampler reads the map itself: its first tile. */
    int first_tile = 1001;
    if (image->source == IMA_SRC_TILED && image->tiles.first != nullptr) {
      const ImageTile *first = static_cast<const ImageTile *>(image->tiles.first);
      first_tile = first->tile_number == 0 ? 1001 : first->tile_number;
    }
    int width = 0;
    int height = 0;
    if (ImBuf *probe = BKE_paint_material_mask_map_acquire(image, first_tile, nullptr, 0, 0)) {
      width = probe->x;
      height = probe->y;
      IMB_freeImBuf(probe);
    }
    /* A mapped item is resampled to the size the merge writes, so its own size never conflicts.
     * An item read at its own size has to be square and to agree with the other such maps. */
    if (!BKE_paint_layers_mapping_applies(ma, item)) {
      if (width <= 0 || height <= 0) {
        BKE_report(reports, RPT_ERROR, "A mask map's size cannot be read; merging is refused");
        return nullptr;
      }
      if (width != height) {
        BKE_report(
            reports, RPT_ERROR, "A mask map is not square; map it or make it square before merging");
        return nullptr;
      }
      if (!found_size) {
        size = width;
        found_size = true;
      }
      else if (width != size) {
        BKE_report(reports,
                   RPT_ERROR,
                   "The mask items use maps of different sizes; resize them before merging");
        return nullptr;
      }
    }
    else if (!found_size && !found_fallback && width > 0 && width == height) {
      /* Every map is mapped: keep the first square size instead of the material's default. */
      size = width;
      found_fallback = true;
    }
  }

  PaintLayersMaskSampler sampler(ma, layer);
  if (sampler.is_empty()) {
    return nullptr;
  }
  if (tiles.is_empty()) {
    tiles.append(1001);
  }
  /* #tile_acquire is not thread safe, so every tile is acquired here, before the parallel loops
   * that sample them. A tile a map does not have is not an error: the sampler reads such an item
   * at its constant, the same fallback the render shows for that tile. */
  for (const int tile_number : tiles) {
    sampler.tile_acquire(tile_number, size, size);
  }

  char name[192];
  BLI_snprintf(name, sizeof(name), "%s Mask Merge", layer.name[0] != '\0' ? layer.name : "Layer");
  const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  /* A map that is not tile 1001 alone has to be tiled, or the other tiles have nowhere to go. */
  const bool tiled = tiles.size() > 1 || tiles[0] != 1001;
  Image *merged = paint_layers_map_create(
      bmain, name, size, /*is_color=*/false, clear, tiled);
  if (merged == nullptr) {
    BKE_report(reports, RPT_ERROR, "Cannot create the merged mask image");
    return nullptr;
  }

  /* A tile that cannot be written would silently drop what the stack covers there, and the merge
   * then deletes the stack: it is refused whole, with the description untouched. */
  for (const int tile_number : tiles) {
    if (tiled && !BKE_paint_layers_map_coverage_tile_ensure(*merged, tile_number)) {
      BKE_id_free(&bmain, merged);
      BKE_reportf(reports, RPT_ERROR, "Cannot add UDIM tile %d to the merged mask", tile_number);
      return nullptr;
    }
    ImageUser iuser;
    BKE_imageuser_default(&iuser);
    iuser.tile = tile_number;
    void *lock = nullptr;
    ImBuf *dst = BKE_image_acquire_ibuf(merged, &iuser, &lock);
    uchar *pixels = (dst != nullptr) ? dst->byte_data_for_write() : nullptr;
    if (pixels == nullptr) {
      BKE_image_release_ibuf(merged, dst, lock);
      BKE_id_free(&bmain, merged);
      BKE_reportf(reports, RPT_ERROR, "Cannot write UDIM tile %d of the merged mask", tile_number);
      return nullptr;
    }
    /* One coverage byte per texel: the factor the whole stack folds into, as grey RGB with an
     * opaque alpha -- the shape the base's Multiply reads back at full weight. */
    threading::parallel_for(IndexRange(dst->y), 8, [&](const IndexRange rows) {
      for (const int y : rows) {
        for (const int x : IndexRange(dst->x)) {
          const float factor = clamp_f(sampler.sample(tile_number, x, y), 0.0f, 1.0f);
          const uchar value = uchar(factor * 255.0f + 0.5f);
          uchar *pixel = pixels + (int64_t(y) * dst->x + x) * 4;
          pixel[0] = value;
          pixel[1] = value;
          pixel[2] = value;
          pixel[3] = 255;
        }
      }
    });
    dst->userflags |= IB_DISPLAY_BUFFER_INVALID;
    BKE_image_mark_dirty(merged, dst);
    BKE_image_release_ibuf(merged, dst, lock);
  }
  BKE_image_partial_update_mark_full_update(merged);
  /* The maps the merge replaces, freed once it has succeeded: the base's own and the corrections'. */
  Vector<Image *> replaced_maps;
  paint_layer_maps_collect(*base, replaced_maps);
  for (const MaterialPaintLayer &item : layer.mask_stack) {
    if ((item.flag & MA_PAINT_LAYER_MASK_BASE) == 0) {
      paint_layer_maps_collect(item, replaced_maps);
    }
  }

  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
      ma, base, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  if (record == nullptr ||
      !BKE_paint_layers_channel_set_image(ma, base, PAINT_MATERIAL_CHANNEL_BASE_COLOR, merged))
  {
    BKE_id_free(&bmain, merged);
    BKE_report(reports, RPT_ERROR, "Cannot write the merged mask into the base mask");
    return nullptr;
  }
  /* The record took its own user; the creation reference goes. */
  id_us_min(&merged->id);
  record->value[0] = record->value[1] = record->value[2] = record->value[3] = 1.0f;

  /* The merged map already holds every item's contribution -- the base's own blend, opacity,
   * invert and mapping included, which the sampler applied while it read the stack. The base goes
   * back to the settings a fresh base mask has, so the map reads as the stack did (Multiply over
   * an opaque grey is the identity the factor was written for), and no leftover value surprises
   * the next time the mapping is switched on. */
  base->opacity = 1.0f;
  base->blend = MA_PAINT_LAYER_BLEND_MULTIPLY;
  base->flag |= MA_PAINT_LAYER_ENABLED;
  BKE_paint_layers_channel_set_invert(ma, base, PAINT_MATERIAL_CHANNEL_BASE_COLOR, false);
  const float no_offset[2] = {0.0f, 0.0f};
  const float unit_scale[2] = {1.0f, 1.0f};
  BKE_paint_layers_mapping_set_offset(ma, base, no_offset);
  BKE_paint_layers_mapping_set_scale(ma, base, unit_scale);
  BKE_paint_layers_mapping_set_rotation(ma, base, 0.0f);
  BKE_paint_layers_mapping_set_wrap(ma, base, MA_PAINT_LAYER_MAPPING_WRAP_REPEAT);
  BKE_paint_layers_mapping_set_enabled(ma, base, false);

  /* The base survives; every correction over it is folded into the merged map. */
  Vector<MaterialPaintLayer *> corrections;
  for (MaterialPaintLayer &item : layer.mask_stack) {
    if ((item.flag & MA_PAINT_LAYER_MASK_BASE) == 0) {
      corrections.append(&item);
    }
  }
  for (MaterialPaintLayer *item : corrections) {
    BKE_paint_layers_mask_item_remove(ma, item);
  }
  BKE_paint_layers_tag_edited(ma);
  /* The merged map is not among them: its record holds a user. */
  paint_layers_orphan_maps_free(bmain, replaced_maps);

  return merged;
}

/* -------------------------------------------------------------------- */
/** \name Bake corrections into a Paint correction
 * \{ */

/** Rows of a bake handled at a time: keeps the float scratch small on a 4K canvas. */
constexpr int CORRECTIONS_BAKE_STRIP_ROWS = 1024;

/**
 * The corrections of one row to bake, bottom to top. False, with the reason reported, when they
 * cannot be baked as one: they hang on different rows, are not next to each other, or one of them is
 * a kind that has no colour of its own to bake (a filter, an Anchor Point, a generator).
 */
static bool corrections_bake_collect(Material &ma,
                                     const Span<MaterialPaintLayer *> selected,
                                     const bool check_locks,
                                     ReportList *reports,
                                     MaterialPaintLayer *&r_owner,
                                     Vector<MaterialPaintLayer *> &r_run)
{
  r_owner = nullptr;
  r_run.clear();
  if (selected.is_empty()) {
    BKE_report(reports, RPT_ERROR, "No corrections selected");
    return false;
  }
  r_owner = BKE_paint_layers_correction_owner(ma, *selected.first());
  if (r_owner == nullptr) {
    BKE_report(reports, RPT_ERROR, "Only content corrections can be baked into a texture");
    return false;
  }
  for (MaterialPaintLayer *effect : selected) {
    if (BKE_paint_layers_correction_owner(ma, *effect) != r_owner) {
      BKE_report(reports, RPT_ERROR, "Only corrections of one layer can be merged");
      return false;
    }
  }
  /* Stacked, the upper one covers the lower: a gap would leave a row between them that the merged
   * correction could not stay on both sides of. */
  bool in_run = false;
  bool run_ended = false;
  for (MaterialPaintLayer &effect : r_owner->effects) {
    if (!selected.contains(&effect)) {
      run_ended |= in_run;
      continue;
    }
    if (run_ended) {
      BKE_report(reports, RPT_ERROR, "Only corrections next to each other can be merged");
      return false;
    }
    in_run = true;
    r_run.append(&effect);
  }
  for (const MaterialPaintLayer *effect : r_run) {
    const char *name = effect->name[0] != '\0' ? effect->name : "Correction";
    /* A filter changes the colour below it, which includes the row's own where the corrections
     * cover nothing: no texture holds that. An Anchor Point is a live link to a slice of the row,
     * which a bake would freeze, and a marker publishes the row's content up to its place.
     * TODO: a Gradient correction (PAINT_LAYER_KIND_GENERATES, once the Gradient series lands) is
     * bakeable in UV space through the same row description; Object and Generated spaces need the
     * object's surface, so they stay refused until the bake can be given one. */
    if (paint_layer_effect_is_filter(*effect) || paint_layer_kind_reads_anchor_point(*effect) ||
        paint_layer_kind_is_anchor_point_marker(*effect) ||
        paint_layer_kind_has_flag(*effect, PAINT_LAYER_KIND_GENERATES) ||
        !ELEM(effect->source,
              MA_PAINT_LAYER_SOURCE_IMAGE,
              MA_PAINT_LAYER_SOURCE_CONSTANT,
              MA_PAINT_LAYER_SOURCE_MATERIAL))
    {
      BKE_reportf(reports,
                  RPT_ERROR,
                  "\"%s\" cannot be baked into a texture: only Paint, Fill and Material "
                  "corrections can",
                  name);
      return false;
    }
    if (!check_locks) {
      continue;
    }
    /* The same lock a stroke answers to: the correction, its row, or a folder above. */
    if (const MaterialPaintLayer *blocker = BKE_paint_layers_lock_blocker(ma, *effect)) {
      BKE_reportf(reports,
                  RPT_ERROR,
                  "\"%s\" is locked; unlock it before baking",
                  blocker->name[0] != '\0' ? blocker->name : "Layer");
      return false;
    }
  }
  return true;
}

/**
 * The channels the corrections of \a run lay in, for the bake. Refused, with the reason reported,
 * when the lowest correction of a channel is not blended with Normal (it goes over the row's colour
 * with the merged blend, so what it did to that colour cannot be kept), when nothing lays, or --
 * with \a require_rendered -- when a Material correction lays in a channel its source has no
 * rendered map for: the CPU cannot read a shader graph, and baking it as empty would change the
 * look silently.
 */
static bool corrections_bake_channels(Material &ma,
                                      const Span<MaterialPaintLayer *> run,
                                      const bool require_rendered,
                                      ReportList *reports,
                                      Vector<int> &r_channels)
{
  r_channels.clear();
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    if (!info.supports_image_paint) {
      continue;
    }
    const int channel = int(info.channel);
    bool lowest_found = false;
    for (const MaterialPaintLayer *effect : run) {
      if (!paint_layer_effect_lays(ma, *effect, channel)) {
        continue;
      }
      const char *name = effect->name[0] != '\0' ? effect->name : "Correction";
      if (!lowest_found) {
        lowest_found = true;
        r_channels.append(channel);
        if (!BKE_paint_layers_correction_blend_is_normal(*effect, channel)) {
          BKE_reportf(reports,
                      RPT_ERROR,
                      "\"%s\" is not blended with Normal in %s: baked, it would no longer read "
                      "the colour below it. Set its blend to Normal first",
                      name,
                      info.ui_name);
          return false;
        }
      }
      if (require_rendered && paint_layer_kind_has_flag(*effect, PAINT_LAYER_KIND_READS_SOURCE) &&
          (paint_layer_material_channel_source(
               ma, *effect, channel, PaintLayerMaterialUse::Composite)
                   .kind == PaintLayerMaterialSourceKind::None ||
           paint_layer_material_coverage_source(ma, *effect, PaintLayerMaterialUse::Composite)
                   .kind == PaintLayerMaterialCoverageKind::Pending))
      {
        BKE_reportf(reports,
                    RPT_ERROR,
                    "The Material correction \"%s\" has no rendered map in %s yet; render it "
                    "(Bake Now) and try again",
                    name,
                    info.ui_name);
        return false;
      }
    }
  }
  if (r_channels.is_empty()) {
    BKE_report(reports, RPT_ERROR, "The corrections lay nothing that could be baked");
    return false;
  }
  return true;
}

bool BKE_paint_layers_corrections_bake_precheck(Material &ma,
                                                const Span<MaterialPaintLayer *> selected,
                                                ReportList *reports)
{
  MaterialPaintLayer *owner = nullptr;
  Vector<MaterialPaintLayer *> run;
  Vector<int> channels;
  if (!corrections_bake_collect(ma, selected, true, reports, owner, run)) {
    return false;
  }
  if (run.size() == 1 && run.first()->source == MA_PAINT_LAYER_SOURCE_IMAGE) {
    BKE_report(reports, RPT_ERROR, "A Paint correction is a texture already");
    return false;
  }
  if (corrections_bake_channels(ma, run, false, nullptr, channels)) {
    return true;
  }
  /* Before a Material correction is rendered its channels do not all lay yet, so a failure here may
   * be only that; the bake checks again once the render landed. */
  for (const MaterialPaintLayer *effect : run) {
    if (effect->source == MA_PAINT_LAYER_SOURCE_MATERIAL) {
      return true;
    }
  }
  corrections_bake_channels(ma, run, false, reports, channels);
  return false;
}
int BKE_paint_layers_corrections_bake_kind(Material &ma, const Span<MaterialPaintLayer *> selected)
{
  MaterialPaintLayer *owner = nullptr;
  Vector<MaterialPaintLayer *> run;
  if (!corrections_bake_collect(ma, selected, false, nullptr, owner, run)) {
    return 0;
  }
  if (run.size() >= 2) {
    return 2;
  }
  /* A Paint correction is a texture already. */
  return run.first()->source == MA_PAINT_LAYER_SOURCE_IMAGE ? 0 : 1;
}

MaterialPaintLayer *BKE_paint_layers_corrections_bake(Main &bmain,
                                                      Material &ma,
                                                      const Span<MaterialPaintLayer *> selected,
                                                      ReportList *reports)
{
  MaterialPaintLayer *owner = nullptr;
  Vector<MaterialPaintLayer *> run;
  if (!corrections_bake_collect(ma, selected, true, reports, owner, run)) {
    return nullptr;
  }
  if (run.size() == 1 && run.first()->source == MA_PAINT_LAYER_SOURCE_IMAGE) {
    BKE_report(reports, RPT_ERROR, "A Paint correction is a texture already");
    return nullptr;
  }

  Vector<int> channels;
  if (!corrections_bake_channels(ma, run, /*require_rendered=*/true, reports, channels)) {
    return nullptr;
  }

  /* The canvas: the size of the maps the set paints at, the material's otherwise. As in the mask
   * merge, a map read at its own size has to be square and to agree with the others, or the bake
   * would silently resample it. */
  int size = (ma.paint_layers_image_size > 0) ? int(ma.paint_layers_image_size) : 1024;
  bool found_size = false;
  Vector<int> tiles;
  for (const MaterialPaintLayer *effect : run) {
    if (paint_layer_kind_has_flag(*effect, PAINT_LAYER_KIND_READS_SOURCE)) {
      continue;
    }
    for (const int channel : channels) {
      const MaterialPaintLayerChannel *record = paint_layer_channel_find(*effect, channel);
      Image *image = (record != nullptr) ? record->image : nullptr;
      if (image == nullptr) {
        continue;
      }
      int first_tile = 1001;
      if (image->source == IMA_SRC_TILED) {
        for (const ImageTile &tile : image->tiles) {
          const int number = tile.tile_number == 0 ? 1001 : tile.tile_number;
          if (!tiles.contains(number)) {
            tiles.append(number);
          }
        }
        if (const ImageTile *first = static_cast<const ImageTile *>(image->tiles.first)) {
          first_tile = first->tile_number == 0 ? 1001 : first->tile_number;
        }
      }
      else if (!tiles.contains(1001)) {
        tiles.append(1001);
      }
      if (BKE_paint_layers_mapping_applies(ma, *effect)) {
        continue;
      }
      int width = 0;
      int height = 0;
      if (ImBuf *probe = BKE_paint_material_mask_map_acquire(image, first_tile, nullptr, 0, 0)) {
        width = probe->x;
        height = probe->y;
        IMB_freeImBuf(probe);
      }
      if (width <= 0 || width != height) {
        BKE_report(reports, RPT_ERROR, "A correction map is not square or cannot be read");
        return nullptr;
      }
      if (found_size && width != size) {
        BKE_report(reports,
                   RPT_ERROR,
                   "The corrections use maps of different sizes; resize them before merging");
        return nullptr;
      }
      size = width;
      found_size = true;
    }
  }
  if (tiles.is_empty()) {
    tiles.append(1001);
  }
  /* A map that is not tile 1001 alone has to be tiled, or the other tiles have nowhere to go. */
  const bool tiled = tiles.size() > 1 || tiles[0] != 1001;

  char merged_name[MAX_NAME];
  BLI_strncpy(merged_name,
              (run.size() == 1 && run.first()->name[0] != '\0') ? run.first()->name :
                                                                  "Merged Correction",
              sizeof(merged_name));

  /* Everything is built before the description is touched, so a failure leaves the material as it
   * was. The maps are owned here until the new correction takes them. */
  struct BakedMap {
    int channel;
    Image *image;
  };
  Vector<BakedMap> baked;
  const auto free_baked = [&]() {
    for (const BakedMap &map : baked) {
      BKE_id_free(&bmain, map.image);
    }
    baked.clear();
  };

  for (const int channel : channels) {
    const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
        eMaterialPaintChannel(channel));
    /* Transparent: a texel no correction reached lays nothing. A Normal map reads flat there. */
    float fill[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
      fill[0] = fill[1] = 0.5f;
      fill[2] = 1.0f;
    }
    char name[192];
    BLI_snprintf(name, sizeof(name), "%s %s", merged_name, info.ui_name);
    Image *image = paint_layers_map_create(bmain, name, size, info.is_color, fill, tiled);
    if (image == nullptr) {
      BKE_report(reports, RPT_ERROR, "Cannot create the baked texture");
      free_baked();
      return nullptr;
    }
    baked.append({channel, image});

    bool wrote_any = false;
    for (const int tile_number : tiles) {
      ImageUser iuser;
      BKE_imageuser_default(&iuser);
      iuser.tile = tile_number;
      PaintMaterialCompositeImageLayer row;
      if (!BKE_paint_layers_corrections_row_describe(
              ma, run, channel, size, &iuser, tile_number, row))
      {
        continue;
      }
      if (tiled && !BKE_paint_layers_map_coverage_tile_ensure(*image, tile_number)) {
        BKE_report(reports, RPT_ERROR, "Cannot add a UDIM tile to the baked texture");
        free_baked();
        return nullptr;
      }
      void *lock = nullptr;
      ImBuf *dst = BKE_image_acquire_ibuf(image, &iuser, &lock);
      bool ok = dst != nullptr && dst->byte_data_for_write() != nullptr;
      for (int y = 0; ok && y < dst->y; y += CORRECTIONS_BAKE_STRIP_ROWS) {
        rcti area;
        BLI_rcti_init(&area, 0, dst->x, y, min_ii(y + CORRECTIONS_BAKE_STRIP_ROWS, dst->y));
        const int64_t count = int64_t(BLI_rcti_size_x(&area)) * BLI_rcti_size_y(&area);
        Vector<float> color(count * 4);
        Vector<float> coverage(count);
        ok = BKE_paint_material_composite_eval_row_content(
            row, color.data(), coverage.data(), &area, channel);
        if (!ok) {
          break;
        }
        /* The map's alpha is the coverage the corrections brought: the merged correction lays by
         * its opacity times that, as the originals did one after the other. */
        for (const int64_t i : IndexRange(count)) {
          color[i * 4 + 3] = coverage[i];
        }
        BKE_paint_material_composite_linear_to_byte(color, area, *dst);
      }
      if (ok) {
        dst->userflags |= IB_DISPLAY_BUFFER_INVALID;
        BKE_image_mark_dirty(image, dst);
      }
      BKE_image_release_ibuf(image, dst, lock);
      if (!ok) {
        BKE_reportf(reports,
                    RPT_ERROR,
                    "Cannot bake the corrections in %s (the maps may differ in size)",
                    info.ui_name);
        free_baked();
        return nullptr;
      }
      wrote_any = true;
    }
    BKE_image_partial_update_mark_full_update(image);
    if (!wrote_any) {
      /* Nothing lays here on any tile: no map, so the merged correction has no record's worth in it. */
      BKE_id_free(&bmain, image);
      baked.remove_last();
    }
  }
  if (baked.is_empty()) {
    BKE_report(reports, RPT_ERROR, "The corrections lay nothing that could be baked");
    return nullptr;
  }

  MaterialPaintLayer *merged = BKE_paint_layers_correction_add(ma, owner, merged_name);
  bool attached = merged != nullptr;
  for (const BakedMap &map : baked) {
    if (!attached) {
      break;
    }
    const eMaterialPaintChannel channel = eMaterialPaintChannel(map.channel);
    attached = (paint_layer_channel_find(*merged, map.channel) != nullptr ||
                BKE_paint_layers_channel_add(ma, merged, channel) != nullptr) &&
               BKE_paint_layers_channel_set_image(ma, merged, channel, map.image);
    if (attached) {
      /* The record took its own user; the one the map was born with goes, or it could never become
       * an orphan once its correction is removed. */
      id_us_min(&map.image->id);
    }
  }
  if (!attached) {
    if (merged != nullptr) {
      BKE_paint_layers_correction_remove(ma, merged);
    }
    free_baked();
    BKE_report(reports, RPT_ERROR, "Cannot attach the baked textures to a new correction");
    return nullptr;
  }
  /* Normal Replace is a choice of the lowest correction that lays there; the merged one keeps it. */
  for (const MaterialPaintLayer *effect : run) {
    if (paint_layer_effect_lays(ma, *effect, PAINT_MATERIAL_CHANNEL_NORMAL)) {
      if (BKE_paint_layers_normal_replace(*effect)) {
        BKE_paint_layers_channel_blend_set(
            ma, *merged, PAINT_MATERIAL_CHANNEL_NORMAL, MA_PAINT_LAYER_BLEND_NORMAL_REPLACE);
      }
      break;
    }
  }

  /* The merged correction takes the place of the topmost one it replaces, so the stack reads as it
   * did. */
  BLI_remlink(&owner->effects, merged);
  BLI_insertlinkafter(&owner->effects, run.last(), merged);
  /* Their maps and the renders a Material correction was baked from: nothing reads them once the
   * corrections are gone. */
  Vector<Image *> replaced_maps;
  for (const MaterialPaintLayer *effect : run) {
    paint_layer_maps_collect(*effect, replaced_maps);
  }
  for (MaterialPaintLayer *effect : run) {
    BKE_paint_layers_correction_remove(ma, effect);
  }
  BKE_paint_layers_active_set(ma, merged->marker);
  BKE_paint_layers_tag_edited(ma);
  paint_layers_orphan_maps_free(bmain, replaced_maps);
  return merged;
}

/** \} */

}  // namespace blender
