/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * See #BKE_paint_layers.hh: the description of a layered material -- adding, removing and looking
 * rows up by marker. Nothing here reads or writes a node tree; every mutation only marks the
 * material stale.
 */

#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_generate.hh"

#include <cstring>

#include "paint_layers_intern.hh"
#include "paint_layers_kind.hh"

#include "MEM_guardedalloc.h"

#include "BKE_idprop.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_report.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "BLI_function_ref.hh"
#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_math_base.h"
#include "BLI_math_vector.h"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_utildefines.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "DEG_depsgraph.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"
#include "DNA_uuid_types.h"

namespace blender {

void BKE_paint_layers_tag_edited(Material &ma)
{
  /* An edit is a reason to try a cancelled heavy bake again. */
  ma.paint_layers_flag &= ~MA_PAINT_LAYERS_BAKE_CANCELLED;
  ma.paint_layers_flag |= MA_PAINT_LAYERS_REGEN;
  DEG_id_tag_update(&ma.id, ID_RECALC_SHADING);
}

void BKE_paint_layers_tag_value_edited(Material &ma)
{
  /* A value can be baked into a row's cache (the hash covers opacity and fill), and this path does
   * not rebuild the tree, so the planner has to be asked for a re-bake as well. */
  ma.paint_layers_flag &= ~MA_PAINT_LAYERS_BAKE_CANCELLED;
  ma.paint_layers_flag |= MA_PAINT_LAYERS_BAKE_STALE;
  /* A value edit changes no socket set: every row's inputs already exist, so the generated tree
   * keeps its nodes and only its values are re-synced. Never MA_PAINT_LAYERS_REGEN; SYNC_TO_EVAL so
   * the evaluated copy the shader reads is refreshed. If the tree does not match the description's
   * values (it was hand-edited), fall back to a full regeneration rather than leaving a stale one. */
  DEG_id_tag_update(&ma.id, ID_RECALC_SHADING | ID_RECALC_SYNC_TO_EVAL);
  if (BKE_paint_layers_values_sync(ma)) {
    BKE_paint_layers_values_tag_trees(ma);
  }
  else {
    ma.paint_layers_flag |= MA_PAINT_LAYERS_REGEN;
  }
}

/**
 * A blend edit that changes no graph shape: the generated Mix nodes take the new `blend_type` in
 * place. Never MA_PAINT_LAYERS_REGEN -- the regeneration point syncs the nodes
 * (#paint_layers_blend_sync) and falls back to a rebuild only when the tree no longer matches the
 * description. A parent folder's bake holds its children's blends, so the bake planner is asked to
 * check as well; the row's own cache does not depend on its own blend. */
static void paint_layers_tag_blend_edited(Material &ma)
{
  ma.paint_layers_flag &= ~MA_PAINT_LAYERS_BAKE_CANCELLED;
  ma.paint_layers_flag |= MA_PAINT_LAYERS_BLEND_SYNC | MA_PAINT_LAYERS_BAKE_STALE;
  DEG_id_tag_update(&ma.id, ID_RECALC_SHADING | ID_RECALC_SYNC_TO_EVAL);
}

bool BKE_paint_layers_is_layered(const Material &ma)
{
  return (ma.paint_layers_flag & MA_PAINT_LAYERED) != 0;
}

using PaintLayerList = ListBaseT<MaterialPaintLayer>;

bool BKE_paint_layers_is_folder(const MaterialPaintLayer &layer)
{
  return layer.source == MA_PAINT_LAYER_SOURCE_STACK;
}

bool BKE_paint_layers_has_children(const MaterialPaintLayer &layer)
{
  return BKE_paint_layers_is_folder(layer) && !BLI_listbase_is_empty(&layer.children);
}

bool BKE_paint_layers_folder_is_pass_through(const MaterialPaintLayer &folder)
{
  if (!BKE_paint_layers_is_folder(folder)) {
    return false;
  }
  /* A mask folds a per-pixel factor into the folder's own row; an inlined folder has no row. The
   * list is read directly because emptiness is the question: a disabled mask item still has to flip
   * the mode, so toggling its visibility later stays a value edit. */
  if (!BLI_listbase_is_empty(&folder.mask_stack)) {
    return false;
  }
  /* A content correction edits the folder's isolated result: an inlined folder has no result of
   * its own to lay it over. Read directly, like the mask, so hiding a correction keeps the mode. */
  if (!BLI_listbase_is_empty(&folder.effects)) {
    return false;
  }
  /* An object mask multiplies the folder's own row factor; an inlined folder has no row, so the
   * mask would silently hide nothing. The flag is sticky, so toggling visibility stays a value
   * edit. */
  if ((folder.object_mask_flag & MA_PAINT_LAYER_OBJECT_MASK_ACTIVE) != 0) {
    return false;
  }
  /* The isolated folder computes `over(below, P/a, a)`; laying the children one after another is
   * the same only for the Mix blend at full weight. Opacity below one breaks it for two or more
   * children, because scaling the whole isolated result is not scaling each child's own factor.
   * The raw opacity is read on purpose: the enabled flag must not change the mode. */
  if (folder.opacity != 1.0f) {
    return false;
  }
  /* A channel's own blend or opacity recombines or scales the isolated result of that channel for
   * the same reason. Read directly, not through the enabled flag, so hiding a row keeps the mode. */
  for (int channel = 0; channel < PAINT_MATERIAL_CHANNEL_NUM; channel++) {
    if (BKE_paint_layers_channel_blend_effective(folder, channel) != MA_PAINT_LAYER_BLEND_MIX ||
        folder.channel_settings[channel].opacity != 1.0f)
    {
      return false;
    }
  }
  return true;
}

/**
 * The list of \a list's subtree that directly holds \a layer: \a list itself or the children of a
 * folder under it, at any depth. Only a folder's children are read -- a leaf row that somehow
 * carries some is not walked, the way no other reader walks it.
 */
static const PaintLayerList *paint_layer_owner_list_find(const PaintLayerList &list,
                                                         const MaterialPaintLayer *layer)
{
  for (const MaterialPaintLayer &row : list) {
    if (&row == layer) {
      return &list;
    }
    if (BKE_paint_layers_is_folder(row)) {
      if (const PaintLayerList *found = paint_layer_owner_list_find(row.children, layer)) {
        return found;
      }
    }
  }
  return nullptr;
}

ListBaseT<MaterialPaintLayer> *BKE_paint_layers_owner_list(Material &ma,
                                                           const MaterialPaintLayer &layer)
{
  return const_cast<PaintLayerList *>(paint_layer_owner_list_find(ma.paint_layers, &layer));
}

/** Whether \a layer is a row of \a ma's stack, at any depth. */
static bool paint_layer_in_stack(const Material &ma, const MaterialPaintLayer *layer)
{
  return layer != nullptr && paint_layer_owner_list_find(ma.paint_layers, layer) != nullptr;
}

static bool paint_layers_foreach_list(
    const PaintLayerList &list,
    const MaterialPaintLayer *parent,
    FunctionRef<bool(const MaterialPaintLayer &, const MaterialPaintLayer *)> fn)
{
  for (const MaterialPaintLayer &layer : list) {
    if (!fn(layer, parent)) {
      return false;
    }
    if (BKE_paint_layers_is_folder(layer) &&
        !paint_layers_foreach_list(layer.children, &layer, fn))
    {
      return false;
    }
  }
  return true;
}

void BKE_paint_layers_foreach(
    const Material &ma,
    FunctionRef<bool(const MaterialPaintLayer &layer, const MaterialPaintLayer *parent)> fn)
{
  paint_layers_foreach_list(ma.paint_layers, nullptr, fn);
}

void BKE_paint_layers_foreach(
    Material &ma, FunctionRef<bool(MaterialPaintLayer &layer, MaterialPaintLayer *parent)> fn)
{
  paint_layers_foreach_list(
      ma.paint_layers,
      nullptr,
      [&](const MaterialPaintLayer &layer, const MaterialPaintLayer *parent) -> bool {
        return fn(const_cast<MaterialPaintLayer &>(layer),
                  const_cast<MaterialPaintLayer *>(parent));
      });
}

MaterialPaintLayer *BKE_paint_layers_parent(Material &ma, const MaterialPaintLayer &layer)
{
  const MaterialPaintLayer *parent = nullptr;
  BKE_paint_layers_foreach(
      static_cast<const Material &>(ma),
      [&](const MaterialPaintLayer &row, const MaterialPaintLayer *row_parent) -> bool {
        if (&row == &layer) {
          parent = row_parent;
          return false;
        }
        return true;
      });
  return const_cast<MaterialPaintLayer *>(parent);
}

bool BKE_paint_layers_subtree_contains(const MaterialPaintLayer &root, const bUUID &marker)
{
  if (BLI_uuid_equal(root.marker, marker)) {
    return true;
  }
  for (const MaterialPaintLayer &effect : root.effects) {
    if (BLI_uuid_equal(effect.marker, marker)) {
      return true;
    }
  }
  for (const MaterialPaintLayer &item : root.mask_stack) {
    if (BLI_uuid_equal(item.marker, marker)) {
      return true;
    }
  }
  if (BKE_paint_layers_is_folder(root)) {
    for (const MaterialPaintLayer &child : root.children) {
      if (BKE_paint_layers_subtree_contains(child, marker)) {
        return true;
      }
    }
  }
  return false;
}

/**
 * Whether \a layer is a row of \a ma's stack, an item of one row's mask stack or one of its content
 * corrections: the rows the channel, enabled and opacity edits may touch. Structural edits (move,
 * duplicate, source) stay on #paint_layer_in_stack.
 */
static bool paint_layer_owned(const Material &ma, const MaterialPaintLayer *layer)
{
  if (layer == nullptr) {
    return false;
  }
  bool owned = false;
  BKE_paint_layers_foreach(
      ma, [&](const MaterialPaintLayer &row, const MaterialPaintLayer * /*parent*/) -> bool {
        owned = &row == layer || BLI_findindex(&row.mask_stack, layer) != -1 ||
                BLI_findindex(&row.effects, layer) != -1;
        return !owned;
      });
  return owned;
}

MaterialPaintLayer *BKE_paint_layers_correction_owner(Material &ma, const MaterialPaintLayer &effect)
{
  if (effect.role != MA_PAINT_LAYER_ROLE_EFFECT) {
    return nullptr;
  }
  MaterialPaintLayer *owner = nullptr;
  BKE_paint_layers_foreach(
      ma, [&](MaterialPaintLayer &row, MaterialPaintLayer * /*parent*/) -> bool {
        if (BLI_findindex(&row.effects, &effect) != -1) {
          owner = &row;
        }
        return owner == nullptr;
      });
  return owner;
}

/** Whether \a layer is a row of \a ma's stack or one of a row's content corrections. */
static bool paint_layer_in_stack_or_effect(Material &ma, const MaterialPaintLayer *layer)
{
  /* A mask correction is an item of its row's mask like a correction is of its effects: the rename,
   * blend and colour setters treat the three alike. */
  return paint_layer_owned(ma, layer);
}

/**
 * The record of \a channel when \a layer is owned by \a ma (see #paint_layer_owned) and carries
 * one, otherwise null. Every channel edit starts with this, so a foreign row and a missing record
 * are refused alike.
 */
static MaterialPaintLayerChannel *paint_layer_channel_in_stack(const Material &ma,
                                                               MaterialPaintLayer *layer,
                                                               const eMaterialPaintChannel channel)
{
  if (!paint_layer_owned(ma, layer)) {
    return nullptr;
  }
  return paint_layer_channel_find(*layer, channel);
}

/**
 * Whether \a anchor can take a row by \a place: it is a row of \a ma's stack, and Into names a
 * folder. Nothing here promotes a plain row to a folder -- its maps would be ignored and the
 * painted result would silently disappear -- so a UI that drops "into" a plain row groups instead.
 */
static bool paint_layer_anchor_valid(const Material &ma,
                                     const MaterialPaintLayer *anchor,
                                     const PaintLayerPlace place)
{
  if (!paint_layer_in_stack(ma, anchor)) {
    return false;
  }
  return place != PaintLayerPlace::Into || BKE_paint_layers_is_folder(*anchor);
}

/**
 * Link the unlinked or detached \a layer into \a ma's stack: appended on top for a null \a anchor,
 * otherwise next to \a anchor by \a place, or inside it for Into. \a anchor must already be
 * validated (#paint_layer_anchor_valid).
 */
static void paint_layer_link(Material &ma,
                             MaterialPaintLayer *layer,
                             MaterialPaintLayer *anchor,
                             const PaintLayerPlace place)
{
  if (anchor == nullptr) {
    BLI_addtail(&ma.paint_layers, layer);
    return;
  }
  if (place == PaintLayerPlace::Into) {
    BLI_addtail(&anchor->children, layer);
    return;
  }
  PaintLayerList *owner = BKE_paint_layers_owner_list(ma, *anchor);
  if (place == PaintLayerPlace::Above) {
    BLI_insertlinkafter(owner, anchor, layer);
  }
  else {
    BLI_insertlinkbefore(owner, anchor, layer);
  }
}

/** A fresh marker no row of \a ma's stack, nor any item of a row's mask, carries yet. */
static bUUID paint_layer_unique_marker(const Material &ma)
{
  bUUID marker;
  bool in_use;
  do {
    marker = BLI_uuid_generate_random();
    in_use = false;
    BKE_paint_layers_foreach(
        ma, [&](const MaterialPaintLayer &layer, const MaterialPaintLayer * /*parent*/) -> bool {
          in_use = BLI_uuid_equal(layer.marker, marker);
          for (const MaterialPaintLayer &item : layer.mask_stack) {
            in_use |= BLI_uuid_equal(item.marker, marker);
          }
          for (const MaterialPaintLayer &effect : layer.effects) {
            in_use |= BLI_uuid_equal(effect.marker, marker);
          }
          return !in_use;
        });
  } while (in_use);
  return marker;
}

/**
 * A detached row with the usual defaults and a marker unique within \a ma. It is not linked yet:
 * #BKE_paint_layers_add links it into the stack.
 */
static MaterialPaintLayer *paint_layer_alloc(const Material &ma, const char *name)
{
  MaterialPaintLayer *layer = MEM_new<MaterialPaintLayer>(__func__);
  layer->flag = MA_PAINT_LAYER_ENABLED;
  layer->opacity = 1.0f;
  STRNCPY(layer->name, name != nullptr ? name : "");
  layer->marker = paint_layer_unique_marker(ma);
  return layer;
}

MaterialPaintLayer *BKE_paint_layers_add(Material &ma,
                                         const char *name,
                                         MaterialPaintLayer *anchor,
                                         const PaintLayerPlace place)
{
  /* The material's channel set is derived while the field is zero; freeze it before the new row
   * is linked, so the per-channel filter reads the stored mask instead of recomputing the union.
   * A no-op once anything has materialized it. */
  BKE_paint_layers_channels_materialize(ma);

  /* Every place validates its anchor: a row may never be linked under a foreign material. */
  if (anchor != nullptr && !paint_layer_anchor_valid(ma, anchor, place)) {
    return nullptr;
  }

  MaterialPaintLayer *layer = paint_layer_alloc(ma, name);
  paint_layer_link(ma, layer, anchor, place);

  ma.paint_layers_flag |= MA_PAINT_LAYERED;
  BKE_paint_layers_tag_edited(ma);
  return layer;
}

/**
 * A copy of the map \a source with its pixels. #BKE_id_copy copies the image's settings but not the
 * buffer behind it, and a painted map is a buffer that exists nowhere else -- unsaved, generated --
 * so the bare copy would come back blank and the layer made from it would show nothing.
 */
static Image *paint_layer_image_duplicate(Main &bmain, Image &source)
{
  Image *copy = id_cast<Image *>(BKE_id_copy(&bmain, &source.id));
  /* A generated map is of type #IMA_TYPE_UV_TEST, not #IMA_TYPE_IMAGE, so the source alone decides
   * whether there is a single buffer to carry over. */
  if (copy == nullptr || !ELEM(source.source, IMA_SRC_FILE, IMA_SRC_GENERATED)) {
    return copy;
  }

  void *source_lock = nullptr;
  ImBuf *source_ibuf = BKE_image_acquire_ibuf(&source, nullptr, &source_lock);
  if (source_ibuf != nullptr) {
    /* The copy's own buffer is what its settings generate or load: same size and kind as the
     * source's, and blank. The pixels go into that buffer rather than replacing it, which keeps
     * everything the image system derived from the settings (tile, colorspace, gen flags). */
    void *copy_lock = nullptr;
    ImBuf *copy_ibuf = BKE_image_acquire_ibuf(copy, nullptr, &copy_lock);
    if (copy_ibuf != nullptr && copy_ibuf->x == source_ibuf->x && copy_ibuf->y == source_ibuf->y) {
      const size_t pixel_num = size_t(source_ibuf->x) * size_t(source_ibuf->y);
      bool copied = false;
      if (source_ibuf->float_data() != nullptr && copy_ibuf->float_data() != nullptr &&
          source_ibuf->channels == copy_ibuf->channels)
      {
        memcpy(copy_ibuf->float_data_for_write(),
               source_ibuf->float_data(),
               sizeof(float) * pixel_num * size_t(copy_ibuf->channels));
        copied = true;
      }
      else if (source_ibuf->byte_data() != nullptr && copy_ibuf->byte_data() != nullptr) {
        memcpy(copy_ibuf->byte_data_for_write(), source_ibuf->byte_data(), pixel_num * 4);
        copied = true;
      }
      if (copied) {
        /* Painted pixels exist only in this buffer, so it has to count as unsaved work. */
        copy_ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;
        BKE_image_mark_dirty(copy, copy_ibuf);
      }
    }
    BKE_image_release_ibuf(copy, copy_ibuf, copy_lock);
  }
  BKE_image_release_ibuf(&source, source_ibuf, source_lock);
  return copy;
}

/** Give \a dst a copy of \a src's channel records, each map copied into an image of its own. */
static void paint_layer_channels_duplicate(Main &bmain,
                                           MaterialPaintLayer &dst,
                                           const MaterialPaintLayer &src)
{
  if (src.channels_num <= 0 || src.channels == nullptr) {
    return;
  }
  dst.channels = MEM_new_array<MaterialPaintLayerChannel>(src.channels_num, __func__);
  dst.channels_num = src.channels_num;
  /* The record is plain data and DNA types delete their copy assignment, so it is copied as one
   * block, the way #BKE_paint_layers_channel_add grows the array. */
  memcpy(dst.channels, src.channels, sizeof(*dst.channels) * src.channels_num);
  for (const int i : IndexRange(src.channels_num)) {
    /* A map is painted in place, so a copy sharing its source's image would turn every stroke on
     * one into a stroke on the other: the copy gets its own. The fresh image carries the one user
     * that this record is; no separate id_us_plus is taken (as #BKE_id_copy returns it). */
    if (dst.channels[i].image != nullptr) {
      dst.channels[i].image = paint_layer_image_duplicate(bmain, *dst.channels[i].image);
    }
  }
}

/** A copy of \a src's Color ID colours linked on \a dst, each with its own record. */
static void paint_layer_id_colors_duplicate(MaterialPaintLayer &dst, const MaterialPaintLayer &src)
{
  for (const MaterialPaintLayerIdColor &color : src.id_colors) {
    MaterialPaintLayerIdColor *copy = MEM_new<MaterialPaintLayerIdColor>(__func__);
    copy->color[0] = color.color[0];
    copy->color[1] = color.color[1];
    copy->color[2] = color.color[2];
    copy->tolerance = color.tolerance;
    copy->flag = color.flag;
    BLI_addtail(&dst.id_colors, copy);
  }
}

/**
 * A copy of \a src's kind parameters onto \a dst. A stateful kind (a map-coverage operator's
 * tolerance and softness) keeps its values with the copy; the destination's own group, if it has
 * one already, is left alone.
 */
static void paint_layer_properties_duplicate(MaterialPaintLayer &dst, const MaterialPaintLayer &src)
{
  if (src.system_properties != nullptr && dst.system_properties == nullptr) {
    dst.system_properties = IDP_CopyProperty_ex(src.system_properties,
                                                LIB_ID_CREATE_NO_USER_REFCOUNT);
  }
}

/**
 * The copy of a Material row reads a copy of the material \a src reads, so the two rows keep
 * settings of their own. A row that has none, or a material that cannot be copied, is read shared.
 */
static void paint_layer_material_duplicate(Main &bmain,
                                           MaterialPaintLayer &dst,
                                           const MaterialPaintLayer &src)
{
  dst.material = src.material;
  if (dst.material == nullptr) {
    return;
  }
  if (src.source == MA_PAINT_LAYER_SOURCE_MATERIAL) {
    if (Material *copy = id_cast<Material *>(BKE_id_copy(&bmain, &src.material->id))) {
      /* The copy was made with one user, which the row takes in its place. */
      dst.material = copy;
      return;
    }
  }
  id_us_plus(&dst.material->id);
}

/**
 * An unlinked copy of the mask item or correction \a item: a marker of its own and maps of its own,
 * for the same reason as the channel maps.
 */
static MaterialPaintLayer *paint_layer_item_duplicate(Main &bmain,
                                                      Material &ma,
                                                      const MaterialPaintLayer &item)
{
  MaterialPaintLayer *item_copy = paint_layer_alloc(ma, item.name);
  item_copy->role = item.role;
  item_copy->source = item.source;
  item_copy->kind_subtype = item.kind_subtype;
  item_copy->blend = item.blend;
  item_copy->flag = item.flag;
  item_copy->opacity = item.opacity;
  /* The row's UV mapping is part of its state, not of its kind: a pasted fill or fill
   * correction has to keep the offset, scale, rotation, lock and wrap the gizmo edits. */
  item_copy->mapping = dna::shallow_copy(item.mapping);
  paint_layer_material_duplicate(bmain, *item_copy, item);
  paint_layer_channels_duplicate(bmain, *item_copy, item);
  paint_layer_id_colors_duplicate(*item_copy, item);
  paint_layer_properties_duplicate(*item_copy, item);
  return item_copy;
}

/**
 * Fill the linked \a dst with a copy of \a src's own state, its mask and, for a folder, its
 * children, each row with a marker of its own. \a dst is linked into \a ma before this runs, so
 * every marker handed out here is unique against it and against the rows copied before.
 */
static void paint_layer_branch_duplicate(Main &bmain,
                                         Material &ma,
                                         MaterialPaintLayer &dst,
                                         const MaterialPaintLayer &src)
{
  dst.flag = src.flag;
  dst.opacity = src.opacity;
  dst.blend = src.blend;
  dst.source = src.source;
  dst.kind_subtype = src.kind_subtype;
  memcpy(dst.channel_settings, src.channel_settings, sizeof(dst.channel_settings));
  /* Keep the row's UV mapping (offset, scale, rotation, lock and wrap) with the copy. */
  dst.mapping = dna::shallow_copy(src.mapping);
  paint_layer_material_duplicate(bmain, dst, src);
  paint_layer_channels_duplicate(bmain, dst, src);
  paint_layer_id_colors_duplicate(dst, src);
  paint_layer_properties_duplicate(dst, src);

  /* The mask and the corrections go with their row, with maps of their own for the same reason as
   * the channel maps. */
  const auto item_duplicate = [&](const MaterialPaintLayer &item,
                                  ListBaseT<MaterialPaintLayer> &r_list) {
    BLI_addtail(&r_list, paint_layer_item_duplicate(bmain, ma, item));
  };
  for (const MaterialPaintLayer &item : src.mask_stack) {
    item_duplicate(item, dst.mask_stack);
  }
  for (const MaterialPaintLayer &effect : src.effects) {
    item_duplicate(effect, dst.effects);
  }

  if (BKE_paint_layers_is_folder(src)) {
    for (const MaterialPaintLayer &child : src.children) {
      MaterialPaintLayer *child_copy = paint_layer_alloc(ma, child.name);
      BLI_addtail(&dst.children, child_copy);
      paint_layer_branch_duplicate(bmain, ma, *child_copy, child);
    }
  }
}

MaterialPaintLayer *BKE_paint_layers_duplicate(Main &bmain,
                                               Material &ma,
                                               MaterialPaintLayer *layer)
{
  /* A correction is copied beside itself in its layer's list, with maps of its own. */
  MaterialPaintLayer *effect_owner = (layer != nullptr) ?
                                         BKE_paint_layers_correction_owner(ma, *layer) :
                                         nullptr;
  /* So is a mask correction, in its row's mask; the base mask is copied with its row only. */
  MaterialPaintLayer *item_owner = (layer != nullptr &&
                                    (layer->flag & MA_PAINT_LAYER_MASK_BASE) == 0) ?
                                       BKE_paint_layers_mask_item_owner(ma, *layer) :
                                       nullptr;
  if (effect_owner == nullptr && item_owner == nullptr && !paint_layer_in_stack(ma, layer)) {
    return nullptr;
  }

  MaterialPaintLayer *copy = paint_layer_alloc(ma, layer->name);
  copy->role = layer->role;
  if (effect_owner != nullptr) {
    BLI_insertlinkafter(&effect_owner->effects, layer, copy);
  }
  else if (item_owner != nullptr) {
    BLI_insertlinkafter(&item_owner->mask_stack, layer, copy);
  }
  else {
    PaintLayerList *owner = BKE_paint_layers_owner_list(ma, *layer);
    BLI_insertlinkafter(owner, layer, copy);
  }
  paint_layer_branch_duplicate(bmain, ma, *copy, *layer);

  BKE_paint_layers_tag_edited(ma);
  return copy;
}

void BKE_paint_layers_default_channels_apply(Material &ma, MaterialPaintLayer &layer)
{
  /* Every channel the material's set names gets a record. A set that was never authored derives
   * from the layer records and the build default, so this is also how the default reaches a fresh
   * stack: one source of truth instead of a second hard-coded list here. */
  const uint16_t set = BKE_paint_layers_channel_set_mask_get(ma);
  for (const int channel : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    if ((set & (uint16_t(1) << channel)) == 0) {
      continue;
    }
    const eMaterialPaintChannel paint_channel = eMaterialPaintChannel(channel);
    if (paint_layer_channel_find(layer, paint_channel) != nullptr) {
      continue;
    }
    BKE_paint_layers_channel_add(ma, &layer, paint_channel);
  }
}

namespace {

/** The channels a layered material offers before any channel set is authored. Base Color included. */
constexpr uint16_t PAINT_LAYERS_DEFAULT_CHANNEL_SET =
    (uint16_t(1) << PAINT_MATERIAL_CHANNEL_BASE_COLOR) |
    (uint16_t(1) << PAINT_MATERIAL_CHANNEL_METALLIC) |
    (uint16_t(1) << PAINT_MATERIAL_CHANNEL_ROUGHNESS) |
    (uint16_t(1) << PAINT_MATERIAL_CHANNEL_NORMAL) |
    (uint16_t(1) << PAINT_MATERIAL_CHANNEL_AO);

}  // namespace

uint16_t BKE_paint_layers_default_channel_set()
{
  return PAINT_LAYERS_DEFAULT_CHANNEL_SET;
}

uint16_t BKE_paint_layers_channel_set_mask_get(const Material &ma)
{
  uint16_t mask = ma.paint_layers_channels;
  if (mask == 0) {
    /* Not authored: the union of the build default and every record any row already carries, so a
     * stack's own channels are never dropped from the set. */
    mask = PAINT_LAYERS_DEFAULT_CHANNEL_SET;
    BKE_paint_layers_foreach(
        ma, [&](const MaterialPaintLayer &layer, const MaterialPaintLayer * /*parent*/) -> bool {
          for (const int i : IndexRange(layer.channels_num)) {
            mask |= uint16_t(uint16_t(1) << int(layer.channels[i].channel));
          }
          for (const MaterialPaintLayer &effect : layer.effects) {
            for (const int i : IndexRange(effect.channels_num)) {
              mask |= uint16_t(uint16_t(1) << int(effect.channels[i].channel));
            }
          }
          return true;
        });
  }
  /* Base Color is the material's constant and can never leave the set. */
  mask |= uint16_t(1) << PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  return mask;
}

void BKE_paint_layers_channels_materialize(Material &ma)
{
  if (ma.paint_layers_channels != 0) {
    return;
  }
  /* Read the derived view while the field is still zero, so it carries the union of the rows and
   * the build default; the write then freezes it. The getter forces Base Color in, so the stored
   * mask is never zero and every later read takes the cheap branch. */
  ma.paint_layers_channels = BKE_paint_layers_channel_set_mask_get(ma);
}

bool BKE_paint_layers_channel_in_set(const Material &ma, const eMaterialPaintChannel channel)
{
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
    return false;
  }
  return (BKE_paint_layers_channel_set_mask_get(ma) & (uint16_t(1) << int(channel))) != 0;
}

bool BKE_paint_layers_channel_set_enable(Material &ma,
                                         const eMaterialPaintChannel channel,
                                         const bool enabled)
{
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
    return false;
  }
  /* Base Color is the material's constant, so switching it off would leave a set with no colour. */
  if (!enabled && channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
    return false;
  }
  uint16_t mask = BKE_paint_layers_channel_set_mask_get(ma);
  if (enabled) {
    mask |= uint16_t(1) << int(channel);
  }
  else {
    mask &= uint16_t(~(uint16_t(1) << int(channel)));
  }
  BKE_paint_layers_channel_set_mask_set(ma, mask);
  return true;
}

void BKE_paint_layers_channel_set_mask_set(Material &ma, uint16_t mask)
{
  /* Base Color is the material's constant and can never leave the set. */
  mask |= uint16_t(1) << PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  if (ma.paint_layers_channels == mask) {
    return;
  }
  ma.paint_layers_channels = mask;
  /* A material setting, not a row: the layers keep every record and map they had. */
  BKE_paint_layers_tag_edited(ma);
}

/**
 * Let go of every data-block \a layer holds a user on. The row's free routine never does, so
 * without this a removed row's map keeps a user and is written into the file as if it were still in
 * use. The data-blocks themselves stay in `Main`: an undo step restores the row, and a map's
 * unsaved pixels live only in memory, so freeing it here would bring the row back with the painting
 * gone. A map left with no user is an orphan, dropped on save or purge like any other.
 */
static void paint_layer_release_users(MaterialPaintLayer &layer)
{
  if (layer.material != nullptr) {
    id_us_min(&layer.material->id);
  }
  for (const int i : IndexRange(layer.channels_num)) {
    if (layer.channels[i].image != nullptr) {
      id_us_min(&layer.channels[i].image->id);
    }
  }
  /* The bake cache is the row's too: its maps lose the row's user and stay as orphans. */
  if (layer.bake != nullptr) {
    for (Image *image : layer.bake->images) {
      if (image != nullptr) {
        id_us_min(&image->id);
      }
    }
    if (layer.bake->coverage != nullptr) {
      id_us_min(&layer.bake->coverage->id);
    }
  }
  for (MaterialPaintLayer &item : layer.mask_stack) {
    paint_layer_release_users(item);
  }
  for (MaterialPaintLayer &effect : layer.effects) {
    paint_layer_release_users(effect);
  }
  for (MaterialPaintLayer &child : layer.children) {
    paint_layer_release_users(child);
  }
}

MaterialPaintLayer *BKE_paint_layers_correction_add(Material &ma,
                                                    MaterialPaintLayer *owner,
                                                    const char *name,
                                                    const eMaterialPaintLayerSource source)
{
  /* A folder takes one too: it corrects the isolated result of what the folder holds. A correction
   * is never a row of the stack and never carries corrections of its own. */
  if (!paint_layer_in_stack(ma, owner)) {
    return nullptr;
  }
  if (!ELEM(source, MA_PAINT_LAYER_SOURCE_IMAGE, MA_PAINT_LAYER_SOURCE_CONSTANT)) {
    return nullptr;
  }
  const bool fill = source == MA_PAINT_LAYER_SOURCE_CONSTANT;
  MaterialPaintLayer *effect = paint_layer_alloc(
      ma, name != nullptr ? name : (fill ? "Correction Fill" : "Correction Paint"));
  effect->role = MA_PAINT_LAYER_ROLE_EFFECT;
  effect->source = source;
  BLI_addtail(&owner->effects, effect);
  /* Linked first: the channel records are added through the ownership check. */
  if (fill) {
    /* A flat colour is one value: Base Color alone, so a channel the user never switched on is not
     * filled with it. Opaque black stands until the caller gives it a colour, as a Fill layer's
     * default does when its source changes. */
    if (MaterialPaintLayerChannel *base = BKE_paint_layers_channel_add(
            ma, effect, PAINT_MATERIAL_CHANNEL_BASE_COLOR))
    {
      const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
      copy_v4_v4(base->value, black);
    }
  }
  else {
    /* The records start transparent, so the correction lays nothing down until a stroke gives it
     * a map. */
    BKE_paint_layers_default_channels_apply(ma, *effect);
  }
  BKE_paint_layers_tag_edited(ma);
  return effect;
}

MaterialPaintLayer *BKE_paint_layers_correction_add(Material &ma,
                                                    MaterialPaintLayer *owner,
                                                    const char *name)
{
  return BKE_paint_layers_correction_add(ma, owner, name, MA_PAINT_LAYER_SOURCE_IMAGE);
}

MaterialPaintLayer *BKE_paint_layers_filter_add(Material &ma,
                                                MaterialPaintLayer *owner,
                                                const eMaterialPaintLayerFilter filter,
                                                const char *name)
{
  const PaintLayerKindInfo *kind = paint_layer_kind_find(MA_PAINT_LAYER_SOURCE_FILTER, filter);
  /* A map-coverage kind is only allowed as a mask item, so it can never be added as a content
   * correction; the role table is the one place the rule lives. */
  if (kind == nullptr || !paint_layer_in_stack(ma, owner) ||
      (kind->allowed_roles & paint_layer_role_bit(MA_PAINT_LAYER_ROLE_EFFECT)) == 0)
  {
    return nullptr;
  }
  MaterialPaintLayer *effect = paint_layer_alloc(ma, name != nullptr ? name : kind->name);
  effect->role = MA_PAINT_LAYER_ROLE_EFFECT;
  effect->source = MA_PAINT_LAYER_SOURCE_FILTER;
  effect->kind_subtype = filter;
  paint_layer_kind_params_init(*effect);
  BLI_addtail(&owner->effects, effect);
  /* Linked first, as the record is added through the ownership check. Base Color is the channel a
   * filter starts out working in; the others are switched on like a layer's. */
  BKE_paint_layers_channel_add(ma, effect, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  BKE_paint_layers_tag_edited(ma);
  return effect;
}

bool BKE_paint_layers_correction_remove(Material &ma, MaterialPaintLayer *effect)
{
  if (effect == nullptr) {
    return false;
  }
  MaterialPaintLayer *owner = BKE_paint_layers_correction_owner(ma, *effect);
  if (owner == nullptr) {
    return false;
  }
  if (!BLI_uuid_is_nil(ma.active_layer_marker) &&
      BLI_uuid_equal(ma.active_layer_marker, effect->marker))
  {
    ma.active_layer_marker = {};
  }
  paint_layer_release_users(*effect);
  BLI_remlink(&owner->effects, effect);
  BKE_material_paint_layer_free(effect);
  BKE_paint_layers_tag_edited(ma);
  return true;
}

bool BKE_paint_layers_remove(Material &ma, MaterialPaintLayer *layer)
{
  if (!paint_layer_in_stack(ma, layer)) {
    return false;
  }

  /* The cursor is cleared when it names the row or anything freed with it. */
  if (!BLI_uuid_is_nil(ma.active_layer_marker) &&
      BKE_paint_layers_subtree_contains(*layer, ma.active_layer_marker))
  {
    ma.active_layer_marker = {};
  }

  paint_layer_release_users(*layer);
  BLI_remlink(BKE_paint_layers_owner_list(ma, *layer), layer);
  BKE_material_paint_layer_free(layer);

  BKE_paint_layers_tag_edited(ma);
  return true;
}

MaterialPaintLayer *BKE_paint_layers_mask_base(MaterialPaintLayer &layer)
{
  for (MaterialPaintLayer &item : layer.mask_stack) {
    if ((item.flag & MA_PAINT_LAYER_MASK_BASE) != 0) {
      return &item;
    }
  }
  return nullptr;
}

const MaterialPaintLayer *BKE_paint_layers_mask_base(const MaterialPaintLayer &layer)
{
  for (const MaterialPaintLayer &item : layer.mask_stack) {
    if ((item.flag & MA_PAINT_LAYER_MASK_BASE) != 0) {
      return &item;
    }
  }
  return nullptr;
}

float BKE_paint_layers_mask_constant(const MaterialPaintLayer &item)
{
  const MaterialPaintLayerChannel *record = paint_layer_channel_find(
      item, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  return (record != nullptr) ? paint_layer_mask_constant(*record) : 1.0f;
}

MaterialPaintLayer *BKE_paint_layers_mask_add(Material &ma,
                                              MaterialPaintLayer *layer,
                                              const float value)
{
  if (!paint_layer_in_stack(ma, layer) || BKE_paint_layers_mask_base(*layer) != nullptr) {
    return nullptr;
  }
  /* The base mask is a constant item with the Multiply blend, so the row's coverage starts at
   * `F = value`. Its Base Color record holds the constant (opaque grey) until a stroke gives the
   * item a map; the item itself is never a row of the stack. */
  MaterialPaintLayer *item = paint_layer_alloc(ma, "Mask");
  item->role = MA_PAINT_LAYER_ROLE_MASK_ITEM;
  item->source = MA_PAINT_LAYER_SOURCE_CONSTANT;
  item->blend = MA_PAINT_LAYER_BLEND_MULTIPLY;
  item->flag = MA_PAINT_LAYER_ENABLED | MA_PAINT_LAYER_MASK_BASE;
  item->channels = MEM_new_array<MaterialPaintLayerChannel>(1, __func__);
  item->channels_num = 1;
  MaterialPaintLayerChannel &record = item->channels[0];
  record.channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  record.state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  record.image = nullptr;
  const float gray = clamp_f(value, 0.0f, 1.0f);
  record.value[0] = record.value[1] = record.value[2] = gray;
  record.value[3] = 1.0f;
  BLI_addhead(&layer->mask_stack, item);

  BKE_paint_layers_tag_edited(ma);
  return item;
}

MaterialPaintLayer *BKE_paint_layers_mask_item_owner(Material &ma, const MaterialPaintLayer &item)
{
  if (item.role != MA_PAINT_LAYER_ROLE_MASK_ITEM) {
    return nullptr;
  }
  MaterialPaintLayer *owner = nullptr;
  BKE_paint_layers_foreach(
      ma, [&](MaterialPaintLayer &row, MaterialPaintLayer * /*parent*/) -> bool {
        if (BLI_findindex(&row.mask_stack, &item) != -1) {
          owner = &row;
        }
        return owner == nullptr;
      });
  return owner;
}

MaterialPaintLayer *BKE_paint_layers_mask_item_add(Material &ma,
                                                   MaterialPaintLayer *layer,
                                                   const eMaterialPaintLayerSource source,
                                                   const char *name,
                                                   const float value)
{
  /* A correction lays over the base mask, so there has to be one; making it is the caller's call
   * (the Add policy does, white). */
  if (!paint_layer_in_stack(ma, layer) || BKE_paint_layers_mask_base(*layer) == nullptr) {
    return nullptr;
  }
  if (!ELEM(source, MA_PAINT_LAYER_SOURCE_IMAGE, MA_PAINT_LAYER_SOURCE_CONSTANT)) {
    return nullptr;
  }
  const bool fill = source == MA_PAINT_LAYER_SOURCE_CONSTANT;
  MaterialPaintLayer *item = paint_layer_alloc(
      ma, name != nullptr ? name : (fill ? "Mask Fill" : "Mask Paint"));
  item->role = MA_PAINT_LAYER_ROLE_MASK_ITEM;
  item->source = source;
  /* Mix: the correction says what the coverage is where it covers, rather than scaling it as the
   * base does. The blend is the item's own setting, like a row's. */
  item->blend = MA_PAINT_LAYER_BLEND_MIX;
  item->flag = MA_PAINT_LAYER_ENABLED;
  item->channels = MEM_new_array<MaterialPaintLayerChannel>(1, __func__);
  item->channels_num = 1;
  MaterialPaintLayerChannel &record = item->channels[0];
  record.channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  record.state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  record.image = nullptr;
  if (fill) {
    const float gray = clamp_f(value, 0.0f, 1.0f);
    record.value[0] = record.value[1] = record.value[2] = gray;
    record.value[3] = 1.0f;
  }
  /* A Paint item starts transparent: with no map it lays nothing, and its first map starts empty,
   * so a fresh item changes no pixel. */
  BLI_addtail(&layer->mask_stack, item);

  BKE_paint_layers_tag_edited(ma);
  return item;
}

MaterialPaintLayer *BKE_paint_layers_mask_filter_add(Material &ma,
                                                     MaterialPaintLayer *layer,
                                                     const eMaterialPaintLayerFilter filter,
                                                     const char *name)
{
  const PaintLayerKindInfo *kind = paint_layer_kind_find(MA_PAINT_LAYER_SOURCE_FILTER, filter);
  if (kind == nullptr || !paint_layer_in_stack(ma, layer) ||
      BKE_paint_layers_mask_base(*layer) == nullptr ||
      (kind->allowed_roles & paint_layer_role_bit(MA_PAINT_LAYER_ROLE_MASK_ITEM)) == 0)
  {
    return nullptr;
  }
  MaterialPaintLayer *item = paint_layer_alloc(ma, name != nullptr ? name : kind->name);
  item->role = MA_PAINT_LAYER_ROLE_MASK_ITEM;
  item->source = MA_PAINT_LAYER_SOURCE_FILTER;
  item->kind_subtype = filter;
  item->flag = MA_PAINT_LAYER_ENABLED;
  paint_layer_kind_params_init(*item);
  BLI_addtail(&layer->mask_stack, item);
  if (paint_layer_kind_is_map_coverage(*item)) {
    /* The Base Color record is structural: it holds the map the operator samples, with no user
     * channel switch. It starts with no map, so the item matches nothing until one is assigned. */
    if (MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
            ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR))
    {
      /* Its value is the Gradient output scale; start at one, the White result, so switching the
       * output to Gradient does not black the mask out before the user moves the slider. */
      const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
      copy_v4_v4(record->value, white);
    }
  }

  BKE_paint_layers_tag_edited(ma);
  return item;
}

bool BKE_paint_layers_mask_item_remove(Material &ma, MaterialPaintLayer *item)
{
  if (item == nullptr || (item->flag & MA_PAINT_LAYER_MASK_BASE) != 0) {
    return false;
  }
  MaterialPaintLayer *owner = BKE_paint_layers_mask_item_owner(ma, *item);
  if (owner == nullptr) {
    return false;
  }
  /* The row stays the one being worked on, so a stroke in Mask mode lands on its base mask. */
  if (BLI_uuid_equal(ma.active_layer_marker, item->marker)) {
    ma.active_layer_marker = owner->marker;
  }
  paint_layer_release_users(*item);
  BLI_remlink(&owner->mask_stack, item);
  BKE_material_paint_layer_free(item);
  BKE_paint_layers_tag_edited(ma);
  return true;
}

/** Whether \a item is a Color ID mask item owned by \a ma (the only kind that has id_colors). */
static bool paint_layer_map_coverage_item_owned(const Material &ma,
                                                const MaterialPaintLayer *item)
{
  return item != nullptr && item->role == MA_PAINT_LAYER_ROLE_MASK_ITEM &&
         paint_layer_kind_is_map_coverage(*item) &&
         item->kind_subtype == MA_PAINT_LAYER_FILTER_COLOR_ID && paint_layer_owned(ma, item);
}

MaterialPaintLayerIdColor *BKE_paint_layers_id_color_add(Material &ma,
                                                         MaterialPaintLayer *item,
                                                         const float color[3])
{
  if (!paint_layer_map_coverage_item_owned(ma, item)) {
    return nullptr;
  }
  MaterialPaintLayerIdColor *record = MEM_new<MaterialPaintLayerIdColor>(__func__);
  copy_v3_v3(record->color, color);
  record->flag = MA_PAINT_LAYER_ID_COLOR_ENABLED;
  /* New colours go on top of the list: the UI shows the newest first. */
  BLI_addhead(&item->id_colors, record);
  BKE_paint_layers_tag_edited(ma);
  return record;
}

bool BKE_paint_layers_id_color_remove(Material &ma,
                                      MaterialPaintLayer *item,
                                      MaterialPaintLayerIdColor *color)
{
  if (!paint_layer_map_coverage_item_owned(ma, item) || color == nullptr ||
      BLI_findindex(&item->id_colors, color) == -1)
  {
    return false;
  }
  BLI_remlink(&item->id_colors, color);
  MEM_delete(color);
  BKE_paint_layers_tag_edited(ma);
  return true;
}

bool BKE_paint_layers_id_color_set_enabled(Material &ma,
                                           MaterialPaintLayer *item,
                                           MaterialPaintLayerIdColor *color,
                                           const bool enabled)
{
  if (!paint_layer_map_coverage_item_owned(ma, item) || color == nullptr ||
      BLI_findindex(&item->id_colors, color) == -1)
  {
    return false;
  }
  /* Which colours take part is an input the graph reads, not a node: a value edit, no rebuild. */
  SET_FLAG_FROM_TEST(color->flag, enabled, MA_PAINT_LAYER_ID_COLOR_ENABLED);
  BKE_paint_layers_tag_value_edited(ma);
  return true;
}

bool BKE_paint_layers_id_color_set_color(Material &ma,
                                         MaterialPaintLayer *item,
                                         MaterialPaintLayerIdColor *color,
                                         const float value[3])
{
  if (!paint_layer_map_coverage_item_owned(ma, item) || color == nullptr ||
      BLI_findindex(&item->id_colors, color) == -1)
  {
    return false;
  }
  copy_v3_v3(color->color, value);
  BKE_paint_layers_tag_value_edited(ma);
  return true;
}

bool BKE_paint_layers_id_color_set_tolerance(Material &ma,
                                             MaterialPaintLayer *item,
                                             MaterialPaintLayerIdColor *color,
                                             const float tolerance)
{
  if (!paint_layer_map_coverage_item_owned(ma, item) || color == nullptr ||
      BLI_findindex(&item->id_colors, color) == -1)
  {
    return false;
  }
  color->tolerance = clamp_f(tolerance, 0.0f, 1.0f);
  BKE_paint_layers_tag_value_edited(ma);
  return true;
}

void BKE_paint_layers_map_coverage_image_use_data(Main &bmain, Image &image)
{
  const char *data_name = IMB_colormanagement_role_colorspace_name_get(COLOR_ROLE_DATA);
  if (data_name == nullptr || STREQ(image.colorspace_settings.name, data_name)) {
    return;
  }
  /* The ID Map is compared texel by texel against stored colours: an sRGB or filmic decode would
   * change the values the shader, the CPU mask and the picker read, so it is read as data. */
  STRNCPY(image.colorspace_settings.name, data_name);
  BKE_image_signal(&bmain, &image, nullptr, IMA_SIGNAL_COLORMANAGE);
}

float BKE_paint_layers_coverage_tolerance_get(const MaterialPaintLayer &item)
{
  return paint_layer_kind_param_get(item, PAINT_LAYER_COVERAGE_TOLERANCE);
}

float BKE_paint_layers_coverage_softness_get(const MaterialPaintLayer &item)
{
  return paint_layer_kind_param_get(item, PAINT_LAYER_COVERAGE_SOFTNESS);
}

bool BKE_paint_layers_mask_item_is_inverted(const MaterialPaintLayer &item)
{
  return paint_layer_mask_item_inverted(item);
}

bool BKE_paint_layers_mask_item_is_gradient(const MaterialPaintLayer &item)
{
  return (item.flag & MA_PAINT_LAYER_MASK_ITEM_GRADIENT) != 0;
}

bool BKE_paint_layers_mask_item_set_gradient(Material &ma,
                                             MaterialPaintLayer *item,
                                             const bool gradient)
{
  if (!paint_layer_map_coverage_item_owned(ma, item)) {
    return false;
  }
  /* The output scale is a value the graph reads, not a node: a value edit, no rebuild. */
  SET_FLAG_FROM_TEST(item->flag, gradient, MA_PAINT_LAYER_MASK_ITEM_GRADIENT);
  BKE_paint_layers_tag_value_edited(ma);
  return true;
}

bool BKE_paint_layers_mask_item_is_color_id_advanced(const MaterialPaintLayer &item)
{
  return (item.flag & MA_PAINT_LAYER_MASK_ITEM_COLOR_ID_ADVANCED) != 0;
}

bool BKE_paint_layers_mask_item_set_color_id_advanced(Material &ma,
                                                      MaterialPaintLayer *item,
                                                      const bool advanced)
{
  if (!paint_layer_map_coverage_item_owned(ma, item) ||
      item->kind_subtype != MA_PAINT_LAYER_FILTER_COLOR_ID)
  {
    return false;
  }
  if (BKE_paint_layers_mask_item_is_color_id_advanced(*item) == advanced) {
    return true;
  }
  /* Each colour grows its own tolerance input, so the graph's shape changes. */
  SET_FLAG_FROM_TEST(item->flag, advanced, MA_PAINT_LAYER_MASK_ITEM_COLOR_ID_ADVANCED);
  BKE_paint_layers_tag_edited(ma);
  return true;
}

bool BKE_paint_layers_mask_item_is_alpha_background(const MaterialPaintLayer &item)
{
  return (item.flag & MA_PAINT_LAYER_MASK_ITEM_ALPHA_BACKGROUND) != 0;
}

bool BKE_paint_layers_mask_item_set_alpha_background(Material &ma,
                                                     MaterialPaintLayer *item,
                                                     const bool alpha_background)
{
  if (!paint_layer_map_coverage_item_owned(ma, item)) {
    return false;
  }
  SET_FLAG_FROM_TEST(item->flag, alpha_background, MA_PAINT_LAYER_MASK_ITEM_ALPHA_BACKGROUND);
  BKE_paint_layers_tag_value_edited(ma);
  return true;
}

float BKE_paint_layers_mask_item_output_value(const MaterialPaintLayer &item)
{
  if (item.role != MA_PAINT_LAYER_ROLE_MASK_ITEM || !paint_layer_kind_is_map_coverage(item) ||
      !paint_layer_mask_item_gradient(item))
  {
    return 1.0f;
  }
  const MaterialPaintLayerChannel *record = paint_layer_map_coverage_record(item);
  return (record != nullptr) ? paint_layer_mask_constant(*record) : 1.0f;
}

bool BKE_paint_layers_is_map_coverage(const MaterialPaintLayer &item)
{
  return paint_layer_kind_is_map_coverage(item);
}

bool BKE_paint_layers_map_coverage_paintable(const MaterialPaintLayer &item)
{
  return paint_layer_kind_is_map_coverage(item) && paint_layer_kind_map_paintable(item);
}

Image *BKE_paint_layers_map_coverage_image_get(const MaterialPaintLayer &item)
{
  if (!paint_layer_map_coverage_has_map(item)) {
    return nullptr;
  }
  return paint_layer_map_coverage_record(item)->image;
}

MaterialPaintLayer *BKE_paint_layers_id_color_owner(Material &ma,
                                                    const MaterialPaintLayerIdColor &color)
{
  MaterialPaintLayer *owner = nullptr;
  BKE_paint_layers_foreach(
      ma, [&](MaterialPaintLayer &row, MaterialPaintLayer * /*parent*/) -> bool {
        for (MaterialPaintLayer &item : row.mask_stack) {
          if (BLI_findindex(&item.id_colors, &color) != -1) {
            owner = &item;
            return false;
          }
        }
        return true;
      });
  return owner;
}

bool BKE_paint_layers_mask_remove(Material &ma, MaterialPaintLayer *layer)
{
  if (!paint_layer_in_stack(ma, layer) || layer->mask_stack.first == nullptr) {
    return false;
  }
  if (BKE_paint_layers_active_mask_item_get(ma) != nullptr &&
      BKE_paint_layers_mask_item_owner(ma, *BKE_paint_layers_active_mask_item_get(ma)) == layer)
  {
    ma.active_layer_marker = layer->marker;
  }
  /* The whole mask goes, the base item with every correction over it: the base is never removed on
   * its own (#MA_PAINT_LAYER_MASK_BASE), a correction through #BKE_paint_layers_mask_item_remove. */
  for (MaterialPaintLayer &item : layer->mask_stack.items_mutable()) {
    paint_layer_release_users(item);
    BLI_remlink(&layer->mask_stack, &item);
    BKE_material_paint_layer_free(&item);
  }
  BKE_paint_layers_tag_edited(ma);
  return true;
}

bool BKE_paint_layers_mask_copy(Main &bmain,
                                Material &ma,
                                const MaterialPaintLayer &source,
                                MaterialPaintLayer &target)
{
  if (&source == &target || source.mask_stack.first == nullptr ||
      !paint_layer_in_stack(ma, &target))
  {
    return false;
  }
  /* A row has one mask, so the copy replaces whatever the target had. The source's items are read
   * after that, which is safe because the two rows are different. */
  BKE_paint_layers_mask_remove(ma, &target);
  for (const MaterialPaintLayer &item : source.mask_stack) {
    BLI_addtail(&target.mask_stack, paint_layer_item_duplicate(bmain, ma, item));
  }
  BKE_paint_layers_tag_edited(ma);
  return true;
}

bool BKE_paint_layers_correction_can_paste(const MaterialPaintLayer &source,
                                           const MaterialPaintLayer &target)
{
  if (target.role != MA_PAINT_LAYER_ROLE_LAYER) {
    return false;
  }
  switch (source.role) {
    case MA_PAINT_LAYER_ROLE_EFFECT:
      return true;
    case MA_PAINT_LAYER_ROLE_MASK_ITEM:
      /* A mask is a stack over its base, so an item that is not the base needs one to sit over; the
       * base itself is always welcome, becoming the target's base or a plain item over it. */
      return (source.flag & MA_PAINT_LAYER_MASK_BASE) != 0 ||
             BKE_paint_layers_mask_base(target) != nullptr;
    default:
      return false;
  }
}

MaterialPaintLayer *BKE_paint_layers_correction_paste(Main &bmain,
                                                      Material &ma,
                                                      const MaterialPaintLayer &source,
                                                      MaterialPaintLayer *target)
{
  if (!paint_layer_in_stack(ma, target) || !BKE_paint_layers_correction_can_paste(source, *target)) {
    return nullptr;
  }
  /* Why copy the images: a map is painted in place, so a pasted correction sharing its source's
   * image would turn every stroke on one into a stroke on the other. */
  MaterialPaintLayer *copy = paint_layer_item_duplicate(bmain, ma, source);
  if (source.role == MA_PAINT_LAYER_ROLE_MASK_ITEM) {
    if (BKE_paint_layers_mask_base(*target) == nullptr) {
      /* The first item of an empty mask is its base, whatever it was in its old home. */
      copy->flag |= MA_PAINT_LAYER_MASK_BASE;
      BLI_addhead(&target->mask_stack, copy);
    }
    else {
      /* One base per layer: the target's own stays, the copy is a plain item over it. */
      copy->flag = eMaterialPaintLayerFlag(int(copy->flag) & ~int(MA_PAINT_LAYER_MASK_BASE));
      BLI_addtail(&target->mask_stack, copy);
    }
  }
  else {
    BLI_addtail(&target->effects, copy);
  }
  BKE_paint_layers_tag_edited(ma);
  return copy;
}

MaterialPaintLayer *BKE_paint_layers_find(Material &ma, const bUUID &marker)
{
  MaterialPaintLayer *found = nullptr;
  BKE_paint_layers_foreach(
      ma, [&](MaterialPaintLayer &layer, MaterialPaintLayer * /*parent*/) -> bool {
        if (BLI_uuid_equal(layer.marker, marker)) {
          found = &layer;
        }
        /* A content correction is addressed by its own marker, like a row. */
        for (MaterialPaintLayer &effect : layer.effects) {
          if (found == nullptr && BLI_uuid_equal(effect.marker, marker)) {
            found = &effect;
          }
        }
        /* A mask item is too: the generator's mask inputs name the item that owns them. */
        for (MaterialPaintLayer &item : layer.mask_stack) {
          if (found == nullptr && BLI_uuid_equal(item.marker, marker)) {
            found = &item;
          }
        }
        return found == nullptr;
      });
  return found;
}

void BKE_paint_layers_active_set(Material &ma, const bUUID &marker)
{
  /* Leaving a row is when its deferred bake becomes due: the row and its ancestor folders left
   * behind may carry a cache that has to catch up (see #BKE_paint_layers_bake_row_is_deferred). */
  if (!BLI_uuid_equal(ma.active_layer_marker, marker)) {
    ma.paint_layers_flag |= MA_PAINT_LAYERS_BAKE_STALE;
  }
  ma.active_layer_marker = marker;
}

bUUID BKE_paint_layers_active_get(const Material &ma)
{
  return ma.active_layer_marker;
}

MaterialPaintLayer *BKE_paint_layers_active_layer_get(Material &ma)
{
  const bUUID marker = BKE_paint_layers_active_get(ma);
  if (BLI_uuid_is_nil(marker)) {
    return nullptr;
  }
  MaterialPaintLayer *layer = BKE_paint_layers_find(ma, marker);
  /* A mask item is no row: its row is what the content is painted on, what a copy takes and what an
   * Add anchors to, so every caller that wants "the layer" gets it. The item itself is
   * #BKE_paint_layers_active_mask_item_get. */
  if (layer != nullptr && layer->role == MA_PAINT_LAYER_ROLE_MASK_ITEM) {
    return BKE_paint_layers_mask_item_owner(ma, *layer);
  }
  return layer;
}

MaterialPaintLayer *BKE_paint_layers_active_mask_item_get(Material &ma)
{
  const bUUID marker = BKE_paint_layers_active_get(ma);
  if (BLI_uuid_is_nil(marker)) {
    return nullptr;
  }
  MaterialPaintLayer *layer = BKE_paint_layers_find(ma, marker);
  if (layer != nullptr && layer->role == MA_PAINT_LAYER_ROLE_MASK_ITEM &&
      (layer->flag & MA_PAINT_LAYER_MASK_BASE) == 0)
  {
    return layer;
  }
  return nullptr;
}

bool BKE_paint_layers_move(Material &ma,
                           MaterialPaintLayer *layer,
                           MaterialPaintLayer *anchor,
                           const PaintLayerPlace place)
{
  if (!paint_layer_in_stack(ma, layer) || anchor == layer) {
    return false;
  }
  if (anchor != nullptr) {
    /* A row moved into its own subtree would walk itself to reach itself. */
    if (!paint_layer_anchor_valid(ma, anchor, place) ||
        BKE_paint_layers_subtree_contains(*layer, anchor->marker))
    {
      return false;
    }
  }

  BLI_remlink(BKE_paint_layers_owner_list(ma, *layer), layer);
  paint_layer_link(ma, layer, anchor, place);
  BKE_paint_layers_tag_edited(ma);
  return true;
}

bool BKE_paint_layers_reorder(Material &ma, MaterialPaintLayer *layer, int index)
{
  if (!paint_layer_in_stack(ma, layer)) {
    return false;
  }
  PaintLayerList *owner = BKE_paint_layers_owner_list(ma, *layer);
  const int count = BLI_listbase_count(owner);
  index = clamp_i(index, 0, count - 1);

  BLI_remlink(owner, layer);
  /* The list is bottom-to-top, so index 0 is the first link of it. */
  if (index == count - 1) {
    BLI_addtail(owner, layer);
  }
  else if (index == 0) {
    BLI_addhead(owner, layer);
  }
  else {
    BLI_insertlinkbefore(owner, BLI_findlink(owner, index), layer);
  }
  BKE_paint_layers_tag_edited(ma);
  return true;
}

MaterialPaintLayer *BKE_paint_layers_group(Material &ma, const Span<MaterialPaintLayer *> layers)
{
  if (layers.is_empty()) {
    return nullptr;
  }
  PaintLayerList *owner = nullptr;
  for (MaterialPaintLayer *layer : layers) {
    if (layer == nullptr) {
      return nullptr;
    }
    PaintLayerList *layer_owner = BKE_paint_layers_owner_list(ma, *layer);
    if (layer_owner == nullptr || (owner != nullptr && owner != layer_owner)) {
      return nullptr;
    }
    owner = layer_owner;
  }

  /* The members are taken in the order of the list, not of \a layers, so their relative order
   * survives the fold whatever order the caller named them in, and a row named twice moves once. */
  Vector<MaterialPaintLayer *> members;
  for (MaterialPaintLayer &row : *owner) {
    if (layers.contains(&row)) {
      members.append(&row);
    }
  }

  /* The folder takes the position of its lowest member: the fold replaces the run the members came
   * from, so the rows that were below it stay below and the ones above stay above. The index counts
   * the surviving rows below that slot, because the members are removed first. */
  const int insert_index = BLI_findindex(owner, members.first());

  MaterialPaintLayer *folder = paint_layer_alloc(ma, "Folder");
  folder->source = MA_PAINT_LAYER_SOURCE_STACK;
  for (MaterialPaintLayer *layer : members) {
    BLI_remlink(owner, layer);
    BLI_addtail(&folder->children, layer);
  }
  if (insert_index >= BLI_listbase_count(owner)) {
    BLI_addtail(owner, folder);
  }
  else {
    BLI_insertlinkbefore(owner, BLI_findlink(owner, insert_index), folder);
  }
  BKE_paint_layers_tag_edited(ma);
  return folder;
}

bool BKE_paint_layers_ungroup(Material &ma, MaterialPaintLayer *folder)
{
  if (folder == nullptr) {
    return false;
  }
  PaintLayerList *owner = BKE_paint_layers_owner_list(ma, *folder);
  if (owner == nullptr || !BKE_paint_layers_is_folder(*folder)) {
    return false;
  }

  /* Only the folder itself disappears: a row that lifts out keeps its marker, so an active cursor
   * naming one stays valid. */
  if (!BLI_uuid_is_nil(ma.active_layer_marker) &&
      BLI_uuid_equal(ma.active_layer_marker, folder->marker))
  {
    ma.active_layer_marker = {};
  }

  /* The folder's own mask and corrections go with it, and their maps lose that user. */
  for (MaterialPaintLayer &item : folder->mask_stack) {
    paint_layer_release_users(item);
  }
  for (MaterialPaintLayer &effect : folder->effects) {
    paint_layer_release_users(effect);
  }

  /* The children lift into the folder's slot in their own order. */
  PaintLayerList lifted = folder->children;
  folder->children = {nullptr, nullptr};
  for (MaterialPaintLayer &child : lifted.items_mutable()) {
    BLI_remlink(&lifted, &child);
    BLI_insertlinkbefore(owner, folder, &child);
  }
  BLI_remlink(owner, folder);
  BKE_material_paint_layer_free(folder);

  BKE_paint_layers_tag_edited(ma);
  return true;
}

bool BKE_paint_layers_channel_set_value(Material &ma,
                                        MaterialPaintLayer *layer,
                                        const eMaterialPaintChannel channel,
                                        const float value[4])
{
  MaterialPaintLayerChannel *record = paint_layer_channel_in_stack(ma, layer, channel);
  if (record == nullptr) {
    return false;
  }
  copy_v4_v4(record->value, value);
  /* A Fill row lays its Base-Color (and every other map-less channel's) constant through a group
   * input, so changing a value there is value-only. A Paint row's value alpha decides whether the
   * channel takes part at all, which is topology: it stays a regeneration. */
  if (layer != nullptr && layer->source == MA_PAINT_LAYER_SOURCE_CONSTANT) {
    BKE_paint_layers_tag_value_edited(ma);
  }
  else {
    BKE_paint_layers_tag_edited(ma);
  }
  return true;
}

bool BKE_paint_layers_channel_set_image(Material &ma,
                                        MaterialPaintLayer *layer,
                                        const eMaterialPaintChannel channel,
                                        Image *image)
{
  MaterialPaintLayerChannel *record = paint_layer_channel_in_stack(ma, layer, channel);
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
                                                        const eMaterialPaintChannel channel)
{
  if (!paint_layer_owned(ma, layer)) {
    return nullptr;
  }
  /* A folder carries no maps of its own: it takes part in a channel through its children. A row
   * that reads a material has none either: a record would be one the kind never reads. */
  if (BKE_paint_layers_is_folder(*layer) ||
      paint_layer_kind_has_flag(*layer, PAINT_LAYER_KIND_READS_SOURCE))
  {
    return nullptr;
  }
  if (MaterialPaintLayerChannel *existing = paint_layer_channel_find(*layer, channel)) {
    return existing;
  }
  MaterialPaintLayerChannel *channels = MEM_new_array<MaterialPaintLayerChannel>(
      layer->channels_num + 1, __func__);
  if (layer->channels_num > 0) {
    memcpy(channels, layer->channels, sizeof(*channels) * layer->channels_num);
  }
  /* A fresh record is transparent: it takes part but lays nothing down until painted. */
  MaterialPaintLayerChannel &record = channels[layer->channels_num];
  record.channel = channel;
  record.state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  record.image = nullptr;
  zero_v4(record.value);
  if (channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
    /* Still transparent, but the colour is the flat tangent (#8080FF), the brush channel's default,
     * so a Fill's constant and the UI swatch do not read as a black, tilted normal. */
    record.value[0] = 0.5f;
    record.value[1] = 0.5f;
    record.value[2] = 1.0f;
  }
  /* A Fill correction's other channel starts at the colour it already lays, so switching one on
   * changes nothing until its own value is edited. Normal keeps the flat tangent above: a Fill lays
   * nothing there, and a map it grows later starts from that. */
  if (layer->role == MA_PAINT_LAYER_ROLE_EFFECT && layer->source == MA_PAINT_LAYER_SOURCE_CONSTANT &&
      !ELEM(channel, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PAINT_MATERIAL_CHANNEL_NORMAL))
  {
    if (const MaterialPaintLayerChannel *base = paint_layer_channel_find(
            *layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR))
    {
      copy_v4_v4(record.value, base->value);
    }
  }

  MEM_delete(layer->channels);
  layer->channels = channels;
  layer->channels_num++;
  /* A new record is an explicit participation. Once the set is materialized a channel a record
   * names must enter it, otherwise the stored mask would disagree with the derived union a zero
   * field computes. A zero field stays derived, so this is skipped until something materializes. */
  if (ma.paint_layers_channels != 0 && channel >= 0 && channel < PAINT_MATERIAL_CHANNEL_NUM) {
    ma.paint_layers_channels |= uint16_t(uint16_t(1) << int(channel));
  }
  BKE_paint_layers_tag_edited(ma);
  return &layer->channels[layer->channels_num - 1];
}

bool BKE_paint_layers_channel_remove(Material &ma,
                                     MaterialPaintLayer *layer,
                                     const eMaterialPaintChannel channel)
{
  MaterialPaintLayerChannel *record = paint_layer_channel_in_stack(ma, layer, channel);
  if (record == nullptr) {
    return false;
  }
  /* Same as removing a row: the record was one of the users keeping its map alive. */
  if (record->image != nullptr) {
    id_us_min(&record->image->id);
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
                                          const eMaterialPaintChannel channel,
                                          const bool enabled)
{
  MaterialPaintLayerChannel *record = paint_layer_channel_in_stack(ma, layer, channel);
  if (record == nullptr) {
    return false;
  }
  /* A disabled record keeps its value and map, so re-enabling restores the channel untouched. */
  record->state = enabled ? MA_PAINT_LAYER_CHANNEL_ENABLED : MA_PAINT_LAYER_CHANNEL_DISABLED;
  BKE_paint_layers_tag_edited(ma);
  return true;
}

bool BKE_paint_layers_channel_set_invert(Material &ma,
                                         MaterialPaintLayer *layer,
                                         const eMaterialPaintChannel channel,
                                         const bool invert)
{
  MaterialPaintLayerChannel *record = paint_layer_channel_in_stack(ma, layer, channel);
  if (record == nullptr) {
    return false;
  }
  if (invert) {
    record->flag |= MA_PAINT_LAYER_CHANNEL_INVERT;
  }
  else {
    record->flag &= ~MA_PAINT_LAYER_CHANNEL_INVERT;
  }
  BKE_paint_layers_tag_edited(ma);
  return true;
}

bool BKE_paint_layers_normal_set_directx(Material &ma, MaterialPaintLayer *layer, const bool directx)
{
  MaterialPaintLayerChannel *record = paint_layer_channel_in_stack(
      ma, layer, PAINT_MATERIAL_CHANNEL_NORMAL);
  if (record == nullptr) {
    return false;
  }
  if (directx) {
    record->flag |= MA_PAINT_LAYER_CHANNEL_NORMAL_DIRECTX;
  }
  else {
    record->flag &= ~MA_PAINT_LAYER_CHANNEL_NORMAL_DIRECTX;
  }
  BKE_paint_layers_tag_edited(ma);
  return true;
}

/* The mapping sits between pointer members of #MaterialPaintLayer: a size that is not a multiple of
 * 8 would leave the row's DNA with a hole. */
static_assert(sizeof(MaterialPaintLayerMapping) == 32, "MaterialPaintLayerMapping must be 32 bytes");

static_assert(sizeof(MaterialPaintLayer::channel_settings) /
                      sizeof(MaterialPaintLayerChannelSettings) ==
                  PAINT_MATERIAL_CHANNEL_NUM,
              "MaterialPaintLayer::channel_settings is indexed by channel; the literal size must "
              "match");

bool BKE_paint_layers_channel_opacity_set(Material &ma,
                                          MaterialPaintLayer &layer,
                                          const int channel,
                                          const float opacity)
{
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM || !paint_layer_owned(ma, &layer)) {
    return false;
  }
  const bool was_pass_through = BKE_paint_layers_folder_is_pass_through(layer);
  layer.channel_settings[channel].opacity = clamp_f(opacity, 0.0f, 1.0f);
  /* An input of the row's group like the row's own opacity, so no new graph shape -- unless it
   * moves a folder across the Pass Through boundary, which changes which nodes exist. */
  if (BKE_paint_layers_folder_is_pass_through(layer) != was_pass_through) {
    BKE_paint_layers_tag_edited(ma);
  }
  else {
    BKE_paint_layers_tag_value_edited(ma);
  }
  return true;
}

bool BKE_paint_layers_channel_blend_set(Material &ma,
                                        MaterialPaintLayer &layer,
                                        const int channel,
                                        const int blend)
{
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM || !paint_layer_owned(ma, &layer)) {
    return false;
  }
  /* The Normal channel forces its combine in the generator and the CPU, and ignores the row's
   * blend, so Replace (or inherit, back to the combine) is the only override that takes effect
   * there. */
  if (channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
    if (!ELEM(blend, -1, MA_PAINT_LAYER_BLEND_NORMAL_REPLACE)) {
      return false;
    }
  }
  else if (blend < -1 || blend > MA_PAINT_LAYER_BLEND_VALUE ||
           blend == MA_PAINT_LAYER_BLEND_NORMAL_COMBINE)
  {
    /* Inherit, or one of the user-facing blends; Normal Combine is the Normal channel's own doing. */
    return false;
  }
  if (layer.channel_settings[channel].blend == blend) {
    return true;
  }
  const uint64_t shape = bke::paint_layers::paint_layer_blend_shape(layer);
  layer.channel_settings[channel].blend = int8_t(blend);
  /* Replace swaps the Normal node and a folder's blend can move it across Pass Through: only then
   * is the graph a different shape. Every other mode is the same Mix node with another type. */
  if (bke::paint_layers::paint_layer_blend_shape(layer) != shape) {
    BKE_paint_layers_tag_edited(ma);
  }
  else {
    paint_layers_tag_blend_edited(ma);
  }
  return true;
}

bool BKE_paint_layers_set_opacity(Material &ma, MaterialPaintLayer *layer, const float opacity)
{
  if (!paint_layer_owned(ma, layer)) {
    return false;
  }
  const bool was_pass_through = BKE_paint_layers_folder_is_pass_through(*layer);
  layer->opacity = clamp_f(opacity, 0.0f, 1.0f);
  /* Opacity is an input of the row's group, so this is value-only -- unless it moves a folder
   * across the Pass Through boundary, which changes which nodes exist. */
  if (BKE_paint_layers_folder_is_pass_through(*layer) != was_pass_through) {
    BKE_paint_layers_tag_edited(ma);
  }
  else {
    BKE_paint_layers_tag_value_edited(ma);
  }
  return true;
}

bool BKE_paint_layers_set_enabled(Material &ma, MaterialPaintLayer *layer, const bool enabled)
{
  if (!paint_layer_owned(ma, layer)) {
    return false;
  }
  SET_FLAG_FROM_TEST(layer->flag, enabled, MA_PAINT_LAYER_ENABLED);
  /* The enabled flag is folded into the row's opacity input (see BKE_paint_layers_effective_opacity),
   * so switching a row off is a value, not a different shape of graph. */
  BKE_paint_layers_tag_value_edited(ma);
  return true;
}

float BKE_paint_layers_mapping_scale_normalize(const float scale)
{
  /* Old files read back as zeroes; a zero axis would collapse UVs, so it means one. */
  if (scale == 0.0f) {
    return 1.0f;
  }
  if (fabsf(scale) < PAINT_LAYER_MAPPING_SCALE_MIN) {
    return copysignf(PAINT_LAYER_MAPPING_SCALE_MIN, scale);
  }
  return scale;
}

bool BKE_paint_layers_fill_reads_map(const Material &ma,
                                     const MaterialPaintLayer &row,
                                     const int channel)
{
  /* A Fill is never painted, but its channels may hold their own image, which is then a texture
   * source for that channel only; the other channels keep the constant. */
  if (!ELEM(row.role, MA_PAINT_LAYER_ROLE_LAYER, MA_PAINT_LAYER_ROLE_EFFECT) ||
      !paint_layer_kind_has_flag(row, PAINT_LAYER_KIND_FLAT_VALUE) ||
      paint_layer_channel_filtered(ma, channel))
  {
    return false;
  }
  const MaterialPaintLayerChannel *record = paint_layer_channel_find(row, channel);
  return paint_layer_channel_live(record) && record->image != nullptr;
}

bool BKE_paint_layers_mapping_supported(const Material &ma, const MaterialPaintLayer &layer)
{
  /* A Material row reads its source at the row's UV: a live Image Texture and a rendered channel
   * map are read through the row's Mapping, and the source graph is wrapped with the same
   * transform inside its wrapper. The source must exist; a material with no node tree has nothing
   * for the mapping to move. */
  if (paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_READS_SOURCE)) {
    return layer.material != nullptr && layer.material->nodetree != nullptr &&
           BKE_paint_layers_source_reads_uv(*layer.material);
  }
  /* Only a Fill's own map repeats under the remap: a painted map is written by strokes at the raw
   * UV, and a filter owns no map to move. */
  if (!paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_FLAT_VALUE)) {
    return false;
  }
  if (layer.role == MA_PAINT_LAYER_ROLE_MASK_ITEM) {
    /* A mask is scalar over its owner; its map lives in the Base Color record of the item itself,
     * which the global channel set never filters. */
    const MaterialPaintLayerChannel *record = paint_layer_channel_find(
        layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    return paint_layer_channel_live(record) && record->image != nullptr;
  }
  for (const int channel : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    if (BKE_paint_layers_fill_reads_map(ma, layer, channel)) {
      return true;
    }
  }
  return false;
}

bool BKE_paint_layers_row_is_paintable(const MaterialPaintLayer &row)
{
  return !paint_layer_kind_has_flag(row, PAINT_LAYER_KIND_FLAT_VALUE) &&
         !paint_layer_kind_has_flag(row, PAINT_LAYER_KIND_READS_SOURCE);
}

bool BKE_paint_layers_mapping_enabled_get(const MaterialPaintLayer &layer)
{
  return (layer.mapping.flag & MA_PAINT_LAYER_MAPPING_ENABLED) != 0;
}

bool BKE_paint_layers_mapping_applies(const Material &ma, const MaterialPaintLayer &layer)
{
  return BKE_paint_layers_mapping_enabled_get(layer) &&
         BKE_paint_layers_mapping_supported(ma, layer);
}

bool BKE_paint_layers_mapping_set_enabled(Material &ma,
                                          MaterialPaintLayer *layer,
                                          const bool enabled)
{
  if (!paint_layer_owned(ma, layer) || (enabled && !BKE_paint_layers_mapping_supported(ma, *layer)))
  {
    return false;
  }
  const bool changed = BKE_paint_layers_mapping_enabled_get(*layer) != enabled;
  SET_FLAG_FROM_TEST(layer->mapping.flag, enabled, MA_PAINT_LAYER_MAPPING_ENABLED);
  if (changed) {
    /* The row gains or loses its remap, which is a different shape of stack. */
    BKE_paint_layers_tag_edited(ma);
  }
  return true;
}

bool BKE_paint_layers_mapping_set_offset(Material &ma,
                                         MaterialPaintLayer *layer,
                                         const float offset[2])
{
  if (!paint_layer_owned(ma, layer)) {
    return false;
  }
  copy_v2_v2(layer->mapping.offset, offset);
  BKE_paint_layers_tag_value_edited(ma);
  return true;
}

bool BKE_paint_layers_mapping_set_scale(Material &ma,
                                        MaterialPaintLayer *layer,
                                        const float scale[2])
{
  if (!paint_layer_owned(ma, layer)) {
    return false;
  }
  /* Normalize through the one helper every consumer shares, so zeroes from old files and tiny
   * values behave the same everywhere. */
  float next[2] = {BKE_paint_layers_mapping_scale_normalize(scale[0]),
                   BKE_paint_layers_mapping_scale_normalize(scale[1])};
  if (BKE_paint_layers_mapping_scale_lock_get(*layer)) {
    /* The caller hands over both axes, so the edited one is the one that moved from the stored
     * value; when both moved (a multi-drag) the first axis leads. */
    const float stored_x = BKE_paint_layers_mapping_scale_normalize(layer->mapping.scale[0]);
    const float lead = (next[0] != stored_x) ? next[0] : next[1];
    next[0] = lead;
    next[1] = lead;
  }
  copy_v2_v2(layer->mapping.scale, next);
  BKE_paint_layers_tag_value_edited(ma);
  return true;
}

bool BKE_paint_layers_mapping_set_values(Material &ma,
                                         MaterialPaintLayer *layer,
                                         const float offset[2],
                                         const float scale[2],
                                         const float rotation)
{
  if (!paint_layer_owned(ma, layer)) {
    return false;
  }
  float next[2] = {BKE_paint_layers_mapping_scale_normalize(scale[0]),
                   BKE_paint_layers_mapping_scale_normalize(scale[1])};
  if (BKE_paint_layers_mapping_scale_lock_get(*layer)) {
    /* The caller owns the lead axis here, so a locked row takes the X value. */
    next[1] = next[0];
  }
  copy_v2_v2(layer->mapping.offset, offset);
  copy_v2_v2(layer->mapping.scale, next);
  layer->mapping.rotation = rotation;
  BKE_paint_layers_tag_value_edited(ma);
  return true;
}

bool BKE_paint_layers_mapping_scale_lock_get(const MaterialPaintLayer &layer)
{
  return (layer.mapping.flag & MA_PAINT_LAYER_MAPPING_SCALE_UNLOCKED) == 0;
}

bool BKE_paint_layers_mapping_set_scale_lock(Material &ma,
                                             MaterialPaintLayer *layer,
                                             const bool locked)
{
  if (!paint_layer_owned(ma, layer)) {
    return false;
  }
  SET_FLAG_FROM_TEST(layer->mapping.flag, !locked, MA_PAINT_LAYER_MAPPING_SCALE_UNLOCKED);
  if (locked) {
    /* Locking joins the axes at the first one, so what the row shows stays where the user left
     * the X value. A value-only change like any other scale edit. */
    const float joined = BKE_paint_layers_mapping_scale_normalize(layer->mapping.scale[0]);
    layer->mapping.scale[0] = joined;
    layer->mapping.scale[1] = joined;
    BKE_paint_layers_tag_value_edited(ma);
  }
  return true;
}

bool BKE_paint_layers_mapping_set_rotation(Material &ma,
                                           MaterialPaintLayer *layer,
                                           const float rotation)
{
  if (!paint_layer_owned(ma, layer)) {
    return false;
  }
  layer->mapping.rotation = rotation;
  BKE_paint_layers_tag_value_edited(ma);
  return true;
}

void BKE_paint_layers_mapping_wrap_axes(const int16_t wrap, bool &r_repeat_u, bool &r_repeat_v)
{
  r_repeat_u = ELEM(wrap, MA_PAINT_LAYER_MAPPING_WRAP_REPEAT, MA_PAINT_LAYER_MAPPING_WRAP_REPEAT_X);
  r_repeat_v = ELEM(wrap, MA_PAINT_LAYER_MAPPING_WRAP_REPEAT, MA_PAINT_LAYER_MAPPING_WRAP_REPEAT_Y);
}

int16_t BKE_paint_layers_mapping_wrap_get(const MaterialPaintLayer &layer)
{
  /* An out-of-range stored value reads as Repeat, the behavior before the option existed. */
  return (layer.mapping.wrap >= MA_PAINT_LAYER_MAPPING_WRAP_REPEAT &&
          layer.mapping.wrap <= MA_PAINT_LAYER_MAPPING_WRAP_CLIP) ?
             layer.mapping.wrap :
             int16_t(MA_PAINT_LAYER_MAPPING_WRAP_REPEAT);
}

bool BKE_paint_layers_mapping_set_wrap(Material &ma, MaterialPaintLayer *layer, const int16_t wrap)
{
  if (!paint_layer_owned(ma, layer)) {
    return false;
  }
  if (wrap < MA_PAINT_LAYER_MAPPING_WRAP_REPEAT || wrap > MA_PAINT_LAYER_MAPPING_WRAP_CLIP) {
    return false;
  }
  if (layer->mapping.wrap == wrap) {
    return true;
  }
  layer->mapping.wrap = wrap;
  /* The wrap adds or removes the clip factor's nodes, which is topology. */
  BKE_paint_layers_tag_edited(ma);
  return true;
}

bool BKE_paint_layers_rename(Material &ma, MaterialPaintLayer *layer, const char *name)
{
  if (!paint_layer_in_stack_or_effect(ma, layer)) {
    return false;
  }
  STRNCPY(layer->name, name != nullptr ? name : "");
  BKE_paint_layers_tag_edited(ma);
  return true;
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
    case MA_PAINT_LAYER_BLEND_NORMAL_REPLACE:
      /* The Normal channel builds a plain Mix for Replace; the code is never read there. */
      return MA_RAMP_BLEND;
  }
  return MA_RAMP_BLEND;
}

bool BKE_paint_layers_set_blend(Material &ma,
                                MaterialPaintLayer *layer,
                                const eMaterialPaintLayerBlend blend)
{
  if (!paint_layer_in_stack_or_effect(ma, layer)) {
    return false;
  }
  /* The Normal channel forces the combine operation by itself, and no other channel can express
   * it; a stored value would only ever be a switch the user cannot make effective. Replace belongs
   * to the Normal channel's per-channel setting alone. */
  if (ELEM(blend, MA_PAINT_LAYER_BLEND_NORMAL_COMBINE, MA_PAINT_LAYER_BLEND_NORMAL_REPLACE)) {
    return false;
  }
  if (layer->blend == blend) {
    return true;
  }
  const uint64_t shape = bke::paint_layers::paint_layer_blend_shape(*layer);
  layer->blend = blend;
  if (bke::paint_layers::paint_layer_blend_shape(*layer) != shape) {
    BKE_paint_layers_tag_edited(ma);
  }
  else {
    paint_layers_tag_blend_edited(ma);
  }
  return true;
}

bool BKE_paint_layers_set_source(Material &ma,
                                 MaterialPaintLayer *layer,
                                 const eMaterialPaintLayerSource source)
{
  if (!paint_layer_in_stack(ma, layer)) {
    return false;
  }
  if (!ELEM(source, MA_PAINT_LAYER_SOURCE_IMAGE, MA_PAINT_LAYER_SOURCE_CONSTANT)) {
    return false;
  }
  /* Turning a folder into a layer would orphan what it holds, and ungrouping is the one way out. A
   * Material row has no map to turn into a colour or back: it is added as one, and removed. */
  if (BKE_paint_layers_is_folder(*layer) || layer->source == MA_PAINT_LAYER_SOURCE_MATERIAL) {
    return false;
  }
  if (layer->source == source) {
    return true;
  }
  MaterialPaintLayerChannel *base = paint_layer_channel_find(*layer,
                                                             PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  if (source == MA_PAINT_LAYER_SOURCE_CONSTANT) {
    /* A painted map is not a constant, so the maps go; the records stay, because a per-channel
     * setting is not a pixel. Base Color already lives in its record, so it survives the flip; a
     * transparent one becomes an opaque black Fill so the row lays a clean constant. The users are
     * let go first, as a removed row does, so the orphaned maps are not saved as if still used. */
    paint_layer_release_users(*layer);
    for (const int i : IndexRange(layer->channels_num)) {
      layer->channels[i].image = nullptr;
    }
    if (base != nullptr && paint_layer_channel_live(base) && base->value[3] == 0.0f) {
      base->value[0] = base->value[1] = base->value[2] = 0.0f;
      base->value[3] = 1.0f;
    }
  }
  else {
    /* A Paint row lays nothing until it is painted, so the Fill's colour must not stay in the
     * record: an opaque value there would cover everything below. The record goes back to the
     * transparent state a fresh one has, and the first stroke's map starts blank. */
    if (base != nullptr && paint_layer_channel_live(base)) {
      zero_v4(base->value);
    }
  }
  layer->source = source;
  BKE_paint_layers_tag_edited(ma);
  return true;
}

/**
 * Whether a Material row of \a list or of what it holds reads \a target, through any depth.
 * \a visited keeps the walk finite and linear when the descriptions are not the acyclic graph the
 * edit paths keep them in (an ID remap, such as merging materials, can bypass those checks).
 */
static bool paint_layers_list_reads_material(const ListBaseT<MaterialPaintLayer> &list,
                                             const Material &target,
                                             Set<const Material *> &visited)
{
  for (const MaterialPaintLayer &layer : list) {
    if (layer.source == MA_PAINT_LAYER_SOURCE_MATERIAL && layer.material != nullptr) {
      if (layer.material == &target) {
        return true;
      }
      if (visited.add(layer.material) &&
          paint_layers_list_reads_material(layer.material->paint_layers, target, visited))
      {
        return true;
      }
    }
    if (paint_layers_list_reads_material(layer.children, target, visited) ||
        paint_layers_list_reads_material(layer.effects, target, visited) ||
        paint_layers_list_reads_material(layer.mask_stack, target, visited))
    {
      return true;
    }
  }
  return false;
}

bool BKE_paint_layers_material_depends_on(const Material &from, const Material &target)
{
  /* The descriptions only; the pair check in #BKE_paint_layers_set_material keeps them acyclic. */
  Set<const Material *> visited;
  visited.add(&from);
  return &from == &target || paint_layers_list_reads_material(from.paint_layers, target, visited);
}

PaintLayerMaterialSourceIssue BKE_paint_layers_material_source_issue(const Material &target,
                                                                     const Material &source)
{
  /* Tested apart from the walk: the walk folds "itself" into "reads", and a UI that explains the
   * refusal is worth the two words being different. */
  if (&source == &target) {
    return PaintLayerMaterialSourceIssue::Self;
  }
  if (BKE_paint_layers_material_depends_on(source, target)) {
    return PaintLayerMaterialSourceIssue::Cycle;
  }
  return PaintLayerMaterialSourceIssue::None;
}

PaintLayerImageMapIssue BKE_paint_layers_row_image_map_issue(const MaterialPaintLayer &row)
{
  const bool is_fill = ELEM(row.role, MA_PAINT_LAYER_ROLE_LAYER, MA_PAINT_LAYER_ROLE_EFFECT) &&
                       row.source == MA_PAINT_LAYER_SOURCE_CONSTANT;
  if (is_fill) {
    return PaintLayerImageMapIssue::None;
  }
  if (row.role == MA_PAINT_LAYER_ROLE_LAYER && row.source == MA_PAINT_LAYER_SOURCE_IMAGE) {
    return PaintLayerImageMapIssue::PaintRowOwnsMaps;
  }
  return PaintLayerImageMapIssue::TakesNoImage;
}

bool BKE_paint_layers_set_material(Material &ma, MaterialPaintLayer *layer, Material *source)
{
  if (!paint_layer_in_stack(ma, layer) || layer->source != MA_PAINT_LAYER_SOURCE_MATERIAL) {
    return false;
  }
  if (source == nullptr ||
      BKE_paint_layers_material_source_issue(ma, *source) != PaintLayerMaterialSourceIssue::None)
  {
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

bool BKE_paint_layers_material_source_can_make_local(const Material &ma,
                                                     const MaterialPaintLayer *layer)
{
  if (!paint_layer_in_stack(ma, layer) || layer->source != MA_PAINT_LAYER_SOURCE_MATERIAL) {
    return false;
  }
  /* A linked or overridden stack is not the user's to edit; localizing a source under it would
   * also leave a linked ID reading a local one. */
  if (!ID_IS_EDITABLE(&ma.id) || ID_IS_OVERRIDE_LIBRARY(&ma.id)) {
    return false;
  }
  const Material *source = layer->material;
  /* Only a directly linked source, the same rule the Outliner's Localize uses. */
  return source != nullptr && ID_IS_LINKED(&source->id) && (source->id.tag & ID_TAG_EXTERN) != 0;
}

Material *BKE_paint_layers_material_source_make_local(Main &bmain,
                                                      Material &ma,
                                                      MaterialPaintLayer *layer)
{
  if (!BKE_paint_layers_material_source_can_make_local(ma, layer)) {
    return nullptr;
  }
  ID &linked_id = layer->material->id;
  if (!BKE_lib_id_make_local(&bmain, &linked_id, LIB_ID_MAKELOCAL_ASSET_DATA_CLEAR)) {
    return nullptr;
  }
  /* A copy leaves its `newid` on the linked original; the remap is done, so drop it like the
   * Outliner's Localize does. */
  BKE_id_newptr_and_tag_clear(&linked_id);
  /* Localizing either strips the library from the ID in place or copies it and remaps every user,
   * including this row. Reading the row back is what tells which of the two happened. */
  Material *local = layer->material;
  if (local == nullptr || ID_IS_LINKED(&local->id)) {
    return nullptr;
  }
  /* The generated tree may still name the linked ID, and the bake hash covers the source. */
  BKE_paint_layers_tag_edited(ma);
  return local;
}

void BKE_paint_layers_base_color_get(const MaterialPaintLayer &layer, float r_color[4])
{
  const MaterialPaintLayerChannel *record = paint_layer_channel_find(
      layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  if (record != nullptr) {
    copy_v4_v4(r_color, record->value);
    return;
  }
  zero_v4(r_color);
}

bool BKE_paint_layers_base_color_active(const MaterialPaintLayer &layer)
{
  return paint_layer_channel_live(
      paint_layer_channel_find(layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
}

bool BKE_paint_layers_set_fill_color(Material &ma, MaterialPaintLayer *layer, const float color[4])
{
  /* A Fill correction hangs on a row rather than being one, and its colour lives in the same
   * record. */
  if (!paint_layer_in_stack_or_effect(ma, layer)) {
    return false;
  }
  /* A Fill's colour lives in its Base-Color record like every other channel; the record is created
   * on demand so a script cannot write into a field nothing reads. */
  if (paint_layer_channel_find(*layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR) == nullptr) {
    if (BKE_paint_layers_channel_add(ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR) == nullptr) {
      return false;
    }
  }
  return BKE_paint_layers_channel_set_value(
      ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, color);
}

/**
 * Report the adjustments of \a list that sit in an isolating folder: its accumulation is
 * pre-multiplied and partly covered, which a filter of the colour cannot read, so the generator and
 * the CPU composite both leave the row out.
 */
static void paint_layers_isolated_adjustments_report(const ListBaseT<MaterialPaintLayer> &list,
                                                     const bool isolating,
                                                     Vector<PaintLayersIssue> &r_issues)
{
  for (const MaterialPaintLayer &row : list) {
    if (isolating && paint_layer_row_is_adjustment(row)) {
      PaintLayersIssue issue;
      issue.layer = row.marker;
      issue.code = PaintLayersIssueCode::KindNotAllowed;
      issue.text = TIP_("An adjustment needs a Pass Through folder or the top of the stack");
      r_issues.append(issue);
    }
    if (BKE_paint_layers_is_folder(row)) {
      paint_layers_isolated_adjustments_report(
          row.children, isolating || !BKE_paint_layers_folder_is_pass_through(row), r_issues);
    }
  }
}

void BKE_paint_layers_issues_get(const Material &ma, Vector<PaintLayersIssue> &r_issues)
{
  paint_layers_isolated_adjustments_report(ma.paint_layers, false, r_issues);
  const bool normal_in_set = !paint_layer_channel_filtered(ma, PAINT_MATERIAL_CHANNEL_NORMAL);
  BKE_paint_layers_foreach(
      ma, [&](const MaterialPaintLayer &layer, const MaterialPaintLayer * /*parent*/) -> bool {
        const auto check_kind = [&](const MaterialPaintLayer &row, const bool is_owned) {
          if (paint_layer_kind_role_allowed(row)) {
            return;
          }
          PaintLayersIssue issue;
          issue.layer = layer.marker;
          if (is_owned) {
            issue.correction = row.marker;
          }
          issue.code = PaintLayersIssueCode::KindNotAllowed;
          issue.text = TIP_("This row's kind is unknown or not allowed in its role; it is ignored");
          r_issues.append(issue);
        };
        check_kind(layer, false);
        for (const MaterialPaintLayer &effect : layer.effects) {
          check_kind(effect, true);
        }
        for (const MaterialPaintLayer &item : layer.mask_stack) {
          check_kind(item, true);
        }
        /* An enabled map-coverage item with no map takes part and matches nothing; say so, or the
         * whole mask seems to vanish for no reason. */
        for (const MaterialPaintLayer &item : layer.mask_stack) {
          if (!paint_layer_kind_is_map_coverage(item) ||
              (item.flag & MA_PAINT_LAYER_ENABLED) == 0 || paint_layer_map_coverage_has_map(item))
          {
            continue;
          }
          PaintLayersIssue issue;
          issue.layer = layer.marker;
          issue.correction = item.marker;
          issue.channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
          issue.code = PaintLayersIssueCode::MapCoverageMissingMap;
          issue.text = TIP_("This mask item has no map; it matches nothing");
          r_issues.append(issue);
        }

        const bool folder = BKE_paint_layers_is_folder(layer);
        if (folder) {
          for (const int i : IndexRange(layer.channels_num)) {
            if (layer.channels[i].image == nullptr) {
              continue;
            }
            PaintLayersIssue issue;
            issue.layer = layer.marker;
            issue.channel = layer.channels[i].channel;
            issue.code = PaintLayersIssueCode::FolderHasMaps;
            issue.text = TIP_("A folder takes part through its layers; its own map is ignored");
            r_issues.append(issue);
          }
        }
        else if (!BLI_listbase_is_empty(&layer.children)) {
          PaintLayersIssue issue;
          issue.layer = layer.marker;
          issue.code = PaintLayersIssueCode::NonFolderHasChildren;
          issue.text = TIP_("Only a folder holds other layers; this row's nested rows are ignored");
          r_issues.append(issue);
        }
        /* A Fill correction that was given the Normal channel and holds no map in it: a flat value
         * is not a tangent normal, so it lays nothing there. One that was never given Normal says
         * nothing, as it does not claim to lay in it. */
        if (!normal_in_set) {
          return true;
        }
        for (const MaterialPaintLayer &effect : layer.effects) {
          if (effect.source != MA_PAINT_LAYER_SOURCE_CONSTANT ||
              (effect.flag & MA_PAINT_LAYER_ENABLED) == 0)
          {
            continue;
          }
          const MaterialPaintLayerChannel *normal = paint_layer_channel_find(
              effect, PAINT_MATERIAL_CHANNEL_NORMAL);
          if (!paint_layer_channel_live(normal) || normal->image != nullptr) {
            continue;
          }
          PaintLayersIssue issue;
          issue.layer = layer.marker;
          issue.correction = effect.marker;
          issue.channel = PAINT_MATERIAL_CHANNEL_NORMAL;
          issue.code = PaintLayersIssueCode::FillCorrectionOnNormal;
          issue.text = TIP_("A Fill correction has no effect on the Normal channel");
          r_issues.append(issue);
        }
        return true;
      });
}

float BKE_paint_layers_effective_opacity(const MaterialPaintLayer &layer)
{
  if ((layer.flag & MA_PAINT_LAYER_ENABLED) == 0) {
    return 0.0f;
  }
  return layer.opacity;
}

float BKE_paint_layers_channel_opacity_effective(const MaterialPaintLayer &layer, const int channel)
{
  const float row_opacity = BKE_paint_layers_effective_opacity(layer);
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
    return row_opacity;
  }
  return row_opacity * layer.channel_settings[channel].opacity;
}

int BKE_paint_layers_channel_blend_effective(const MaterialPaintLayer &layer, const int channel)
{
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
    return layer.blend;
  }
  const int blend = layer.channel_settings[channel].blend;
  return (blend < 0) ? layer.blend : blend;
}

bool BKE_paint_layers_normal_replace(const MaterialPaintLayer &layer)
{
  return layer.channel_settings[PAINT_MATERIAL_CHANNEL_NORMAL].blend ==
         MA_PAINT_LAYER_BLEND_NORMAL_REPLACE;
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

void BKE_paint_layers_maps_collect(const Main &bmain, Vector<Image *> &r_images)
{
  Set<Image *> seen;
  const auto add_channels = [&](const MaterialPaintLayer &row) {
    paint_layer_dependencies(row, [&](const PaintLayerDependency &dep) {
      if (dep.kind != PaintLayerDependencyKind::ImageMap || *dep.slot == nullptr) {
        return;
      }
      Image *image = reinterpret_cast<Image *>(*dep.slot);
      if (seen.add(image)) {
        r_images.append(image);
      }
    });
  };
  for (const Material &ma : bmain.materials) {
    if (!BKE_paint_layers_is_layered(ma)) {
      continue;
    }
    BKE_paint_layers_foreach(
        ma, [&](const MaterialPaintLayer &layer, const MaterialPaintLayer * /*parent*/) -> bool {
          add_channels(layer);
          for (const MaterialPaintLayer &effect : layer.effects) {
            add_channels(effect);
          }
          for (const MaterialPaintLayer &item : layer.mask_stack) {
            add_channels(item);
          }
          return true;
        });
  }
}

bool BKE_paint_layers_map_is_packed_on_save(const Image &image)
{
  return ID_IS_EDITABLE(&image.id) &&
         (image.source == IMA_SRC_GENERATED || BKE_image_has_packedfile(&image));
}

int BKE_paint_layers_pack_maps(const Main &bmain, const bool for_autosave, ReportList *reports)
{
  Vector<Image *> images;
  BKE_paint_layers_maps_collect(bmain, images);

  int packed_num = 0;
  for (Image *image : images) {
    if (!ID_IS_EDITABLE(&image->id)) {
      continue;
    }
    const bool generated = image->source == IMA_SRC_GENERATED;
    if (for_autosave) {
      /* The image editor's own flush already wrote the modified ones; what is left is a generated
       * map nothing has touched since it was made, which the file would still reopen empty. */
      if (!generated || image->autosave_packedfiles.first != nullptr) {
        continue;
      }
    }
    else if (!generated && !(BKE_image_has_packedfile(image) && BKE_image_is_dirty(image))) {
      /* A map already packed and not modified since is in the file as it stands; encoding it
       * again would cost seconds on a 4K map for the same bytes. */
      continue;
    }

    /* The pack encodes the buffer the image holds, so it has to be there; a packed file the pack
     * dropped for want of one would leave the map with nothing at all. */
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
    const bool has_buffer = ibuf != nullptr;
    if (has_buffer) {
      /* A map's alpha is its coverage; the pack writes as many channels as the color mode names,
       * and maps made with 24 planes would lose it. */
      ibuf->color_mode = ImColorMode::RGBA;
    }
    BKE_image_release_ibuf(image, ibuf, lock);

    const bool ok = has_buffer && (for_autosave ? BKE_image_autosave_memorypack(image) :
                                                  BKE_image_memorypack(image));
    if (ok) {
      packed_num++;
    }
    else if (reports != nullptr) {
      BKE_reportf(reports,
                  RPT_WARNING,
                  "Paint layer map \"%s\" could not be packed and will be empty in the file",
                  image->id.name + 2);
    }
  }
  return packed_num;
}

}  // namespace blender
