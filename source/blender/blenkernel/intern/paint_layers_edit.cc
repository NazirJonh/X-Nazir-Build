/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_paint_layers_edit.hh"

#include <algorithm>

#include "BLI_listbase.h"
#include "BLI_utildefines.h"

#include "DNA_material_types.h"
#include "DNA_node_types.h"

#include "BKE_idprop.hh"
#include "BKE_lib_id.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Default names
 * \{ */

/** Highest N among the rows named `"<base> N"` in \a list and everything nested under it. */
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

static bool paint_layer_add_kind_is_correction(const PaintLayerAddKind kind)
{
  return kind != PaintLayerAddKind::Paint && kind != PaintLayerAddKind::Fill &&
         kind != PaintLayerAddKind::Folder && kind != PaintLayerAddKind::Material;
}

static bool paint_layer_add_kind_is_mask(const PaintLayerAddKind kind)
{
  return kind == PaintLayerAddKind::MaskPaint || kind == PaintLayerAddKind::MaskFill ||
         kind == PaintLayerAddKind::MaskMeshMap || kind == PaintLayerAddKind::MaskMaterial ||
         kind == PaintLayerAddKind::MaskNodeGroup || kind == PaintLayerAddKind::MaskStack;
}

MaterialPaintLayer *BKE_paint_layers_add_with_policy(Material &ma,
                                                     const PaintLayerAddParams &params)
{
  const PaintLayerAddKind kind = params.kind;
  MaterialPaintLayer *anchor = params.anchor;
  if (paint_layer_add_kind_is_correction(kind)) {
    if (anchor == nullptr) {
      return nullptr;
    }
    /* A correction layers onto the owning row: aimed at a correction, it goes to that
     * correction's owner. A folder takes corrections too: content corrections edit its isolated
     * result, mask corrections edit its coverage, exactly as they do for a leaf. */
    MaterialPaintLayer *layer = (BKE_paint_layers_role(*anchor) != PaintLayerRole::Layer) ?
                                    BKE_paint_layers_parent(ma, *anchor) :
                                    anchor;
    if (layer == nullptr) {
      return nullptr;
    }
    const bool mask_section = paint_layer_add_kind_is_mask(kind);
    const bool fill_effect = ELEM(kind,
                                  PaintLayerAddKind::EffectFill,
                                  PaintLayerAddKind::MaskFill);
    int source = MA_PAINT_LAYER_SOURCE_IMAGE;
    if (fill_effect) {
      source = MA_PAINT_LAYER_SOURCE_CONSTANT;
    }
    else if (kind == PaintLayerAddKind::EffectMeshMap ||
             kind == PaintLayerAddKind::MaskMeshMap)
    {
      source = MA_PAINT_LAYER_SOURCE_MESH_MAP;
    }
    else if (kind == PaintLayerAddKind::EffectMaterial ||
             kind == PaintLayerAddKind::MaskMaterial)
    {
      source = MA_PAINT_LAYER_SOURCE_MATERIAL;
    }
    else if (kind == PaintLayerAddKind::EffectNodeGroup ||
             kind == PaintLayerAddKind::MaskNodeGroup)
    {
      source = MA_PAINT_LAYER_SOURCE_NODE_GROUP;
    }
    else if (kind == PaintLayerAddKind::EffectStack || kind == PaintLayerAddKind::MaskStack)
    {
      source = MA_PAINT_LAYER_SOURCE_STACK;
    }
    if (mask_section && BKE_paint_layers_mask_base(*layer) == nullptr) {
      /* A correction layers over the base mask; make the base first, so the user can paint in the
       * mask without adding one by hand and the hierarchy never shifts afterwards. */
      BKE_paint_layers_mask_add(ma, layer, 1.0f);
    }
    const char *base_name = fill_effect             ? "Fill" :
                            (source == MA_PAINT_LAYER_SOURCE_IMAGE) ? "Paint" :
                                                                      "Correction";
    MaterialPaintLayer *created = BKE_paint_layers_correction_add(
        ma,
        layer,
        mask_section ? MA_PAINT_LAYER_ROLE_MASK_ITEM : MA_PAINT_LAYER_ROLE_EFFECT,
        source,
        (params.name != nullptr) ? params.name : base_name,
        anchor);
    /* A fresh Fill starts with Base Color on; without any record it would be the legacy constant in
     * every channel, which fills channels the user never switched on. */
    if (created != nullptr && !mask_section && source == MA_PAINT_LAYER_SOURCE_CONSTANT) {
      BKE_paint_layers_channel_add(ma, created, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    }
    /* A fresh Paint correction has every channel of the set on, like a Paint layer. */
    if (created != nullptr && !mask_section && source == MA_PAINT_LAYER_SOURCE_IMAGE) {
      BKE_paint_layers_default_channels_apply(ma, *created);
    }
    /* Material and Node Group take their source from \a params.source, but unlike a Layer row
     * this is not an eager bake: the correction is left empty when no source is given (or the
     * wrong ID type is), pickable afterward through the same Source Material / Custom Group panel
     * a Layer row of that source already reuses. */
    if (created != nullptr && kind == PaintLayerAddKind::EffectMaterial &&
        params.source != nullptr && GS(params.source->name) == ID_MA)
    {
      BKE_paint_layers_set_material(ma, created, id_cast<Material *>(params.source));
    }
    else if (created != nullptr && kind == PaintLayerAddKind::EffectNodeGroup &&
             params.source != nullptr && GS(params.source->name) == ID_NT)
    {
      BKE_paint_layers_set_custom_group(ma, created, id_cast<bNodeTree *>(params.source));
    }
    return created;
  }

  PaintLayerPlace place = params.place;
  if (anchor != nullptr && BKE_paint_layers_is_folder(*anchor) &&
      place == PaintLayerPlace::Above)
  {
    place = PaintLayerPlace::Into;
  }
  if (kind == PaintLayerAddKind::Material) {
    /* A Material layer bakes a source material; the bake itself is requested by the caller that
     * has a context, so this stays description-only. */
    Material *source = (params.source != nullptr && GS(params.source->name) == ID_MA) ?
                           id_cast<Material *>(params.source) :
                           nullptr;
    if (source == nullptr) {
      return nullptr;
    }
    MaterialPaintLayer *created = BKE_paint_layers_add(
        ma, MA_PAINT_LAYER_SOURCE_MATERIAL, source->id.name + 2, anchor, place);
    if (created != nullptr && !BKE_paint_layers_set_material(ma, created, source)) {
      /* A refused source (the material bakes itself, or it depends on this one) must not read as
       * a silent add. */
      BKE_paint_layers_remove(ma, created);
      created = nullptr;
    }
    return created;
  }
  const char *name_base = (kind == PaintLayerAddKind::Fill)      ? "Fill" :
                          (kind == PaintLayerAddKind::Folder)    ? "Folder" :
                                                                   "Layer";
  const std::string name = (params.name != nullptr) ?
                               std::string(params.name) :
                               BKE_paint_layers_unique_default_name(ma, name_base);
  const eMaterialPaintLayerSource layer_source = (kind == PaintLayerAddKind::Fill) ?
                                                     MA_PAINT_LAYER_SOURCE_CONSTANT :
                                                 (kind == PaintLayerAddKind::Folder) ?
                                                     MA_PAINT_LAYER_SOURCE_STACK :
                                                     MA_PAINT_LAYER_SOURCE_IMAGE;
  MaterialPaintLayer *created = BKE_paint_layers_add(
      ma, layer_source, name.c_str(), anchor, place);
  if (created != nullptr) {
    /* The default channel set is a policy of its own (see the BKE helper); the Add only places
     * the row. */
    BKE_paint_layers_default_channels_apply(ma, *created);
  }
  if (created != nullptr && kind == PaintLayerAddKind::Fill && params.fill_color != nullptr) {
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
  /* Effect and mask-item rows are children of a layer, not stack members: they move only within
   * their owner's corrections and never into a folder. */
  auto is_correction = [](const MaterialPaintLayer *layer) {
    return layer != nullptr && BKE_paint_layers_role(*layer) != PaintLayerRole::Layer;
  };
  if (place == PaintLayerPlace::Into && anchor != nullptr && !BKE_paint_layers_is_folder(*anchor))
  {
    if (is_correction(&from) || is_correction(anchor)) {
      return false;
    }
    Vector<MaterialPaintLayer *> to_group{anchor, &from};
    return BKE_paint_layers_group(ma, to_group) != nullptr;
  }
  if (is_correction(&from) || is_correction(anchor)) {
    if (!is_correction(&from) || anchor == nullptr || !is_correction(anchor) ||
        place == PaintLayerPlace::Into)
    {
      return false;
    }
    ListBase *owner_list = BKE_paint_layers_owner_list(ma, from);
    if (owner_list == nullptr || BKE_paint_layers_owner_list(ma, *anchor) != owner_list) {
      return false;
    }
    const int anchor_index = BLI_findindex(owner_list, anchor);
    const int from_index = BLI_findindex(owner_list, &from);
    if (anchor_index < 0 || from_index < 0) {
      return false;
    }
    /* The screen lists attached rows top first (#outliner_stack_attached_rows_plan walks them
     * backward), so the row drawn above the anchor sits later in storage, and the row drawn below
     * it earlier. #BKE_paint_layers_reorder takes the index the row ends up at, after it left its
     * old slot, so an anchor behind the row has shifted one place by the time the row is put
     * back. */
    int target_index = (place == PaintLayerPlace::Below) ? anchor_index : anchor_index + 1;
    if (from_index < target_index) {
      target_index--;
    }
    return BKE_paint_layers_reorder(ma, &from, target_index);
  }
  return BKE_paint_layers_move(ma, &from, anchor, place);
}

bool BKE_paint_layers_edit_mask_set(Material &ma, MaterialPaintLayer &layer, const bool add)
{
  /* A correction's own coverage is its mask stack; a folder takes a mask like any row. */
  if (BKE_paint_layers_role(layer) != PaintLayerRole::Layer) {
    return false;
  }
  if (add) {
    /* One base mask per layer: a second Add Mask would move the base out from under the user. */
    if (BKE_paint_layers_mask_base(layer) != nullptr) {
      return false;
    }
    return BKE_paint_layers_mask_add(ma, &layer, 1.0f) != nullptr;
  }
  /* Remove Mask takes the base together with every correction over it: the mask is gone. */
  const Vector<MaterialPaintLayer *> items = BKE_paint_layers_mask_items(layer);
  bool removed = false;
  for (MaterialPaintLayer *item : items) {
    removed |= BKE_paint_layers_remove(ma, item);
  }
  return removed;
}

bool BKE_paint_layers_edit_mask_toggle(Material &ma, MaterialPaintLayer &layer)
{
  /* The layer's mask is its base item; corrections over it keep their own flags. */
  MaterialPaintLayer *base = BKE_paint_layers_mask_base(layer);
  if (base == nullptr) {
    return false;
  }
  return BKE_paint_layers_set_enabled(ma, base, (base->flag & MA_PAINT_LAYER_ENABLED) == 0);
}

bool BKE_paint_layers_edit_reorder(Material &ma,
                                   MaterialPaintLayer &from,
                                   MaterialPaintLayer &to)
{
  if (&from == &to || BKE_paint_layers_role(from) != PaintLayerRole::Layer ||
      BKE_paint_layers_role(to) != PaintLayerRole::Layer)
  {
    return false;
  }
  ListBase *owner_list = BKE_paint_layers_owner_list(ma, from);
  if (owner_list == nullptr || BKE_paint_layers_owner_list(ma, to) != owner_list) {
    return false;
  }
  const int index = BLI_findindex(owner_list, &to);
  return index >= 0 && BKE_paint_layers_reorder(ma, &from, index);
}

MaterialPaintLayer *BKE_paint_layers_edit_merge_down(Material &ma,
                                                     MaterialPaintLayer &upper,
                                                     MaterialPaintLayer &lower)
{
  if (BKE_paint_layers_role(upper) != PaintLayerRole::Layer ||
      BKE_paint_layers_role(lower) != PaintLayerRole::Layer)
  {
    return nullptr;
  }
  ListBase *owner_list = BKE_paint_layers_owner_list(ma, upper);
  if (owner_list == nullptr || BKE_paint_layers_owner_list(ma, lower) != owner_list) {
    return nullptr;
  }
  Vector<MaterialPaintLayer *> to_group{&lower, &upper};
  return BKE_paint_layers_group(ma, to_group);
}

MaterialPaintLayer *BKE_paint_layers_edit_group_range(Material &ma,
                                                      const Span<MaterialPaintLayer *> rows)
{
  if (rows.is_empty()) {
    return nullptr;
  }
  for (const MaterialPaintLayer *layer : rows) {
    if (layer == nullptr || BKE_paint_layers_role(*layer) != PaintLayerRole::Layer) {
      return nullptr;
    }
  }
  return BKE_paint_layers_group(ma, rows);
}

MaterialPaintLayer *BKE_paint_layers_edit_group_add(Material &ma, MaterialPaintLayer *anchor)
{
  const std::string name = BKE_paint_layers_unique_default_name(ma, "Folder");
  return BKE_paint_layers_add(
      ma, MA_PAINT_LAYER_SOURCE_STACK, name.c_str(), anchor, PaintLayerPlace::Above);
}

/** \} */

}  // namespace blender
