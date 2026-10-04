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
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "paint_layers_intern.hh"
#include "paint_layers_runtime.hh"

#include "MEM_guardedalloc.h"

#include "BKE_global.hh"
#include "BKE_attribute.hh"
#include "BKE_idprop.hh"
#include "BKE_image.hh"
#include "BKE_image_partial_update.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_paint.hh"

#include "BLI_hash.hh"
#include "BLI_set.hh"
#include "BLI_time.h"

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
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_uuid_types.h"

namespace blender {

/** #MaterialPaintLayerBake::images is sized by the channel count; keep the two in step. */
static_assert(PAINT_MATERIAL_CHANNEL_NUM == 10);

/** #MaterialPaintLayer::channel_settings is indexed by channel; the literal size must match. */
static_assert(sizeof(MaterialPaintLayer::channel_settings) /
                  sizeof(MaterialPaintLayerChannelSettings) ==
              PAINT_MATERIAL_CHANNEL_NUM);

/** #MaterialPaintLayer::source stores the same values as #PaintLayerSourceType. */
static_assert(int8_t(MA_PAINT_LAYER_SOURCE_IMAGE) == int8_t(PaintLayerSourceType::Image));
static_assert(int8_t(MA_PAINT_LAYER_SOURCE_CONSTANT) == int8_t(PaintLayerSourceType::Constant));
static_assert(int8_t(MA_PAINT_LAYER_SOURCE_MATERIAL) == int8_t(PaintLayerSourceType::Material));
static_assert(int8_t(MA_PAINT_LAYER_SOURCE_NODE_GROUP) == int8_t(PaintLayerSourceType::NodeGroup));
static_assert(int8_t(MA_PAINT_LAYER_SOURCE_STACK) == int8_t(PaintLayerSourceType::Stack));
static_assert(int8_t(MA_PAINT_LAYER_SOURCE_MESH_MAP) == int8_t(PaintLayerSourceType::MeshMap));

/** #MaterialPaintLayer::role stores the same values as #PaintLayerRole. */
static_assert(int8_t(MA_PAINT_LAYER_ROLE_LAYER) == int8_t(PaintLayerRole::Layer));
static_assert(int8_t(MA_PAINT_LAYER_ROLE_EFFECT) == int8_t(PaintLayerRole::Effect));
static_assert(int8_t(MA_PAINT_LAYER_ROLE_MASK_ITEM) == int8_t(PaintLayerRole::MaskItem));

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
 *
 * \param include_self: whether \a target's own bake counts. A row's own visibility is not part of its
 * bake hash, so a visibility edit passes false and only a baked ancestor makes it topology.
 */
static bool paint_layer_or_ancestor_has_bake(const ListBaseT<MaterialPaintLayer> &list,
                                             const MaterialPaintLayer *target,
                                             const bool ancestor_has_bake,
                                             const bool include_self = true)
{
  for (const MaterialPaintLayer &layer : list) {
    /* A Material layer's bake is its source's maps, not a cache of the row: its values stay live
     * group inputs, so editing them is not topology. */
    const bool has_bake = ancestor_has_bake ||
                          (layer.bake != nullptr && layer.source != MA_PAINT_LAYER_SOURCE_MATERIAL);
    if (&layer == target) {
      return include_self ? has_bake : ancestor_has_bake;
    }
    if (paint_layer_or_ancestor_has_bake(layer.effects, target, has_bake, include_self) ||
        paint_layer_or_ancestor_has_bake(layer.mask_stack, target, has_bake, include_self))
    {
      return true;
    }
    if (paint_layer_or_ancestor_has_bake(layer.children, target, has_bake, include_self)) {
      return true;
    }
  }
  return false;
}

/**
 * One bit per bake-carrying ancestor of \a target (outermost first), set while that bake is valid,
 * i.e. while the generator replaces the row by its bake. \a target's own bake is not counted.
 * Comparing the value before and after an edit says whether the graph went live or back to
 * substituted: only then did its topology change.
 *
 * \return whether \a target was found in \a list.
 */
static bool paint_layer_ancestor_bake_state(const Material &ma,
                                            const ListBaseT<MaterialPaintLayer> &list,
                                            const MaterialPaintLayer *target,
                                            uint64_t &r_state)
{
  for (const MaterialPaintLayer &layer : list) {
    if (&layer == target) {
      return true;
    }
    const bool found = paint_layer_ancestor_bake_state(ma, layer.effects, target, r_state) ||
                       paint_layer_ancestor_bake_state(ma, layer.mask_stack, target, r_state) ||
                       paint_layer_ancestor_bake_state(ma, layer.children, target, r_state);
    if (found) {
      /* Same rule as #row_is_substituted: a Material row's bake is not a cache of the row. */
      if (layer.bake != nullptr && layer.source != MA_PAINT_LAYER_SOURCE_MATERIAL) {
        r_state = (r_state << 1) | (BKE_paint_layers_bake_is_valid(ma, layer) ? 1 : 0);
      }
      return true;
    }
  }
  return false;
}

void paint_layers_tag_value_edited(Material &ma,
                                   const MaterialPaintLayer *edited,
                                   const bool value_in_own_bake = true,
                                   const bool regen_for_baked_ancestor = true)
{
  /* A value can be baked into a row's cache (the hash covers opacity/fill), and the value-only path
   * does not rebuild the tree, so it has to ask the bake planner for a re-bake as well. The planner
   * at K-1 re-bakes exactly the rows whose stored hash no longer matches. */
  ma.paint_layers_flag |= MA_PAINT_LAYERS_BAKE_STALE;
  /* The generated tree substituted only the baked rows, so only a value change to such a row (or
   * one of its ancestors) is topology. Anything else stays the free path an animation needs. */
  if (regen_for_baked_ancestor && edited != nullptr &&
      paint_layer_or_ancestor_has_bake(ma.paint_layers, edited, false, value_in_own_bake))
  {
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
MaterialPaintLayer *paint_layer_find_in_list(const ListBaseT<MaterialPaintLayer> &list,
                                             const bUUID &marker)
{
  for (const MaterialPaintLayer &layer : list) {
    if (BLI_uuid_equal(layer.marker, marker)) {
      return const_cast<MaterialPaintLayer *>(&layer);
    }
    if (MaterialPaintLayer *found = paint_layer_find_in_list(layer.children, marker)) {
      return found;
    }
    if (MaterialPaintLayer *found = paint_layer_find_in_list(layer.effects, marker)) {
      return found;
    }
    if (MaterialPaintLayer *found = paint_layer_find_in_list(layer.mask_stack, marker)) {
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
  } while (paint_layer_find_in_list(const_cast<Material &>(ma).paint_layers, marker) != nullptr);
  return marker;
}

/**
 * A detached row with the usual defaults and a marker unique within \a ma. It is not linked yet:
 * #BKE_paint_layers_add links it into the stack, #BKE_paint_layers_correction_add into a parent's
 * correction list, and neither shares this through the other.
 */
MaterialPaintLayer *paint_layer_alloc(Material &ma,
                                      eMaterialPaintLayerSource source,
                                      const char *name)
{
  MaterialPaintLayer *layer = MEM_new<MaterialPaintLayer>(__func__);
  layer->source = int8_t(source);
  /* A fresh row is always a stack Layer; #BKE_paint_layers_correction_add sets the role once it
   * links the row into an owner's effects/mask_stack list instead. */
  layer->role = MA_PAINT_LAYER_ROLE_LAYER;
  layer->blend = MA_PAINT_LAYER_BLEND_MIX;
  layer->flag = MA_PAINT_LAYER_ENABLED;
  layer->opacity = 1.0f;
  /* A fresh row carries no mapping: the DNA default already gives scale one, set it explicitly
   * so the intent survives even if the default ever changes. */
  layer->mapping.offset[0] = 0.0f;
  layer->mapping.offset[1] = 0.0f;
  layer->mapping.scale[0] = 1.0f;
  layer->mapping.scale[1] = 1.0f;
  layer->mapping.rotation = 0.0f;
  layer->mapping.space = MA_PAINT_LAYER_MAPPING_SPACE_UV;
  /* Uniform scale is the common case, so a new row starts with the axes locked together. */
  layer->mapping.flag = MA_PAINT_LAYER_MAPPING_SCALE_LOCK;
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

  for (const MaterialPaintLayer &child : src.children) {
    BLI_addtail(&dst->children, paint_layer_branch_duplicate(bmain, ma, child, copy_images));
  }
  for (const MaterialPaintLayer &effect : src.effects) {
    BLI_addtail(&dst->effects, paint_layer_branch_duplicate(bmain, ma, effect, copy_images));
  }
  for (const MaterialPaintLayer &mask_item : src.mask_stack) {
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
  for (const MaterialPaintLayer &child : layer.children) {
    if (BKE_paint_layers_subtree_contains(child, marker)) {
      return true;
    }
  }
  for (const MaterialPaintLayer &effect : layer.effects) {
    if (BKE_paint_layers_subtree_contains(effect, marker)) {
      return true;
    }
  }
  for (const MaterialPaintLayer &mask_item : layer.mask_stack) {
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

const char *BKE_paint_layers_uv_map_name(const Material &ma)
{
  return ma.paint_layers_uv_map;
}

const char *BKE_paint_layers_uv_map_resolve(const Mesh &mesh,
                                            const Material &ma,
                                            bool *r_missing)
{
  const char *name = ma.paint_layers_uv_map;
  if (name[0] == '\0') {
    /* No name set: the behavior before names existed. The active UV map's name lives on the mesh,
     * so the returned pointer stays valid as long as the mesh does. */
    return mesh.active_uv_map_name().c_str();
  }
  if (bke::mesh::is_uv_map(mesh.attributes().lookup_meta_data(name))) {
    return name;
  }
  if (r_missing != nullptr) {
    *r_missing = true;
  }
  return nullptr;
}

void BKE_paint_layers_uv_map_autofill(Material &ma, const Object *ob)
{
  if (ma.paint_layers_uv_map[0] != '\0' || ob == nullptr || ob->type != OB_MESH ||
      ob->data == nullptr)
  {
    return;
  }
  /* Only an object that actually uses the material may name its UV layer. The object in context can
   * be unrelated to the material being edited (a different object's slot, an override, an addon
   * passing the wrong pointer); taking its mesh's active UV would then silently fix the stack to a
   * layer no object using the material is unwrapped with. */
  if (BKE_object_material_index_get(const_cast<Object *>(ob), &ma) < 0) {
    return;
  }
  const Mesh &mesh = *id_cast<const Mesh *>(ob->data);
  const StringRefNull active = mesh.active_uv_map_name();
  if (active.is_empty()) {
    return;
  }
  BLI_strncpy(ma.paint_layers_uv_map, active.c_str(), sizeof(ma.paint_layers_uv_map));
  /* A chosen name is topology: the generated graph gains a UV Map node, so the material has to be
   * marked stale for the next regeneration to pick it up. */
  BKE_paint_layers_tag_edited(ma);
}

const PaintLayerKindInfo &BKE_paint_layers_kind_info(const int source)
{
  static const PaintLayerKindInfo table[] = {
      {MA_PAINT_LAYER_SOURCE_IMAGE, "IMAGE", "Image", false, false, false},
      {MA_PAINT_LAYER_SOURCE_CONSTANT, "CONSTANT", "Constant", false, true, false},
      {MA_PAINT_LAYER_SOURCE_MATERIAL, "MATERIAL", "Material", false, false, true},
      {MA_PAINT_LAYER_SOURCE_NODE_GROUP, "NODE_GROUP", "Node Group", false, false, true},
      {MA_PAINT_LAYER_SOURCE_STACK, "STACK", "Folder", true, false, false},
      /* A MESH_MAP row names a geometry map of the object, not a painted channel: it has no
       * generated subtree of its own and no fill colour, and the CPU cannot evaluate it either. */
      {MA_PAINT_LAYER_SOURCE_MESH_MAP, "MESH_MAP", "Mesh Map", false, false, false},
  };
  for (const PaintLayerKindInfo &info : table) {
    if (info.source == source) {
      return info;
    }
  }
  /* An unknown source reads as Image, the same compatibility rule the DNA enum documents. */
  return table[0];
}

bool BKE_paint_layers_is_folder(const MaterialPaintLayer &layer)
{
  return BKE_paint_layers_kind_info(layer.source).is_folder;
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
  return PaintLayerSourceType(layer.source);
}

PaintLayerRole BKE_paint_layers_role(const MaterialPaintLayer &layer)
{
  return PaintLayerRole(layer.role);
}

namespace {

void paint_layer_list_collect(const ListBaseT<MaterialPaintLayer> &list,
                              Vector<const MaterialPaintLayer *> &r_items)
{
  for (const MaterialPaintLayer &item : list) {
    r_items.append(&item);
  }
}

void paint_layer_list_collect(ListBaseT<MaterialPaintLayer> &list,
                              Vector<MaterialPaintLayer *> &r_items)
{
  for (MaterialPaintLayer &item : list) {
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

MaterialPaintLayer *BKE_paint_layers_mask_base(MaterialPaintLayer &layer)
{
  for (MaterialPaintLayer &item : layer.mask_stack) {
    if (item.flag & MA_PAINT_LAYER_MASK_BASE) {
      return &item;
    }
  }
  return nullptr;
}

const MaterialPaintLayer *BKE_paint_layers_mask_base(const MaterialPaintLayer &layer)
{
  for (const MaterialPaintLayer &item : layer.mask_stack) {
    if (item.flag & MA_PAINT_LAYER_MASK_BASE) {
      return &item;
    }
  }
  return nullptr;
}

bool BKE_paint_layers_fill_to_paint(Material &ma, MaterialPaintLayer &layer, float r_fill[4])
{
  BKE_paint_layers_base_color_get(layer, r_fill);
  /* Only a Layer-role Constant row reads as Fill; a Fill-effect correction is untouched by this
   * conversion, matching the old kind table (which had no entry for a correction). */
  if (BKE_paint_layers_role(layer) != PaintLayerRole::Layer ||
      !BKE_paint_layers_kind_info(layer.source).uses_fill_color)
  {
    return false;
  }
  /* A Layer-role Fill carries a value per channel in its records (Base Color included), so each
   * record already keeps what the Fill showed; the flip only changes the source. */
  layer.source = MA_PAINT_LAYER_SOURCE_IMAGE;
  BKE_paint_layers_tag_edited(ma);
  return true;
}

namespace {

void paint_layers_flatten_list(const ListBaseT<MaterialPaintLayer> &list,
                               Vector<const MaterialPaintLayer *> &r_layers)
{
  for (const MaterialPaintLayer &layer : list) {
    /* A correction row is reached through its owner's effects/mask_stack lists, not here. A Custom and a
     * Material layer alike take part through their bake, so they are walked like any other row. */
    if (BKE_paint_layers_role(layer) != PaintLayerRole::Layer) {
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

namespace {

void paint_layers_flatten_all_list(const ListBaseT<MaterialPaintLayer> &list,
                                   Vector<const MaterialPaintLayer *> &r_rows)
{
  for (const MaterialPaintLayer &layer : list) {
    r_rows.append(&layer);
    paint_layers_flatten_all_list(layer.effects, r_rows);
    paint_layers_flatten_all_list(layer.mask_stack, r_rows);
    if (BKE_paint_layers_is_folder(layer)) {
      paint_layers_flatten_all_list(layer.children, r_rows);
    }
  }
}

}  // namespace

void BKE_paint_layers_flatten_all(const Material &ma, Vector<const MaterialPaintLayer *> &r_rows)
{
  paint_layers_flatten_all_list(ma.paint_layers, r_rows);
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

bool BKE_paint_layers_normal_replace(const MaterialPaintLayer &layer)
{
  return layer.channel_settings[PAINT_MATERIAL_CHANNEL_NORMAL].blend ==
         MA_PAINT_LAYER_BLEND_NORMAL_REPLACE;
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
  const MaterialPaintLayerChannel *record = paint_layer_channel_find(correction, channel);
  if (correction.source == MA_PAINT_LAYER_SOURCE_CONSTANT) {
    /* A Fill's live record overrides its constant for that one channel; a channel with no live
     * record (absent, or DISABLED) keeps the row's fill_color, and a row with no records at all is
     * the legacy constant everywhere. New records start at fill_color (see
     * #BKE_paint_layers_channel_add), so switching a channel on does not change the view. */
    if (record != nullptr && record->state == MA_PAINT_LAYER_CHANNEL_ENABLED) {
      copy_v4_v4(r_color, record->value);
      return;
    }
    copy_v4_v4(r_color, correction.fill_color);
    return;
  }
  if (record != nullptr) {
    copy_v4_v4(r_color, record->value);
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
    case MA_PAINT_LAYER_BLEND_NORMAL_REPLACE:
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
  if (layer.source != MA_PAINT_LAYER_SOURCE_NODE_GROUP) {
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
      ma, MA_PAINT_LAYER_SOURCE_NODE_GROUP, layer_name, anchor, place);
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
    if (layer->source != MA_PAINT_LAYER_SOURCE_NODE_GROUP || layer->custom_group == nullptr) {
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
  if (layer.source != MA_PAINT_LAYER_SOURCE_NODE_GROUP || layer.custom_group == nullptr) {
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

void BKE_paint_layers_baked_channels_get(const Material &ma,
                                         const MaterialPaintLayer &layer,
                                         Vector<int> &r_channels)
{
  r_channels.clear();
  /* The `hash` is the whole validity test (see #MaterialPaintLayerBake): a row without a bake, or
   * with a stale one, has no current maps to report. */
  if (layer.bake == nullptr || !BKE_paint_layers_bake_is_valid(ma, layer)) {
    return;
  }
  for (const int channel : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    if (layer.bake->images[channel] != nullptr) {
      r_channels.append(channel);
    }
  }
  /* The coverage map is the Alpha channel's data (#paint_layer_material_source_map). */
  if (layer.bake->coverage != nullptr && !r_channels.contains(int(PAINT_MATERIAL_CHANNEL_ALPHA))) {
    r_channels.append(int(PAINT_MATERIAL_CHANNEL_ALPHA));
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
    if (layer->source == MA_PAINT_LAYER_SOURCE_NODE_GROUP && layer->custom_group != nullptr) {
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
    if (!paint_layer_channel_present(ma, *layer, PAINT_MATERIAL_CHANNEL_NORMAL)) {
      continue;
    }
    for (const MaterialPaintLayer &effect : layer->effects) {
      if (effect.source != MA_PAINT_LAYER_SOURCE_CONSTANT ||
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
static MaterialPaintLayer *paint_layer_correction_owner(ListBaseT<MaterialPaintLayer> &list,
                                                        const MaterialPaintLayer &correction)
{
  for (MaterialPaintLayer &layer : list) {
    for (const MaterialPaintLayer &candidate : layer.effects) {
      if (&candidate == &correction) {
        return &layer;
      }
    }
    for (const MaterialPaintLayer &candidate : layer.mask_stack) {
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
                                         eMaterialPaintLayerSource source,
                                         const char *name,
                                         MaterialPaintLayer *anchor,
                                         PaintLayerPlace place)
{
  /* The material's channel set is derived while the field is zero; freeze it before the new row is
   * linked, so the per-channel filter reads the stored mask instead of recomputing the union. A
   * no-op once anything has materialized it. */
  BKE_paint_layers_channels_materialize(ma);

  /* Beside a correction would mean into its owner's corrections list, where a layer is neither
   * listed as a row nor composited as one (it would silently ride on the owner's visibility).
   * This function only ever creates a Layer-role row (a correction is linked through
   * #BKE_paint_layers_correction_add instead), so the owner row always stands in here --
   * except when \a place is Into and \a anchor is itself a Stack correction or mask item: a
   * Stack correction is a folder in every sense #BKE_paint_layers_is_folder cares about (its
   * source, not its role, decides that), and Into a folder means inside that folder's own
   * #children, the correction's included. Redirecting it to the owner row first would have
   * "Into <the Stack correction>" place the new row inside the *owner's* children instead --
   * the wrong list, and one the owner row usually is not even a folder for. Above/Below still
   * redirect a correction anchor to its owner row unconditionally (unchanged): those mean
   * "beside the owner's own row", never "beside the correction". */
  const bool into_stack_correction = place == PaintLayerPlace::Into && anchor != nullptr &&
                                     BKE_paint_layers_role(*anchor) != PaintLayerRole::Layer &&
                                     BKE_paint_layers_is_folder(*anchor);
  if (anchor != nullptr && BKE_paint_layers_role(*anchor) != PaintLayerRole::Layer &&
      !into_stack_correction)
  {
    anchor = paint_layer_correction_owner(ma.paint_layers, *anchor);
    if (anchor == nullptr) {
      return nullptr;
    }
  }

  MaterialPaintLayer *layer = paint_layer_alloc(ma, source, name);

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
      /* Only a folder holds other rows. BKE never changes a source implicitly: promoting an Image
       * or Constant layer to a folder would make its maps ignored (FolderHasMaps) and the painted
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

/**
 * Let go of every data-block \a layer and what hangs off it (folder children, effects, mask items)
 * holds a user on. The row's free routine never does, so without this a removed mask's map keeps a
 * user and is written into the file as if it were still in use. The data-blocks themselves stay in
 * `Main`: an undo step restores the row, and a map's unsaved pixels live only in memory, so freeing
 * it here would bring the row back with the painting gone. A map left with no user is an orphan,
 * dropped on save or purge like any other.
 */
static void paint_layer_release_users(MaterialPaintLayer &layer)
{
  for (MaterialPaintLayer &child : layer.children) {
    paint_layer_release_users(child);
  }
  for (MaterialPaintLayer &effect : layer.effects) {
    paint_layer_release_users(effect);
  }
  for (MaterialPaintLayer &item : layer.mask_stack) {
    paint_layer_release_users(item);
  }
  for (int i = 0; i < layer.channels_num; i++) {
    if (layer.channels[i].image != nullptr) {
      id_us_min(&layer.channels[i].image->id);
    }
  }
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
  if (layer.material != nullptr) {
    id_us_min(&layer.material->id);
  }
  if (layer.custom_group != nullptr) {
    id_us_min(&layer.custom_group->id);
  }
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
  /* The base flag goes away with its item; the row's remaining first mask item takes over, so the
   * row keeps the one base a stroke lands on (#BKE_paint_layers_mask_add). */
  const bool was_mask_base = (layer->flag & MA_PAINT_LAYER_MASK_BASE) != 0;
  MaterialPaintLayer *mask_owner = was_mask_base ?
                                       paint_layer_correction_owner(ma.paint_layers, *layer) :
                                       nullptr;

  paint_layer_release_users(*layer);
  BLI_remlink(owner, layer);
  BKE_material_paint_layer_free(layer);

  if (mask_owner != nullptr && mask_owner->mask_stack.first != nullptr) {
    static_cast<MaterialPaintLayer *>(mask_owner->mask_stack.first)->flag |=
        MA_PAINT_LAYER_MASK_BASE;
  }
  if (clears_active) {
    ma.active_layer_marker = {};
  }
  BKE_paint_layers_tag_edited(ma);
  return true;
}

MaterialPaintLayer *BKE_paint_layers_find(Material &ma, const bUUID &marker)
{
  return paint_layer_find_in_list(ma.paint_layers, marker);
}

void BKE_paint_layers_active_set(Material &ma, const bUUID &marker)
{
  // TODO(debug): remove
  {
    char dbg_old[UUID_STRING_SIZE];
    char dbg_new[UUID_STRING_SIZE];
    BLI_uuid_format(dbg_old, ma.active_layer_marker);
    BLI_uuid_format(dbg_new, marker);
    printf("[STACK_DBG] active_set old=%.8s new=%.8s\n", dbg_old, dbg_new);
  }
  /* Leaving a row is when its deferred bake becomes due. The editor's Material-row planner reads
   * this after the depsgraph update; the CPU planner's #MA_PAINT_LAYERS_BAKE_STALE cannot carry the
   * signal because it is cleared at the K-1 point, before that update runs. */
  if (!BLI_uuid_equal(ma.active_layer_marker, marker)) {
    ma.paint_layers_flag |= MA_PAINT_LAYERS_MATERIAL_BAKE_DUE;
    /* The row and its ancestor folders left behind may carry a cache that has to catch up, and the
     * chain that bakes Material rows by hand (#material_bake_layered_rows_ensure) only knows
     * Material rows. This signal makes the K-1 planner (#BKE_paint_layers_bake_plan_run, reached
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
  /* Mask items are ordered by the mask semantics -- the base is always first and corrections sit
   * above it -- so they are never moved or used as move anchors. */
  if (BKE_paint_layers_role(*layer) == PaintLayerRole::MaskItem ||
      (anchor != nullptr && BKE_paint_layers_role(*anchor) == PaintLayerRole::MaskItem))
  {
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
    /* Only a folder holds other rows, and BKE never promotes a source implicitly; refuse before
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
  /* The base mask is always the first item of its stack, so it never moves and nothing moves in
   * front of it; the corrections above it may be rearranged among themselves. */
  const bool is_mask_item = BKE_paint_layers_role(*layer) == PaintLayerRole::MaskItem;
  if (is_mask_item && (layer->flag & MA_PAINT_LAYER_MASK_BASE) != 0) {
    return false;
  }
  ListBase *owner = paint_layer_owner_list(&ma.paint_layers, layer);
  if (owner == nullptr) {
    return false;
  }
  const int count = BLI_listbase_count(owner);
  const MaterialPaintLayer *head = static_cast<const MaterialPaintLayer *>(owner->first);
  const int min_index = (is_mask_item && head != nullptr &&
                         (head->flag & MA_PAINT_LAYER_MASK_BASE) != 0) ?
                            1 :
                            0;
  index = clamp_i(index, min_index, count - 1);

  BLI_remlink(owner, layer);
  /* The stack is bottom-to-top, so index 0 is the first link of the list. */
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
      ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
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
  ListBaseT<MaterialPaintLayer> lifted = folder->children;
  folder->children = {nullptr, nullptr};
  BKE_material_paint_layer_free(folder);

  int insert_index = folder_index;
  for (MaterialPaintLayer &child : lifted.items_mutable()) {
    BLI_remlink(&lifted, &child);
    if (insert_index >= BLI_listbase_count(owner)) {
      BLI_addtail(owner, &child);
    }
    else {
      BLI_insertlinkbefore(owner, BLI_findlink(owner, insert_index), &child);
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
      ma, layer, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_CONSTANT, "Mask");
  if (item == nullptr) {
    return nullptr;
  }
  item->fill_color[0] = item->fill_color[1] = item->fill_color[2] = value;
  item->fill_color[3] = 1.0f;
  item->blend = MA_PAINT_LAYER_BLEND_MULTIPLY;
  item->opacity = 1.0f;
  /* A layer has exactly one base mask, and it is always the first item. */
  for (MaterialPaintLayer &existing : layer->mask_stack) {
    existing.flag &= ~MA_PAINT_LAYER_MASK_BASE;
  }
  item->flag |= MA_PAINT_LAYER_MASK_BASE;
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
  /* A Layer-role Constant row and a Fill-effect correction both carry their per-channel constant
   * on a group input, so a value edit is value-only and must not rebuild the tree. A mask item's
   * record is not read this way (its strength lives in fill_color), and a Paint row's value alpha
   * decides whether it lays a constant at all, which is topology. */
  const bool fill_value_input =
      BKE_paint_layers_kind_info(layer->source).uses_fill_color &&
      ELEM(BKE_paint_layers_role(*layer), PaintLayerRole::Layer, PaintLayerRole::Effect);
  if (fill_value_input) {
    paint_layers_tag_value_only(ma);
    BKE_paint_layers_values_sync(ma);
  }
  else {
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
   * children's (design §5). Folder-ness is the source, so an emptied folder is refused all the
   * same. */
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
  if (BKE_paint_layers_role(*layer) == PaintLayerRole::Layer &&
      BKE_paint_layers_kind_info(layer->source).uses_fill_color)
  {
    /* A Fill shows a value per channel from the start; the Principled defaults keep the material
     * looking like it did before the layer. Base Color is the exception: a fresh Fill lays nothing
     * until its colour is set (the Principled 0.8 default would silently make every new Fill a
     * grey layer), so its record starts transparent. #BKE_paint_layers_set_fill_color gives it a
     * colour. A Fill-effect correction's own first record instead falls to the transparent branch
     * below, exactly as it did through the old kind table (which had no entry for a correction). */
    if (channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
      zero_v4(record.value);
    }
    else {
      BKE_paint_layers_channel_default_value(ma, channel, record.value);
    }
  }
  else if (BKE_paint_layers_kind_info(layer->source).uses_fill_color) {
    /* A Fill-effect correction's channel record starts at the row's constant, so enabling a channel
     * through the widget does not change the view until its own value is edited (the constant then
     * overrides fill_color for that channel). See #BKE_paint_layers_correction_constant. */
    copy_v4_v4(record.value, layer->fill_color);
  }
  else {
    /* A fresh Paint record is transparent: it takes part but lays nothing down until painted. */
    zero_v4(record.value);
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

void BKE_paint_layers_default_channels_apply(Material &ma, MaterialPaintLayer &layer)
{
  /* A Paint correction (content effect) takes every channel of the set too, so the user can paint
   * into any of them at once; other corrections keep their channels opt-in. */
  const bool paint_effect = BKE_paint_layers_role(layer) == PaintLayerRole::Effect &&
                            layer.source == MA_PAINT_LAYER_SOURCE_IMAGE;
  if (BKE_paint_layers_role(layer) != PaintLayerRole::Layer && !paint_effect) {
    return;
  }
  /* A MESH_MAP row reads the material's shared atlas; a layer has a single explicit participation
   * in it, so it defaults to Base Color alone. Its records name which channels the row paints, and
   * channels are added and removed like a Paint row's, so the user opts into more. */
  if (layer.source == MA_PAINT_LAYER_SOURCE_MESH_MAP) {
    if (paint_layer_channel_find(layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR) == nullptr) {
      BKE_paint_layers_channel_add(ma, &layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    }
    return;
  }
  if (!ELEM(layer.source, MA_PAINT_LAYER_SOURCE_IMAGE, MA_PAINT_LAYER_SOURCE_CONSTANT)) {
    return;
  }
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

uint16_t BKE_paint_layers_channel_set_mask_get(const Material &ma)
{
  uint16_t mask = ma.paint_layers_channels;
  if (mask == 0) {
    /* Not authored: the union of the build default and every record any row already carries, so a
     * stack's own channels are never dropped from the set. */
    mask = PAINT_LAYERS_DEFAULT_CHANNEL_SET;
    Vector<const MaterialPaintLayer *> rows;
    BKE_paint_layers_flatten_all(ma, rows);
    for (const MaterialPaintLayer *row : rows) {
      for (const int i : IndexRange(row->channels_num)) {
        mask |= uint16_t(uint16_t(1) << int(row->channels[i].channel));
      }
      /* A Material row carries no channel records: it reads its content from the bake, so a map it
       * actually baked is what makes that channel part of the material. Without this the set would
       * drop the baked Specular/Emission (and the Alpha coverage) of an old file written before the
       * field existed, which has no doversion. */
      if (row->source == MA_PAINT_LAYER_SOURCE_MATERIAL && row->bake != nullptr) {
        for (const int channel : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
          if (row->bake->images[channel] != nullptr) {
            mask |= uint16_t(uint16_t(1) << channel);
          }
        }
        if (row->bake->coverage != nullptr) {
          mask |= uint16_t(1) << PAINT_MATERIAL_CHANNEL_ALPHA;
        }
      }
    }
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

bool paint_layer_effects_lay_content(const Material &ma,
                                     const MaterialPaintLayer &layer,
                                     const int channel)
{
  if (BKE_paint_layers_role(layer) != PaintLayerRole::Layer ||
      layer.source != MA_PAINT_LAYER_SOURCE_IMAGE)
  {
    return false;
  }
  for (const MaterialPaintLayer *effect : BKE_paint_layers_effects(layer)) {
    if ((effect->flag & MA_PAINT_LAYER_ENABLED) == 0) {
      continue;
    }
    if (effect->source == MA_PAINT_LAYER_SOURCE_IMAGE &&
        paint_layer_channel_image(ma, *effect, channel) != nullptr)
    {
      return true;
    }
    /* A constant normal makes no sense, so a Fill lays nothing in the Normal channel. */
    if (effect->source == MA_PAINT_LAYER_SOURCE_CONSTANT &&
        channel != PAINT_MATERIAL_CHANNEL_NORMAL)
    {
      return true;
    }
  }
  return false;
}

void paint_layer_channel_constant(const MaterialPaintLayer &layer,
                                  const int channel,
                                  float r_color[4])
{
  /* INVARIANT: a row's channel constant is read only through this function. A Layer-role row keeps
   * every channel -- Base Color included -- in its #MaterialPaintLayerChannel records; the DNA
   * #MaterialPaintLayer::fill_color field is the constant of rows that carry no channel records
   * (masks and Fill-effect corrections) and is never read here. A correction reads through
   * #BKE_paint_layers_correction_constant instead, which keeps its own fill-colour rule. */
  const MaterialPaintLayerChannel *entry = paint_layer_channel_find(layer, channel);
  if (entry != nullptr) {
    copy_v4_v4(r_color, entry->value);
    return;
  }
  zero_v4(r_color);
}

void BKE_paint_layers_base_color_get(const MaterialPaintLayer &layer, float r_color[4])
{
  /* A Layer-role row shows Base Color from its channel record, like the generator and the CPU; a
   * mask or Fill-effect correction has no Base-Color record and keeps the DNA field. */
  if (BKE_paint_layers_role(layer) == PaintLayerRole::Layer) {
    paint_layer_channel_constant(layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, r_color);
    return;
  }
  copy_v4_v4(r_color, layer.fill_color);
}

namespace {

/** Whether \a target is reachable from \a from through MATERIAL layers' source materials. */
bool paint_layers_material_depends_on(const Material &from, const Material &target)
{
  if (&from == &target) {
    return true;
  }
  std::function<bool(const ListBaseT<MaterialPaintLayer> &)> walk =
      [&](const ListBaseT<MaterialPaintLayer> &list) {
        for (const MaterialPaintLayer &layer : list) {
          if (layer.source == MA_PAINT_LAYER_SOURCE_MATERIAL && layer.material != nullptr &&
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
  if (layer->source != MA_PAINT_LAYER_SOURCE_MATERIAL) {
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
  /* INVARIANT: a Layer-role Fill always carries its Base-Color record. Base Color is the row's
   * constant, read through #paint_layer_channel_constant with no DNA `fill_color` fallback for a
   * Layer-role row, so removing the record would silently make the Fill paint nothing. Other
   * channels -- and a mask/Fill-effect correction's records -- are removed as before. */
  if (BKE_paint_layers_role(*layer) == PaintLayerRole::Layer &&
      BKE_paint_layers_kind_info(layer->source).uses_fill_color &&
      channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR)
  {
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
  /* INVARIANT: a Layer-role Fill's Base Color cannot be switched off, the same way its record
   * cannot be removed (#BKE_paint_layers_channel_remove). Base Color is the Fill's constant, so a
   * disabled record would make the layer paint nothing while still claiming to take part. */
  if (!enabled && BKE_paint_layers_role(*layer) == PaintLayerRole::Layer &&
      BKE_paint_layers_kind_info(layer->source).uses_fill_color &&
      channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR)
  {
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
  /* The Normal channel forces its own combine in the generator and the CPU and ignores the row's
   * blend, so the only override that can take effect there is Replace (or -1 back to Combine). */
  if (channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
    if (!ELEM(blend, -1, MA_PAINT_LAYER_BLEND_NORMAL_REPLACE)) {
      return false;
    }
    layer.channel_settings[channel].blend = int8_t(blend);
    BKE_paint_layers_tag_edited(ma);
    return true;
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

bool BKE_paint_layers_source_change(Material &ma,
                                    MaterialPaintLayer *layer,
                                    int8_t source)
{
  if (layer == nullptr || paint_layer_owner_list(&ma.paint_layers, layer) == nullptr) {
    return false;
  }
  /* A source change between Image and Constant is a Layer-row operation only: a correction's
   * source is changed through #BKE_paint_layers_correction_source_set instead, which carries the
   * same restriction under a name that matches what it is changing. */
  if (BKE_paint_layers_role(*layer) != PaintLayerRole::Layer) {
    return false;
  }
  /* Only a painted/filled row converts: a Material, Node Group, Folder or Mesh Map row is a
   * different kind of source, recreated rather than converted. */
  if (!ELEM(layer->source, MA_PAINT_LAYER_SOURCE_IMAGE, MA_PAINT_LAYER_SOURCE_CONSTANT)) {
    return false;
  }
  if (!ELEM(source, MA_PAINT_LAYER_SOURCE_IMAGE, MA_PAINT_LAYER_SOURCE_CONSTANT)) {
    return false;
  }
  if (layer->source == source) {
    return true;
  }
  if (source == MA_PAINT_LAYER_SOURCE_CONSTANT) {
    /* A painted map is not a constant, so the maps go; the records stay, because a per-channel
     * blend/opacity override is a setting of the pair, not a pixel. Only the image is forgotten;
     * an image is owned by Main, so it is detached, not freed. Base Color already lives in its
     * record, so it survives the flip like every other channel. A transparent Base Color becomes an
     * opaque black Fill default, so the row shows a clean constant rather than laying nothing. */
    for (const int i : IndexRange(layer->channels_num)) {
      layer->channels[i].image = nullptr;
    }
    MaterialPaintLayerChannel *base = paint_layer_channel_find(
        *layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    if (base != nullptr && base->value[3] == 0.0f) {
      base->value[0] = base->value[1] = base->value[2] = 0.0f;
      base->value[3] = 1.0f;
    }
  }
  else {
    /* Fill's color becomes the starting point of the first stroke's map, not a map of its own. The
     * Base-Color record is reset, not the DNA field: a Layer-role row keeps Base Color there. */
    MaterialPaintLayerChannel *base = paint_layer_channel_find(
        *layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    if (base != nullptr) {
      static const float fill_default[4] = {0.0f, 0.0f, 0.0f, 1.0f};
      copy_v4_v4(base->value, fill_default);
    }
  }
  layer->source = source;
  BKE_paint_layers_tag_edited(ma);
  return true;
}

MaterialPaintLayer *BKE_paint_layers_correction_add(Material &ma,
                                                    MaterialPaintLayer *owner,
                                                    int role,
                                                    int source,
                                                    const char *name,
                                                    MaterialPaintLayer *after)
{
  if (owner == nullptr || paint_layer_owner_list(&ma.paint_layers, owner) == nullptr) {
    return nullptr;
  }
  if (!ELEM(role, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_ROLE_MASK_ITEM)) {
    return nullptr;
  }
  /* An Effect or a Mask Item also reads a Material, a Node Group, or a Stack, exactly like a Layer
   * row of the same kind: it behaves as that row's own channel content (an Effect) or as the one
   * number its `mask_channel` picks out (a Mask Item), baked and tracked the same way. A Stack
   * correction composites its own children in isolation, like a Layer folder (phase 4). */
  const bool source_ok = ELEM(source,
                              MA_PAINT_LAYER_SOURCE_IMAGE,
                              MA_PAINT_LAYER_SOURCE_CONSTANT,
                              MA_PAINT_LAYER_SOURCE_MESH_MAP) ||
                         ELEM(source,
                              MA_PAINT_LAYER_SOURCE_MATERIAL,
                              MA_PAINT_LAYER_SOURCE_NODE_GROUP,
                              MA_PAINT_LAYER_SOURCE_STACK);
  if (!source_ok) {
    return nullptr;
  }
  /* A correction is its own list on the owner, not a folder child: #MaterialPaintLayer::children
   * holds nesting, #MaterialPaintLayer::effects the adjustments to the row and
   * #MaterialPaintLayer::mask_stack its mask items. It is linked directly, never through add():
   * adding Into would promote the owner to a folder. */
  MaterialPaintLayer *correction = paint_layer_alloc(
      ma, eMaterialPaintLayerSource(source), name != nullptr ? name : "Correction");
  correction->role = int8_t(role);
  ListBase *destination = (role == MA_PAINT_LAYER_ROLE_MASK_ITEM) ? &owner->mask_stack :
                                                                    &owner->effects;
  if (after != nullptr && BLI_findindex(destination, after) != -1) {
    /* Lands right behind the correction the user added from, rather than behind the whole list. */
    BLI_insertlinkafter(destination, after, correction);
  }
  else {
    BLI_addtail(destination, correction);
  }
  paint_layer_mark_owned(ma);
  return correction;
}

bool BKE_paint_layers_role_set(Material &ma, MaterialPaintLayer *correction, int role)
{
  if (correction == nullptr || BKE_paint_layers_role(*correction) == PaintLayerRole::Layer) {
    return false;
  }
  if (!ELEM(role, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_ROLE_MASK_ITEM)) {
    return false;
  }
  MaterialPaintLayer *owner = paint_layer_correction_owner(ma.paint_layers, *correction);
  if (owner == nullptr) {
    return false;
  }
  if (correction->role == role) {
    return true;
  }
  /* The role chooses the list the row lives in, so changing it moves the row between the owner's
   * effects and mask_stack, keeping it at the top of its new list. */
  ListBase *source_list = (correction->role == MA_PAINT_LAYER_ROLE_MASK_ITEM) ? &owner->mask_stack :
                                                                                &owner->effects;
  ListBase *destination = (role == MA_PAINT_LAYER_ROLE_MASK_ITEM) ? &owner->mask_stack :
                                                                    &owner->effects;
  BLI_remlink(source_list, correction);
  correction->role = int8_t(role);
  BLI_addtail(destination, correction);
  BKE_paint_layers_tag_edited(ma);
  return true;
}

bool BKE_paint_layers_correction_source_set(Material &ma, MaterialPaintLayer *correction, int source)
{
  if (correction == nullptr || BKE_paint_layers_role(*correction) == PaintLayerRole::Layer ||
      paint_layer_owner_list(&ma.paint_layers, correction) == nullptr)
  {
    return false;
  }
  /* Symmetric with #BKE_paint_layers_correction_add: both roles accept Material/Node Group/Stack
   * now (phase 4). */
  const bool source_ok = ELEM(source,
                              MA_PAINT_LAYER_SOURCE_IMAGE,
                              MA_PAINT_LAYER_SOURCE_CONSTANT,
                              MA_PAINT_LAYER_SOURCE_MESH_MAP) ||
                         ELEM(source,
                              MA_PAINT_LAYER_SOURCE_MATERIAL,
                              MA_PAINT_LAYER_SOURCE_NODE_GROUP,
                              MA_PAINT_LAYER_SOURCE_STACK);
  if (!source_ok) {
    return false;
  }
  correction->source = int8_t(source);
  BKE_paint_layers_tag_edited(ma);
  return true;
}

int BKE_paint_layers_mesh_map_type_get(const MaterialPaintLayer &layer)
{
  return (layer.source == MA_PAINT_LAYER_SOURCE_MESH_MAP) ? layer.mesh_map_type : -1;
}

bool BKE_paint_layers_mesh_map_type_set(Material &ma, MaterialPaintLayer *layer, const int8_t type)
{
  if (layer == nullptr || paint_layer_owner_list(&ma.paint_layers, layer) == nullptr ||
      layer->source != MA_PAINT_LAYER_SOURCE_MESH_MAP || type < 0 || type >= MA_MESH_MAP_TYPE_NUM)
  {
    return false;
  }
  if (layer->mesh_map_type == type) {
    return true;
  }
  layer->mesh_map_type = type;
  BKE_paint_layers_tag_edited(ma);
  return true;
}

namespace {

void paint_layer_assert_consistent_row(const MaterialPaintLayer &layer)
{
  for (const MaterialPaintLayer &effect : layer.effects) {
    BLI_assert(BKE_paint_layers_role(effect) == PaintLayerRole::Effect);
    paint_layer_assert_consistent_row(effect);
  }
  for (const MaterialPaintLayer &mask_item : layer.mask_stack) {
    BLI_assert(BKE_paint_layers_role(mask_item) == PaintLayerRole::MaskItem);
    paint_layer_assert_consistent_row(mask_item);
  }
  for (const MaterialPaintLayer &child : layer.children) {
    paint_layer_assert_consistent_row(child);
  }
}

}  // namespace

void BKE_paint_layers_assert_consistent(const Material &ma)
{
  for (const MaterialPaintLayer &layer : ma.paint_layers) {
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
  if (ELEM(blend, MA_PAINT_LAYER_BLEND_NORMAL_COMBINE, MA_PAINT_LAYER_BLEND_NORMAL_REPLACE)) {
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
  /* A Layer-role row keeps Base Color in its channel record, like every other channel, so the
   * record is the single place a Fill's colour lives; the DNA field stays the constant of rows
   * without records (masks and Fill-effect corrections). The record is guaranteed to exist for a
   * Fill (the default channel set creates Base Color), and created on demand for a Paint row that
   * lost it, so a script cannot write into a field nothing reads. */
  if (BKE_paint_layers_role(*layer) == PaintLayerRole::Layer) {
    if (paint_layer_channel_find(*layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR) == nullptr) {
      if (BKE_paint_layers_channel_add(ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR) == nullptr) {
        return false;
      }
    }
    return BKE_paint_layers_channel_set_value(
        ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, color);
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
  /* [PL-DIAG] Visibility edit: the hash of the row whose bake covers this item, before and after. */
  const MaterialPaintLayer *diag_owner = paint_layer_correction_owner(ma.paint_layers, *layer);
  const MaterialPaintLayer &diag_row = (diag_owner != nullptr) ? *diag_owner : *layer;
  uint32_t diag_hash_before[2];
  BKE_paint_layers_bake_hash(ma, diag_row, diag_hash_before);
  const bool diag_regen_before = (ma.paint_layers_flag & MA_PAINT_LAYERS_REGEN) != 0;
  /* Whether each baked ancestor replaces its row right now, before the edit changes its hash. */
  uint64_t bake_state_before = 0;
  paint_layer_ancestor_bake_state(ma, ma.paint_layers, layer, bake_state_before);
  SET_FLAG_FROM_TEST(layer->flag, enabled, MA_PAINT_LAYER_ENABLED);
  /* Start (or clear) the hidden row's cold-tier clock. The mark is what lets the idle tick drop a
   * row hidden past #PAINT_LAYERS_COLD_TIER_SECONDS while a quick off/on stays a value edit. A
   * localized or evaluated copy shares its description and must not get a runtime. */
  const int no_runtime_tags = ID_TAG_LOCALIZED | ID_TAG_COPIED_ON_EVAL | ID_TAG_NO_MAIN;
  if ((ma.id.tag & no_runtime_tags) == 0) {
    bke::MaterialPaintLayersRuntime &runtime = bke::paint_layers_runtime_ensure(ma);
    if (enabled) {
      runtime.hidden_since.remove(layer->marker);
    }
    else {
      runtime.hidden_since.add_overwrite(layer->marker, BLI_time_now_seconds());
    }
  }
  /* Enabled folds into the group-input factor (#BKE_paint_layers_effective_opacity), and the
   * generator keeps a disabled row in the topology with factor zero, so this is a value edit: an
   * F-curve on it reaches the shader without a rebuild. A row an earlier rebuild actually dropped
   * from the graph has to come back though, and that is a topology change. */
  if (enabled && BKE_paint_layers_row_removed_clear(ma, layer->marker)) {
    ma.paint_layers_flag |= MA_PAINT_LAYERS_REGEN;
    BKE_paint_layers_root_hash_invalidate(ma);
  }
  /* A row's own visibility is not in its bake hash, so its own bake is no reason to rebuild:
   * toggling the row being looked at must not cost a graph rebuild. */
  /* A baked ancestor is only topology while its substitution flips: a substituted row has no live
   * nodes to carry the new factor, and a row that just became valid again has to go back to its
   * bake. A row that stays live (its bake already stale) keeps every node, so flipping one more
   * correction is a value sync -- that is what lets a series of toggles cost no graph rebuild. */
  paint_layers_tag_value_edited(
      ma, layer, /*value_in_own_bake=*/false, /*regen_for_baked_ancestor=*/false);
  uint64_t bake_state_after = 0;
  paint_layer_ancestor_bake_state(ma, ma.paint_layers, layer, bake_state_after);
  const bool substitution_flipped = bake_state_before != bake_state_after;
  if (substitution_flipped) {
    ma.paint_layers_flag |= MA_PAINT_LAYERS_REGEN;
  }
  /* The heavy re-bake waits for a pause in the toggling instead of starting per flip. */
  BKE_paint_layers_bake_debounce_extend(ma);
  printf("[PL-DIAG] set_enabled toggle item='%s' value=%d regen_set=%d bake_stale_set=%d "
         "ancestor_valid_bits %llx -> %llx (1 = substituted by bake) debounce_s=%.2f\n",
         layer->name,
         int(enabled),
         int(substitution_flipped),
         int((ma.paint_layers_flag & MA_PAINT_LAYERS_BAKE_STALE) != 0),
         static_cast<unsigned long long>(bake_state_before),
         static_cast<unsigned long long>(bake_state_after),
         PAINT_LAYERS_BAKE_EDIT_QUIET_SECONDS);
  {
    uint32_t hash_after[2];
    BKE_paint_layers_bake_hash(ma, diag_row, hash_after);
    printf(
        "[PL-DIAG] set_enabled item='%s' role=%d enabled=%d row='%s' row_has_bake=%d "
        "row_bake_valid=%d hash %08x%08x -> %08x%08x regen %d -> %d active_row_in_subtree=%d\n",
        layer->name,
        int(layer->role),
        int(enabled),
        diag_row.name,
        int(diag_row.bake != nullptr),
        int(BKE_paint_layers_bake_is_valid(ma, diag_row)),
        diag_hash_before[0],
        diag_hash_before[1],
        hash_after[0],
        hash_after[1],
        int(diag_regen_before),
        int((ma.paint_layers_flag & MA_PAINT_LAYERS_REGEN) != 0),
        int(BKE_paint_layers_subtree_contains(diag_row, ma.active_layer_marker)));
    printf("[PL-DIAG] set_enabled multiplier item='%s' value=%.1f regen_called=%d row_substituted=%d\n",
           layer->name,
           enabled ? 1.0 : 0.0,
           int((ma.paint_layers_flag & MA_PAINT_LAYERS_REGEN) != 0),
           int(diag_row.bake != nullptr && BKE_paint_layers_bake_is_valid(ma, diag_row)));
    /* [PL-DIAG] Which effect of the row was switched (the names are all alike) and what every
     * effect of the row reads now; effect 0 is the head of the list. */
    int toggled_index = -1;
    int effect_index = 0;
    for (const MaterialPaintLayer &effect : diag_row.effects) {
      if (&effect == layer) {
        toggled_index = effect_index;
      }
      effect_index++;
    }
    printf("[PL-DIAG] set_enabled effects row='%s' toggled_index=%d marker=%08x effect_num=%d\n",
           diag_row.name,
           toggled_index,
           unsigned(layer->marker.time_low),
           effect_index);
    effect_index = 0;
    for (const MaterialPaintLayer &effect : diag_row.effects) {
      printf("[PL-DIAG]   effect[%d] marker=%08x enabled=%d opacity=%.3f effective=%.3f\n",
             effect_index++,
             unsigned(effect.marker.time_low),
             int((effect.flag & MA_PAINT_LAYER_ENABLED) != 0),
             effect.opacity,
             BKE_paint_layers_effective_opacity(effect));
    }
  }
  return true;
}

void BKE_paint_layers_bake_debounce_extend(Material &ma)
{
  /* Only the owning material has a runtime; a localized or evaluated copy never gets one. */
  if ((ma.id.tag & (ID_TAG_LOCALIZED | ID_TAG_COPIED_ON_EVAL | ID_TAG_NO_MAIN)) != 0) {
    return;
  }
  bke::paint_layers_runtime_ensure(ma).bake_debounce_until = BLI_time_now_seconds() +
                                                           PAINT_LAYERS_BAKE_EDIT_QUIET_SECONDS;
}

double BKE_paint_layers_bake_debounce_seconds(const Material &ma, const double base_seconds)
{
  const bke::MaterialPaintLayersRuntime *runtime = bke::paint_layers_runtime_get(ma);
  if (runtime == nullptr) {
    return base_seconds;
  }
  const double rest = runtime->bake_debounce_until - BLI_time_now_seconds();
  return std::max(base_seconds, rest);
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

float BKE_paint_layers_mapping_scale_normalize(const float scale)
{
  /* Old files read back as zeroes; a zero axis would collapse UVs, so it means one. */
  if (scale == 0.0f) {
    return 1.0f;
  }
  const float magnitude = fabsf(scale);
  if (magnitude < PAINT_LAYER_MAPPING_SCALE_MIN) {
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
  if (!ELEM(row.role, MA_PAINT_LAYER_ROLE_LAYER, MA_PAINT_LAYER_ROLE_EFFECT)) {
    return false;
  }
  if (row.source != MA_PAINT_LAYER_SOURCE_CONSTANT) {
    return false;
  }
  return paint_layer_channel_image(ma, row, channel) != nullptr;
}

bool BKE_paint_layers_mapping_supported(const Material &ma, const MaterialPaintLayer &layer)
{
  /* A mask is scalar over its owner; its map lives in the Base Color record of the item itself,
   * never in coverage maps or the owner's records. Only a Fill mask's map is remappable: a
   * painted (Image) mask is written by strokes at the raw UV, mesh-map atlases stay Extend and
   * Material/Node-Group/Stack masks copy their source's extension. */
  if (BKE_paint_layers_role(layer) == PaintLayerRole::MaskItem) {
    /* A Material mask repeats its live map through the item's own Mapping, like a Layer. */
    if (layer.source == MA_PAINT_LAYER_SOURCE_MATERIAL) {
      return layer.material != nullptr;
    }
    if (layer.source != MA_PAINT_LAYER_SOURCE_CONSTANT) {
      return false;
    }
    return paint_layer_mask_correction_image(ma, layer, 0) != nullptr;
  }
  /* A Material row's live maps repeat through the row's own Mapping, whatever its role: the
   * wrapper is shared per (owner, source) but every row owns its instance and its values. */
  if (layer.source == MA_PAINT_LAYER_SOURCE_MATERIAL) {
    return layer.material != nullptr;
  }
  /* Only a Fill row's own maps repeat: the graph forces Repeat on them. Any other source either
   * owns no repeatable map (Mesh-Map atlases stay Extend) or copies its source's extension
   * (Material live maps). */
  if (!ELEM(layer.role, MA_PAINT_LAYER_ROLE_LAYER, MA_PAINT_LAYER_ROLE_EFFECT)) {
    return false;
  }
  if (layer.source != MA_PAINT_LAYER_SOURCE_CONSTANT) {
    return false;
  }
  for (const int channel : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    if (BKE_paint_layers_fill_reads_map(ma, layer, channel)) {
      return true;
    }
  }
  return false;
}

bool BKE_paint_layers_mapping_enabled_get(const MaterialPaintLayer &layer)
{
  return (layer.mapping.flag & MA_PAINT_LAYER_MAPPING_ENABLED) != 0;
}

bool BKE_paint_layers_mapping_set_enabled(Material &ma, MaterialPaintLayer *layer, bool enabled)
{
  if (layer == nullptr || paint_layer_owner_list(&ma.paint_layers, layer) == nullptr) {
    return false;
  }
  /* Enabling mapping without a repeatable map would add a node the graph never builds, so refuse
   * it while the row reads no map. Disabling is always allowed. */
  if (enabled && !BKE_paint_layers_mapping_supported(ma, *layer)) {
    return false;
  }
  const bool changed = BKE_paint_layers_mapping_enabled_get(*layer) != enabled;
  SET_FLAG_FROM_TEST(layer->mapping.flag, enabled, MA_PAINT_LAYER_MAPPING_ENABLED);
  if (changed) {
    /* The row gains or loses its Mapping node, which is topology. */
    BKE_paint_layers_tag_edited(ma);
  }
  return true;
}

bool BKE_paint_layers_mapping_applies(const Material &ma,
                                      const MaterialPaintLayer &layer,
                                      const PaintLayersRegenCache *cache)
{
  /* The one predicate that decides whether the row carries a mapping anywhere: enabled, reading a
   * repeatable map, and -- a Material row only -- in a mode that actually builds one. The values
   * never move this answer; a slider drag must not rebuild anything. */
  if (!BKE_paint_layers_mapping_enabled_get(layer) ||
      !BKE_paint_layers_mapping_supported(ma, layer))
  {
    return false;
  }
  /* A Fill (or Fill mask) row always carries its Mapping node: the graph builds it and the CPU
   * resamples through it unconditionally. */
  if (layer.source != MA_PAINT_LAYER_SOURCE_MATERIAL) {
    return true;
  }
  /* A Material row's mapping is built where its source is shown: the live textures of a Hybrid
   * row get their own Mapping node, and a SourceGroup row's wrapper carries it. A Baked row --
   * forced by the sampler budget -- shows its raw maps and ignores the mapping (reported by the
   * regeneration). A BAKE_NEVER row stays live and keeps it. */
  const PaintLayerMaterialMode mode = BKE_paint_layers_material_mode(ma, layer, cache);
  return ELEM(mode, PaintLayerMaterialMode::Hybrid, PaintLayerMaterialMode::SourceGroup);
}

bool BKE_paint_layers_mapping_blocks_bake(const Material &ma,
                                          const MaterialPaintLayer &layer,
                                          const PaintLayersRegenCache *cache)
{
  /* A row with a mapping anywhere inside it stays live: a bake would freeze the slider values, and
   * re-rendering it on every drag tick is what this rule avoids. */
  if (BKE_paint_layers_mapping_applies(ma, layer, cache)) {
    return true;
  }
  for (const MaterialPaintLayer &effect : layer.effects) {
    if (BKE_paint_layers_mapping_blocks_bake(ma, effect, cache)) {
      return true;
    }
  }
  for (const MaterialPaintLayer &mask_item : layer.mask_stack) {
    if (BKE_paint_layers_mapping_blocks_bake(ma, mask_item, cache)) {
      return true;
    }
  }
  for (const MaterialPaintLayer &child : layer.children) {
    if (BKE_paint_layers_mapping_blocks_bake(ma, child, cache)) {
      return true;
    }
  }
  return false;
}

/**
 * A mapping edit is a pure shading value: its row is live and never baked, so unlike
 * #paint_layers_tag_value_only it must not mark the bake planner stale.
 */
static void paint_layers_tag_mapping_value(Material &ma)
{
  DEG_id_tag_update(&ma.id, ID_RECALC_SHADING | ID_RECALC_SYNC_TO_EVAL);
}

bool BKE_paint_layers_mapping_set_offset(Material &ma,
                                         MaterialPaintLayer *layer,
                                         const float offset[2])
{
  if (layer == nullptr || paint_layer_owner_list(&ma.paint_layers, layer) == nullptr) {
    return false;
  }
  copy_v2_v2(layer->mapping.offset, offset);
  /* Values ride the row group's inputs, so the tree keeps its nodes. */
  paint_layers_tag_mapping_value(ma);
  BKE_paint_layers_values_sync(ma);
  return true;
}

bool BKE_paint_layers_mapping_set_scale(Material &ma,
                                        MaterialPaintLayer *layer,
                                        const float scale[2])
{
  if (layer == nullptr || paint_layer_owner_list(&ma.paint_layers, layer) == nullptr) {
    return false;
  }
  /* Normalize through the one helper the graph and the CPU share, so zeroes from old files and
   * tiny values behave the same on both sides. */
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
  layer->mapping.scale[0] = next[0];
  layer->mapping.scale[1] = next[1];
  paint_layers_tag_mapping_value(ma);
  BKE_paint_layers_values_sync(ma);
  return true;
}

bool BKE_paint_layers_mapping_scale_lock_get(const MaterialPaintLayer &layer)
{
  return (layer.mapping.flag & MA_PAINT_LAYER_MAPPING_SCALE_LOCK) != 0;
}

bool BKE_paint_layers_mapping_set_scale_lock(Material &ma, MaterialPaintLayer *layer, bool locked)
{
  if (layer == nullptr || paint_layer_owner_list(&ma.paint_layers, layer) == nullptr) {
    return false;
  }
  SET_FLAG_FROM_TEST(layer->mapping.flag, locked, MA_PAINT_LAYER_MAPPING_SCALE_LOCK);
  if (locked) {
    /* Locking joins the axes at the first one, so what the row shows stays where the user left
     * the X value. A value-only change like any other scale edit. */
    const float joined = BKE_paint_layers_mapping_scale_normalize(layer->mapping.scale[0]);
    layer->mapping.scale[0] = joined;
    layer->mapping.scale[1] = joined;
    paint_layers_tag_mapping_value(ma);
    BKE_paint_layers_values_sync(ma);
  }
  return true;
}

bool BKE_paint_layers_mapping_set_rotation(Material &ma, MaterialPaintLayer *layer, float rotation)
{
  if (layer == nullptr || paint_layer_owner_list(&ma.paint_layers, layer) == nullptr) {
    return false;
  }
  layer->mapping.rotation = rotation;
  paint_layers_tag_mapping_value(ma);
  BKE_paint_layers_values_sync(ma);
  return true;
}

}  // namespace blender


