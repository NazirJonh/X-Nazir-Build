/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * See #BKE_paint_layers_target.hh: the paint target of a layered material, and growing the
 * description on the first stroke into a channel or mask the active row does not have yet.
 */

#include "BKE_paint_layers_target.hh"

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_math_base.hh"
#include "BLI_math_vector.h"
#include "BLI_string.h"
#include "BLI_uuid.h"

#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_material.hh"
#include "BKE_object.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"

#include "paint_layers_intern.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

namespace blender {

namespace {

MaterialPaintLayerChannel *paint_layer_channel_find(MaterialPaintLayer &layer, const int channel)
{
  for (int i = 0; i < layer.channels_num; i++) {
    if (layer.channels[i].channel == channel) {
      return &layer.channels[i];
    }
  }
  return nullptr;
}

/** The neutral a brand-new Paint map starts from: flat tangent for Normal, opaque black else. */
void paint_layer_channel_neutral(const int channel, float r_color[4])
{
  zero_v4(r_color);
  r_color[3] = 1.0f;
  if (channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
    r_color[0] = 0.5f;
    r_color[1] = 0.5f;
    r_color[2] = 1.0f;
  }
}

/** Write one solid colour into \a image's byte buffer, encoded into the image's colorspace. */
void paint_layers_image_fill(Image &image, const bool is_color, const float rgba[4])
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
      BKE_paint_layers_sample_from_linear(image, encoded);
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

Image *paint_layers_map_create(Main &bmain,
                               const char *name,
                               const int size,
                               const bool is_color,
                               const float rgba[4])
{
  Image *image = BKE_image_add_generated(
      &bmain, size, size, name, 24, false, IMA_GENTYPE_BLANK, rgba, false, !is_color, false);
  if (image == nullptr) {
    return nullptr;
  }
  image->alpha_mode = IMA_ALPHA_STRAIGHT;
  image->flag |= IMA_PAINT_CANVAS;
  paint_layers_image_fill(*image, is_color, rgba);
  return image;
}

/** `"<layer> <channel>"`, the naming a fresh channel map gets. Never translated. */
void paint_layers_channel_image_name(const MaterialPaintLayer &layer,
                                     const int channel,
                                     char *r_name,
                                     const size_t name_size)
{
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(channel));
  BLI_snprintf(r_name,
               name_size,
               "%s %s",
               layer.name[0] != '\0' ? layer.name : "Layer",
               info.ui_name);
}

}  // namespace

bool BKE_paint_layers_target_get(Material &ma,
                                 const int channel,
                                 const PaintLayersTargetMode mode,
                                 PaintLayersTarget &r_target)
{
  if (!paint_layers_is_layered(ma)) {
    return false;
  }
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
    return false;
  }
  const bUUID marker = BKE_paint_layers_active_get(ma);
  if (BLI_uuid_is_nil(marker)) {
    return false;
  }
  MaterialPaintLayer *layer = BKE_paint_layers_find(ma, marker);
  /* A folder carries no channel maps of its own, so it is never a content target; its mask is a
   * scalar coverage over the whole subtree and is edited like any other row's. */
  if (layer == nullptr ||
      (BKE_paint_layers_is_folder(*layer) && mode != PaintLayersTargetMode::Mask))
  {
    return false;
  }
  r_target = {};
  r_target.material = &ma;
  r_target.layer = layer;
  r_target.channel = channel;
  r_target.mode = mode;
  if (mode == PaintLayersTargetMode::Content) {
    r_target.channel_record = paint_layer_channel_find(*layer, channel);
  }
  else {
    /* The active mask item is the selected row itself when it is a mask item; otherwise the active
     * row's first mask element. An empty stack resolves to no item, and the first stroke creates
     * one. */
    if (BKE_paint_layers_role(*layer) == PaintLayerRole::MaskItem) {
      r_target.mask_item = layer;
    }
    else {
      const Vector<MaterialPaintLayer *> items = BKE_paint_layers_mask_items(*layer);
      r_target.mask_item = items.is_empty() ? nullptr : items.first();
    }
  }
  return true;
}

Material *BKE_paint_layers_active_material_get(Object *ob)
{
  if (ob == nullptr) {
    return nullptr;
  }
  Material *ma = BKE_object_material_get(ob, ob->actcol);
  return (ma != nullptr && paint_layers_is_layered(*ma)) ? ma : nullptr;
}

MaterialPaintLayer *BKE_paint_layers_active_layer_get(Material &ma)
{
  const bUUID marker = BKE_paint_layers_active_get(ma);
  if (BLI_uuid_is_nil(marker)) {
    return nullptr;
  }
  return BKE_paint_layers_find(ma, marker);
}

namespace {

/**
 * Whether \a layer's own channels or mask carry \a image, then its corrections, then its children;
 * fills \a r_use on a match. \a is_correction and \a correction_marker describe which list \a layer
 * was reached through, so the match reads back with the right role.
 */
bool paint_layers_image_use_match(MaterialPaintLayer &layer,
                                  const Image &image,
                                  const bool is_correction,
                                  const bUUID &correction_marker,
                                  PaintLayersImageUse &r_use)
{
  for (int i = 0; i < layer.channels_num; i++) {
    if (layer.channels[i].image == &image) {
      const bool mask_item = BKE_paint_layers_role(layer) == PaintLayerRole::MaskItem;
      r_use.layer = &layer;
      r_use.channel = mask_item ? PAINT_LAYER_MAP_MASK : layer.channels[i].channel;
      r_use.role = mask_item ? PaintLayersImageUseRole::Mask :
                   is_correction ? PaintLayersImageUseRole::Correction :
                                   PaintLayersImageUseRole::Content;
      r_use.correction = correction_marker;
      return true;
    }
  }
  for (MaterialPaintLayer &effect :
       *reinterpret_cast<ListBaseT<MaterialPaintLayer> *>(&layer.effects))
  {
    if (paint_layers_image_use_match(effect, image, true, effect.marker, r_use)) {
      return true;
    }
  }
  for (MaterialPaintLayer &mask_item :
       *reinterpret_cast<ListBaseT<MaterialPaintLayer> *>(&layer.mask_stack))
  {
    if (paint_layers_image_use_match(mask_item, image, true, mask_item.marker, r_use)) {
      return true;
    }
  }
  for (MaterialPaintLayer &child :
       *reinterpret_cast<ListBaseT<MaterialPaintLayer> *>(&layer.children))
  {
    if (paint_layers_image_use_match(child, image, false, {}, r_use)) {
      return true;
    }
  }
  return false;
}

}  // namespace

bool BKE_paint_layers_find_image_use(Material &ma,
                                     const Image &image,
                                     PaintLayersImageUse &r_use)
{
  for (MaterialPaintLayer &layer :
       *reinterpret_cast<ListBaseT<MaterialPaintLayer> *>(&ma.paint_layers))
  {
    if (paint_layers_image_use_match(layer, image, false, {}, r_use)) {
      return true;
    }
  }
  return false;
}

bool BKE_paint_layers_target_get(Object &ob,
                                 const int material_slot,
                                 const int channel,
                                 const PaintLayersTargetMode mode,
                                 PaintLayersTarget &r_target)
{
  /* The active slot goes through the one helper that names the active layered material; an
   * explicit slot resolves its own material here. */
  Material *ma = (material_slot < 0) ?
                     BKE_paint_layers_active_material_get(&ob) :
                     BKE_object_material_get(&ob, short(material_slot + 1));
  if (ma == nullptr) {
    return false;
  }
  return BKE_paint_layers_target_get(*ma, channel, mode, r_target);
}

bool BKE_paint_layers_target_get(Object &ob,
                                 const int material_slot,
                                 const int channel,
                                 const PaintModeSettings &mode_settings,
                                 PaintLayersTarget &r_target)
{
  const PaintLayersTargetMode mode = (mode_settings.layer_target_mode == PAINT_LAYER_TARGET_MASK) ?
                                         PaintLayersTargetMode::Mask :
                                         PaintLayersTargetMode::Content;
  return BKE_paint_layers_target_get(ob, material_slot, channel, mode, r_target);
}

Image *BKE_paint_layers_target_image(const PaintLayersTarget &target)
{
  if (target.layer == nullptr) {
    return nullptr;
  }
  if (target.mode == PaintLayersTargetMode::Content) {
    if (target.channel_record == nullptr ||
        target.channel_record->state == MA_PAINT_LAYER_CHANNEL_ABSENT)
    {
      return nullptr;
    }
    return target.channel_record->image;
  }
  if (target.mask_item == nullptr ||
      (target.mask_item->flag & MA_PAINT_LAYER_ENABLED) == 0)
  {
    return nullptr;
  }
  return paint_layer_mask_correction_image(*target.mask_item, target.channel);
}

/** Whether \a target, or any ancestor of it, is set to bake always: a frozen paint target. */
static bool paint_layer_always_baked(const ListBase &list,
                                     const MaterialPaintLayer *target,
                                     const bool ancestor_always)
{
  for (const MaterialPaintLayer &layer :
       *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&list))
  {
    const bool always = ancestor_always ||
                        (layer.bake != nullptr &&
                         layer.bake->mode == MA_PAINT_LAYER_BAKE_ALWAYS);
    if (&layer == target) {
      return always;
    }
    if (paint_layer_always_baked(layer.children, target, always)) {
      return true;
    }
    if (paint_layer_always_baked(layer.effects, target, always) ||
        paint_layer_always_baked(layer.mask_stack, target, always))
    {
      return true;
    }
  }
  return false;
}

bool BKE_paint_layers_target_is_frozen(const PaintLayersTarget &target)
{
  if (target.material == nullptr || target.layer == nullptr) {
    return false;
  }
  return paint_layer_always_baked(target.material->paint_layers, target.layer, false);
}

const char *BKE_paint_layers_target_refusal(const PaintLayersTarget &target)
{
  if (BKE_paint_layers_target_is_frozen(target)) {
    return "The layer is frozen (bake); unfreeze it to paint";
  }
  if (target.layer != nullptr && target.mode == PaintLayersTargetMode::Content &&
      target.layer->source == MA_PAINT_LAYER_SOURCE_CONSTANT)
  {
    return "A Fill layer is a color: add a Correction to paint on it, or paint its mask";
  }
  /* A Material layer's content is its source baked into maps, which a re-bake rewrites: strokes
   * there would be lost, so like a Fill it is changed through a Correction or its mask. */
  if (target.layer != nullptr && target.mode == PaintLayersTargetMode::Content &&
      target.layer->source == MA_PAINT_LAYER_SOURCE_MATERIAL)
  {
    return "A Material layer shows its source material: add a Correction to paint on it, or "
           "paint its mask";
  }
  return nullptr;
}

Image *BKE_paint_layers_target_ensure_writable(Main &bmain,
                                               PaintLayersTarget &target,
                                               const int image_size,
                                               const float *new_channel_fill_linear)
{
  if (target.material == nullptr || target.layer == nullptr) {
    return nullptr;
  }
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(target.channel));

  if (target.mode == PaintLayersTargetMode::Mask) {
    if (target.mask_item == nullptr) {
      if (!BKE_paint_layers_mask_add(*target.material, target.layer, 1.0f)) {
        return nullptr;
      }
      const Vector<MaterialPaintLayer *> items = BKE_paint_layers_mask_items(*target.layer);
      if (items.is_empty()) {
        return nullptr;
      }
      target.mask_item = items.first();
    }
    MaterialPaintLayer *item = target.mask_item;
    if (Image *existing = paint_layer_mask_correction_image(*item, target.channel)) {
      return existing;
    }
    /* The map lives in the item's Base-Color channel record; the index is a scalar placeholder. */
    const bool fill_effect = BKE_paint_layers_source_type(*item) == PaintLayerSourceType::Constant;
    MaterialPaintLayerChannel *record = paint_layer_channel_find(
        *item, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    if (record == nullptr) {
      record = BKE_paint_layers_channel_add(
          *target.material, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
      if (record == nullptr) {
        return nullptr;
      }
    }
    float fill[4];
    if (new_channel_fill_linear != nullptr) {
      copy_v4_v4(fill, new_channel_fill_linear);
    }
    else if (fill_effect) {
      /* A constant item's first map keeps its look: opaque, filled with the constant. */
      BKE_paint_layers_correction_constant(*item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, fill);
      fill[3] = 1.0f;
    }
    else {
      /* A painted item starts transparent, so an unpainted item changes nothing. */
      zero_v4(fill);
    }
    Image *image = paint_layers_map_create(bmain, "Mask", image_size, false, fill);
    if (image == nullptr) {
      return nullptr;
    }
    /* One convention for every paint-layer map: the byte buffer stays straight (#IMA_ALPHA_STRAIGHT
     * from #paint_layers_map_create). The GPU texture upload premultiplies the straight bytes once,
     * the generator's Divide un-premultiplies, and the CPU reads straight and never divides; a
     * premultiplied byte buffer here would be premultiplied a second time at upload (C * A^2 in the
     * texture), which is the halo along a soft mask stroke. #IMA_GPU_LINEAR_PREMUL keeps the
     * texture scene-linear and pre-multiplied for correct filtering. */
    image->flag |= IMA_GPU_LINEAR_PREMUL;
    if (!BKE_paint_layers_channel_set_image(
            *target.material, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, image))
    {
      BKE_id_free(&bmain, image);
      return nullptr;
    }
    /* A constant item now reads its map; switch the effect so the generator uses it. */
    if (fill_effect) {
      BKE_paint_layers_correction_source_set(*target.material, item, MA_PAINT_LAYER_SOURCE_IMAGE);
    }
    return paint_layer_mask_correction_image(*item, target.channel);
  }

  /* Content. A Fill row is a colour and never grows a map: it is changed through a Correction or
   * its mask (#BKE_paint_layers_target_refusal). The entry points refuse before getting here; this
   * keeps any other caller from converting the row behind the user's back. */
  if (ELEM(target.layer->source, MA_PAINT_LAYER_SOURCE_CONSTANT, MA_PAINT_LAYER_SOURCE_MATERIAL)) {
    return nullptr;
  }
  MaterialPaintLayerChannel *record = paint_layer_channel_find(*target.layer, target.channel);
  const bool had_record = record != nullptr && record->state != MA_PAINT_LAYER_CHANNEL_ABSENT;
  if (record == nullptr) {
    record = BKE_paint_layers_channel_add(
        *target.material, target.layer, eMaterialPaintChannel(target.channel));
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

  float fill[4];
  if (had_record || BKE_paint_layers_role(*target.layer) != PaintLayerRole::Layer) {
    /* The record carried a flat value; the first map keeps that look. A correction's own alpha is
     * its coverage (#composite_correction_pixel_mask_factor's `corr_alpha`), unlike a plain row's,
     * so its fresh record's transparent default (#BKE_paint_layers_channel_add) has to reach the
     * map too -- an opaque neutral here would make it cover its row before a stroke lands. */
    copy_v4_v4(fill, record->value);
  }
  else if (new_channel_fill_linear != nullptr) {
    copy_v4_v4(fill, new_channel_fill_linear);
  }
  else {
    paint_layer_channel_neutral(target.channel, fill);
  }

  char name[192];
  paint_layers_channel_image_name(*target.layer, target.channel, name, sizeof(name));
  Image *image = paint_layers_map_create(bmain, name, image_size, info.is_color, fill);
  if (image == nullptr) {
    return nullptr;
  }
  if (BKE_paint_layers_role(*target.layer) != PaintLayerRole::Layer) {
    /* Transparent around every stroke: see #IMA_GPU_LINEAR_PREMUL. */
    image->flag |= IMA_GPU_LINEAR_PREMUL;
  }
  if (!BKE_paint_layers_channel_set_image(
          *target.material, target.layer, eMaterialPaintChannel(target.channel), image))
  {
    BKE_id_free(&bmain, image);
    return nullptr;
  }
  return record->image;
}

}  // namespace blender
