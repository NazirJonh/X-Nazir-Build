/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * See #BKE_paint_layers.hh: the description-only half of a layered material -- adding, removing
 * and looking rows up by marker. The node tree is generated from this description in phase 1 and
 * is deliberately never touched here; every mutation only marks it stale.
 */

#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_generate.hh"
#include "BKE_paint_material_resolve.hh"

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "paint_layers_intern.hh"

#include "MEM_guardedalloc.h"

#include "BKE_global.hh"
#include "BKE_idprop.hh"
#include "BKE_image.hh"
#include "BKE_image_partial_update.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_paint.hh"

#include "BLI_hash.hh"
#include "BLI_set.hh"

#include "BLT_translation.hh"

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_math_base.hh"
#include "BLI_math_vector.h"
#include "BLI_string.h"
#include "BLI_ustring.hh"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "DEG_depsgraph.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf_types.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"
#include "DNA_uuid_types.h"

namespace blender {

/** #MaterialPaintLayerBake::images is sized by the channel count; keep the two in step. */
static_assert(PAINT_MATERIAL_CHANNEL_NUM == 10);

/** #MaterialPaintLayer::channel_settings is indexed by channel; the literal size must match. */
static_assert(sizeof(MaterialPaintLayer::channel_settings) /
                  sizeof(MaterialPaintLayerChannelSettings) ==
              PAINT_MATERIAL_CHANNEL_NUM);

/** A description edit: the generated tree is stale until it is rebuilt. Shared with the bake file. */
void BKE_paint_layers_tag_edited(Material &ma)
{
  /* Any structural edit can change which maps the texture-paint slots name. */
  ma.paint_layers_flag |= MA_PAINT_LAYERS_REGEN | MA_PAINT_LAYERS_SLOTS_STALE;
  DEG_id_tag_update(&ma.id, ID_RECALC_SHADING);
}

namespace {

/**
 * Whether \a target, or any ancestor of it, carries a bake: only then does a value edit on \a target
 * invalidate a substituted node. Walks \a list and reports through the return whether the target was
 * found with a baked ancestor on the way.
 */
static bool paint_layer_or_ancestor_has_bake(const ListBase &list,
                                             const MaterialPaintLayer *target,
                                             const bool ancestor_has_bake)
{
  for (const MaterialPaintLayer &layer :
       *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&list))
  {
    /* A Material layer's bake is its source's maps, not a cache of the row: its values stay live
     * group inputs, so editing them is not topology. */
    const bool has_bake = ancestor_has_bake ||
                          (layer.bake != nullptr && layer.kind != MA_PAINT_LAYER_KIND_MATERIAL);
    if (&layer == target) {
      return has_bake;
    }
    if (paint_layer_or_ancestor_has_bake(layer.effects, target, has_bake) ||
        paint_layer_or_ancestor_has_bake(layer.mask_stack, target, has_bake))
    {
      return true;
    }
    if (paint_layer_or_ancestor_has_bake(layer.children, target, has_bake)) {
      return true;
    }
  }
  return false;
}

void paint_layers_tag_value_edited(Material &ma, const MaterialPaintLayer *edited)
{
  /* A value can be baked into a row's cache (the hash covers opacity/fill), and the value-only path
   * does not rebuild the tree, so it has to ask the bake planner for a re-bake as well. The planner
   * at K-1 re-bakes exactly the rows whose stored hash no longer matches. */
  ma.paint_layers_flag |= MA_PAINT_LAYERS_BAKE_STALE;
  /* The generated tree substituted only the baked rows, so only a value change to such a row (or
   * one of its ancestors) is topology. Anything else stays the free path an animation needs. */
  if (edited != nullptr && paint_layer_or_ancestor_has_bake(ma.paint_layers, edited, false)) {
    ma.paint_layers_flag |= MA_PAINT_LAYERS_REGEN;
  }
  /* The evaluated copy syncs the instance's values from its own copy of the description (see
   * #BKE_paint_layers_values_sync), and SHADING alone does not refresh that copy. An RNA property
   * update adds SYNC_TO_EVAL by itself; a direct BKE caller (the Outliner) has to ask for it. */
  DEG_id_tag_update(&ma.id, ID_RECALC_SHADING | ID_RECALC_SYNC_TO_EVAL);
}

/**
 * A strictly value-only edit: mark the bake stale and the shading dirty, never #MA_PAINT_LAYERS_REGEN.
 *
 * A per-channel opacity override is a value of the generated tree's per (row, channel) input -- the
 * socket set is fixed by topology, so changing the record cannot add or remove one -- and a baked
 * row re-bakes itself at K-1, which is where its substitution's rebuild comes from.
 */
void paint_layers_tag_value_only(Material &ma)
{
  ma.paint_layers_flag |= MA_PAINT_LAYERS_BAKE_STALE;
  /* SYNC_TO_EVAL for the same reason as #paint_layers_tag_value_edited. */
  DEG_id_tag_update(&ma.id, ID_RECALC_SHADING | ID_RECALC_SYNC_TO_EVAL);
}

/** Depth-first search for \a marker through \a list, its folders and its corrections. */
MaterialPaintLayer *paint_layer_find_in_list(const ListBase *list, const bUUID &marker)
{
  for (const MaterialPaintLayer &layer :
       *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(list))
  {
    if (BLI_uuid_equal(layer.marker, marker)) {
      return const_cast<MaterialPaintLayer *>(&layer);
    }
    if (MaterialPaintLayer *found = paint_layer_find_in_list(&layer.children, marker)) {
      return found;
    }
    if (MaterialPaintLayer *found = paint_layer_find_in_list(&layer.effects, marker)) {
      return found;
    }
    if (MaterialPaintLayer *found = paint_layer_find_in_list(&layer.mask_stack, marker)) {
      return found;
    }
  }
  return nullptr;
}

/** The record of \a layer's channels array that stands for \a channel, or null when it has none. */
MaterialPaintLayerChannel *paint_layer_channel_find(MaterialPaintLayer &layer,
                                                    const eMaterialPaintChannel channel)
{
  for (const int i : IndexRange(layer.channels_num)) {
    if (layer.channels[i].channel == channel) {
      return &layer.channels[i];
    }
  }
  return nullptr;
}

/** A fresh marker no row of \a ma's stack carries yet. */
bUUID paint_layer_unique_marker(const Material &ma)
{
  bUUID marker;
  do {
    marker = BLI_uuid_generate_random();
  } while (paint_layer_find_in_list(&const_cast<Material &>(ma).paint_layers, marker) != nullptr);
  return marker;
}

/**
 * A detached row with the usual defaults and a marker unique within \a ma. It is not linked yet:
 * #BKE_paint_layers_add links it into the stack, #BKE_paint_layers_correction_add into a parent's
 * correction list, and neither shares this through the other.
 */
MaterialPaintLayer *paint_layer_alloc(Material &ma,
                                      eMaterialPaintLayerKind kind,
                                      const char *name)
{
  MaterialPaintLayer *layer = MEM_new<MaterialPaintLayer>(__func__);
  layer->kind = kind;
  layer->blend = MA_PAINT_LAYER_BLEND_MIX;
  layer->flag = MA_PAINT_LAYER_ENABLED;
  layer->opacity = 1.0f;
  STRNCPY(layer->name, name != nullptr ? name : "");
  layer->marker = paint_layer_unique_marker(ma);
  return layer;
}

/** A material that owns a description is layered and generated in Locked mode; mark it stale. */
void paint_layer_mark_owned(Material &ma)
{
  ma.paint_layers_flag |= MA_PAINT_LAYERED | MA_PAINT_LAYERS_LOCKED;
  if (BLI_uuid_is_nil(ma.paint_layers_owner_uid)) {
    ma.paint_layers_owner_uid = BLI_uuid_generate_random();
  }
  BKE_paint_layers_tag_edited(ma);
}

/**
 * Deep copy of the branch at \a src: fresh markers for every row, a copy of the IDProperty group,
 * and -- only where \a copy_images asks for it -- copies of the channel and mask images. The
 * custom group pointer is carried over as-is; its user count is the caller's business.
 */
MaterialPaintLayer *paint_layer_branch_duplicate(Main &bmain,
                                                 const Material &ma,
                                                 const MaterialPaintLayer &src,
                                                 bool copy_images)
{
  MaterialPaintLayer *dst = static_cast<MaterialPaintLayer *>(MEM_dupalloc(&src));
  dst->next = nullptr;
  dst->prev = nullptr;
  dst->marker = paint_layer_unique_marker(ma);
  dst->children = {nullptr, nullptr};
  dst->effects = {nullptr, nullptr};
  dst->mask_stack = {nullptr, nullptr};
  dst->channels = nullptr;
  dst->properties = nullptr;

  for (const MaterialPaintLayer &child :
       *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&src.children))
  {
    BLI_addtail(&dst->children, paint_layer_branch_duplicate(bmain, ma, child, copy_images));
  }
  for (const MaterialPaintLayer &effect :
       *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&src.effects))
  {
    BLI_addtail(&dst->effects, paint_layer_branch_duplicate(bmain, ma, effect, copy_images));
  }
  for (const MaterialPaintLayer &mask_item :
       *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&src.mask_stack))
  {
    BLI_addtail(&dst->mask_stack, paint_layer_branch_duplicate(bmain, ma, mask_item, copy_images));
  }

  if (src.channels != nullptr && src.channels_num > 0) {
    dst->channels = static_cast<MaterialPaintLayerChannel *>(MEM_dupalloc(src.channels));
    dst->channels_num = src.channels_num;
    if (copy_images) {
      for (const int i : IndexRange(dst->channels_num)) {
        if (dst->channels[i].image != nullptr) {
          dst->channels[i].image = id_cast<Image *>(
              BKE_id_copy(&bmain, &dst->channels[i].image->id));
        }
      }
    }
  }
  if (src.properties != nullptr) {
    dst->properties = IDP_CopyProperty(src.properties);
  }

  return dst;
}

}  // namespace

bool BKE_paint_layers_subtree_contains(const MaterialPaintLayer &layer, const bUUID &marker)
{
  if (BLI_uuid_equal(layer.marker, marker)) {
    return true;
  }
  for (const MaterialPaintLayer &child :
       *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&layer.children))
  {
    if (BKE_paint_layers_subtree_contains(child, marker)) {
      return true;
    }
  }
  for (const MaterialPaintLayer &effect :
       *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&layer.effects))
  {
    if (BKE_paint_layers_subtree_contains(effect, marker)) {
      return true;
    }
  }
  for (const MaterialPaintLayer &mask_item :
       *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&layer.mask_stack))
  {
    if (BKE_paint_layers_subtree_contains(mask_item, marker)) {
      return true;
    }
  }
  return false;
}

bool paint_layers_is_layered(const Material &ma)
{
  return (ma.paint_layers_flag & MA_PAINT_LAYERED) != 0;
}

const PaintLayerKindInfo &BKE_paint_layers_kind_info(const int kind)
{
  static const PaintLayerKindInfo table[] = {
      {MA_PAINT_LAYER_KIND_PAINT, "PAINT", "Paint", false, false, false},
      {MA_PAINT_LAYER_KIND_FILL, "FILL", "Fill", false, true, false},
      {MA_PAINT_LAYER_KIND_MATERIAL, "MATERIAL", "Material", false, false, true},
      {MA_PAINT_LAYER_KIND_CORRECTION, "CORRECTION", "Correction", false, false, false},
      {MA_PAINT_LAYER_KIND_FOLDER, "FOLDER", "Folder", true, false, false},
      {MA_PAINT_LAYER_KIND_CUSTOM, "CUSTOM", "Custom", false, false, true},
  };
  for (const PaintLayerKindInfo &info : table) {
    if (info.kind == kind) {
      return info;
    }
  }
  /* An unknown kind reads as Paint, the same compatibility rule the DNA enum documents. */
  return table[0];
}

bool BKE_paint_layers_is_folder(const MaterialPaintLayer &layer)
{
  return BKE_paint_layers_kind_info(layer.kind).is_folder;
}

bool BKE_paint_layers_folder_is_pass_through(const Material &ma,
                                             const MaterialPaintLayer &folder)
{
  if (!BKE_paint_layers_is_folder(folder)) {
    return false;
  }
  /* A folder whose bake already stands in is the baked result, not its live children; keeping it
   * exactly as it was is the whole point of the substitution. #BKE_paint_layers_bake_is_valid is
   * the same validity test the generator and the compositor use, so a valid cache disables the
   * mode on both sides at once. */
  if (BKE_paint_layers_bake_is_valid(ma, folder)) {
    return false;
  }
  /* Any mask item or content correction would fold a per-pixel or per-channel factor into the
   * folder's own row; without them the only thing the folder contributes is grouping. The lists
   * are read directly because emptiness is the question, not which items are enabled: a disabled
   * mask item still has to flip the mode so toggling its visibility later stays a value edit. */
  if (folder.mask_stack.first != nullptr || folder.effects.first != nullptr) {
    return false;
  }
  /* The isolated folder computes `over(below, P/a, a)`; sequential over of the children is the
   * same only when every child is blended with the associative Normal/Mix operator at the folder's
   * full weight. Opacity below one breaks it for two or more children: scaling the whole isolated
   * result by `o` is not scaling each child's own factor by `o`, so opacity below one isolates.
   * Visibility is absent from this loop on purpose (see #BKE_paint_layers_effective_opacity): it
   * is folded into the children's factors as a value and must not change the mode. */
  for (int channel = 0; channel < PAINT_MATERIAL_CHANNEL_NUM; channel++) {
    if (BKE_paint_layers_channel_blend_effective(folder, channel) != MA_PAINT_LAYER_BLEND_MIX) {
      return false;
    }
    if (folder.opacity != 1.0f || folder.channel_settings[channel].opacity != 1.0f) {
      return false;
    }
  }
  return true;
}

PaintLayerSourceType BKE_paint_layers_source_type(const MaterialPaintLayer &layer)
{
  switch (layer.kind) {
    case MA_PAINT_LAYER_KIND_FILL:
      return PaintLayerSourceType::Constant;
    case MA_PAINT_LAYER_KIND_MATERIAL:
      return PaintLayerSourceType::Material;
    case MA_PAINT_LAYER_KIND_CUSTOM:
      return PaintLayerSourceType::NodeGroup;
    case MA_PAINT_LAYER_KIND_FOLDER:
      return PaintLayerSourceType::Stack;
    case MA_PAINT_LAYER_KIND_CORRECTION:
      return (layer.effect == MA_PAINT_LAYER_EFFECT_FILL) ? PaintLayerSourceType::Constant :
                                                            PaintLayerSourceType::Image;
    case MA_PAINT_LAYER_KIND_PAINT:
      return PaintLayerSourceType::Image;
  }
  /* An unknown kind composites as a Paint row, the same compatibility rule #PaintLayerKindInfo. */
  return PaintLayerSourceType::Image;
}

PaintLayerRole BKE_paint_layers_role(const MaterialPaintLayer &layer)
{
  if (layer.kind != MA_PAINT_LAYER_KIND_CORRECTION) {
    return PaintLayerRole::Layer;
  }
  return (layer.section == MA_PAINT_LAYER_SECTION_MASK) ? PaintLayerRole::MaskItem :
                                                          PaintLayerRole::Effect;
}

namespace {

void paint_layer_list_collect(const ListBase &list, Vector<const MaterialPaintLayer *> &r_items)
{
  for (const MaterialPaintLayer &item :
       *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&list))
  {
    r_items.append(&item);
  }
}

void paint_layer_list_collect(ListBase &list, Vector<MaterialPaintLayer *> &r_items)
{
  for (MaterialPaintLayer &item : *reinterpret_cast<ListBaseT<MaterialPaintLayer> *>(&list)) {
    r_items.append(&item);
  }
}

}  // namespace

Vector<MaterialPaintLayer *> BKE_paint_layers_effects(MaterialPaintLayer &layer)
{
  Vector<MaterialPaintLayer *> items;
  paint_layer_list_collect(layer.effects, items);
  return items;
}

Vector<const MaterialPaintLayer *> BKE_paint_layers_effects(const MaterialPaintLayer &layer)
{
  Vector<const MaterialPaintLayer *> items;
  paint_layer_list_collect(layer.effects, items);
  return items;
}

Vector<MaterialPaintLayer *> BKE_paint_layers_mask_items(MaterialPaintLayer &layer)
{
  Vector<MaterialPaintLayer *> items;
  paint_layer_list_collect(layer.mask_stack, items);
  return items;
}

Vector<const MaterialPaintLayer *> BKE_paint_layers_mask_items(const MaterialPaintLayer &layer)
{
  Vector<const MaterialPaintLayer *> items;
  paint_layer_list_collect(layer.mask_stack, items);
  return items;
}

bool BKE_paint_layers_fill_to_paint(Material &ma, MaterialPaintLayer &layer, float r_fill[4])
{
  copy_v4_v4(r_fill, layer.fill_color);
  if (!BKE_paint_layers_kind_info(layer.kind).uses_fill_color) {
    return false;
  }
  /* A Fill carries a value per channel (Base Color in `fill_color`, the rest in their records); a
   * Paint row carries the constant per channel, so each record keeps what the Fill showed before
   * the kind flips and `fill_color` is reset. */
  for (int i = 0; i < layer.channels_num; i++) {
    paint_layer_channel_constant(layer, layer.channels[i].channel, layer.channels[i].value);
  }
  BKE_paint_layers_kind_change(ma, &layer, MA_PAINT_LAYER_KIND_PAINT);
  return true;
}

namespace {

void paint_layers_flatten_list(const ListBase &list,
                               Vector<const MaterialPaintLayer *> &r_layers)
{
  for (const MaterialPaintLayer &layer :
       *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&list))
  {
    /* A correction row is reached through its owner's effects/mask_stack lists, not here. A Custom and a
     * Material layer alike take part through their bake, so they are walked like any other row. */
    if (layer.kind == MA_PAINT_LAYER_KIND_CORRECTION) {
      continue;
    }
    r_layers.append(&layer);
    if (BKE_paint_layers_is_folder(layer)) {
      paint_layers_flatten_list(layer.children, r_layers);
    }
  }
}

}  // namespace

void BKE_paint_layers_flatten(const Material &ma, Vector<const MaterialPaintLayer *> &r_layers)
{
  paint_layers_flatten_list(ma.paint_layers, r_layers);
}

float BKE_paint_layers_effective_opacity(const MaterialPaintLayer &layer)
{
  if ((layer.flag & MA_PAINT_LAYER_ENABLED) == 0) {
    return 0.0f;
  }
  /* A constant mask is a mask-stack element now; the stack carries it, not the factor. */
  return layer.opacity;
}

int BKE_paint_layers_channel_blend_effective(const MaterialPaintLayer &layer, const int channel)
{
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
    return layer.blend;
  }
  const int blend = layer.channel_settings[channel].blend;
  return (blend < 0) ? layer.blend : blend;
}

float BKE_paint_layers_channel_opacity_effective(const MaterialPaintLayer &layer, const int channel)
{
  const float multiplier = (channel >= 0 && channel < PAINT_MATERIAL_CHANNEL_NUM) ?
                               layer.channel_settings[channel].opacity :
                               1.0f;
  return BKE_paint_layers_effective_opacity(layer) * multiplier;
}

void BKE_paint_layers_correction_constant(const MaterialPaintLayer &correction,
                                          const eMaterialPaintChannel channel,
                                          float r_color[4])
{
  for (int i = 0; i < correction.channels_num; i++) {
    if (correction.channels[i].channel == channel) {
      if (correction.effect == MA_PAINT_LAYER_EFFECT_FILL) {
        copy_v4_v4(r_color, correction.fill_color);
        return;
      }
      copy_v4_v4(r_color, correction.channels[i].value);
      return;
    }
  }
  /* No record in this channel: a Fill still contributes its colour, a Paint contributes nothing. */
  if (correction.effect == MA_PAINT_LAYER_EFFECT_FILL) {
    copy_v4_v4(r_color, correction.fill_color);
    return;
  }
  zero_v4(r_color);
}

void BKE_paint_layers_channel_bottom_color(const eMaterialPaintChannel channel, float r_color[4])
{
  zero_v4(r_color);
  r_color[3] = 1.0f;
  switch (channel) {
    case PAINT_MATERIAL_CHANNEL_ROUGHNESS:
    case PAINT_MATERIAL_CHANNEL_SPECULAR:
    case PAINT_MATERIAL_CHANNEL_HEIGHT:
      /* A neutral mid value, so a partially covered scalar row fades towards the Principled
       * default rather than towards zero. */
      r_color[0] = r_color[1] = r_color[2] = 0.5f;
      break;
    case PAINT_MATERIAL_CHANNEL_NORMAL:
      /* Flat tangent-space: straight out of the surface. */
      r_color[0] = 0.5f;
      r_color[1] = 0.5f;
      r_color[2] = 1.0f;
      break;
    case PAINT_MATERIAL_CHANNEL_ALPHA:
    case PAINT_MATERIAL_CHANNEL_AO:
      r_color[0] = r_color[1] = r_color[2] = 1.0f;
      break;
    case PAINT_MATERIAL_CHANNEL_BASE_COLOR:
    case PAINT_MATERIAL_CHANNEL_METALLIC:
    case PAINT_MATERIAL_CHANNEL_CUSTOM:
    case PAINT_MATERIAL_CHANNEL_EMISSION:
      break;
  }
}

namespace {

/** The colorspace \a image stores its pixels in, or null when it names none. */
const ColorSpace *paint_layer_image_colorspace(const Image &image)
{
  if (image.colorspace_settings.name[0] == '\0') {
    return nullptr;
  }
  return IMB_colormanagement_space_get_named(image.colorspace_settings.name);
}

/** Whether a conversion through \a colorspace would do anything at all. */
bool paint_layer_colorspace_is_identity(const ColorSpace *colorspace)
{
  return colorspace == nullptr || IMB_colormanagement_space_is_data(colorspace) ||
         IMB_colormanagement_space_is_scene_linear(colorspace);
}

}  // namespace

void BKE_paint_layers_sample_to_linear(const Image &image, float rgba[4])
{
  const ColorSpace *colorspace = paint_layer_image_colorspace(image);
  if (paint_layer_colorspace_is_identity(colorspace)) {
    return;
  }
  IMB_colormanagement_colorspace_to_scene_linear_v4(rgba, false, colorspace);
}

void BKE_paint_layers_sample_from_linear(const Image &image, float rgba[4])
{
  const ColorSpace *colorspace = paint_layer_image_colorspace(image);
  if (paint_layer_colorspace_is_identity(colorspace)) {
    return;
  }
  IMB_colormanagement_scene_linear_to_colorspace(rgba, 1, 1, 4, colorspace);
}

void BKE_paint_layers_constant_to_linear(const eMaterialPaintChannel channel,
                                         const float rgba[4],
                                         float r_linear[4])
{
  /* The description stores RNA `PROP_COLOR` values, which are scene linear already; a scalar
   * channel's flat value is a plain number, likewise already in the shader's space. */
  UNUSED_VARS(channel);
  copy_v4_v4(r_linear, rgba);
}

int BKE_paint_layers_blend_to_ramp(const eMaterialPaintLayerBlend blend)
{
  switch (blend) {
    case MA_PAINT_LAYER_BLEND_MIX:
      return MA_RAMP_BLEND;
    case MA_PAINT_LAYER_BLEND_MULTIPLY:
      return MA_RAMP_MULT;
    case MA_PAINT_LAYER_BLEND_OVERLAY:
      return MA_RAMP_OVERLAY;
    case MA_PAINT_LAYER_BLEND_ADD:
      return MA_RAMP_ADD;
    case MA_PAINT_LAYER_BLEND_DARKEN:
      return MA_RAMP_DARK;
    case MA_PAINT_LAYER_BLEND_BURN:
      return MA_RAMP_BURN;
    case MA_PAINT_LAYER_BLEND_LIGHTEN:
      return MA_RAMP_LIGHT;
    case MA_PAINT_LAYER_BLEND_SCREEN:
      return MA_RAMP_SCREEN;
    case MA_PAINT_LAYER_BLEND_DODGE:
      return MA_RAMP_DODGE;
    case MA_PAINT_LAYER_BLEND_SUBTRACT:
      return MA_RAMP_SUB;
    case MA_PAINT_LAYER_BLEND_DIVIDE:
      return MA_RAMP_DIV;
    case MA_PAINT_LAYER_BLEND_DIFFERENCE:
      return MA_RAMP_DIFF;
    case MA_PAINT_LAYER_BLEND_EXCLUSION:
      return MA_RAMP_EXCLUSION;
    case MA_PAINT_LAYER_BLEND_SOFT_LIGHT:
      return MA_RAMP_SOFT;
    case MA_PAINT_LAYER_BLEND_LINEAR_LIGHT:
      return MA_RAMP_LINEAR;
    case MA_PAINT_LAYER_BLEND_HUE:
      return MA_RAMP_HUE;
    case MA_PAINT_LAYER_BLEND_SATURATION:
      return MA_RAMP_SAT;
    case MA_PAINT_LAYER_BLEND_COLOR:
      return MA_RAMP_COLOR;
    case MA_PAINT_LAYER_BLEND_VALUE:
      return MA_RAMP_VAL;
    case MA_PAINT_LAYER_BLEND_NORMAL_COMBINE:
      return MA_RAMP_BLEND;
  }
  return MA_RAMP_BLEND;
}

namespace {

/** The IDProperty key a Custom group's interface socket carries its role under. */
constexpr const char *CUSTOM_ROLE_PROP = "pbr_custom_role";

struct CustomChannelName {
  const char *identifier;
  eMaterialPaintChannel channel;
};

/** The channel identifiers a `COLOR:`/`BELOW:` role may name. */
const CustomChannelName custom_channel_names[] = {
    {"BASE_COLOR", PAINT_MATERIAL_CHANNEL_BASE_COLOR},
    {"METALLIC", PAINT_MATERIAL_CHANNEL_METALLIC},
    {"ROUGHNESS", PAINT_MATERIAL_CHANNEL_ROUGHNESS},
    {"SPECULAR", PAINT_MATERIAL_CHANNEL_SPECULAR},
    {"NORMAL", PAINT_MATERIAL_CHANNEL_NORMAL},
    {"HEIGHT", PAINT_MATERIAL_CHANNEL_HEIGHT},
    {"ALPHA", PAINT_MATERIAL_CHANNEL_ALPHA},
    {"AO", PAINT_MATERIAL_CHANNEL_AO},
    {"EMISSION", PAINT_MATERIAL_CHANNEL_EMISSION},
};

void custom_role_set(IDProperty *&properties, const char *role)
{
  if (properties == nullptr) {
    IDPropertyTemplate val = {};
    properties = IDP_New(IDP_GROUP, &val, "properties");
  }
  IDProperty *prop = IDP_GetPropertyTypeFromGroup(properties, CUSTOM_ROLE_PROP, IDP_STRING);
  if (prop != nullptr) {
    IDP_AssignString(prop, role);
    return;
  }
  IDP_AddToGroup(properties, IDP_NewString(role, CUSTOM_ROLE_PROP));
}

std::optional<eMaterialPaintChannel> custom_channel_from_identifier(const char *identifier)
{
  for (const CustomChannelName &entry : custom_channel_names) {
    if (STREQ(entry.identifier, identifier)) {
      return entry.channel;
    }
  }
  return std::nullopt;
}

const char *custom_channel_identifier(const eMaterialPaintChannel channel)
{
  for (const CustomChannelName &entry : custom_channel_names) {
    if (entry.channel == channel) {
      return entry.identifier;
    }
  }
  return nullptr;
}

enum class CustomRoleKind : int8_t {
  None,
  Below,
  Color,
  Coverage,
  CoverageBelow,
  UV,
  Unknown,
};

struct CustomRole {
  CustomRoleKind kind = CustomRoleKind::None;
  /** The channel identifier after `BELOW:`/`COLOR:`, or null. */
  const char *channel = nullptr;
};

CustomRole custom_role_parse(const char *role)
{
  if (role == nullptr) {
    return {};
  }
  if (STRPREFIX(role, "BELOW:")) {
    return {CustomRoleKind::Below, role + 6};
  }
  if (STRPREFIX(role, "COLOR:")) {
    return {CustomRoleKind::Color, role + 6};
  }
  if (STREQ(role, "COVERAGE")) {
    return {CustomRoleKind::Coverage, nullptr};
  }
  if (STREQ(role, "COVERAGE_BELOW")) {
    return {CustomRoleKind::CoverageBelow, nullptr};
  }
  if (STREQ(role, "UV")) {
    return {CustomRoleKind::UV, nullptr};
  }
  return {CustomRoleKind::Unknown, nullptr};
}

bool custom_role_is_input(const CustomRoleKind kind)
{
  return ELEM(kind, CustomRoleKind::Below, CustomRoleKind::CoverageBelow, CustomRoleKind::UV);
}

bool custom_role_is_output(const CustomRoleKind kind)
{
  return ELEM(kind, CustomRoleKind::Color, CustomRoleKind::Coverage);
}

const char *custom_role_channel_expected_type(const eMaterialPaintChannel channel)
{
  if (channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
    return "NodeSocketVector";
  }
  return BKE_paint_material_channel_info(channel).is_color ? "NodeSocketColor" :
                                                             "NodeSocketFloat";
}

/** Validate one socket's role and append issues; the `seen_` lists catch duplicate roles. */
void custom_role_validate_socket(const MaterialPaintLayer &layer,
                                 const bNodeTreeInterfaceSocket &socket,
                                 const bool is_input,
                                 Vector<std::string> &seen_inputs,
                                 Vector<std::string> &seen_outputs,
                                 Vector<PaintLayersIssue> &r_issues)
{
  const char *role = custom_role_get(socket.properties);
  if (role == nullptr) {
    return;
  }
  auto issue = [&](const PaintLayersIssueCode code, const int channel, const char *text) {
    PaintLayersIssue out;
    out.layer = layer.marker;
    out.correction = {};
    out.channel = channel;
    out.code = code;
    out.text = text;
    r_issues.append(out);
  };

  const CustomRole parsed = custom_role_parse(role);
  if (parsed.kind == CustomRoleKind::Unknown) {
    issue(PaintLayersIssueCode::CustomUnknownRole, 0, TIP_("Unknown paint layer role"));
    return;
  }
  if (is_input && custom_role_is_output(parsed.kind)) {
    issue(PaintLayersIssueCode::CustomRoleDirection,
          0,
          TIP_("This role belongs on a group output, not an input"));
    return;
  }
  if (!is_input && custom_role_is_input(parsed.kind)) {
    issue(PaintLayersIssueCode::CustomRoleDirection,
          0,
          TIP_("This role belongs on a group input, not an output"));
    return;
  }

  Vector<std::string> &seen = is_input ? seen_inputs : seen_outputs;
  for (const std::string &other : seen) {
    if (other == role) {
      issue(PaintLayersIssueCode::CustomDuplicateRole, 0, TIP_("Duplicate paint layer role"));
      return;
    }
  }
  seen.append(role);

  if (parsed.kind == CustomRoleKind::Below || parsed.kind == CustomRoleKind::Color) {
    const std::optional<eMaterialPaintChannel> channel = custom_channel_from_identifier(
        parsed.channel);
    if (!channel.has_value()) {
      issue(PaintLayersIssueCode::CustomUnknownChannel, 0, TIP_("Unknown paint channel name"));
      return;
    }
    const char *expected = custom_role_channel_expected_type(*channel);
    if (socket.socket_type != nullptr && !STREQ(socket.socket_type, expected)) {
      issue(PaintLayersIssueCode::CustomSocketType,
            int(*channel),
            TIP_("The socket type does not match the channel it names"));
      return;
    }
  }
  else if (parsed.kind == CustomRoleKind::Coverage ||
           parsed.kind == CustomRoleKind::CoverageBelow)
  {
    if (socket.socket_type != nullptr && !STREQ(socket.socket_type, "NodeSocketFloat")) {
      issue(PaintLayersIssueCode::CustomSocketType, 0, TIP_("Coverage must be a Float socket"));
      return;
    }
  }
  else if (parsed.kind == CustomRoleKind::UV) {
    if (socket.socket_type != nullptr &&
        !STRPREFIX(socket.socket_type, "NodeSocketVector") &&
        !STRPREFIX(socket.socket_type, "NodeSocketFloat") &&
        !STRPREFIX(socket.socket_type, "NodeSocketInt"))
    {
      issue(PaintLayersIssueCode::CustomSocketType, 0, TIP_("UV must be a vector socket"));
      return;
    }
  }
}

}  // namespace

namespace {

void custom_interface_role_socket_add(bNodeTreeInterface &interface,
                                      const char *name,
                                      const char *socket_type,
                                      const NodeTreeInterfaceSocketFlag direction,
                                      const char *role)
{
  bNodeTreeInterfaceSocket *socket = interface.add_socket(name, "", socket_type, direction,
                                                          nullptr);
  if (socket != nullptr && role != nullptr) {
    custom_role_set(socket->properties, role);
  }
}

}  // namespace

PaintLayerCustomRole BKE_paint_layers_custom_role_kind(const char *role)
{
  switch (custom_role_parse(role).kind) {
    case CustomRoleKind::None:
      return PaintLayerCustomRole::None;
    case CustomRoleKind::Below:
      return PaintLayerCustomRole::Below;
    case CustomRoleKind::Color:
      return PaintLayerCustomRole::Color;
    case CustomRoleKind::Coverage:
      return PaintLayerCustomRole::Coverage;
    case CustomRoleKind::CoverageBelow:
      return PaintLayerCustomRole::CoverageBelow;
    case CustomRoleKind::UV:
      return PaintLayerCustomRole::UV;
    case CustomRoleKind::Unknown:
      break;
  }
  return PaintLayerCustomRole::Unknown;
}

int BKE_paint_layers_custom_role_channel(const char *role)
{
  const CustomRole parsed = custom_role_parse(role);
  if (parsed.kind != CustomRoleKind::Color && parsed.kind != CustomRoleKind::Below) {
    return -1;
  }
  const std::optional<eMaterialPaintChannel> channel = custom_channel_from_identifier(
      parsed.channel);
  return channel.has_value() ? int(*channel) : -1;
}

const char *BKE_paint_layers_custom_channel_identifier(const int channel)
{
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
    return nullptr;
  }
  return custom_channel_identifier(eMaterialPaintChannel(channel));
}

bool BKE_paint_layers_custom_channel_add(Main &bmain,
                                         Material &ma,
                                         MaterialPaintLayer &layer,
                                         const eMaterialPaintChannel channel)
{
  if (layer.kind != MA_PAINT_LAYER_KIND_CUSTOM) {
    return false;
  }
  if (layer.custom_group == nullptr) {
    char name[128];
    SNPRINTF(name, "Custom Layer (%s)", ma.id.name + 2);
    bNodeTree *group = bke::node_tree_add_tree(&bmain, name, "ShaderNodeTree");
    if (group == nullptr) {
      return false;
    }
    layer.custom_group = group;
  }
  const char *identifier = custom_channel_identifier(channel);
  if (identifier == nullptr) {
    return false;
  }
  const char *channel_type = custom_role_channel_expected_type(channel);
  const char *ui_name = BKE_paint_material_channel_info(channel).ui_name;

  /* The group may already declare the channel: one pair of roles per channel. */
  for (const bNodeTreeInterfaceSocket *socket : layer.custom_group->interface_outputs()) {
    const CustomRole existing = custom_role_parse(custom_role_get(socket->properties));
    if (existing.kind == CustomRoleKind::Color && existing.channel != nullptr &&
        STREQ(existing.channel, identifier))
    {
      return false;
    }
  }

  char below_name[160];
  SNPRINTF(below_name, "%s Below", ui_name);
  char color_name[160];
  SNPRINTF(color_name, "%s", ui_name);
  char below_role[128];
  SNPRINTF(below_role, "BELOW:%s", identifier);
  char color_role[128];
  SNPRINTF(color_role, "COLOR:%s", identifier);

  custom_interface_role_socket_add(
      layer.custom_group->tree_interface, below_name, channel_type, NODE_INTERFACE_SOCKET_INPUT,
      below_role);
  custom_interface_role_socket_add(
      layer.custom_group->tree_interface, color_name, channel_type, NODE_INTERFACE_SOCKET_OUTPUT,
      color_role);
  BKE_ntree_update_tag_all(layer.custom_group);
  BKE_ntree_update_after_single_tree_change(bmain, *layer.custom_group);
  BKE_paint_layers_tag_edited(ma);
  return true;
}

MaterialPaintLayer *BKE_paint_layers_custom_layer_add(Main &bmain,
                                                      Material &ma,
                                                      const char *name,
                                                      MaterialPaintLayer *anchor,
                                                      const PaintLayerPlace place)
{
  const char *layer_name = (name != nullptr && name[0] != '\0') ? name : "Custom";
  char group_name[128];
  SNPRINTF(group_name, "Custom Layer (%s)", ma.id.name + 2);
  bNodeTree *group = bke::node_tree_add_tree(&bmain, group_name, "ShaderNodeTree");
  if (group == nullptr) {
    return nullptr;
  }

  bNodeTreeInterface &interface = group->tree_interface;
  bNodeTreeInterfaceSocket *below_iface = interface.add_socket(
      "Below", "", "NodeSocketColor", NODE_INTERFACE_SOCKET_INPUT, nullptr);
  bNodeTreeInterfaceSocket *uv_iface = interface.add_socket(
      "UV", "", "NodeSocketVector", NODE_INTERFACE_SOCKET_INPUT, nullptr);
  bNodeTreeInterfaceSocket *color_iface = interface.add_socket(
      "Color", "", "NodeSocketColor", NODE_INTERFACE_SOCKET_OUTPUT, nullptr);
  bNodeTreeInterfaceSocket *coverage_iface = interface.add_socket(
      "Coverage", "", "NodeSocketFloat", NODE_INTERFACE_SOCKET_OUTPUT, nullptr);
  if (below_iface != nullptr) {
    custom_role_set(below_iface->properties, "BELOW:BASE_COLOR");
  }
  if (uv_iface != nullptr) {
    custom_role_set(uv_iface->properties, "UV");
  }
  if (color_iface != nullptr) {
    custom_role_set(color_iface->properties, "COLOR:BASE_COLOR");
  }
  if (coverage_iface != nullptr) {
    custom_role_set(coverage_iface->properties, "COVERAGE");
  }
  const char *below_id = (below_iface != nullptr) ? below_iface->identifier : nullptr;
  const char *color_id = (color_iface != nullptr) ? color_iface->identifier : nullptr;
  const char *coverage_id = (coverage_iface != nullptr) ? coverage_iface->identifier : nullptr;

  bNode *group_input = bke::node_add_node(nullptr, *group, "NodeGroupInput"_ustr);
  bNode *group_output = bke::node_add_node(nullptr, *group, "NodeGroupOutput"_ustr);
  BKE_ntree_update_tag_all(group);
  BKE_ntree_update_after_single_tree_change(bmain, *group);

  UNUSED_VARS(below_id, color_id, coverage_id);
  if (group_input != nullptr && group_output != nullptr) {
    /* The default template passes the row below through: the first interface input (Below) feeds
     * the first interface output (Color); the second output (Coverage) stays an opaque constant.
     * The sockets follow the interface order the update just built. */
    bNodeSocket *below_socket = static_cast<bNodeSocket *>(group_input->outputs.first);
    bNodeSocket *color_socket = static_cast<bNodeSocket *>(group_output->inputs.first);
    if (below_socket != nullptr && color_socket != nullptr) {
      bke::node_add_link(*group, *group_input, *below_socket, *group_output, *color_socket);
    }
    bNodeSocket *coverage_socket = static_cast<bNodeSocket *>(group_output->inputs.last);
    if (coverage_socket != nullptr && coverage_socket->default_value != nullptr) {
      static_cast<bNodeSocketValueFloat *>(coverage_socket->default_value)->value = 1.0f;
    }
    /* The body changed after the update above, so the runtime topology has to be rebuilt before the
     * first render samples it; otherwise the template bakes empty. */
    BKE_ntree_update_tag_all(group);
    BKE_ntree_update_after_single_tree_change(bmain, *group);
  }

  MaterialPaintLayer *layer = BKE_paint_layers_add(
      ma, MA_PAINT_LAYER_KIND_CUSTOM, layer_name, anchor, place);
  if (layer == nullptr) {
    BKE_id_free(&bmain, &group->id);
    return nullptr;
  }
  /* The group belongs to the layer alone; the tree's initial Main user is the layer's. */
  layer->custom_group = group;
  BKE_paint_layers_tag_edited(ma);
  BKE_paint_layers_active_set(ma, layer->marker);
  return layer;
}

void BKE_paint_layers_custom_properties_sync(Material &ma)
{
  Vector<const MaterialPaintLayer *> layers;
  BKE_paint_layers_flatten(ma, layers);
  for (const MaterialPaintLayer *layer : layers) {
    if (layer->kind != MA_PAINT_LAYER_KIND_CUSTOM || layer->custom_group == nullptr) {
      continue;
    }
    MaterialPaintLayer *writable = const_cast<MaterialPaintLayer *>(layer);
    for (const bNodeTreeInterfaceSocket *socket : layer->custom_group->interface_inputs()) {
      /* A socket with a role is part of the contract; one without is a user parameter whose value
       * lives on the layer. */
      const char *role = custom_role_get(socket->properties);
      if (role != nullptr && role[0] != '\0') {
        continue;
      }
      if (socket->identifier == nullptr || socket->socket_type == nullptr ||
          !STREQ(socket->socket_type, "NodeSocketFloat"))
      {
        continue;
      }
      if (writable->properties != nullptr &&
          IDP_GetPropertyTypeFromGroup(writable->properties, socket->identifier, IDP_DOUBLE) !=
              nullptr)
      {
        continue;
      }
      double value = 0.0;
      if (socket->socket_data != nullptr) {
        value = double(static_cast<bNodeSocketValueFloat *>(socket->socket_data)->value);
      }
      if (writable->properties == nullptr) {
        IDPropertyTemplate group_val = {};
        writable->properties = IDP_New(IDP_GROUP, &group_val, "properties");
      }
      IDPropertyTemplate val = {};
      val.d = value;
      IDP_AddToGroup(writable->properties, IDP_New(IDP_DOUBLE, &val, socket->identifier));
    }
  }
}

void BKE_paint_layers_custom_channels_get(const MaterialPaintLayer &layer,
                                          Vector<int> &r_channels)
{
  r_channels.clear();
  if (layer.kind != MA_PAINT_LAYER_KIND_CUSTOM || layer.custom_group == nullptr) {
    return;
  }
  for (const bNodeTreeInterfaceSocket *socket : layer.custom_group->interface_outputs()) {
    const char *role = custom_role_get(socket->properties);
    const CustomRole parsed = custom_role_parse(role);
    if (parsed.kind != CustomRoleKind::Color) {
      continue;
    }
    const std::optional<eMaterialPaintChannel> channel = custom_channel_from_identifier(
        parsed.channel);
    if (channel.has_value() && !r_channels.contains(int(*channel))) {
      r_channels.append(int(*channel));
    }
  }
}

void BKE_paint_layers_issues_get(const Material &ma, Vector<PaintLayersIssue> &r_issues)
{
  r_issues.clear();
  Vector<const MaterialPaintLayer *> layers;
  BKE_paint_layers_flatten(ma, layers);
  for (const MaterialPaintLayer *layer : layers) {
    if (BKE_paint_layers_is_folder(*layer)) {
      bool has_map = false;
      for (const int i : IndexRange(layer->channels_num)) {
        if (layer->channels[i].image != nullptr) {
          has_map = true;
          break;
        }
      }
      if (has_map) {
        PaintLayersIssue issue;
        issue.layer = layer->marker;
        issue.correction = {};
        issue.channel = 0;
        issue.code = PaintLayersIssueCode::FolderHasMaps;
        issue.text = TIP_("A folder must not have maps of its own; its channels come from its "
                          "children");
        r_issues.append(issue);
      }
    }
    else if (!BLI_listbase_is_empty(&layer->children)) {
      PaintLayersIssue issue;
      issue.layer = layer->marker;
      issue.correction = {};
      issue.channel = 0;
      issue.code = PaintLayersIssueCode::NonFolderHasChildren;
      issue.text = TIP_("Only a folder holds other layers; this row's nested rows are ignored");
      r_issues.append(issue);
    }
    if (layer->kind == MA_PAINT_LAYER_KIND_CUSTOM && layer->custom_group != nullptr) {
      Vector<std::string> seen_inputs;
      Vector<std::string> seen_outputs;
      for (const bNodeTreeInterfaceSocket *socket : layer->custom_group->interface_inputs()) {
        custom_role_validate_socket(
            *layer, *socket, true, seen_inputs, seen_outputs, r_issues);
      }
      for (const bNodeTreeInterfaceSocket *socket : layer->custom_group->interface_outputs()) {
        custom_role_validate_socket(
            *layer, *socket, false, seen_inputs, seen_outputs, r_issues);
      }
    }
    if (!paint_layer_channel_present(*layer, PAINT_MATERIAL_CHANNEL_NORMAL)) {
      continue;
    }
    for (const MaterialPaintLayer &effect :
         *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&layer->effects))
    {
      if (effect.effect != MA_PAINT_LAYER_EFFECT_FILL ||
          (effect.flag & MA_PAINT_LAYER_ENABLED) == 0)
      {
        continue;
      }
      PaintLayersIssue issue;
      issue.layer = layer->marker;
      issue.correction = effect.marker;
      issue.channel = PAINT_MATERIAL_CHANNEL_NORMAL;
      issue.code = PaintLayersIssueCode::FillCorrectionOnNormal;
      issue.text = TIP_("A Fill correction has no effect on the Normal channel");
      r_issues.append(issue);
    }
  }
}

/** The row whose effects/mask_stack holds \a correction, searching \a list and its folders. */
static MaterialPaintLayer *paint_layer_correction_owner(ListBase &list,
                                                        const MaterialPaintLayer &correction)
{
  for (MaterialPaintLayer &layer : *reinterpret_cast<ListBaseT<MaterialPaintLayer> *>(&list)) {
    for (const MaterialPaintLayer &candidate :
         *reinterpret_cast<ListBaseT<MaterialPaintLayer> *>(&layer.effects))
    {
      if (&candidate == &correction) {
        return &layer;
      }
    }
    for (const MaterialPaintLayer &candidate :
         *reinterpret_cast<ListBaseT<MaterialPaintLayer> *>(&layer.mask_stack))
    {
      if (&candidate == &correction) {
        return &layer;
      }
    }
    if (MaterialPaintLayer *owner = paint_layer_correction_owner(layer.children, correction)) {
      return owner;
    }
    if (MaterialPaintLayer *owner = paint_layer_correction_owner(layer.effects, correction)) {
      return owner;
    }
    if (MaterialPaintLayer *owner = paint_layer_correction_owner(layer.mask_stack, correction)) {
      return owner;
    }
  }
  return nullptr;
}

MaterialPaintLayer *BKE_paint_layers_add(Material &ma,
                                         eMaterialPaintLayerKind kind,
                                         const char *name,
                                         MaterialPaintLayer *anchor,
                                         PaintLayerPlace place)
{
  /* Beside a correction would mean into its owner's corrections list, where a layer is neither
   * listed as a row nor composited as one (it would silently ride on the owner's visibility).
   * Any caller anchoring on the active row can meet a correction, so the owner row stands in. */
  if (anchor != nullptr && BKE_paint_layers_role(*anchor) != PaintLayerRole::Layer &&
      kind != MA_PAINT_LAYER_KIND_CORRECTION)
  {
    anchor = paint_layer_correction_owner(ma.paint_layers, *anchor);
    if (anchor == nullptr) {
      return nullptr;
    }
  }

  MaterialPaintLayer *layer = paint_layer_alloc(ma, kind, name);

  if (anchor == nullptr) {
    BLI_addtail(&ma.paint_layers, layer);
  }
  else {
    /* Every place validates its anchor: a row may never be linked under a foreign material. */
    ListBase *owner = paint_layer_owner_list(&ma.paint_layers, anchor);
    if (owner == nullptr) {
      MEM_delete(layer);
      return nullptr;
    }
    if (place == PaintLayerPlace::Into) {
      /* Only a folder holds other rows. BKE never changes a kind implicitly: promoting a Paint or
       * Fill layer to a folder would make its maps ignored (FolderHasMaps) and the painted
       * result would silently disappear, so a UI that drops "into" a plain row groups instead. */
      if (!BKE_paint_layers_is_folder(*anchor)) {
        MEM_delete(layer);
        return nullptr;
      }
      BLI_addtail(&anchor->children, layer);
    }
    else if (place == PaintLayerPlace::Above) {
      BLI_insertlinkafter(owner, anchor, layer);
    }
    else {
      BLI_insertlinkbefore(owner, anchor, layer);
    }
  }

  paint_layer_mark_owned(ma);
  return layer;
}

bool BKE_paint_layers_remove(Material &ma, MaterialPaintLayer *layer)
{
  if (layer == nullptr) {
    return false;
  }
  ListBase *owner = paint_layer_owner_list(&ma.paint_layers, layer);
  if (owner == nullptr) {
    return false;
  }

  const bool clears_active = !BLI_uuid_is_nil(ma.active_layer_marker) &&
                             BKE_paint_layers_subtree_contains(*layer, ma.active_layer_marker);

  BLI_remlink(owner, layer);
  BKE_material_paint_layer_free(layer);

  if (clears_active) {
    ma.active_layer_marker = {};
  }
  BKE_paint_layers_tag_edited(ma);
  return true;
}

MaterialPaintLayer *BKE_paint_layers_find(Material &ma, const bUUID &marker)
{
  return paint_layer_find_in_list(&ma.paint_layers, marker);
}

void BKE_paint_layers_active_set(Material &ma, const bUUID &marker)
{
  /* Leaving a row is when its deferred bake becomes due. The editor's Material-row planner reads
   * this after the depsgraph update; the CPU planner's #MA_PAINT_LAYERS_BAKE_STALE cannot carry the
   * signal because it is cleared at the K-1 point, before that update runs. */
  if (!BLI_uuid_equal(ma.active_layer_marker, marker)) {
    ma.paint_layers_flag |= MA_PAINT_LAYERS_MATERIAL_BAKE_DUE;
    /* The row and its ancestor folders left behind may carry a cache that has to catch up, and the
     * chain that bakes Material rows by hand (#material_bake_layered_rows_ensure) only knows
     * Material rows. This signal makes the K-1 planner (#BKE_paint_layers_bake_ensure, reached
     * from #BKE_paint_layers_regenerate_tagged) pick the stale non-Material rows too; it is not
     * cleared until no baked row is invalid any more. */
    ma.paint_layers_flag |= MA_PAINT_LAYERS_BAKE_STALE;
  }
  ma.active_layer_marker = marker;
  /* The slots name the active layer's maps, so moving the cursor invalidates them. */
  ma.paint_layers_flag |= MA_PAINT_LAYERS_SLOTS_STALE;
}

bUUID BKE_paint_layers_active_get(const Material &ma)
{
  return ma.active_layer_marker;
}

bool BKE_paint_layers_move(Material &ma,
                           MaterialPaintLayer *layer,
                           MaterialPaintLayer *anchor,
                           PaintLayerPlace place)
{
  if (layer == nullptr) {
    return false;
  }
  ListBase *owner = paint_layer_owner_list(&ma.paint_layers, layer);
  if (owner == nullptr) {
    return false;
  }
  if (anchor == layer) {
    return false;
  }
  if (anchor != nullptr) {
    if (paint_layer_owner_list(&ma.paint_layers, anchor) == nullptr) {
      return false;
    }
    /* A row moved into its own subtree would walk itself to reach itself. */
    if (BKE_paint_layers_subtree_contains(*layer, anchor->marker)) {
      return false;
    }
    /* Only a folder holds other rows, and BKE never promotes a kind implicitly; refuse before
     * touching the lists so the move is atomic. */
    if (place == PaintLayerPlace::Into && !BKE_paint_layers_is_folder(*anchor)) {
      return false;
    }
  }

  BLI_remlink(owner, layer);
  if (anchor == nullptr) {
    BLI_addtail(&ma.paint_layers, layer);
  }
  else if (place == PaintLayerPlace::Into) {
    BLI_addtail(&anchor->children, layer);
  }
  else {
    ListBase *anchor_owner = paint_layer_owner_list(&ma.paint_layers, anchor);
    if (place == PaintLayerPlace::Above) {
      BLI_insertlinkafter(anchor_owner, anchor, layer);
    }
    else {
      BLI_insertlinkbefore(anchor_owner, anchor, layer);
    }
  }
  BKE_paint_layers_tag_edited(ma);
  return true;
}

bool BKE_paint_layers_reorder(Material &ma, MaterialPaintLayer *layer, int index)
{
  if (layer == nullptr) {
    return false;
  }
  ListBase *owner = paint_layer_owner_list(&ma.paint_layers, layer);
  if (owner == nullptr) {
    return false;
  }
  const int count = BLI_listbase_count(owner);
  index = clamp_i(index, 0, count - 1);

  BLI_remlink(owner, layer);
  /* The stack is bottom-to-top, so index 0 is the first link of the list. */
  if (index == count - 1) {
    BLI_addtail(owner, layer);
  }
  else if (index == 0) {
    BLI_addhead(owner, layer);
  }
  else {
    BLI_insertlinkbefore(owner, static_cast<MaterialPaintLayer *>(BLI_findlink(owner, index)), layer);
  }
  BKE_paint_layers_tag_edited(ma);
  return true;
}

MaterialPaintLayer *BKE_paint_layers_group(Material &ma, Span<MaterialPaintLayer *> layers)
{
  if (layers.is_empty()) {
    return nullptr;
  }
  ListBase *owner = nullptr;
  for (MaterialPaintLayer *layer : layers) {
    if (layer == nullptr) {
      return nullptr;
    }
    ListBase *layer_owner = paint_layer_owner_list(&ma.paint_layers, layer);
    if (layer_owner == nullptr) {
      return nullptr;
    }
    if (owner == nullptr) {
      owner = layer_owner;
    }
    else if (owner != layer_owner) {
      return nullptr;
    }
  }

  /* The folder takes the position of its lowest member: the fold replaces the run the members
   * came from, so the rows that were below it stay below and the ones above stay above. The index
   * is a count of surviving rows below that slot, because the members are removed first. */
  const int owner_count = BLI_listbase_count(owner);
  int bottom_index = 0;
  for (const int i : IndexRange(owner_count)) {
    if (layers.contains(static_cast<MaterialPaintLayer *>(BLI_findlink(owner, i)))) {
      bottom_index = i;
      break;
    }
  }
  int insert_index = 0;
  for (const int i : IndexRange(bottom_index)) {
    if (!layers.contains(static_cast<MaterialPaintLayer *>(BLI_findlink(owner, i)))) {
      insert_index++;
    }
  }

  MaterialPaintLayer *folder = BKE_paint_layers_add(
      ma, MA_PAINT_LAYER_KIND_FOLDER, "Folder", nullptr, PaintLayerPlace::Above);
  if (folder == nullptr) {
    return nullptr;
  }
  BLI_remlink(&ma.paint_layers, folder);

  for (MaterialPaintLayer *layer : layers) {
    BLI_remlink(owner, layer);
    BLI_addtail(&folder->children, layer);
  }

  if (insert_index >= BLI_listbase_count(owner)) {
    BLI_addtail(owner, folder);
  }
  else {
    BLI_insertlinkbefore(owner,
                         static_cast<MaterialPaintLayer *>(BLI_findlink(owner, insert_index)),
                         folder);
  }
  BKE_paint_layers_tag_edited(ma);
  return folder;
}

bool BKE_paint_layers_ungroup(Material &ma, MaterialPaintLayer *folder)
{
  if (folder == nullptr) {
    return false;
  }
  ListBase *owner = paint_layer_owner_list(&ma.paint_layers, folder);
  if (owner == nullptr || !BKE_paint_layers_is_folder(*folder)) {
    return false;
  }

  /* Only the folder itself disappears: a descendant that lifts out keeps its marker, so an active
   * cursor naming one stays valid. */
  const bool clears_active = !BLI_uuid_is_nil(ma.active_layer_marker) &&
                             BLI_uuid_equal(ma.active_layer_marker, folder->marker);

  const int folder_index = BLI_findindex(owner, folder);
  BLI_remlink(owner, folder);

  /* The children lift into the folder's slot in their own order; the folder itself keeps nothing. */
  ListBase lifted = folder->children;
  folder->children = {nullptr, nullptr};
  BKE_material_paint_layer_free(folder);

  int insert_index = folder_index;
  for (MaterialPaintLayer *child = static_cast<MaterialPaintLayer *>(lifted.first), *next;
       child != nullptr;
       child = next)
  {
    next = child->next;
    BLI_remlink(&lifted, child);
    if (insert_index >= BLI_listbase_count(owner)) {
      BLI_addtail(owner, child);
    }
    else {
      BLI_insertlinkbefore(
          owner, static_cast<MaterialPaintLayer *>(BLI_findlink(owner, insert_index)), child);
    }
    insert_index++;
  }

  if (clears_active) {
    ma.active_layer_marker = {};
  }
  BKE_paint_layers_tag_edited(ma);
  return true;
}

MaterialPaintLayer *BKE_paint_layers_duplicate(Main &bmain,
                                               Material &ma,
                                               MaterialPaintLayer *layer)
{
  if (layer == nullptr) {
    return nullptr;
  }
  ListBase *owner = paint_layer_owner_list(&ma.paint_layers, layer);
  if (owner == nullptr) {
    return nullptr;
  }

  MaterialPaintLayer *copy = paint_layer_branch_duplicate(bmain, ma, *layer, true);
  BLI_insertlinkafter(owner, layer, copy);
  BKE_paint_layers_tag_edited(ma);
  return copy;
}

MaterialPaintLayer *BKE_paint_layers_mask_add(Material &ma, MaterialPaintLayer *layer, float value)
{
  if (layer == nullptr || paint_layer_owner_list(&ma.paint_layers, layer) == nullptr) {
    return nullptr;
  }
  /* The base mask is the first element of the mask stack: a constant item with the MULTIPLY blend,
   * so `F = F * value`. It is inserted first; a stroke turns it into a map in place. */
  MaterialPaintLayer *item = BKE_paint_layers_correction_add(
      ma, layer, MA_PAINT_LAYER_SECTION_MASK, MA_PAINT_LAYER_EFFECT_FILL, "Mask");
  if (item == nullptr) {
    return nullptr;
  }
  item->fill_color[0] = item->fill_color[1] = item->fill_color[2] = value;
  item->fill_color[3] = 1.0f;
  item->blend = MA_PAINT_LAYER_BLEND_MULTIPLY;
  item->opacity = 1.0f;
  BLI_remlink(&layer->mask_stack, item);
  BLI_addhead(&layer->mask_stack, item);
  return item;
}

bool BKE_paint_layers_channel_set_value(Material &ma,
                                        MaterialPaintLayer *layer,
                                        eMaterialPaintChannel channel,
                                        const float value[4])
{
  if (layer == nullptr || paint_layer_owner_list(&ma.paint_layers, layer) == nullptr) {
    return false;
  }
  MaterialPaintLayerChannel *record = paint_layer_channel_find(*layer, channel);
  if (record == nullptr) {
    return false;
  }
  copy_v4_v4(record->value, value);
  if (BKE_paint_layers_kind_info(layer->kind).uses_fill_color) {
    /* A Fill's channel value is a group input, like `fill_color`: a value-only edit, so animating
     * it does not rebuild the tree. */
    paint_layers_tag_value_only(ma);
    BKE_paint_layers_values_sync(ma);
  }
  else {
    /* A Paint value's alpha decides whether the row lays a constant at all, which is topology. */
    BKE_paint_layers_tag_edited(ma);
  }
  return true;
}

bool BKE_paint_layers_channel_set_image(Material &ma,
                                        MaterialPaintLayer *layer,
                                        eMaterialPaintChannel channel,
                                        Image *image)
{
  if (layer == nullptr || paint_layer_owner_list(&ma.paint_layers, layer) == nullptr) {
    return false;
  }
  MaterialPaintLayerChannel *record = paint_layer_channel_find(*layer, channel);
  if (record == nullptr) {
    return false;
  }
  if (record->image != nullptr) {
    id_us_min(&record->image->id);
  }
  record->image = image;
  if (record->image != nullptr) {
    id_us_plus(&record->image->id);
  }
  BKE_paint_layers_tag_edited(ma);
  return true;
}

MaterialPaintLayerChannel *BKE_paint_layers_channel_add(Material &ma,
                                                        MaterialPaintLayer *layer,
                                                        eMaterialPaintChannel channel)
{
  if (layer == nullptr || paint_layer_owner_list(&ma.paint_layers, layer) == nullptr) {
    return nullptr;
  }
  /* A folder carries no maps of its own: its participation in a channel is the union of its
   * children's (design §5). Folder-ness is the kind, so an emptied folder is refused all the same. */
  if (BKE_paint_layers_is_folder(*layer)) {
    return nullptr;
  }
  for (const int i : IndexRange(layer->channels_num)) {
    if (layer->channels[i].channel == channel) {
      return &layer->channels[i];
    }
  }
  MaterialPaintLayerChannel *channels = MEM_new_array<MaterialPaintLayerChannel>(
      layer->channels_num + 1, __func__);
  if (layer->channels_num > 0) {
    memcpy(channels, layer->channels, sizeof(*channels) * layer->channels_num);
  }
  MaterialPaintLayerChannel &record = channels[layer->channels_num];
  record.channel = channel;
  record.state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  record.image = nullptr;
  if (BKE_paint_layers_kind_info(layer->kind).uses_fill_color) {
    /* A Fill shows a value per channel from the start; the Principled defaults keep the material
     * looking like it did before the layer. */
    BKE_paint_layers_channel_default_value(ma, channel, record.value);
  }
  else {
    /* A fresh Paint record is transparent: it takes part but lays nothing down until painted. */
    zero_v4(record.value);
  }
  MEM_delete(layer->channels);
  layer->channels = channels;
  layer->channels_num++;
  BKE_paint_layers_tag_edited(ma);
  return &layer->channels[layer->channels_num - 1];
}

void BKE_paint_layers_default_channels_apply(Material &ma, MaterialPaintLayer &layer)
{
  if (!ELEM(layer.kind, MA_PAINT_LAYER_KIND_PAINT, MA_PAINT_LAYER_KIND_FILL)) {
    return;
  }
  /* The channels that reach a Principled socket and hold no viewport-wide side effect: Alpha would
   * change the material's transparency and Emission its glow, so both are opted into explicitly
   * through channel_add. Normal and Height are painter-authored and stay out too. */
  static constexpr eMaterialPaintChannel default_channels[] = {
      PAINT_MATERIAL_CHANNEL_BASE_COLOR,
      PAINT_MATERIAL_CHANNEL_METALLIC,
      PAINT_MATERIAL_CHANNEL_ROUGHNESS,
  };
  for (const eMaterialPaintChannel channel : default_channels) {
    if (paint_layer_channel_find(layer, channel) != nullptr) {
      continue;
    }
    BKE_paint_layers_channel_add(ma, &layer, channel);
  }
}

void BKE_paint_layers_channel_default_value(const Material &ma,
                                            const int channel,
                                            float r_value[4])
{
  zero_v4(r_value);
  r_value[3] = 1.0f;
  /* The Principled BSDF defaults, used when the material has no Principled of its own. */
  switch (channel) {
    case PAINT_MATERIAL_CHANNEL_BASE_COLOR:
      r_value[0] = r_value[1] = r_value[2] = 0.8f;
      break;
    case PAINT_MATERIAL_CHANNEL_ROUGHNESS:
    case PAINT_MATERIAL_CHANNEL_SPECULAR:
      r_value[0] = r_value[1] = r_value[2] = 0.5f;
      break;
    case PAINT_MATERIAL_CHANNEL_ALPHA:
    case PAINT_MATERIAL_CHANNEL_AO:
      r_value[0] = r_value[1] = r_value[2] = 1.0f;
      break;
    case PAINT_MATERIAL_CHANNEL_NORMAL:
      r_value[0] = 0.5f;
      r_value[1] = 0.5f;
      r_value[2] = 1.0f;
      break;
    default:
      break;
  }

  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  const bNode *principled = BKE_paint_material_principled_find(ma, reason);
  if (principled == nullptr) {
    return;
  }
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(channel));
  if (info.socket_name == nullptr) {
    return;
  }
  const bNodeSocket *socket = bke::node_find_socket(const_cast<bNode &>(*principled),
                                                    SOCK_IN,
                                                    UString::from_ptr_noinline(info.socket_name));
  if (socket == nullptr || socket->default_value == nullptr) {
    return;
  }
  if (socket->type == SOCK_FLOAT) {
    const float value = static_cast<const bNodeSocketValueFloat *>(socket->default_value)->value;
    r_value[0] = r_value[1] = r_value[2] = value;
    r_value[3] = 1.0f;
  }
  else if (socket->type == SOCK_RGBA) {
    const float *value = static_cast<const bNodeSocketValueRGBA *>(socket->default_value)->value;
    copy_v4_v4(r_value, value);
  }
}

void paint_layer_channel_constant(const MaterialPaintLayer &layer,
                                  const int channel,
                                  float r_color[4])
{
  if (BKE_paint_layers_kind_info(layer.kind).uses_fill_color &&
      channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR)
  {
    copy_v4_v4(r_color, layer.fill_color);
    return;
  }
  const MaterialPaintLayerChannel *entry = paint_layer_channel_find(layer, channel);
  if (entry != nullptr) {
    copy_v4_v4(r_color, entry->value);
    return;
  }
  zero_v4(r_color);
}

namespace {

/** Whether \a target is reachable from \a from through MATERIAL layers' source materials. */
bool paint_layers_material_depends_on(const Material &from, const Material &target)
{
  if (&from == &target) {
    return true;
  }
  std::function<bool(const ListBase &)> walk = [&](const ListBase &list) {
    for (const MaterialPaintLayer &layer :
         *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&list))
    {
      if (layer.kind == MA_PAINT_LAYER_KIND_MATERIAL && layer.material != nullptr &&
          paint_layers_material_depends_on(*layer.material, target))
      {
        return true;
      }
      if (walk(layer.children) || walk(layer.effects) || walk(layer.mask_stack)) {
        return true;
      }
    }
    return false;
  };
  return walk(from.paint_layers);
}

}  // namespace

bool BKE_paint_layers_material_depends_on(const Material &from, const Material &target)
{
  return paint_layers_material_depends_on(from, target);
}

bool BKE_paint_layers_set_material(Material &ma, MaterialPaintLayer *layer, Material *source)
{
  if (layer == nullptr || paint_layer_owner_list(&ma.paint_layers, layer) == nullptr) {
    return false;
  }
  if (layer->kind != MA_PAINT_LAYER_KIND_MATERIAL) {
    return false;
  }
  if (source == nullptr || source == &ma) {
    return false;
  }
  /* A source that already bakes this material would recurse on the next bake. */
  if (paint_layers_material_depends_on(*source, ma)) {
    return false;
  }
  if (layer->material == source) {
    return true;
  }
  if (layer->material != nullptr) {
    id_us_min(&layer->material->id);
  }
  layer->material = source;
  id_us_plus(&source->id);
  BKE_paint_layers_tag_edited(ma);
  return true;
}

bool BKE_paint_layers_channel_remove(Material &ma,
                                     MaterialPaintLayer *layer,
                                     eMaterialPaintChannel channel)
{
  if (layer == nullptr || paint_layer_owner_list(&ma.paint_layers, layer) == nullptr) {
    return false;
  }
  MaterialPaintLayerChannel *record = paint_layer_channel_find(*layer, channel);
  if (record == nullptr) {
    return false;
  }
  const int index = int(record - layer->channels);
  if (layer->channels_num > 1) {
    memmove(layer->channels + index,
            layer->channels + index + 1,
            sizeof(*layer->channels) * (layer->channels_num - index - 1));
    layer->channels_num--;
  }
  else {
    MEM_delete(layer->channels);
    layer->channels = nullptr;
    layer->channels_num = 0;
  }
  BKE_paint_layers_tag_edited(ma);
  return true;
}

bool BKE_paint_layers_channel_set_enabled(Material &ma,
                                          MaterialPaintLayer *layer,
                                          eMaterialPaintChannel channel,
                                          bool enabled)
{
  if (layer == nullptr || paint_layer_owner_list(&ma.paint_layers, layer) == nullptr) {
    return false;
  }
  MaterialPaintLayerChannel *record = paint_layer_channel_find(*layer, channel);
  if (record == nullptr) {
    return false;
  }
  record->state = enabled ? MA_PAINT_LAYER_CHANNEL_ENABLED : MA_PAINT_LAYER_CHANNEL_DISABLED;
  BKE_paint_layers_tag_edited(ma);
  return true;
}

bool BKE_paint_layers_channel_blend_set(Material &ma,
                                        MaterialPaintLayer &layer,
                                        int channel,
                                        int blend)
{
  if (paint_layer_owner_list(&ma.paint_layers, &layer) == nullptr) {
    return false;
  }
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
    return false;
  }
  /* The Normal channel forces its own combine in the generator and the CPU, so a blend override
   * there could never take effect; refuse it rather than store a setting that does nothing. */
  if (channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
    return false;
  }
  /* Inherit, or one of the user-facing blends; Normal Combine is the Normal channel's own doing. */
  if (blend < -1 || blend > MA_PAINT_LAYER_BLEND_VALUE ||
      blend == MA_PAINT_LAYER_BLEND_NORMAL_COMBINE)
  {
    return false;
  }
  layer.channel_settings[channel].blend = int8_t(blend);
  /* A blend is baked into the generated Mix node, so this is a structural edit. */
  BKE_paint_layers_tag_edited(ma);
  return true;
}

bool BKE_paint_layers_channel_opacity_set(Material &ma,
                                          MaterialPaintLayer &layer,
                                          int channel,
                                          float opacity)
{
  if (paint_layer_owner_list(&ma.paint_layers, &layer) == nullptr) {
    return false;
  }
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
    return false;
  }
  layer.channel_settings[channel].opacity = clamp_f(opacity, 0.0f, 1.0f);
  /* Value-only: the per (row, channel) group input carries it, so no rebuild. */
  paint_layers_tag_value_only(ma);
  BKE_paint_layers_values_sync(ma);
  return true;
}

bool BKE_paint_layers_kind_change(Material &ma,
                                  MaterialPaintLayer *layer,
                                  eMaterialPaintLayerKind kind)
{
  if (layer == nullptr || paint_layer_owner_list(&ma.paint_layers, layer) == nullptr) {
    return false;
  }
  if (!ELEM(kind, MA_PAINT_LAYER_KIND_PAINT, MA_PAINT_LAYER_KIND_FILL)) {
    return false;
  }
  if (layer->kind == kind) {
    return true;
  }
  if (kind == MA_PAINT_LAYER_KIND_FILL) {
    /* A painted map is not a constant, so the maps go; the records stay, because a per-channel
     * blend/opacity override is a setting of the pair, not a pixel. Only the image is forgotten;
     * an image is owned by Main, so it is detached, not freed. */
    for (const int i : IndexRange(layer->channels_num)) {
      layer->channels[i].image = nullptr;
    }
    if (layer->fill_color[3] == 0.0f) {
      layer->fill_color[0] = layer->fill_color[1] = layer->fill_color[2] = 0.0f;
      layer->fill_color[3] = 1.0f;
    }
  }
  else {
    /* Fill's color becomes the starting point of the first stroke's map, not a map of its own. */
    static const float fill_default[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    copy_v4_v4(layer->fill_color, fill_default);
  }
  layer->kind = kind;
  BKE_paint_layers_tag_edited(ma);
  return true;
}

MaterialPaintLayer *BKE_paint_layers_correction_add(Material &ma,
                                                    MaterialPaintLayer *owner,
                                                    int section,
                                                    int effect,
                                                    const char *name)
{
  if (owner == nullptr || paint_layer_owner_list(&ma.paint_layers, owner) == nullptr) {
    return nullptr;
  }
  if (!ELEM(section, MA_PAINT_LAYER_SECTION_CONTENT, MA_PAINT_LAYER_SECTION_MASK) ||
      !ELEM(effect, MA_PAINT_LAYER_EFFECT_PAINT, MA_PAINT_LAYER_EFFECT_FILL))
  {
    return nullptr;
  }
  /* A correction is its own list on the owner, not a folder child: #MaterialPaintLayer::children
   * holds nesting, #MaterialPaintLayer::effects the adjustments to the row and
   * #MaterialPaintLayer::mask_stack its mask items. It is linked directly, never through add():
   * adding Into would promote the owner to a folder. */
  MaterialPaintLayer *correction = paint_layer_alloc(
      ma, MA_PAINT_LAYER_KIND_CORRECTION, name != nullptr ? name : "Correction");
  correction->section = int8_t(section);
  correction->effect = int8_t(effect);
  ListBase *destination = (section == MA_PAINT_LAYER_SECTION_MASK) ? &owner->mask_stack :
                                                                     &owner->effects;
  BLI_addtail(destination, correction);
  paint_layer_mark_owned(ma);
  return correction;
}

bool BKE_paint_layers_correction_set_section(Material &ma,
                                             MaterialPaintLayer *correction,
                                             int section)
{
  if (correction == nullptr || correction->kind != MA_PAINT_LAYER_KIND_CORRECTION) {
    return false;
  }
  if (!ELEM(section, MA_PAINT_LAYER_SECTION_CONTENT, MA_PAINT_LAYER_SECTION_MASK)) {
    return false;
  }
  MaterialPaintLayer *owner = paint_layer_correction_owner(ma.paint_layers, *correction);
  if (owner == nullptr) {
    return false;
  }
  if (correction->section == section) {
    return true;
  }
  /* The section chooses the list the row lives in, so changing it moves the row between the
   * owner's effects and mask_stack, keeping it at the top of its new list. */
  ListBase *source = (correction->section == MA_PAINT_LAYER_SECTION_MASK) ? &owner->mask_stack :
                                                                            &owner->effects;
  ListBase *destination = (section == MA_PAINT_LAYER_SECTION_MASK) ? &owner->mask_stack :
                                                                     &owner->effects;
  BLI_remlink(source, correction);
  correction->section = int8_t(section);
  BLI_addtail(destination, correction);
  BKE_paint_layers_tag_edited(ma);
  return true;
}

bool BKE_paint_layers_correction_set_effect(Material &ma,
                                            MaterialPaintLayer *correction,
                                            int effect)
{
  if (correction == nullptr || correction->kind != MA_PAINT_LAYER_KIND_CORRECTION ||
      paint_layer_owner_list(&ma.paint_layers, correction) == nullptr)
  {
    return false;
  }
  if (!ELEM(effect, MA_PAINT_LAYER_EFFECT_PAINT, MA_PAINT_LAYER_EFFECT_FILL)) {
    return false;
  }
  correction->effect = int8_t(effect);
  BKE_paint_layers_tag_edited(ma);
  return true;
}

namespace {

void paint_layer_assert_consistent_row(const MaterialPaintLayer &layer)
{
  for (const MaterialPaintLayer &effect :
       *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&layer.effects))
  {
    BLI_assert(effect.section == MA_PAINT_LAYER_SECTION_CONTENT);
    paint_layer_assert_consistent_row(effect);
  }
  for (const MaterialPaintLayer &mask_item :
       *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&layer.mask_stack))
  {
    BLI_assert(mask_item.section == MA_PAINT_LAYER_SECTION_MASK);
    paint_layer_assert_consistent_row(mask_item);
  }
  for (const MaterialPaintLayer &child :
       *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&layer.children))
  {
    paint_layer_assert_consistent_row(child);
  }
}

}  // namespace

void BKE_paint_layers_assert_consistent(const Material &ma)
{
  for (const MaterialPaintLayer &layer :
       *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&ma.paint_layers))
  {
    paint_layer_assert_consistent_row(layer);
  }
}

bool BKE_paint_layers_set_blend(Material &ma,
                                MaterialPaintLayer *layer,
                                eMaterialPaintLayerBlend blend)
{
  if (layer == nullptr || paint_layer_owner_list(&ma.paint_layers, layer) == nullptr) {
    return false;
  }
  /* The Normal channel forces the combine operation by itself, and no other channel can express
   * it; a stored value would only ever be a switch the user cannot make effective. */
  if (blend == MA_PAINT_LAYER_BLEND_NORMAL_COMBINE) {
    return false;
  }
  layer->blend = blend;
  BKE_paint_layers_tag_edited(ma);
  return true;
}

bool BKE_paint_layers_set_opacity(Material &ma, MaterialPaintLayer *layer, float opacity)
{
  if (layer == nullptr || paint_layer_owner_list(&ma.paint_layers, layer) == nullptr) {
    return false;
  }
  layer->opacity = clamp_f(opacity, 0.0f, 1.0f);
  /* Opacity is a group input, not topology: animating it must not rebuild the tree. */
  paint_layers_tag_value_edited(ma, layer);
  return true;
}

bool BKE_paint_layers_set_fill_color(Material &ma, MaterialPaintLayer *layer, const float color[4])
{
  if (layer == nullptr || paint_layer_owner_list(&ma.paint_layers, layer) == nullptr) {
    return false;
  }
  copy_v4_v4(layer->fill_color, color);
  /* A Fill colour is a group input as well. */
  paint_layers_tag_value_edited(ma, layer);
  return true;
}

bool BKE_paint_layers_set_color_tag(Material &ma, MaterialPaintLayer *layer, int8_t color_tag)
{
  if (layer == nullptr || paint_layer_owner_list(&ma.paint_layers, layer) == nullptr) {
    return false;
  }
  layer->color_tag = color_tag;
  BKE_paint_layers_tag_edited(ma);
  return true;
}

bool BKE_paint_layers_rename(Material &ma, MaterialPaintLayer *layer, const char *name)
{
  if (layer == nullptr || paint_layer_owner_list(&ma.paint_layers, layer) == nullptr) {
    return false;
  }
  STRNCPY(layer->name, name != nullptr ? name : "");
  BKE_paint_layers_tag_edited(ma);
  return true;
}


bool BKE_paint_layers_set_enabled(Material &ma, MaterialPaintLayer *layer, bool enabled)
{
  if (layer == nullptr || paint_layer_owner_list(&ma.paint_layers, layer) == nullptr) {
    return false;
  }
  SET_FLAG_FROM_TEST(layer->flag, enabled, MA_PAINT_LAYER_ENABLED);
  /* Enabled folds into the group-input factor (#BKE_paint_layers_effective_opacity), and the
   * generator keeps a disabled row in the topology with factor zero, so this is a value edit: an
   * F-curve on it reaches the shader without a rebuild. A row an earlier rebuild actually dropped
   * from the graph has to come back though, and that is a topology change. */
  if (enabled && BKE_paint_layers_row_removed_clear(ma, layer->marker)) {
    ma.paint_layers_flag |= MA_PAINT_LAYERS_REGEN;
    BKE_paint_layers_root_hash_invalidate(ma);
  }
  paint_layers_tag_value_edited(ma, layer);
  return true;
}

bool BKE_paint_layers_set_custom_group(Material &ma,
                                       MaterialPaintLayer *layer,
                                       bNodeTree *group)
{
  if (layer == nullptr || paint_layer_owner_list(&ma.paint_layers, layer) == nullptr) {
    return false;
  }
  if (layer->custom_group != nullptr) {
    id_us_min(&layer->custom_group->id);
  }
  if (group != nullptr) {
    id_us_plus(&group->id);
  }
  layer->custom_group = group;
  BKE_paint_layers_tag_edited(ma);
  return true;
}

}  // namespace blender
