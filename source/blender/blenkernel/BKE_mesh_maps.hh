/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * The data model of Mesh Maps: the shared UV-atlas slots that live on a layered material
 * (`Material::mesh_map_slots`, one per #eMaterialMeshMapType).
 *
 * The pixels are a resource of the material, because one material is one generated shader shared
 * by every object that uses it: a row can only read an image the material knows. The per-object
 * part -- validity and the content hash of the object's own contribution -- lives on the object and
 * is added by its own feature.
 *
 * These helpers are description-only: they never render and never touch a node tree. The bake side
 * allocates the atlas Image; #BKE_mesh_maps_slot_ensure only creates the slot record.
 */

#include <cstdint>

namespace blender {

struct Image;
struct Material;
struct MaterialMeshMapSlot;

/** The slot of \a ma for mesh map \a type, or null. A type outside the enum reads as null. */
MaterialMeshMapSlot *BKE_mesh_maps_slot_find(Material &ma, int8_t type);
const MaterialMeshMapSlot *BKE_mesh_maps_slot_find(const Material &ma, int8_t type);

/**
 * The slot of \a ma for mesh map \a type, creating an empty one (no Image) when absent.
 *
 * Never allocates an Image: the bake side owns that, so merely naming a map from a row never
 * leaves a blank atlas behind. Returns null for a type outside the enum.
 */
MaterialMeshMapSlot *BKE_mesh_maps_slot_ensure(Material &ma, int8_t type);

/**
 * Point \a ma's mesh map \a type at \a image, or detach it with a null one. User counts are
 * maintained here, exactly as #BKE_paint_layers_channel_set_image does for a channel map.
 *
 * \return false for a type outside the enum.
 */
bool BKE_mesh_maps_slot_image_set(Material &ma, int8_t type, Image *image);

/** Free the slot records of \a ma. The Images are released by the generic free pass. */
void BKE_mesh_maps_material_slots_free(Material &ma);

/**
 * Give \a ma_dst its own slot records, one per slot of \a ma_src. The Image references themselves
 * are re-counted by the generic copy machinery through `material_foreach_id`.
 */
void BKE_mesh_maps_material_slots_copy(Material &ma_dst, const Material &ma_src);

}  // namespace blender
