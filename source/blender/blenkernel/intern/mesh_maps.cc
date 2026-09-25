/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * See BKE_mesh_maps.hh for the data model. This file only creates, reads and links the DNA records;
 * it never renders and never touches a node tree.
 */

#include "BKE_mesh_maps.hh"

#include "BLI_listbase.h"
#include "MEM_guardedalloc.h"

#include "BKE_lib_id.hh"
#include "BKE_paint_layers.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_object_types.h"

namespace blender {

namespace {

/** Whether \a type names a #eMaterialMeshMapType. */
bool mesh_map_type_valid(const int8_t type)
{
  return type >= 0 && type < MA_MESH_MAP_TYPE_NUM;
}

}  // namespace

MaterialMeshMapSlot *BKE_mesh_maps_slot_find(Material &ma, const int8_t type)
{
  for (MaterialMeshMapSlot &slot :
       *reinterpret_cast<ListBaseT<MaterialMeshMapSlot> *>(&ma.mesh_map_slots))
  {
    if (slot.type == type) {
      return &slot;
    }
  }
  return nullptr;
}

const MaterialMeshMapSlot *BKE_mesh_maps_slot_find(const Material &ma, const int8_t type)
{
  return BKE_mesh_maps_slot_find(const_cast<Material &>(ma), type);
}

MaterialMeshMapSlot *BKE_mesh_maps_slot_ensure(Material &ma, const int8_t type)
{
  if (!mesh_map_type_valid(type)) {
    return nullptr;
  }
  if (MaterialMeshMapSlot *existing = BKE_mesh_maps_slot_find(ma, type)) {
    return existing;
  }
  MaterialMeshMapSlot *slot = MEM_new<MaterialMeshMapSlot>(__func__);
  slot->type = type;
  BLI_addtail(&ma.mesh_map_slots, slot);
  return slot;
}

bool BKE_mesh_maps_slot_image_set(Material &ma, const int8_t type, Image *image)
{
  MaterialMeshMapSlot *slot = BKE_mesh_maps_slot_find(ma, type);
  if (slot == nullptr) {
    if (!mesh_map_type_valid(type)) {
      return false;
    }
    slot = BKE_mesh_maps_slot_ensure(ma, type);
  }
  if (slot->image == image) {
    return true;
  }
  if (slot->image != nullptr) {
    id_us_min(&slot->image->id);
  }
  slot->image = image;
  if (slot->image != nullptr) {
    id_us_plus(&slot->image->id);
  }
  /* The atlas is a map a MESH_MAP row reads, so pointing the slot at another Image is a structural
   * edit: the generated tree is stale and the evaluated copy has to sync. Mirrors what
   * #BKE_paint_layers_channel_set_image does for a channel map. */
  BKE_paint_layers_tag_edited(ma);
  return true;
}

int BKE_mesh_maps_object_states_prune(Object &ob)
{
  int removed = 0;
  for (ObjectMeshMapState *state = static_cast<ObjectMeshMapState *>(ob.mesh_map_states.first),
                           *next = nullptr;
       state != nullptr;
       state = next)
  {
    next = state->next;
    if (state->material == nullptr) {
      BLI_remlink(&ob.mesh_map_states, state);
      MEM_delete(state);
      removed++;
    }
  }
  return removed;
}

ObjectMeshMapState *BKE_mesh_maps_object_state_find(Object &ob,
                                                    const Material &ma,
                                                    const int8_t type)
{
  BKE_mesh_maps_object_states_prune(ob);
  for (ObjectMeshMapState &state :
       *reinterpret_cast<ListBaseT<ObjectMeshMapState> *>(&ob.mesh_map_states))
  {
    if (state.material == &ma && state.type == type) {
      return &state;
    }
  }
  return nullptr;
}

ObjectMeshMapState *BKE_mesh_maps_object_state_ensure(Object &ob,
                                                      Material &ma,
                                                      const int8_t type)
{
  if (!mesh_map_type_valid(type)) {
    return nullptr;
  }
  if (ObjectMeshMapState *existing = BKE_mesh_maps_object_state_find(ob, ma, type)) {
    return existing;
  }
  ObjectMeshMapState *state = MEM_new<ObjectMeshMapState>(__func__);
  state->material = &ma;
  state->type = type;
  state->status = OB_MESH_MAP_STATUS_NONE;
  BLI_addtail(&ob.mesh_map_states, state);
  return state;
}

void BKE_mesh_maps_object_state_status_set(ObjectMeshMapState &state, const int8_t status)
{
  state.status = status;
}

/* -------------------------------------------------------------------- */
/** \name Lifecycle
 * \{ */

void BKE_mesh_maps_material_slots_free(Material &ma)
{
  for (MaterialMeshMapSlot *slot = static_cast<MaterialMeshMapSlot *>(ma.mesh_map_slots.first),
                             *next = nullptr;
       slot != nullptr;
       slot = next)
  {
    next = slot->next;
    MEM_delete(slot);
  }
  ma.mesh_map_slots = {nullptr, nullptr};
}

void BKE_mesh_maps_material_slots_copy(Material &ma_dst, const Material &ma_src)
{
  /* `DNA_DEFINE_CXX_METHODS` deletes the copy assignment, so the settings are copied field by
   * field. */
  ma_dst.mesh_map_settings.resolution = ma_src.mesh_map_settings.resolution;
  ma_dst.mesh_map_settings.samples = ma_src.mesh_map_settings.samples;
  ma_dst.mesh_map_settings.use_denoise = ma_src.mesh_map_settings.use_denoise;
  ma_dst.mesh_map_settings.margin = ma_src.mesh_map_settings.margin;
  ma_dst.mesh_map_settings.ao_distance = ma_src.mesh_map_settings.ao_distance;
  ma_dst.mesh_map_settings.edge_radius = ma_src.mesh_map_settings.edge_radius;
  ma_dst.mesh_map_slots = {nullptr, nullptr};
  for (const MaterialMeshMapSlot &src :
       *reinterpret_cast<const ListBaseT<MaterialMeshMapSlot> *>(&ma_src.mesh_map_slots))
  {
    MaterialMeshMapSlot *dst = MEM_dupalloc(&src);
    dst->next = nullptr;
    dst->prev = nullptr;
    BLI_addtail(&ma_dst.mesh_map_slots, dst);
  }
}

void BKE_mesh_maps_object_states_free(Object &ob)
{
  for (ObjectMeshMapState *state = static_cast<ObjectMeshMapState *>(ob.mesh_map_states.first),
                             *next = nullptr;
       state != nullptr;
       state = next)
  {
    next = state->next;
    MEM_delete(state);
  }
  ob.mesh_map_states = {nullptr, nullptr};
}

void BKE_mesh_maps_object_states_copy(Object &ob_dst, const Object &ob_src)
{
  ob_dst.mesh_map_states = {nullptr, nullptr};
  for (const ObjectMeshMapState &src :
       *reinterpret_cast<const ListBaseT<ObjectMeshMapState> *>(&ob_src.mesh_map_states))
  {
    ObjectMeshMapState *dst = MEM_dupalloc(&src);
    dst->next = nullptr;
    dst->prev = nullptr;
    BLI_addtail(&ob_dst.mesh_map_states, dst);
  }
}

/** \} */

}  // namespace blender
