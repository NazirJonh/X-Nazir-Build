/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * The data model of Mesh Maps: the shared UV-atlas slots that live on a layered material
 * (`Material::mesh_map_slots`, one per #eMaterialMeshMapType) and the per-object bake state that
 * lives on an object (`Object::mesh_map_states`, one per (material, map type)).
 *
 * The pixels are a resource of the material, because one material is one generated shader shared by
 * every object that uses it: a row can only read an image the material knows. The per-object part
 * -- validity, the content hash of the object's own contribution, the reserved high-poly source --
 * belongs to the object, because that is what changes when the object is edited.
 *
 * These helpers are description-only: they never render and never touch a node tree. The bake side
 * allocates the atlas Image; #BKE_mesh_maps_slot_ensure only creates the slot record.
 */

#include <cstdint>

namespace blender {

struct Image;
struct Material;
struct MaterialMeshMapSlot;
struct Object;
struct ObjectMeshMapState;

/** The slot of \a ma for mesh map \a type, or null. A type outside the enum reads as null. */
MaterialMeshMapSlot *BKE_mesh_maps_slot_find(Material &ma, int8_t type);
const MaterialMeshMapSlot *BKE_mesh_maps_slot_find(const Material &ma, int8_t type);

/**
 * The slot of \a ma for mesh map \a type, creating an empty one (no Image) when absent.
 *
 * Never allocates an Image: the bake side owns that, so merely naming a map from a row never leaves
 * a blank atlas behind. Returns null for a type outside the enum.
 */
MaterialMeshMapSlot *BKE_mesh_maps_slot_ensure(Material &ma, int8_t type);

/**
 * Point \a ma's mesh map \a type at \a image, or detach it with a null one. User counts are
 * maintained here, exactly as #BKE_paint_layers_channel_set_image does for a channel map.
 *
 * \return false for a type outside the enum.
 */
bool BKE_mesh_maps_slot_image_set(Material &ma, int8_t type, Image *image);

/**
 * The per-object state of mesh map \a type for object \a ob and material \a ma, or null.
 *
 * Entries whose material was removed (the pointer remapped to null) are skipped and dropped, so a
 * stale key can never dangle.
 */
ObjectMeshMapState *BKE_mesh_maps_object_state_find(Object &ob, const Material &ma, int8_t type);

/** The state of \a ob for (\a ma, \a type), creating an empty one when absent. */
ObjectMeshMapState *BKE_mesh_maps_object_state_ensure(Object &ob, Material &ma, int8_t type);

/** Set the #eObjectMeshMapStatus of \a state. */
void BKE_mesh_maps_object_state_status_set(ObjectMeshMapState &state, int8_t status);

/** Drop every state of \a ob whose material pointer is null. Returns how many were removed. */
int BKE_mesh_maps_object_states_prune(Object &ob);

/* -------------------------------------------------------------------- */
/** \name Lifecycle helpers
 *
 * Called from `material.cc` / `object.cc`. The Image is an ID reference registered through
 * `foreach_id` with #IDWALK_CB_USER, so the generic copy/free machinery maintains its user count;
 * these only own the plain sub-data.
 * \{ */

void BKE_mesh_maps_material_slots_free(Material &ma);
void BKE_mesh_maps_material_slots_copy(Material &ma_dst, const Material &ma_src);

void BKE_mesh_maps_object_states_free(Object &ob);
void BKE_mesh_maps_object_states_copy(Object &ob_dst, const Object &ob_src);

/** \} */

}  // namespace blender
