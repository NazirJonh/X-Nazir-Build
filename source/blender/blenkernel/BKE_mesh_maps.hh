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
 * The pixels are a resource of the material, because one material is one generated shader shared
 * by every object that uses it: a row can only read an image the material knows. The per-object
 * part -- validity, the content hash of the object's own contribution, the reserved high-poly
 * source -- belongs to the object, because that is what changes when the object is edited.
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
struct Mesh;
struct Object;
struct ObjectMeshMapSource;
struct ObjectMeshMapState;

/**
 * The slot of \a ma for mesh map \a type, or null. A type outside the enum reads as null.
 *
 * The type's primary (uid 0) slot wherever it sits, falling back to the first slot of the type
 * when no uid-0 slot exists (the fallback is also what keeps #BKE_mesh_maps_slot_ensure from
 * appending a second slot of the type). The primary normally sits first, because it is ensured
 * before any copy is appended. That is what makes this the source resolver: the Packing channel
 * atlases and the Curvature Normal Filter's Tangent Normal atlas are read through this function,
 * so a bake source is always the type's primary (uid 0) atlas and the copies of the source type
 * are never read as sources while the primary stands.
 */
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
 * The slot of \a type whose \a uid names it, or null. Uid zero is the type's primary atlas, the
 * one #BKE_mesh_maps_slot_find returns first.
 *
 * The lookup is a linear scan on the exact uid, so it never assumes the uids of one type are
 * contiguous: values are only ever handed out by #BKE_mesh_maps_slot_new and never reused, and
 * the holes a removal leaves behind are expected. A uid whose copy was removed resolves to null,
 * which is what empties the copy's readers instead of silently rebinding them to another copy.
 */
MaterialMeshMapSlot *BKE_mesh_maps_slot_find_uid(Material &ma, int8_t type, int32_t uid);
const MaterialMeshMapSlot *BKE_mesh_maps_slot_find_uid(const Material &ma,
                                                       int8_t type,
                                                       int32_t uid);

/**
 * Append a new copy of \a type with \a name (empty means the type's label) and a fresh uid from
 * #Material::mesh_map_slot_uid_next. The copy starts with no atlas; bake it to fill one.
 *
 * The uid is only ever taken from the running counter and the counter never walks back, so a
 * removed copy's uid is not reused and nothing that keyed it can come back pointing at a
 * different copy. The copy is appended after the primary, so #BKE_mesh_maps_slot_find keeps
 * resolving the primary first.
 */
MaterialMeshMapSlot *BKE_mesh_maps_slot_new(Material &ma, int8_t type, const char *name);

/**
 * Remove \a slot, releasing its atlas's user; true when it belonged to \a ma.
 *
 * The slot record and its Image user are all it touches. The per-object states and the MESH_MAP
 * rows keyed on the slot's uid stay as they are: they stop resolving (the uid lookups return
 * null), so a reader goes empty rather than reading another copy's atlas, and the stale state a
 * removed copy leaves behind is reset by the next clear of its type or freed with the object.
 */
bool BKE_mesh_maps_slot_remove(Material &ma, MaterialMeshMapSlot *slot);

/**
 * Point \a ma's mesh map \a type at \a image, or detach it with a null one. User counts are
 * maintained here, exactly as #BKE_paint_layers_channel_set_image does for a channel map.
 *
 * \return false for a type outside the enum.
 */
bool BKE_mesh_maps_slot_image_set(Material &ma, int8_t type, Image *image);

/**
 * Point \a slot itself at \a image (or detach it), maintaining the Image user exactly like
 * #BKE_mesh_maps_slot_image_set. Used when the caller already holds the exact copy's slot, so the
 * type's primary is never touched.
 */
bool BKE_mesh_maps_slot_image_set_slot(MaterialMeshMapSlot &slot, Image *image);

/**
 * The per-object state of mesh map \a type for object \a ob and material \a ma, or null.
 *
 * Entries whose material was removed (the pointer remapped to null) are skipped and dropped, so a
 * stale key can never dangle. The uid is not part of this key: the first state of (material, type)
 * in list order comes back, whichever copy it belongs to, so a caller that needs one copy's state
 * in particular uses the find_uid/ensure_uid forms below.
 */
ObjectMeshMapState *BKE_mesh_maps_object_state_find(Object &ob, const Material &ma, int8_t type);

/** The state of \a ob for (\a ma, \a type), creating an empty one (the primary's) when absent. */
ObjectMeshMapState *BKE_mesh_maps_object_state_ensure(Object &ob, Material &ma, int8_t type);

/**
 * The state of one atlas copy, or null. Uid zero is the primary atlas's state.
 *
 * States key on (material, type, slot_uid), so every atlas copy of one type carries its own
 * validity and hash and baking a copy never flips the primary's status. The content hash itself
 * is keyed by type alone (see the hash section below), so the copies of one type share the same
 * hash value. A state whose copy has been removed stays behind keyed on the dead uid -- inert,
 * because nothing resolves to it anymore -- until the next clear of the type resets it or the
 * object is freed.
 */
ObjectMeshMapState *BKE_mesh_maps_object_state_find_uid(Object &ob,
                                                        const Material &ma,
                                                        int8_t type,
                                                        int32_t slot_uid);
ObjectMeshMapState *BKE_mesh_maps_object_state_ensure_uid(Object &ob,
                                                          Material &ma,
                                                          int8_t type,
                                                          int32_t slot_uid);

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
 * The UV map every mesh map of \a mesh is sampled through: the mesh's active UV map. This build
 * has no material-named UV layer (`Material::paint_layers_uv_map` does not exist), so one resolve
 * serves the content hash, the bake and the foreign-coverage scan alike -- they must never disagree
 * about which layer a texel belongs to.
 *
 * \return the layer name, or null when the mesh has no usable UV map (no layer at all, or none
 * active). The pointer reads into the mesh and stays valid while its UV layers do not change.
 */
const char *BKE_mesh_maps_uv_resolve(const Mesh &mesh);

/**
 * Whether the object's mesh has no usable UV map, so a map cannot be sampled and its bake must
 * be refused. False when the object is not a mesh with a mesh.
 *
 * This build has no material-named UV map (`Material::paint_layers_uv_map` does not exist), so the
 * maps are sampled through the mesh's active UV map; the material parameter is unused, kept so the
 * signature stays stable if that feature lands.
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

/** Free the slot records of \a ma. The Images are released by the generic free pass. */
void BKE_mesh_maps_material_slots_free(Material &ma);

/**
 * Give \a ma_dst its own slot records, one per slot of \a ma_src. The Image references themselves
 * are re-counted by the generic copy machinery through `material_foreach_id`.
 */
void BKE_mesh_maps_material_slots_copy(Material &ma_dst, const Material &ma_src);

void BKE_mesh_maps_object_states_free(Object &ob);
void BKE_mesh_maps_object_states_copy(Object &ob_dst, const Object &ob_src);

void BKE_mesh_maps_object_sources_free(Object &ob);
void BKE_mesh_maps_object_sources_copy(Object &ob_dst, const Object &ob_src);

/** \} */

}  // namespace blender
