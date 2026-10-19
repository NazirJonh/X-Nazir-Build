/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_paint_layers_edit.hh"

#include <algorithm>

#include "BLI_listbase.h"
#include "BLI_vector.hh"

#include "DNA_material_types.h"
#include "DNA_scene_types.h"

#include "BKE_lib_id.hh"

#include "paint_layers_kind.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Default names
 * \{ */

/** Highest N among the rows named `"<base> N"` in \a list and every folder under it. */
static int paint_layer_name_number_max(const ListBaseT<MaterialPaintLayer> &list,
                                       const StringRef base)
{
  int highest = 0;
  for (const MaterialPaintLayer &layer : list) {
    const StringRef name = layer.name;
    if (name.startswith(base) && name.size() > base.size() + 1 && name[base.size()] == ' ') {
      const StringRef digits = name.drop_prefix(base.size() + 1);
      int number = 0;
      bool all_digits = true;
      for (const char c : digits) {
        all_digits &= (c >= '0' && c <= '9');
        number = (number < 100000) ? number * 10 + (c - '0') : number;
      }
      if (all_digits) {
        highest = std::max(highest, number);
      }
    }
    highest = std::max(highest, paint_layer_name_number_max(layer.children, base));
    /* A correction is numbered among the names too, so its default name stays unique. */
    highest = std::max(highest, paint_layer_name_number_max(layer.effects, base));
    highest = std::max(highest, paint_layer_name_number_max(layer.mask_stack, base));
  }
  return highest;
}

std::string BKE_paint_layers_unique_default_name(const Material &ma, const StringRef base)
{
  const int next = paint_layer_name_number_max(ma.paint_layers, base) + 1;
  return std::string(base) + " " + std::to_string(next);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add
 * \{ */

MaterialPaintLayer *BKE_paint_layers_add_with_policy(Material &ma,
                                                     const PaintLayerAddParams &params)
{
  if (params.kind == PaintLayerAddKind::Correction ||
      params.kind == PaintLayerAddKind::CorrectionFill)
  {
    const bool fill = params.kind == PaintLayerAddKind::CorrectionFill;
    /* An Add aimed at a correction means another one on the row it hangs on. */
    MaterialPaintLayer *owner = params.anchor;
    if (owner != nullptr) {
      if (MaterialPaintLayer *parent = BKE_paint_layers_correction_owner(ma, *owner)) {
        owner = parent;
      }
    }
    if (owner == nullptr) {
      return nullptr;
    }
    /* Numbered among the corrections of this one layer only: two layers each have their own
     * "Correction Paint 1", the way the rows of two folders do not share a count. */
    const char *name_base = fill ? "Correction Fill" : "Correction Paint";
    const std::string name = (params.name != nullptr) ?
                                 std::string(params.name) :
                                 std::string(name_base) + " " +
                                     std::to_string(
                                         paint_layer_name_number_max(owner->effects, name_base) +
                                         1);
    MaterialPaintLayer *effect = BKE_paint_layers_correction_add(
        ma,
        owner,
        name.c_str(),
        fill ? MA_PAINT_LAYER_SOURCE_CONSTANT : MA_PAINT_LAYER_SOURCE_IMAGE);
    if (effect != nullptr && fill && params.fill_color != nullptr) {
      BKE_paint_layers_set_fill_color(ma, effect, params.fill_color);
    }
    return effect;
  }
  if (params.kind == PaintLayerAddKind::Filter) {
    /* Like a correction, an Add aimed at one means another on the row it hangs on. */
    MaterialPaintLayer *owner = params.anchor;
    if (owner != nullptr) {
      if (MaterialPaintLayer *parent = BKE_paint_layers_correction_owner(ma, *owner)) {
        owner = parent;
      }
    }
    if (owner == nullptr) {
      return nullptr;
    }
    const PaintLayerKindInfo *kind = paint_layer_kind_find(MA_PAINT_LAYER_SOURCE_FILTER,
                                                           params.filter);
    if (kind == nullptr) {
      return nullptr;
    }
    const std::string name = (params.name != nullptr) ?
                                 std::string(params.name) :
                                 std::string(kind->name) + " " +
                                     std::to_string(
                                         paint_layer_name_number_max(owner->effects, kind->name) +
                                         1);
    return BKE_paint_layers_filter_add(
        ma, owner, eMaterialPaintLayerFilter(params.filter), name.c_str());
  }
  if (params.kind == PaintLayerAddKind::Adjustment) {
    const PaintLayerKindInfo *kind = paint_layer_kind_find(MA_PAINT_LAYER_SOURCE_FILTER,
                                                           params.filter);
    if (kind == nullptr || (kind->allowed_roles & paint_layer_role_bit(MA_PAINT_LAYER_ROLE_LAYER)) == 0)
    {
      return nullptr;
    }
    PaintLayerPlace place = params.place;
    if (params.anchor != nullptr && BKE_paint_layers_is_folder(*params.anchor) &&
        place == PaintLayerPlace::Above)
    {
      place = PaintLayerPlace::Into;
    }
    /* A filter of the colour cannot read an isolating folder's pre-multiplied, partly covered
     * accumulation, so only Pass Through folders (and the top of the stack) take one. */
    const MaterialPaintLayer *parent = nullptr;
    if (params.anchor != nullptr) {
      parent = (place == PaintLayerPlace::Into) ? params.anchor :
                                                  BKE_paint_layers_parent(ma, *params.anchor);
    }
    for (; parent != nullptr; parent = BKE_paint_layers_parent(ma, *parent)) {
      if (!BKE_paint_layers_folder_is_pass_through(*parent)) {
        return nullptr;
      }
    }
    const std::string name = (params.name != nullptr) ?
                                 std::string(params.name) :
                                 BKE_paint_layers_unique_default_name(ma, kind->name);
    MaterialPaintLayer *created = BKE_paint_layers_add(ma, name.c_str(), params.anchor, place);
    if (created == nullptr) {
      return nullptr;
    }
    created->source = MA_PAINT_LAYER_SOURCE_FILTER;
    created->kind_subtype = params.filter;
    paint_layer_kind_params_init(*created);
    /* The channels a filter works in are records like a layer's, without a map; it starts in Base
     * Color. */
    BKE_paint_layers_channel_add(ma, created, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    /* The kind decides which nodes the row builds, so the add's own tag was too early. */
    BKE_paint_layers_tag_edited(ma);
    return created;
  }
  if (params.kind == PaintLayerAddKind::MaskFilter) {
    /* Aimed at a correction or a mask item, an Add means one more on the row it hangs on. */
    MaterialPaintLayer *owner = params.anchor;
    if (owner != nullptr) {
      if (MaterialPaintLayer *parent = BKE_paint_layers_correction_owner(ma, *owner)) {
        owner = parent;
      }
      else if (MaterialPaintLayer *item_parent = BKE_paint_layers_mask_item_owner(ma, *owner)) {
        owner = item_parent;
      }
    }
    const PaintLayerKindInfo *kind = paint_layer_kind_find(MA_PAINT_LAYER_SOURCE_FILTER,
                                                           params.filter);
    if (owner == nullptr || kind == nullptr) {
      return nullptr;
    }
    /* Like any mask correction, it lays over the base mask: made first, white, so it changes
     * nothing until the filter does. */
    if (BKE_paint_layers_mask_base(*owner) == nullptr &&
        BKE_paint_layers_mask_add(ma, owner, 1.0f) == nullptr)
    {
      return nullptr;
    }
    const std::string name = (params.name != nullptr) ?
                                 std::string(params.name) :
                                 std::string(kind->name) + " " +
                                     std::to_string(
                                         paint_layer_name_number_max(owner->mask_stack, kind->name) +
                                         1);
    return BKE_paint_layers_mask_filter_add(
        ma, owner, eMaterialPaintLayerFilter(params.filter), name.c_str());
  }
  if (params.kind == PaintLayerAddKind::MaskCorrection ||
      params.kind == PaintLayerAddKind::MaskCorrectionFill)
  {
    const bool fill = params.kind == PaintLayerAddKind::MaskCorrectionFill;
    /* Aimed at a correction of either kind, an Add means one more on the row it hangs on. */
    MaterialPaintLayer *owner = params.anchor;
    if (owner != nullptr) {
      if (MaterialPaintLayer *parent = BKE_paint_layers_correction_owner(ma, *owner)) {
        owner = parent;
      }
      else if (MaterialPaintLayer *item_parent = BKE_paint_layers_mask_item_owner(ma, *owner)) {
        owner = item_parent;
      }
    }
    if (owner == nullptr) {
      return nullptr;
    }
    /* A correction lays over the base mask: made first, white, so it hides nothing until the
     * correction does and the user need not add one by hand. */
    if (BKE_paint_layers_mask_base(*owner) == nullptr &&
        BKE_paint_layers_mask_add(ma, owner, 1.0f) == nullptr)
    {
      return nullptr;
    }
    const char *mask_name_base = fill ? "Mask Fill" : "Mask Paint";
    const std::string mask_name = (params.name != nullptr) ?
                                      std::string(params.name) :
                                      std::string(mask_name_base) + " " +
                                          std::to_string(paint_layer_name_number_max(
                                                             owner->mask_stack, mask_name_base) +
                                                         1);
    MaterialPaintLayer *item = BKE_paint_layers_mask_item_add(
        ma,
        owner,
        fill ? MA_PAINT_LAYER_SOURCE_CONSTANT : MA_PAINT_LAYER_SOURCE_IMAGE,
        mask_name.c_str());
    if (item != nullptr && fill && params.fill_color != nullptr) {
      BKE_paint_layers_set_fill_color(ma, item, params.fill_color);
    }
    return item;
  }
  if (params.kind == PaintLayerAddKind::Material &&
      (params.source == nullptr ||
       BKE_paint_layers_material_source_issue(ma, *params.source) !=
           PaintLayerMaterialSourceIssue::None))
  {
    return nullptr;
  }
  const char *name_base = (params.kind == PaintLayerAddKind::Fill)   ? "Fill" :
                          (params.kind == PaintLayerAddKind::Folder) ? "Folder" :
                                                                       "Layer";
  std::string name;
  if (params.name != nullptr) {
    name = params.name;
  }
  else if (params.kind == PaintLayerAddKind::Material) {
    /* A material row goes by the name of what it reads, the way a dropped map names its layer. */
    name = params.source->id.name + 2;
  }
  else {
    name = BKE_paint_layers_unique_default_name(ma, name_base);
  }
  PaintLayerPlace place = params.place;
  if (params.fill_folder && params.anchor != nullptr &&
      BKE_paint_layers_is_folder(*params.anchor) && place == PaintLayerPlace::Above)
  {
    place = PaintLayerPlace::Into;
  }
  MaterialPaintLayer *created = BKE_paint_layers_add(ma, name.c_str(), params.anchor, place);
  if (created == nullptr) {
    return nullptr;
  }
  if (params.kind == PaintLayerAddKind::Material) {
    /* A material row owns no channel records: it lays what its source shows. The source was checked
     * above, so the row never exists without one. */
    created->source = MA_PAINT_LAYER_SOURCE_MATERIAL;
    Material *source = params.source;
    if (params.bmain != nullptr) {
      /* The copy is made once the row exists, so a refused Add leaves no orphan behind. */
      if (Material *copy = id_cast<Material *>(BKE_id_copy(params.bmain, &params.source->id))) {
        source = copy;
      }
    }
    BKE_paint_layers_set_material(ma, created, source);
    if (source != params.source) {
      /* The copy was made with one user, which the row now holds in its place. */
      id_us_min(&source->id);
    }
    return created;
  }
  if (params.kind == PaintLayerAddKind::Folder) {
    /* A folder owns no channel records: it takes part through its children. */
    created->source = MA_PAINT_LAYER_SOURCE_STACK;
    return created;
  }
  /* A Fill is a Constant row. Set before the channels, so a fresh Fill's default Base Color starts
   * transparent, the way a Fill expects until #BKE_paint_layers_set_fill_color gives it a colour. */
  if (params.kind == PaintLayerAddKind::Fill) {
    created->source = MA_PAINT_LAYER_SOURCE_CONSTANT;
  }
  /* The default channel set is a policy of its own (see the BKE helper); the Add only places
   * the row. */
  BKE_paint_layers_default_channels_apply(ma, *created);
  if (params.kind == PaintLayerAddKind::Fill && params.fill_color != nullptr) {
    BKE_paint_layers_set_fill_color(ma, created, params.fill_color);
  }
  return created;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Edits
 * \{ */

bool BKE_paint_layers_edit_move(Material &ma,
                                MaterialPaintLayer &from,
                                MaterialPaintLayer *anchor,
                                PaintLayerPlace place)
{
  /* A mask correction lives in its row's mask: it reorders there and never leaves it, and the base
   * stays the first item. */
  MaterialPaintLayer *from_item_owner = ((from.flag & MA_PAINT_LAYER_MASK_BASE) == 0) ?
                                            BKE_paint_layers_mask_item_owner(ma, from) :
                                            nullptr;
  MaterialPaintLayer *anchor_item_owner = (anchor != nullptr) ?
                                              BKE_paint_layers_mask_item_owner(ma, *anchor) :
                                              nullptr;
  if (from_item_owner != nullptr) {
    if (anchor == nullptr || anchor == &from || anchor_item_owner != from_item_owner) {
      return false;
    }
    BLI_remlink(&from_item_owner->mask_stack, &from);
    if (place == PaintLayerPlace::Below && (anchor->flag & MA_PAINT_LAYER_MASK_BASE) == 0) {
      BLI_insertlinkbefore(&from_item_owner->mask_stack, anchor, &from);
    }
    else {
      BLI_insertlinkafter(&from_item_owner->mask_stack, anchor, &from);
    }
    BKE_paint_layers_tag_edited(ma);
    return true;
  }
  MaterialPaintLayer *from_owner = BKE_paint_layers_correction_owner(ma, from);
  MaterialPaintLayer *anchor_owner = (anchor != nullptr) ?
                                         BKE_paint_layers_correction_owner(ma, *anchor) :
                                         nullptr;
  if (from_owner != nullptr) {
    /* A correction is never a row of the stack: it lands beside another correction, in that one's
     * layer, or joins a layer it is dropped on (a paste aimed at the active layer). */
    if (anchor == nullptr || anchor == &from) {
      return false;
    }
    if (anchor_owner != nullptr) {
      BLI_remlink(&from_owner->effects, &from);
      if (place == PaintLayerPlace::Below) {
        BLI_insertlinkbefore(&anchor_owner->effects, anchor, &from);
      }
      else {
        BLI_insertlinkafter(&anchor_owner->effects, anchor, &from);
      }
    }
    else {
      if (BKE_paint_layers_owner_list(ma, *anchor) == nullptr ||
          BKE_paint_layers_is_folder(*anchor))
      {
        return false;
      }
      BLI_remlink(&from_owner->effects, &from);
      BLI_addtail(&anchor->effects, &from);
    }
    BKE_paint_layers_tag_edited(ma);
    return true;
  }
  if (anchor_owner != nullptr || anchor_item_owner != nullptr) {
    /* A row aimed at a correction, of either kind, goes beside the layer that holds it, never
     * inside it. */
    anchor = (anchor_owner != nullptr) ? anchor_owner : anchor_item_owner;
    if (place == PaintLayerPlace::Into) {
      place = PaintLayerPlace::Above;
    }
  }
  if (place == PaintLayerPlace::Into && anchor != nullptr &&
      !BKE_paint_layers_is_folder(*anchor))
  {
    /* Dropping onto a plain row means "put these together": nothing here turns a row into a folder
     * in place, since its maps would then be ignored. */
    if (anchor == &from) {
      return false;
    }
    Vector<MaterialPaintLayer *> to_group{anchor, &from};
    return BKE_paint_layers_group(ma, to_group) != nullptr;
  }
  return BKE_paint_layers_move(ma, &from, anchor, place);
}

bool BKE_paint_layers_edit_reorder(Material &ma,
                                   MaterialPaintLayer &from,
                                   MaterialPaintLayer &to)
{
  if (&from == &to) {
    return false;
  }
  ListBaseT<MaterialPaintLayer> *owner = BKE_paint_layers_owner_list(ma, from);
  if (owner == nullptr || BKE_paint_layers_owner_list(ma, to) != owner) {
    return false;
  }
  return BKE_paint_layers_reorder(ma, &from, BLI_findindex(owner, &to));
}

MaterialPaintLayer *BKE_paint_layers_edit_merge_down(Material &ma,
                                                     MaterialPaintLayer &upper,
                                                     MaterialPaintLayer &lower)
{
  ListBaseT<MaterialPaintLayer> *owner = BKE_paint_layers_owner_list(ma, upper);
  if (owner == nullptr || BKE_paint_layers_owner_list(ma, lower) != owner || &upper == &lower) {
    return nullptr;
  }
  Vector<MaterialPaintLayer *> to_group{&lower, &upper};
  return BKE_paint_layers_group(ma, to_group);
}

MaterialPaintLayer *BKE_paint_layers_edit_group_range(Material &ma,
                                                      const Span<MaterialPaintLayer *> rows)
{
  return BKE_paint_layers_group(ma, rows);
}

MaterialPaintLayer *BKE_paint_layers_edit_group_add(Material &ma, MaterialPaintLayer *anchor)
{
  /* Beside the anchor even when it is a folder: this is the "new group" verb, and the redirect an
   * Add applies to a folder anchor would nest the new one inside it instead. */
  const std::string name = BKE_paint_layers_unique_default_name(ma, "Folder");
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      ma, name.c_str(), anchor, PaintLayerPlace::Above);
  if (folder != nullptr) {
    folder->source = MA_PAINT_LAYER_SOURCE_STACK;
  }
  return folder;
}

/** \} */

}  // namespace blender
