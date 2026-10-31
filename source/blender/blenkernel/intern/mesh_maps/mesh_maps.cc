/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * See BKE_mesh_maps.hh for the data model. This file only creates, reads and links the DNA
 * records; it never renders and never touches a node tree.
 */

#include "BKE_mesh_maps.hh"

#include <cstddef>

#include "BLI_listbase.h"

#include "BKE_lib_id.hh"

#include "MEM_guardedalloc.h"

#include "DNA_image_types.h"
#include "DNA_material_types.h"

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
  for (MaterialMeshMapSlot &slot : ma.mesh_map_slots) {
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
  return true;
}

void BKE_mesh_maps_material_slots_free(Material &ma)
{
  /* The slot records are plain sub-data with no ID references of their own, so they are freed
   * outright, the way the Color ID colours are. The Images are released by the generic free
   * pass. */
  ma.mesh_map_slots.free_no_destruct();
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
  for (const MaterialMeshMapSlot &src : ma_src.mesh_map_slots) {
    MaterialMeshMapSlot *dst = MEM_dupalloc(&src);
    dst->next = nullptr;
    dst->prev = nullptr;
    BLI_addtail(&ma_dst.mesh_map_slots, dst);
  }
}

}  // namespace blender
