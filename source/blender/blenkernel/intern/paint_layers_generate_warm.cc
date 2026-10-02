/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Warm slots of the Paint Layers generator: virtual, neutral items that a root row carries so that
 * adding a real mask or effect later does not change the shader code (see the declarations in
 * `paint_layers_generate_intern.hh`).
 */

#include <memory>

#include "BLI_math_vector.h"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"

#include "paint_layers_generate_intern.hh"
#include "paint_layers_intern.hh"
#include "paint_layers_runtime.hh"

/* NOTE: consuming or replenishing a slot changes the generated graph of the owning material. When
 * that material is another material's source, #BKE_paint_layers_source_material_tree_hash sees the
 * change and the consumer's bake is re-rendered once with an identical result. That is accepted: the
 * spares are ordinary nodes inside the row groups, and skipping the generated trees in that hash
 * would hide real edits of the source's own layers from its consumers. */

namespace blender {

using bke::WarmRow;

/**
 * Whether \a real's chain would actually be built. An IMAGE-source item with no map yet
 * contributes no nodes (the builder skips it), so it cannot take the slot: consuming on it anyway
 * would drop the spare's chain while nothing real replaces it, and the single user action of
 * "add the item, then pick its map" would change the shader code twice.
 */
static bool warm_item_builds_a_chain(const Material &ma,
                                     const MaterialPaintLayer &owner,
                                     const MaterialPaintLayer &real,
                                     const WarmKind kind)
{
  if (kind == WarmKind::Mask) {
    /* A mask is scalar: its one map lives in the Base Color record whatever the owner builds. */
    return paint_layer_mask_correction_image(ma, real, PAINT_MATERIAL_CHANNEL_BASE_COLOR) !=
           nullptr;
  }
  for (int i = 0; i < owner.channels_num; i++) {
    if (paint_layer_channel_image(ma, real, owner.channels[i].channel) != nullptr) {
      return true;
    }
  }
  return false;
}

/**
 * Whether \a real is the kind of item the virtual one of \a kind stands in for. The role, source and
 * blend decide the code the chain is built from, so an item that differs in any of them cannot take
 * the slot without changing the shader; and until an IMAGE-source item builds its chain, the spare
 * is what the graph still carries.
 */
static bool warm_item_is_compatible(const Material &ma,
                                    const MaterialPaintLayer &owner,
                                    const MaterialPaintLayer &real,
                                    const WarmKind kind,
                                    const PaintLayerWarmEffect effect)
{
  if (real.blend != MA_PAINT_LAYER_BLEND_MIX) {
    return false;
  }
  if (kind == WarmKind::Mask) {
    return real.source == MA_PAINT_LAYER_SOURCE_IMAGE &&
           warm_item_builds_a_chain(ma, owner, real, kind);
  }
  if (effect == PaintLayerWarmEffect::Fill) {
    /* A constant builds its mix from the description's fill color, no map is needed. */
    return real.source == MA_PAINT_LAYER_SOURCE_CONSTANT;
  }
  return real.source == MA_PAINT_LAYER_SOURCE_IMAGE &&
         warm_item_builds_a_chain(ma, owner, real, kind);
}

static int warm_real_count(const Material &ma,
                           const MaterialPaintLayer &owner,
                           const WarmKind kind,
                           const PaintLayerWarmEffect effect)
{
  int count = 0;
  const ListBaseT<MaterialPaintLayer> &list = (kind == WarmKind::Mask) ? owner.mask_stack :
                                                                         owner.effects;
  for (const MaterialPaintLayer &item : list) {
    if (warm_item_is_compatible(ma, owner, item, kind, effect)) {
      count++;
    }
  }
  return count;
}

/**
 * The marker of a virtual item, derived from its owner's so it is the same in every session: the
 * marker is part of the group's topology hash, and a random one would rebuild every root row after
 * a file load or each time the entry is recreated.
 */
static bUUID warm_marker(const bUUID &owner_marker, const uint8_t salt, const uint8_t generation)
{
  bUUID marker = owner_marker;
  marker.time_low ^= 0x57415200u | salt;
  marker.node[5] ^= salt;
  marker.time_mid ^= generation;
  return marker;
}

/** The marker of the last item in \a list that is compatible with the slot: the one just added. */
static bUUID warm_last_compatible_marker(const Material &ma,
                                         const MaterialPaintLayer &owner,
                                         const WarmKind kind,
                                         const PaintLayerWarmEffect effect)
{
  bUUID marker = {};
  const ListBaseT<MaterialPaintLayer> &list = (kind == WarmKind::Mask) ? owner.mask_stack :
                                                                         owner.effects;
  for (const MaterialPaintLayer &item : list) {
    if (warm_item_is_compatible(ma, owner, item, kind, effect)) {
      marker = item.marker;
    }
  }
  return marker;
}

static bool warm_owner_has_item(const MaterialPaintLayer &owner, const bUUID &marker)
{
  for (const MaterialPaintLayer &item : owner.mask_stack) {
    if (BLI_uuid_equal(item.marker, marker)) {
      return true;
    }
  }
  for (const MaterialPaintLayer &item : owner.effects) {
    if (BLI_uuid_equal(item.marker, marker)) {
      return true;
    }
  }
  return false;
}

/** Give \a item its fixed identity: a neutral (opacity zero) item of \a role. */
static void warm_item_init(MaterialPaintLayer &item,
                           const PaintLayerRole role,
                           const char *name,
                           const bUUID &marker)
{
  item.role = int8_t(role);
  item.blend = MA_PAINT_LAYER_BLEND_MIX;
  /* Zero opacity is what makes the item neutral for any image it points at. */
  item.opacity = 0.0f;
  item.flag = MA_PAINT_LAYER_ENABLED;
  STRNCPY(item.name, name);
  item.marker = marker;
}

/**
 * Point \a item's channel records at the warm images, one per channel in \a channels. A channel the
 * paint code writes as color data gets \a color_image, every other one \a data_image, exactly like
 * the maps a stroke creates: whether a texture is color data decides its generated code.
 */
static void warm_item_channels_set(MaterialPaintLayer &item,
                                   const Span<int> channels,
                                   Image *data_image,
                                   Image *color_image)
{
  MEM_delete(item.channels);
  item.channels = nullptr;
  item.channels_num = 0;
  if (channels.is_empty()) {
    return;
  }
  item.channels = MEM_new_array<MaterialPaintLayerChannel>(channels.size(), __func__);
  for (const int64_t i : channels.index_range()) {
    MaterialPaintLayerChannel &record = item.channels[i];
    record.channel = int8_t(channels[i]);
    record.state = MA_PAINT_LAYER_CHANNEL_ENABLED;
    const bool is_color =
        BKE_paint_material_channel_info(eMaterialPaintChannel(channels[i])).is_color;
    record.image = (is_color && color_image != nullptr) ? color_image : data_image;
  }
  item.channels_num = int(channels.size());
}

static const WarmRow *warm_row_find(const Material &ma, const bUUID &marker)
{
  const bke::MaterialPaintLayersRuntime *runtime = bke::paint_layers_runtime_get(ma);
  if (runtime == nullptr) {
    return nullptr;
  }
  for (const std::unique_ptr<WarmRow> &row : runtime->warm_rows) {
    if (BLI_uuid_equal(row->marker, marker)) {
      return row.get();
    }
  }
  return nullptr;
}

/**
 * The level of \a target for the warm slots: the number of isolating folders around it (counting
 * itself when it is one). A Pass Through folder inlines its children into the enclosing level, so a
 * row in it builds exactly like a row beside it and keeps its slots: grouping and ungrouping it
 * must not move them.
 */
static int warm_level_in_list(const Material &ma,
                              const ListBaseT<MaterialPaintLayer> &list,
                              const MaterialPaintLayer *target,
                              const int enclosing)
{
  for (const MaterialPaintLayer &layer : list) {
    const bool folder = BKE_paint_layers_is_folder(layer);
    const bool isolating = folder && !BKE_paint_layers_folder_is_pass_through(ma, layer);
    const int own = enclosing + (isolating ? 1 : 0);
    if (&layer == target) {
      return own;
    }
    int found = warm_level_in_list(ma, layer.effects, target, enclosing);
    if (found < 0) {
      found = warm_level_in_list(ma, layer.mask_stack, target, enclosing);
    }
    if (found < 0 && folder) {
      found = warm_level_in_list(ma, layer.children, target, own);
    }
    if (found >= 0) {
      return found;
    }
  }
  return -1;
}

PaintLayerWarmPlan paint_layers_warm_plan(const Material &ma,
                                          const MaterialPaintLayer &layer,
                                          const PaintLayersRegenCache * /*cache*/)
{
  PaintLayerWarmPlan plan;
  if (BKE_paint_layers_role(layer) != PaintLayerRole::Layer) {
    return plan;
  }
  /* Only what the user keeps working in: a row in the stack root, or an isolating folder in it. */
  const bool folder = BKE_paint_layers_is_folder(layer);
  if (warm_level_in_list(ma, ma.paint_layers, &layer, 0) != (folder ? 1 : 0)) {
    return plan;
  }
  switch (layer.source) {
    case MA_PAINT_LAYER_SOURCE_IMAGE:
      plan.mask = true;
      plan.effect = PaintLayerWarmEffect::Paint;
      break;
    case MA_PAINT_LAYER_SOURCE_CONSTANT:
      plan.mask = true;
      plan.effect = PaintLayerWarmEffect::Fill;
      break;
    case MA_PAINT_LAYER_SOURCE_STACK:
      /* A Pass Through folder inlines its children, so a mask chain could not apply to it. */
      plan.mask = !BKE_paint_layers_folder_is_pass_through(ma, layer);
      break;
    case MA_PAINT_LAYER_SOURCE_MATERIAL:
      /* Any Material row with a source keeps its slot, whatever mode it is in right now. The mode
       * moves on its own (a fresh row reads as Baked until its wrapper exists, a bake landing turns
       * it Baked, the sampler budget pins it, a source edit flips it back), and a slot that followed
       * it appeared a regeneration after the row did and rebuilt the root a second time, straight
       * after the user added it. A row with no source has nothing to mask. */
      plan.mask = layer.material != nullptr && layer.material->nodetree != nullptr;
      break;
    default:
      break;
  }
  return plan;
}

bool paint_layers_warm_needed(const Material &ma, const PaintLayersRegenCache *cache)
{
  Vector<const MaterialPaintLayer *> rows;
  BKE_paint_layers_flatten(ma, rows);
  for (const MaterialPaintLayer *row : rows) {
    const PaintLayerWarmPlan plan = paint_layers_warm_plan(ma, *row, cache);
    if (plan.mask || plan.effect != PaintLayerWarmEffect::None) {
      return true;
    }
  }
  return false;
}

void paint_layers_warm_reconcile(Material &ma,
                                 Image *warm_image,
                                 const PaintLayersRegenCache *cache,
                                 Image *warm_color_image)
{
  if ((ma.id.tag & (ID_TAG_LOCALIZED | ID_TAG_COPIED_ON_EVAL | ID_TAG_NO_MAIN)) != 0) {
    return;
  }
  Vector<const MaterialPaintLayer *> rows;
  BKE_paint_layers_flatten(ma, rows);

  bke::MaterialPaintLayersRuntime *runtime = bke::paint_layers_runtime_mutable(ma);
  Set<UUID> kept;
  for (const MaterialPaintLayer *row : rows) {
    PaintLayerWarmPlan plan;
    if (warm_image != nullptr) {
      plan = paint_layers_warm_plan(ma, *row, cache);
    }
    if (!plan.mask && plan.effect == PaintLayerWarmEffect::None) {
      continue;
    }
    if (runtime == nullptr) {
      runtime = &bke::paint_layers_runtime_ensure(ma);
    }
    WarmRow *entry = nullptr;
    for (const std::unique_ptr<WarmRow> &candidate : runtime->warm_rows) {
      if (BLI_uuid_equal(candidate->marker, row->marker)) {
        entry = candidate.get();
        break;
      }
    }
    const bool fresh = entry == nullptr;
    if (fresh) {
      runtime->warm_rows.append(std::make_unique<WarmRow>());
      entry = runtime->warm_rows.last().get();
      entry->marker = row->marker;
      /* The base is what the UI creates first when a mask is added: a constant that multiplies by
       * one. A stroke later turns it into a map in place. */
      /* The names differ from the real items' on purpose: a spare and a real item of the same kind
       * coexist once the spare is replenished, and a group reuses a socket by its name. The real
       * item takes over the spare's sockets through the slot marker instead (#taker). */
      warm_item_init(
          entry->base_item, PaintLayerRole::MaskItem, "Warm Base", warm_marker(row->marker, 1, 0));
      entry->base_item.source = MA_PAINT_LAYER_SOURCE_CONSTANT;
      entry->base_item.blend = MA_PAINT_LAYER_BLEND_MULTIPLY;
      entry->base_item.opacity = 1.0f;
      copy_v4_fl(entry->base_item.fill_color, 1.0f);
      warm_item_init(
          entry->mask_item, PaintLayerRole::MaskItem, "Warm Mask", warm_marker(row->marker, 2, 0));
      entry->mask_item.source = MA_PAINT_LAYER_SOURCE_IMAGE;
      warm_item_init(
          entry->effect_item, PaintLayerRole::Effect, "Warm Effect", warm_marker(row->marker, 3, 0));
    }
    kept.add(row->marker);

    /* The mask is one record whatever the owner paints; the effect reads the owner's channels. */
    const int mask_channels[1] = {PAINT_MATERIAL_CHANNEL_BASE_COLOR};
    warm_item_channels_set(entry->mask_item, Span<int>(mask_channels, 1), warm_image, nullptr);

    entry->effect_item.source = (plan.effect == PaintLayerWarmEffect::Fill) ?
                                    MA_PAINT_LAYER_SOURCE_CONSTANT :
                                    MA_PAINT_LAYER_SOURCE_IMAGE;
    Vector<int> effect_channels;
    if (plan.effect == PaintLayerWarmEffect::Paint) {
      for (const int i : IndexRange(row->channels_num)) {
        effect_channels.append(row->channels[i].channel);
      }
    }
    warm_item_channels_set(entry->effect_item, effect_channels, warm_image, warm_color_image);

    const int mask_real = warm_real_count(ma, *row, WarmKind::Mask, PaintLayerWarmEffect::None);
    const int effect_real = warm_real_count(ma, *row, WarmKind::Effect, plan.effect);
    if (fresh) {
      entry->mask_present = plan.mask;
      entry->effect_present = plan.effect != PaintLayerWarmEffect::None;
    }
    else {
      /* A compatible real item that was not there last time took the slot; one that is gone gives
       * it back in this very rebuild, which the removal causes anyway. */
      if (mask_real > entry->mask_real_seen) {
        if (entry->mask_present) {
          /* The real item builds into the spare's sockets; the spare that comes back later gets a
           * marker of its own. */
          entry->taker[1] = warm_last_compatible_marker(
              ma, *row, WarmKind::Mask, PaintLayerWarmEffect::None);
          entry->taker_slot[1] = entry->mask_item.marker;
          entry->mask_item.marker = warm_marker(row->marker, 2, ++entry->mask_gen);
        }
        entry->mask_present = false;
      }
      else if (mask_real < entry->mask_real_seen && plan.mask) {
        entry->mask_present = true;
      }
      if (effect_real > entry->effect_real_seen) {
        if (entry->effect_present) {
          entry->taker[2] = warm_last_compatible_marker(ma, *row, WarmKind::Effect, plan.effect);
          entry->taker_slot[2] = entry->effect_item.marker;
          entry->effect_item.marker = warm_marker(row->marker, 3, ++entry->effect_gen);
        }
        entry->effect_present = false;
      }
      else if (effect_real < entry->effect_real_seen &&
               plan.effect != PaintLayerWarmEffect::None)
      {
        entry->effect_present = true;
      }
      if (!plan.mask) {
        entry->mask_present = false;
      }
      if (plan.effect == PaintLayerWarmEffect::None) {
        entry->effect_present = false;
      }
    }
    entry->mask_real_seen = mask_real;
    entry->effect_real_seen = effect_real;

    /* The base is never replenished: the spare is in the graph exactly while the row has no real
     * base, and a real base that appears while it stands takes its sockets. */
    const MaterialPaintLayer *real_base = nullptr;
    for (const MaterialPaintLayer &item : row->mask_stack) {
      if ((item.flag & MA_PAINT_LAYER_MASK_BASE) != 0) {
        real_base = &item;
        break;
      }
    }
    if (!fresh && entry->base_present && real_base != nullptr) {
      entry->taker[0] = real_base->marker;
      entry->taker_slot[0] = entry->base_item.marker;
    }
    entry->base_present = plan.mask && real_base == nullptr;

    /* A taker that is gone leaves its slot: the spare (same marker for the base) stands there. */
    for (int slot = 0; slot < 3; slot++) {
      if (!BLI_uuid_is_nil(entry->taker[slot]) && !warm_owner_has_item(*row, entry->taker[slot])) {
        entry->taker[slot] = {};
        entry->taker_slot[slot] = {};
      }
    }
  }

  if (runtime != nullptr) {
    for (int i = int(runtime->warm_rows.size()) - 1; i >= 0; i--) {
      if (!kept.contains(runtime->warm_rows[i]->marker)) {
        runtime->warm_rows.remove_and_reorder(i);
      }
    }
  }
}

bUUID paint_layers_warm_slot_of(const Material &ma,
                                const bUUID &owner_marker,
                                const bUUID &item_marker)
{
  const WarmRow *row = warm_row_find(ma, owner_marker);
  if (row == nullptr) {
    return {};
  }
  for (int slot = 0; slot < 3; slot++) {
    if (!BLI_uuid_is_nil(row->taker[slot]) && BLI_uuid_equal(row->taker[slot], item_marker)) {
      return row->taker_slot[slot];
    }
  }
  if (BLI_uuid_equal(row->base_item.marker, item_marker) ||
      BLI_uuid_equal(row->mask_item.marker, item_marker) ||
      BLI_uuid_equal(row->effect_item.marker, item_marker))
  {
    return item_marker;
  }
  return {};
}

bool paint_layers_warm_defers_item(const Material &ma,
                                   const MaterialPaintLayer &owner,
                                   const MaterialPaintLayer &item,
                                   const bool mask_item)
{
  if (item.source != MA_PAINT_LAYER_SOURCE_IMAGE) {
    return false;
  }
  const WarmKind kind = mask_item ? WarmKind::Mask : WarmKind::Effect;
  if (paint_layers_warm_item(ma, owner, kind) == nullptr) {
    return false;
  }
  return !warm_item_builds_a_chain(ma, owner, item, kind);
}

bool paint_layers_warm_item_present(const Material &ma, const bUUID &marker)
{
  const bke::MaterialPaintLayersRuntime *runtime = bke::paint_layers_runtime_get(ma);
  if (runtime == nullptr) {
    return false;
  }
  for (const std::unique_ptr<WarmRow> &row : runtime->warm_rows) {
    if ((row->base_present && BLI_uuid_equal(row->base_item.marker, marker)) ||
        (row->mask_present && BLI_uuid_equal(row->mask_item.marker, marker)) ||
        (row->effect_present && BLI_uuid_equal(row->effect_item.marker, marker)))
    {
      return true;
    }
  }
  return false;
}

const MaterialPaintLayer *paint_layers_warm_item(const Material &ma,
                                                 const MaterialPaintLayer &layer,
                                                 const WarmKind kind)
{
  const WarmRow *row = warm_row_find(ma, layer.marker);
  if (row == nullptr) {
    return nullptr;
  }
  switch (kind) {
    case WarmKind::MaskBase:
      return row->base_present ? &row->base_item : nullptr;
    case WarmKind::Mask:
      return row->mask_present ? &row->mask_item : nullptr;
    case WarmKind::Effect:
      return row->effect_present ? &row->effect_item : nullptr;
  }
  return nullptr;
}

Vector<const MaterialPaintLayer *> paint_layers_build_effects(const Material &ma,
                                                              const MaterialPaintLayer &layer)
{
  Vector<const MaterialPaintLayer *> items = BKE_paint_layers_effects(layer);
  /* Last, because a real item is added at the tail of its list: it lands where the virtual one
   * stood, so the chain keeps its shape. */
  if (const MaterialPaintLayer *warm = paint_layers_warm_item(ma, layer, WarmKind::Effect)) {
    items.append(warm);
  }
  return items;
}

Vector<const MaterialPaintLayer *> paint_layers_build_mask_items(const Material &ma,
                                                                 const MaterialPaintLayer &layer)
{
  Vector<const MaterialPaintLayer *> items;
  /* The base goes first, as the real one is inserted at the head of the stack; the spare mask goes
   * last, where a real item is appended. */
  if (const MaterialPaintLayer *base = paint_layers_warm_item(ma, layer, WarmKind::MaskBase)) {
    items.append(base);
  }
  items.extend(BKE_paint_layers_mask_items(layer).as_span());
  if (const MaterialPaintLayer *warm = paint_layers_warm_item(ma, layer, WarmKind::Mask)) {
    items.append(warm);
  }
  return items;
}

bool BKE_paint_layers_warm_replenish(Material &ma)
{
  bke::MaterialPaintLayersRuntime *runtime = bke::paint_layers_runtime_mutable(ma);
  if (runtime == nullptr) {
    return false;
  }
  Vector<const MaterialPaintLayer *> rows;
  BKE_paint_layers_flatten(ma, rows);
  bool changed = false;
  for (const std::unique_ptr<WarmRow> &entry : runtime->warm_rows) {
    for (const MaterialPaintLayer *row : rows) {
      if (!BLI_uuid_equal(row->marker, entry->marker)) {
        continue;
      }
      const PaintLayerWarmPlan plan = paint_layers_warm_plan(ma, *row);
      if (plan.mask && !entry->mask_present) {
        entry->mask_present = true;
        changed = true;
      }
      if (plan.effect != PaintLayerWarmEffect::None && !entry->effect_present) {
        entry->effect_present = true;
        changed = true;
      }
      break;
    }
  }
  if (changed) {
    BKE_paint_layers_tag_edited(ma);
  }
  return changed;
}

}  // namespace blender
