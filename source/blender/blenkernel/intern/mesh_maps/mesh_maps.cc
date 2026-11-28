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
#include "BKE_mesh_maps_bake.hh"

#include <algorithm>
#include <cstddef>

#include "BLI_array.hh"
#include "BLI_cpp_type.hh"
#include "BLI_generic_span.hh"
#include "BLI_generic_virtual_array.hh"
#include "BLI_hash_mm2a.hh"
#include "BLI_listbase.h"
#include "BLI_math_matrix.hh"
#include "BLI_string.h"
#include "BLI_string_ref.hh"
#include "MEM_guardedalloc.h"

#include "BKE_attribute.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_maps_types.hh"
#include "BKE_object.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "DNA_collection_types.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"

namespace blender {

/* The appended tail of #MaterialMeshMapSettings and the tail of #Material are one layout
 * contract: the settings grow by appending, and #Material's closing int closes it on the pointer
 * boundary. The asserts fail to compile the day the pads drift. */
static_assert(sizeof(MaterialMeshMapSettings) == 36,
              "MaterialMeshMapSettings DNA layout changed");
static_assert(offsetof(MaterialMeshMapSettings, cpu_rays) == 24,
              "MaterialMeshMapSettings DNA layout changed");
static_assert(offsetof(MaterialMeshMapSettings, position_normalization) == 28,
              "MaterialMeshMapSettings DNA layout changed");
static_assert(offsetof(MaterialMeshMapSettings, use_directx_normal) == 29,
              "MaterialMeshMapSettings DNA layout changed");
static_assert(offsetof(Material, mesh_map_settings) % 8 == 0,
              "Material mesh_map_settings must stay pointer-aligned");
static_assert(offsetof(Material, mesh_map_slot_uid_next) + 4 == sizeof(Material),
              "Material must end on its explicit tail int without compiler padding");

bool BKE_mesh_maps_pair_renders(const Material &ma, const int8_t type)
{
  if (type == MA_MESH_MAP_CURVATURE &&
      ma.mesh_map_settings.curvature_method == MA_MESH_MAP_CURVATURE_NORMAL_FILTER)
  {
    /* The filter runs over the baked Tangent Normal atlas, no engine involved. */
    return false;
  }
  /* The static half of the classification, straight from the descriptor table. */
  const MeshMapTypeInfo *info = BKE_mesh_maps_type_info_get(type);
  return info != nullptr && (info->flags & MESH_MAP_TYPE_RENDERED) != 0;
}

namespace {

/** Whether \a type names a #eMaterialMeshMapType. */
bool mesh_map_type_valid(const int8_t type)
{
  return type >= 0 && type < MA_MESH_MAP_TYPE_NUM;
}

/* -------------------------------------------------------------------- */
/** \name Content hash helpers
 *
 * Scalars and small arrays are folded with the same FNV-style mix the layer bake hash uses. The
 * large per-element arrays (positions, topology, UVs) go through #BLI_hash_mm2a, which reads raw
 * bytes instead of one function call per float: a byte-wise FNV over a million vertices is far too
 * slow. The seed is derived from the running hash so the folds stay order-dependent.
 * \{ */

constexpr uint64_t mesh_map_hash_seed = 1469598103934665603ull;

inline uint64_t mesh_map_hash_mix(uint64_t h, const uint64_t value)
{
  h ^= value;
  h *= 1099511628211ull;
  return h;
}

/** Fold \a size raw bytes at \a data into \a h. Null or empty data is a distinct marker. */
uint64_t mesh_map_hash_bytes(uint64_t h, const void *data, const size_t size)
{
  if (data == nullptr || size == 0) {
    return mesh_map_hash_mix(h, 0);
  }
  BLI_HashMurmur2A mm2;
  BLI_hash_mm2a_init(&mm2, uint32_t(h) ^ uint32_t(h >> 32));
  BLI_hash_mm2a_add(&mm2, static_cast<const unsigned char *>(data), size);
  return mesh_map_hash_mix(h, uint64_t(BLI_hash_mm2a_end(&mm2)));
}

inline uint64_t mesh_map_hash_string(uint64_t h, const StringRef text)
{
  h = mesh_map_hash_mix(h, uint64_t(text.size()));
  return mesh_map_hash_bytes(h, text.data(), text.size());
}

template<typename T> uint64_t mesh_map_hash_span(uint64_t h, const Span<T> span)
{
  return mesh_map_hash_bytes(h, span.data(), size_t(span.size()) * sizeof(T));
}

/**
 * Fold one mesh attribute by content, through the generic virtual-array interface so a span, a
 * single broadcast value and a computed attribute all hash the same way. The type name and element
 * count are folded too, so two attributes that happen to share bytes with different layouts cannot
 * collide.
 */
uint64_t mesh_map_hash_attribute(uint64_t h,
                                 const bke::AttributeAccessor &attributes,
                                 const StringRef name)
{
  const bke::GAttributeReader attribute = attributes.lookup(name);
  h = mesh_map_hash_mix(h, attribute ? 1 : 0);
  if (!attribute) {
    return h;
  }
  const GVArray &varray = attribute.varray;
  const CPPType &type = varray.type();
  h = mesh_map_hash_string(h, type.name());
  h = mesh_map_hash_mix(h, uint64_t(varray.size()));
  if (varray.is_span()) {
    const GSpan span = varray.get_internal_span();
    return mesh_map_hash_bytes(h, span.data(), size_t(span.size()) * type.size);
  }
  Array<std::byte> value(type.size);
  if (varray.is_single()) {
    varray.get_internal_single(value.data());
    return mesh_map_hash_bytes(h, value.data(), type.size);
  }
  /* A computed attribute: no contiguous storage to read, so each element is folded in turn. */
  for (int64_t i = 0; i < varray.size(); i++) {
    varray.get(i, value.data());
    h = mesh_map_hash_bytes(h, value.data(), type.size);
  }
  return h;
}

/** Fold a high-poly/cage mesh's geometry: topology, positions, and the material index for IDs. */
uint64_t mesh_map_hash_source_mesh(uint64_t h, const Mesh &mesh, const int8_t type)
{
  h = mesh_map_hash_mix(h, uint64_t(uint32_t(mesh.verts_num)));
  h = mesh_map_hash_mix(h, uint64_t(uint32_t(mesh.edges_num)));
  h = mesh_map_hash_mix(h, uint64_t(uint32_t(mesh.faces_num)));
  h = mesh_map_hash_mix(h, uint64_t(uint32_t(mesh.corners_num)));
  h = mesh_map_hash_span(h, mesh.face_offsets());
  h = mesh_map_hash_span(h, mesh.corner_verts());
  h = mesh_map_hash_span(h, mesh.vert_positions());
  if (type == MA_MESH_MAP_ID_MATERIAL) {
    h = mesh_map_hash_attribute(h, mesh.attributes(), "material_index");
  }
  return h;
}

/** The mesh the hash reads: the evaluated one when available, else the object's own data mesh. */
const Mesh *mesh_map_object_mesh(const Object &ob)
{
  if (ob.type != OB_MESH) {
    return nullptr;
  }
  if (const Mesh *evaluated = BKE_object_get_evaluated_mesh(&ob)) {
    return evaluated;
  }
  return id_cast<const Mesh *>(ob.data);
}

/** Whether \a mesh has no UV layer at all, so no map can be sampled through it. */
bool mesh_map_uv_missing(const Mesh &mesh)
{
  bool has_uv = false;
  mesh.attributes().foreach_attribute([&](const bke::AttributeIter &iter) {
    if (bke::mesh::is_uv_map(bke::AttributeMetaData{iter.domain, iter.data_type})) {
      has_uv = true;
      iter.stop();
    }
  });
  return !has_uv;
}

void mesh_map_state_tag_changed(const Object &ob_eval)
{
  /* Shading alone: the state is read by RNA and the UI, and none of it changes the geometry, so a
   * GEOMETRY tag would force a needless re-evaluation. The original object owns the state. */
  Object *ob_orig = const_cast<Object *>(DEG_get_original(&ob_eval));
  if (ob_orig != nullptr) {
    DEG_id_tag_update(&ob_orig->id, ID_RECALC_SHADING);
  }
}

/** \} */

/** Collect the mesh objects of \a collection (recursively), minus \a low and \a cage. */
void mesh_map_source_add_collection(const Collection &collection,
                                    const Object &low,
                                    const Object *cage,
                                    Vector<Object *> &r_objects)
{
  for (const CollectionObject &link : collection.gobject) {
    Object *ob = link.ob;
    if (ob == nullptr || ob == &low || ob == cage || ob->type != OB_MESH || ob->data == nullptr) {
      continue;
    }
    if (!r_objects.contains(ob)) {
      r_objects.append(ob);
    }
  }
  for (const CollectionChild &child : collection.children) {
    if (child.collection != nullptr) {
      mesh_map_source_add_collection(*child.collection, low, cage, r_objects);
    }
  }
}

}  // namespace

/* Bake sources rely on the find below for the primary-source rule: the Curvature Normal Filter
 * resolves its source atlas through it, so it must name the type's primary (uid 0) atlas and
 * never a copy while the primary stands. The uid-0 slot normally sits first, because it is
 * ensured before any copy is appended; the scan still prefers it wherever it sits, so the rule
 * keeps holding whatever order later edits leave behind. A type with no uid-0 slot at all falls
 * back to its first remaining slot -- the same slot the ensure below reuses, so ensure never
 * appends a second slot of the type behind a copy. */
MaterialMeshMapSlot *BKE_mesh_maps_slot_find(Material &ma, const int8_t type)
{
  MaterialMeshMapSlot *first_of_type = nullptr;
  for (MaterialMeshMapSlot &slot : ma.mesh_map_slots) {
    if (slot.type != type) {
      continue;
    }
    if (slot.uid == 0) {
      return &slot;
    }
    if (first_of_type == nullptr) {
      first_of_type = &slot;
    }
  }
  return first_of_type;
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

/* A linear scan keyed on the exact uid: holes left by removed copies are fine, and a removed
 * uid resolves to null, so its readers go empty instead of silently reading another copy's
 * atlas. Nothing ever indexes slots by uid as a position. */
MaterialMeshMapSlot *BKE_mesh_maps_slot_find_uid(Material &ma, const int8_t type, const int32_t uid)
{
  for (MaterialMeshMapSlot &slot : ma.mesh_map_slots) {
    if (slot.type == type && slot.uid == uid) {
      return &slot;
    }
  }
  return nullptr;
}

const MaterialMeshMapSlot *BKE_mesh_maps_slot_find_uid(const Material &ma,
                                                       const int8_t type,
                                                       const int32_t uid)
{
  for (const MaterialMeshMapSlot &slot : ma.mesh_map_slots) {
    if (slot.type == type && slot.uid == uid) {
      return &slot;
    }
  }
  return nullptr;
}

/* Uids only ever grow: a removed copy's value is not reused, so a state or a MESH_MAP row keyed
 * on it can never come back pointing at a different copy. Holes from removals are expected. */
MaterialMeshMapSlot *BKE_mesh_maps_slot_new(Material &ma, const int8_t type, const char *name)
{
  if (!mesh_map_type_valid(type)) {
    return nullptr;
  }
  MaterialMeshMapSlot *slot = MEM_new<MaterialMeshMapSlot>(__func__);
  slot->type = type;
  slot->uid = std::max(ma.mesh_map_slot_uid_next, 1);
  ma.mesh_map_slot_uid_next = slot->uid + 1;
  if (name != nullptr) {
    STRNCPY(slot->name, name);
  }
  BLI_addtail(&ma.mesh_map_slots, slot);
  return slot;
}

/* Removing a copy touches its slot record and its Image user alone. The per-object states and
 * the MESH_MAP rows keyed on the copy's uid are left as they are: they stop resolving (the
 * find-by-uid above returns null), so a reader goes empty rather than rebinding to another
 * copy, and the next clear of the type resets the stale state alongside the live ones. */
bool BKE_mesh_maps_slot_remove(Material &ma, MaterialMeshMapSlot *slot)
{
  if (slot == nullptr) {
    return false;
  }
  bool found = false;
  for (MaterialMeshMapSlot &candidate : ma.mesh_map_slots) {
    if (&candidate == slot) {
      found = true;
      break;
    }
  }
  if (!found) {
    return false;
  }
  if (slot->image != nullptr) {
    id_us_min(&slot->image->id);
  }
  BLI_remlink(&ma.mesh_map_slots, slot);
  MEM_delete(slot);
  return true;
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
  return BKE_mesh_maps_slot_image_set_slot(*slot, image);
}

bool BKE_mesh_maps_slot_image_set_slot(MaterialMeshMapSlot &slot, Image *image)
{
  if (slot.image == image) {
    return true;
  }
  if (slot.image != nullptr) {
    id_us_min(&slot.image->id);
  }
  slot.image = image;
  if (slot.image != nullptr) {
    id_us_plus(&slot.image->id);
  }
  return true;
}

/* -------------------------------------------------------------------- */
/** \name Object states
 * \{ */

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
  for (ObjectMeshMapState &state : ob.mesh_map_states) {
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

/* The per-copy states key on (material, type, slot_uid): every atlas copy of one type carries
 * its own validity and hash, so baking a copy never flips the primary's status. A state whose
 * copy has been removed stays behind keyed on the dead uid -- inert, since nothing resolves to
 * it anymore -- until the next clear of the type resets it or the object is freed. */
ObjectMeshMapState *BKE_mesh_maps_object_state_find_uid(Object &ob,
                                                        const Material &ma,
                                                        const int8_t type,
                                                        const int32_t slot_uid)
{
  BKE_mesh_maps_object_states_prune(ob);
  for (ObjectMeshMapState &state : ob.mesh_map_states) {
    if (state.material == &ma && state.type == type && state.slot_uid == slot_uid) {
      return &state;
    }
  }
  return nullptr;
}

ObjectMeshMapState *BKE_mesh_maps_object_state_ensure_uid(Object &ob,
                                                          Material &ma,
                                                          const int8_t type,
                                                          const int32_t slot_uid)
{
  if (!mesh_map_type_valid(type)) {
    return nullptr;
  }
  if (ObjectMeshMapState *existing = BKE_mesh_maps_object_state_find_uid(ob, ma, type, slot_uid)) {
    return existing;
  }
  ObjectMeshMapState *state = MEM_new<ObjectMeshMapState>(__func__);
  state->material = &ma;
  state->type = type;
  state->slot_uid = slot_uid;
  state->status = OB_MESH_MAP_STATUS_NONE;
  BLI_addtail(&ob.mesh_map_states, state);
  return state;
}

void BKE_mesh_maps_object_state_status_set(ObjectMeshMapState &state, const int8_t status)
{
  state.status = status;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name High-poly sources
 * \{ */

int BKE_mesh_maps_sources_prune(Object &ob)
{
  int removed = 0;
  for (ObjectMeshMapSource *source = static_cast<ObjectMeshMapSource *>(ob.mesh_map_sources.first),
                                *next = nullptr;
       source != nullptr;
       source = next)
  {
    next = source->next;
    if (source->material == nullptr) {
      BLI_remlink(&ob.mesh_map_sources, source);
      MEM_delete(source);
      removed++;
    }
  }
  return removed;
}

ObjectMeshMapSource *BKE_mesh_maps_source_find(Object &ob, const Material &ma)
{
  BKE_mesh_maps_sources_prune(ob);
  for (ObjectMeshMapSource &source : ob.mesh_map_sources) {
    if (source.material == &ma) {
      return &source;
    }
  }
  return nullptr;
}

const ObjectMeshMapSource *BKE_mesh_maps_source_find(const Object &ob, const Material &ma)
{
  for (const ObjectMeshMapSource &source : ob.mesh_map_sources) {
    if (source.material == &ma) {
      return &source;
    }
  }
  return nullptr;
}

ObjectMeshMapSource *BKE_mesh_maps_source_ensure(Object &ob, Material &ma)
{
  if (ObjectMeshMapSource *existing = BKE_mesh_maps_source_find(ob, ma)) {
    return existing;
  }
  ObjectMeshMapSource *source = MEM_new<ObjectMeshMapSource>(__func__);
  source->material = &ma;
  BLI_addtail(&ob.mesh_map_sources, source);
  return source;
}

bool BKE_mesh_maps_source_remove(Object &ob, const Material &ma)
{
  ObjectMeshMapSource *source = BKE_mesh_maps_source_find(ob, ma);
  if (source == nullptr) {
    return false;
  }
  BLI_remlink(&ob.mesh_map_sources, source);
  MEM_delete(source);
  return true;
}

void BKE_mesh_maps_source_resolve(const Object &ob,
                                  const Material &ma,
                                  Vector<Object *> &r_objects,
                                  Object **r_cage)
{
  r_objects.clear();
  if (r_cage != nullptr) {
    *r_cage = nullptr;
  }
  /* The const overload: resolving runs from evaluation and RNA, where pruning the original's list
   * would free records under a concurrent reader. */
  const ObjectMeshMapSource *source = BKE_mesh_maps_source_find(ob, ma);
  if (source == nullptr) {
    return;
  }
  if (r_cage != nullptr) {
    *r_cage = source->cage;
  }
  if (source->high_poly != nullptr) {
    if (source->high_poly->type == OB_MESH && source->high_poly->data != nullptr &&
        source->high_poly != &ob && source->high_poly != source->cage)
    {
      r_objects.append(source->high_poly);
    }
  }
  else if (source->high_poly_collection != nullptr) {
    mesh_map_source_add_collection(
        *source->high_poly_collection, ob, source->cage, r_objects);
  }
}

void BKE_mesh_maps_source_collect_for_material(Main &bmain,
                                               const Material &ma,
                                               Set<const Object *> &r_objects)
{
  for (Object &ob : bmain.objects) {
    /* Only a mesh can own a mesh map; other types (camera, light, empty) have no material count
     * and #BKE_object_material_index_get dereferences it. */
    if (ob.type != OB_MESH || ob.data == nullptr) {
      continue;
    }
    if (BKE_object_material_index_get(&ob, &ma) < 0) {
      continue;
    }
    Vector<Object *> objects;
    Object *cage = nullptr;
    BKE_mesh_maps_source_resolve(ob, ma, objects, &cage);
    for (Object *source : objects) {
      r_objects.add(source);
    }
    if (cage != nullptr) {
      r_objects.add(cage);
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Content hash and bake staleness
 * \{ */

void BKE_mesh_maps_object_hash(const Object &ob_eval,
                               const Material &ma,
                               const int8_t type,
                               uint32_t r_hash[2])
{
  r_hash[0] = 0;
  r_hash[1] = 0;
  if (!mesh_map_type_valid(type)) {
    return;
  }
  const Mesh *mesh = mesh_map_object_mesh(ob_eval);
  if (mesh == nullptr) {
    return;
  }

  uint64_t h = mesh_map_hash_seed;
  h = mesh_map_hash_mix(h, uint8_t(type));

  /* Topology by content: counts, face sizes and the corner-to-vertex/edge maps. The edge arrays are
   * only folded in for the maps that read them. */
  h = mesh_map_hash_mix(h, uint64_t(uint32_t(mesh->verts_num)));
  h = mesh_map_hash_mix(h, uint64_t(uint32_t(mesh->edges_num)));
  h = mesh_map_hash_mix(h, uint64_t(uint32_t(mesh->faces_num)));
  h = mesh_map_hash_mix(h, uint64_t(uint32_t(mesh->corners_num)));
  h = mesh_map_hash_span(h, mesh->face_offsets());
  h = mesh_map_hash_span(h, mesh->corner_verts());
  if (ELEM(type, MA_MESH_MAP_EDGE, MA_MESH_MAP_CURVATURE)) {
    h = mesh_map_hash_span(h, mesh->corner_edges());
    h = mesh_map_hash_span(h, mesh->edges());
  }

  h = mesh_map_hash_span(h, mesh->vert_positions());

  /* The maps are sampled through the mesh's active UV map: this build has no material-named UV
   * layer, and the paint brushes read the same active map. A mesh with no usable UV map folds a
   * distinct marker instead of another layer's data, so no valid bake can ever match a missing
   * layer. */
  if (const char *uv_name = BKE_mesh_maps_uv_resolve(*mesh); uv_name != nullptr) {
    h = mesh_map_hash_string(h, StringRef(uv_name));
    h = mesh_map_hash_attribute(h, mesh->attributes(), uv_name);
  }
  else {
    h = mesh_map_hash_mix(h, 0x9E3779B97F4A7C15ull);
  }

  /* Material settings that shape this type's bake. Resolution and margin come from the slot's
   * effective values (a slot can override them per map) and are used by every merge;
   * samples/denoise belong to the ray-traced maps, ao_distance to AO and edge_radius to Edge. The
   * margin kind moves every merge, and the position and tangent encodings move their own types. */
  const uint32_t effective_resolution = uint32_t(BKE_mesh_maps_bake_slot_resolution(ma, type));
  const float effective_margin = BKE_mesh_maps_bake_slot_margin(ma, type);
  h = mesh_map_hash_mix(h, uint64_t(effective_resolution));
  h = mesh_map_hash_bytes(h, &effective_margin, sizeof(float));
  h = mesh_map_hash_mix(h, uint64_t(uint8_t(ma.mesh_map_settings.margin_type)));
  /* The dilation pushes every merge's result further into the background. */
  h = mesh_map_hash_mix(h, uint64_t(uint8_t(ma.mesh_map_settings.dilate_iterations)));
  if (ELEM(type, MA_MESH_MAP_AO, MA_MESH_MAP_CURVATURE, MA_MESH_MAP_EDGE)) {
    h = mesh_map_hash_mix(h, uint64_t(uint32_t(BKE_mesh_maps_bake_slot_samples(ma, type))));
    h = mesh_map_hash_mix(h, uint64_t(BKE_mesh_maps_bake_slot_denoise(ma, type) ? 1 : 0));
  }
  if (BKE_mesh_maps_pair_renders(ma, type)) {
    /* The engine rasterizes at the supersampled size before the commit boxes down. */
    h = mesh_map_hash_mix(h, uint64_t(uint8_t(ma.mesh_map_settings.supersampling)));
  }
  if (type == MA_MESH_MAP_AO) {
    h = mesh_map_hash_bytes(h, &ma.mesh_map_settings.ao_distance, sizeof(float));
    h = mesh_map_hash_mix(h, uint64_t(ma.mesh_map_settings.use_global_ao != 0 ? 1 : 0));
  }
  if (type == MA_MESH_MAP_EDGE) {
    h = mesh_map_hash_bytes(h, &ma.mesh_map_settings.edge_radius, sizeof(float));
  }
  if (type == MA_MESH_MAP_CURVATURE &&
      ma.mesh_map_settings.curvature_method == MA_MESH_MAP_CURVATURE_NORMAL_FILTER)
  {
    h = mesh_map_hash_mix(h, uint64_t(uint8_t(ma.mesh_map_settings.curvature_method)));
    h = mesh_map_hash_mix(h, uint64_t(uint8_t(ma.mesh_map_settings.curvature_radius)));
    h = mesh_map_hash_bytes(h, &ma.mesh_map_settings.curvature_scale, sizeof(float));
    h = mesh_map_hash_mix(h, uint64_t(ma.mesh_map_settings.use_directx_normal != 0 ? 1 : 0));
    /* The filter reads the object's baked Tangent Normal contribution: when the object's
     * normal state moves, the curvature built over it is stale too. */
    uint32_t normal_hash[2] = {0, 0};
    BKE_mesh_maps_object_hash(ob_eval, ma, MA_MESH_MAP_NORMAL_TANGENT, normal_hash);
    h = mesh_map_hash_bytes(h, normal_hash, sizeof(normal_hash));
    /* The bake states live on the original object, while the hash receives the evaluated one; the
     * find takes a mutable reference yet only prunes null-material entries and otherwise reads, so
     * the const-cast reflects the live state instead of a copy. */
    const Object *ob_orig = DEG_get_original(&ob_eval);
    ObjectMeshMapState *tangent_state = nullptr;
    if (ob_orig != nullptr) {
      /* The filter reads the type's primary (uid 0) Tangent Normal atlas, so its freshness is
       * gated by the primary's state; a copy's bake never makes the filter's source valid. */
      tangent_state = BKE_mesh_maps_object_state_find_uid(const_cast<Object &>(*ob_orig),
                                                          ma,
                                                          MA_MESH_MAP_NORMAL_TANGENT,
                                                          0);
    }
    /* The filter composes over the baked Tangent Normal atlas, so the curvature's freshness
     * inherits the source slot's: while the atlas is not the valid baked one, the curvature claim
     * differs from the claim after a tangent re-bake. A missing state and any non-valid status mix
     * the same "not valid" marker; only the valid baked slot mixes the "valid" one. */
    const bool tangent_valid = tangent_state != nullptr &&
                               tangent_state->status == OB_MESH_MAP_STATUS_VALID;
    h = mesh_map_hash_mix(h, uint64_t(tangent_valid ? 1 : 0));
  }
  if (ELEM(type, MA_MESH_MAP_THICKNESS, MA_MESH_MAP_BENT_NORMAL)) {
    h = mesh_map_hash_mix(h, uint64_t(uint8_t(ma.mesh_map_settings.cpu_rays)));
    /* Thickness reads world-space distances and Bent Normal stores world-space directions, so the
     * object transform is part of the claim in both: scaling changes the distance, and rotating
     * turns every bent direction. With no high-poly source this is the only transform in the
     * hash (the source block folds relative transforms only when a source exists). */
    h = mesh_map_hash_bytes(h, ob_eval.object_to_world().ptr(), sizeof(float4x4));
  }
  if (type == MA_MESH_MAP_POSITION) {
    /* The raw mode stores the world position as it lies, and the box/sphere modes fit the
     * *world-space* bounds of the object. Translation and a uniform scale may cancel in the
     * normalized result, but a rotation changes the world AABB, so the transform is part of the
     * claim in every mode. Hashing it always is safer than leaving a rotated object marked VALID
     * with pixels baked from the old orientation. */
    h = mesh_map_hash_mix(h, uint64_t(uint8_t(ma.mesh_map_settings.position_normalization)));
    h = mesh_map_hash_bytes(h, ob_eval.object_to_world().ptr(), sizeof(float4x4));
  }
  if (type == MA_MESH_MAP_NORMAL_TANGENT) {
    h = mesh_map_hash_mix(h, uint64_t(ma.mesh_map_settings.use_directx_normal != 0 ? 1 : 0));
  }

  if (ELEM(type,
           MA_MESH_MAP_NORMAL_WORLD,
           MA_MESH_MAP_NORMAL_OBJECT,
           MA_MESH_MAP_NORMAL_TANGENT))
  {
    /* What the evaluated normals depend on beyond positions and topology: the sharp flags and the
     * custom normals. Their presence is part of the hash, so turning split normals on or off moves
     * it. */
    h = mesh_map_hash_attribute(h, mesh->attributes(), "sharp_face");
    h = mesh_map_hash_attribute(h, mesh->attributes(), "sharp_edge");
    h = mesh_map_hash_attribute(h, mesh->attributes(), "custom_normal");
  }
  if (type == MA_MESH_MAP_NORMAL_WORLD) {
    /* World-space normals follow the object transform; object- and tangent-space and the rest do
     * not (the tangent frame comes from the mesh and the conversion undoes the transform). AO is a
     * per-object effect in v1 and ignores the transform too. */
    h = mesh_map_hash_bytes(h, ob_eval.object_to_world().ptr(), sizeof(float4x4));
  }
  if (type == MA_MESH_MAP_ID_MATERIAL) {
    h = mesh_map_hash_attribute(h, mesh->attributes(), "material_index");
  }
  if (type == MA_MESH_MAP_ID_OBJECT) {
    h = mesh_map_hash_string(h, StringRef(ob_eval.id.name + 2));
  }

  /* A high-poly source contributes its geometry, its transform relative to the low-poly, the cage
   * and its transform, and the auto-cage/ray controls. An object the viewport depsgraph does not
   * evaluate falls back to its original data mesh, exactly like the low-poly hash itself. */
  const Object *ob_orig = DEG_get_original(&ob_eval);
  const ObjectMeshMapSource *source =
      ob_orig != nullptr ? BKE_mesh_maps_source_find(*ob_orig, ma) : nullptr;
  if (source != nullptr) {
    Depsgraph *depsgraph = DEG_get_depsgraph_by_id(ob_eval.id);
    Vector<Object *> source_objects;
    Object *cage = nullptr;
    BKE_mesh_maps_source_resolve(*ob_orig, ma, source_objects, &cage);
    const float4x4 mat_low = ob_eval.object_to_world();
    for (Object *hp : source_objects) {
      const Object *hp_eval = (depsgraph != nullptr) ? DEG_get_evaluated(depsgraph, hp) : hp;
      const Mesh *hp_mesh = (hp_eval != nullptr) ? mesh_map_object_mesh(*hp_eval) : nullptr;
      if (hp_mesh == nullptr) {
        hp_mesh = id_cast<const Mesh *>(hp->data);
      }
      if (hp_mesh != nullptr) {
        h = mesh_map_hash_source_mesh(h, *hp_mesh, type);
      }
      const Object *hp_ref = (hp_eval != nullptr) ? hp_eval : hp;
      const float4x4 rel = math::invert(mat_low) * hp_ref->object_to_world();
      h = mesh_map_hash_bytes(h, rel.ptr(), sizeof(float4x4));
      if (ELEM(type, MA_MESH_MAP_ID_OBJECT, MA_MESH_MAP_ID_MATERIAL)) {
        h = mesh_map_hash_string(h, StringRef(hp->id.name + 2));
      }
    }
    if (cage != nullptr) {
      const Object *cage_eval = (depsgraph != nullptr) ? DEG_get_evaluated(depsgraph, cage) : cage;
      const Object *cage_ref = (cage_eval != nullptr) ? cage_eval : cage;
      const Mesh *cage_mesh = (cage_eval != nullptr) ? mesh_map_object_mesh(*cage_eval) : nullptr;
      if (cage_mesh == nullptr) {
        cage_mesh = id_cast<const Mesh *>(cage->data);
      }
      if (cage_mesh != nullptr) {
        h = mesh_map_hash_source_mesh(h, *cage_mesh, type);
      }
      const float4x4 rel = math::invert(mat_low) * cage_ref->object_to_world();
      h = mesh_map_hash_bytes(h, rel.ptr(), sizeof(float4x4));
    }
    h = mesh_map_hash_bytes(h, &source->cage_extrusion, sizeof(float));
    h = mesh_map_hash_bytes(h, &source->max_ray_distance, sizeof(float));
    h = mesh_map_hash_bytes(h, &source->max_rear_ray_distance, sizeof(float));
    h = mesh_map_hash_mix(h, uint64_t(source->use_ignore_backfacing != 0 ? 1 : 0));
    h = mesh_map_hash_bytes(h, &source->skew, sizeof(float));
  }

  r_hash[0] = uint32_t(h & 0xFFFFFFFFu);
  r_hash[1] = uint32_t(h >> 32);
}

const char *BKE_mesh_maps_uv_resolve(const Mesh &mesh)
{
  /* A mesh with no UV layer at all, and one whose layers exist but none is active, both leave the
   * bake with no texel source: one answer for both, so every caller refuses the same way. */
  if (mesh_map_uv_missing(mesh)) {
    return nullptr;
  }
  const StringRefNull name = mesh.active_uv_map_name();
  return name.is_empty() ? nullptr : name.c_str();
}

bool BKE_mesh_maps_object_uv_missing(const Object &ob_eval, const Material & /*ma*/)
{
  const Mesh *mesh = mesh_map_object_mesh(ob_eval);
  if (mesh == nullptr) {
    return false;
  }
  return BKE_mesh_maps_uv_resolve(*mesh) == nullptr;
}

bool BKE_mesh_maps_object_state_is_current(const ObjectMeshMapState &state,
                                           const Object &ob_eval,
                                           const Material &ma)
{
  uint32_t hash[2];
  BKE_mesh_maps_object_hash(ob_eval, ma, state.type, hash);
  return hash[0] == state.hash[0] && hash[1] == state.hash[1];
}

int8_t BKE_mesh_maps_object_state_refresh(ObjectMeshMapState &state,
                                          const Object &ob_eval,
                                          const Material &ma)
{
  if (!ELEM(state.status, OB_MESH_MAP_STATUS_VALID, OB_MESH_MAP_STATUS_STALE)) {
    return state.status;
  }
  const bool current = BKE_mesh_maps_object_state_is_current(state, ob_eval, ma);
  if (state.status == OB_MESH_MAP_STATUS_VALID && !current) {
    state.status = OB_MESH_MAP_STATUS_STALE;
    mesh_map_state_tag_changed(ob_eval);
  }
  else if (state.status == OB_MESH_MAP_STATUS_STALE && current) {
    state.status = OB_MESH_MAP_STATUS_VALID;
    mesh_map_state_tag_changed(ob_eval);
  }
  return state.status;
}

void BKE_mesh_maps_object_state_mark_baked(ObjectMeshMapState &state,
                                           const uint32_t hash[2],
                                           const int baked_time)
{
  state.hash[0] = hash[0];
  state.hash[1] = hash[1];
  state.baked_time = baked_time;
  state.status = OB_MESH_MAP_STATUS_VALID;
}

int BKE_mesh_maps_object_refresh_all(Object &ob, const Object &ob_eval)
{
  int changed = 0;
  for (ObjectMeshMapState &state : ob.mesh_map_states) {
    if (state.material == nullptr) {
      continue;
    }
    const int8_t before = state.status;
    BKE_mesh_maps_object_state_refresh(state, ob_eval, *state.material);
    if (state.status != before) {
      changed++;
    }
  }
  return changed;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Lifecycle
 * \{ */

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
  ma_dst.mesh_map_settings.margin_type = ma_src.mesh_map_settings.margin_type;
  ma_dst.mesh_map_settings.position_normalization = ma_src.mesh_map_settings.position_normalization;
  ma_dst.mesh_map_settings.use_directx_normal = ma_src.mesh_map_settings.use_directx_normal;
  ma_dst.mesh_map_settings.margin = ma_src.mesh_map_settings.margin;
  ma_dst.mesh_map_settings.ao_distance = ma_src.mesh_map_settings.ao_distance;
  ma_dst.mesh_map_settings.edge_radius = ma_src.mesh_map_settings.edge_radius;
  ma_dst.mesh_map_settings.curvature_scale = ma_src.mesh_map_settings.curvature_scale;
  ma_dst.mesh_map_settings.cpu_rays = ma_src.mesh_map_settings.cpu_rays;
  ma_dst.mesh_map_settings.use_global_ao = ma_src.mesh_map_settings.use_global_ao;
  ma_dst.mesh_map_settings.dilate_iterations = ma_src.mesh_map_settings.dilate_iterations;
  ma_dst.mesh_map_settings.curvature_method = ma_src.mesh_map_settings.curvature_method;
  ma_dst.mesh_map_settings.supersampling = ma_src.mesh_map_settings.supersampling;
  ma_dst.mesh_map_settings.curvature_radius = ma_src.mesh_map_settings.curvature_radius;
  ma_dst.mesh_map_slot_uid_next = ma_src.mesh_map_slot_uid_next;
  ma_dst.mesh_map_slots = {nullptr, nullptr};
  for (const MaterialMeshMapSlot &src : ma_src.mesh_map_slots) {
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
  for (const ObjectMeshMapState &src : ob_src.mesh_map_states) {
    ObjectMeshMapState *dst = MEM_dupalloc(&src);
    dst->next = nullptr;
    dst->prev = nullptr;
    BLI_addtail(&ob_dst.mesh_map_states, dst);
  }
}

void BKE_mesh_maps_object_sources_free(Object &ob)
{
  for (ObjectMeshMapSource *source = static_cast<ObjectMeshMapSource *>(ob.mesh_map_sources.first),
                                *next = nullptr;
       source != nullptr;
       source = next)
  {
    next = source->next;
    MEM_delete(source);
  }
  ob.mesh_map_sources = {nullptr, nullptr};
}

void BKE_mesh_maps_object_sources_copy(Object &ob_dst, const Object &ob_src)
{
  ob_dst.mesh_map_sources = {nullptr, nullptr};
  for (const ObjectMeshMapSource &src : ob_src.mesh_map_sources) {
    ObjectMeshMapSource *dst = MEM_dupalloc(&src);
    dst->next = nullptr;
    dst->prev = nullptr;
    BLI_addtail(&ob_dst.mesh_map_sources, dst);
  }
}

/** \} */

}  // namespace blender
