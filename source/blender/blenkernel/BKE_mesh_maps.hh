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

#include "BLI_set.hh"
#include "BLI_vector.hh"

namespace blender {

struct Collection;
struct Image;
struct Main;
struct Material;
struct MaterialMeshMapSlot;
struct Object;
struct ObjectMeshMapSource;
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
/** \name High-poly sources
 *
 * One #ObjectMeshMapSource per (low-poly object, material), shared by every map type. The source is
 * a single object or a collection, at most one of the two. It is a reference, never an owning user.
 * \{ */

/** The source record of \a ob for \a ma, or null. "Find" prunes records whose material is gone. */
ObjectMeshMapSource *BKE_mesh_maps_source_find(Object &ob, const Material &ma);
const ObjectMeshMapSource *BKE_mesh_maps_source_find(const Object &ob, const Material &ma);

/** The source record of \a ob for \a ma, creating an empty one when absent. */
ObjectMeshMapSource *BKE_mesh_maps_source_ensure(Object &ob, Material &ma);

/** Remove the source record of \a ob for \a ma. Returns whether one was removed. */
bool BKE_mesh_maps_source_remove(Object &ob, const Material &ma);

/** Drop every source of \a ob whose material pointer is null. Returns how many were removed. */
int BKE_mesh_maps_sources_prune(Object &ob);

/**
 * Resolve the source of (\a ob, \a ma) into the objects to bake *from*: the single high-poly object,
 * or every mesh object of the collection recursively. \a ob itself and the cage are always excluded.
 * An unset source yields an empty list. \a r_cage receives the cage object, or null.
 *
 * The objects are original data-blocks; the caller evaluates them in its depsgraph.
 */
void BKE_mesh_maps_source_resolve(const Object &ob,
                                  const Material &ma,
                                  Vector<Object *> &r_objects,
                                  Object **r_cage);

/**
 * Union, across every object of \a bmain that uses \a ma in a slot, of the resolved source objects
 * and cage. Used to keep high-poly out of the "foreign" atlas coverage and out of the all-objects
 * bake scope: a high-poly is a source, never a low-poly to bake.
 */
void BKE_mesh_maps_source_collect_for_material(Main &bmain,
                                               const Material &ma,
                                               Set<const Object *> &r_objects);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Content hash and bake staleness
 *
 * The hash is a content hash of the object's own contribution to one map: the evaluated geometry,
 * the UV layer the maps are sampled through, the type, the material settings that move the result
 * and -- for the normal maps -- what determines the evaluated normals. It never reads pointers,
 * `session_uid` or update counters, so an unchanged object hashes to the same value between runs.
 * The bake side stores it when a bake starts and recomputes it on commit; a mismatch means the
 * result is stale and must not be written.
 * \{ */

/**
 * Compute the content hash of \a ob_eval's contribution to \a ma's mesh map \a type, low word
 * first.
 *
 * \param ob_eval: the evaluated object from a depsgraph (#DEG_get_evaluated). The mesh is the
 * evaluated one after modifiers; an object evaluated outside a depsgraph falls back to its own
 * data mesh. A non-mesh object, or a mesh object without a mesh, yields a zero hash.
 * \param ma: the material that owns the shared atlas and the settings that shape this map.
 * \param type: an #eMaterialMeshMapType; a value outside the enum yields a zero hash.
 */
void BKE_mesh_maps_object_hash(const Object &ob_eval,
                               const Material &ma,
                               int8_t type,
                               uint32_t r_hash[2]);

/**
 * Whether the UV layer \a ma names is missing from \a ob_eval's mesh, so a map cannot be sampled and
 * its bake must be refused. False when no name is set, or the object is not a mesh with a mesh.
 */
bool BKE_mesh_maps_object_uv_missing(const Object &ob_eval, const Material &ma);

/** Whether \a state's stored hash matches the object's current one. */
bool BKE_mesh_maps_object_state_is_current(const ObjectMeshMapState &state,
                                           const Object &ob_eval,
                                           const Material &ma);

/**
 * Re-evaluate \a state against the object's current content.
 *
 * A #OB_MESH_MAP_STATUS_VALID state whose content moved becomes #OB_MESH_MAP_STATUS_STALE; a stale
 * state whose content is back to the stored hash becomes valid again. The terminal states
 * (#OB_MESH_MAP_STATUS_NONE, #OB_MESH_MAP_STATUS_BAKING, #OB_MESH_MAP_STATUS_ERROR) are left alone.
 * Returns the (possibly updated) status.
 */
int8_t BKE_mesh_maps_object_state_refresh(ObjectMeshMapState &state,
                                          const Object &ob_eval,
                                          const Material &ma);

/**
 * Record a successful bake: \a hash is the hash computed when the bake started, \a baked_time its
 * Unix time. The status becomes #OB_MESH_MAP_STATUS_VALID. C++ only; not exposed to RNA so a script
 * cannot fake a valid state.
 */
void BKE_mesh_maps_object_state_mark_baked(ObjectMeshMapState &state,
                                           const uint32_t hash[2],
                                           int baked_time);

/**
 * Refresh every state of \a ob (each with the material stored on it). Returns how many states
 * changed status.
 */
int BKE_mesh_maps_object_refresh_all(Object &ob, const Object &ob_eval);

/** \} */

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

void BKE_mesh_maps_object_sources_free(Object &ob);
void BKE_mesh_maps_object_sources_copy(Object &ob_dst, const Object &ob_src);

/** \} */

}  // namespace blender
