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

#include <cstddef>

#include "BLI_array.hh"
#include "BLI_cpp_type.hh"
#include "BLI_generic_span.hh"
#include "BLI_generic_virtual_array.hh"
#include "BLI_hash_mm2a.hh"
#include "BLI_listbase.h"
#include "BLI_math_matrix.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"
#include "MEM_guardedalloc.h"

#include "BKE_attribute.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_paint_layers.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "DNA_collection_types.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"

namespace blender {

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
  for (ObjectMeshMapSource &source :
       *reinterpret_cast<ListBaseT<ObjectMeshMapSource> *>(&ob.mesh_map_sources))
  {
    if (source.material == &ma) {
      return &source;
    }
  }
  return nullptr;
}

const ObjectMeshMapSource *BKE_mesh_maps_source_find(const Object &ob, const Material &ma)
{
  for (const ObjectMeshMapSource &source :
       *reinterpret_cast<const ListBaseT<ObjectMeshMapSource> *>(&ob.mesh_map_sources))
  {
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

namespace {

void mesh_map_source_add_collection(const Collection &collection,
                                    const Object &low,
                                    const Object *cage,
                                    Vector<Object *> &r_objects)
{
  for (const CollectionObject &link :
       *reinterpret_cast<const ListBaseT<CollectionObject> *>(&collection.gobject))
  {
    Object *ob = link.ob;
    if (ob == nullptr || ob == &low || ob == cage || ob->type != OB_MESH || ob->data == nullptr) {
      continue;
    }
    if (!r_objects.contains(ob)) {
      r_objects.append(ob);
    }
  }
  for (const CollectionChild &child :
       *reinterpret_cast<const ListBaseT<CollectionChild> *>(&collection.children))
  {
    if (child.collection != nullptr) {
      mesh_map_source_add_collection(*child.collection, low, cage, r_objects);
    }
  }
}

}  // namespace

void BKE_mesh_maps_source_resolve(const Object &ob,
                                  const Material &ma,
                                  Vector<Object *> &r_objects,
                                  Object **r_cage)
{
  r_objects.clear();
  if (r_cage != nullptr) {
    *r_cage = nullptr;
  }
  Object *ob_mut = const_cast<Object *>(&ob);
  const ObjectMeshMapSource *source = BKE_mesh_maps_source_find(*ob_mut, ma);
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

  /* The UV layer the maps are sampled through, chosen by the material's name when it has one and
   * otherwise the mesh's active UV map. The generated chain, painting and this hash all read the
   * same resolver. A name the mesh does not have folds a distinct marker instead of another layer's
   * data, so no valid bake can ever match a missing layer. */
  bool uv_missing = false;
  const char *uv_name = BKE_paint_layers_uv_map_resolve(*mesh, ma, &uv_missing);
  if (uv_missing) {
    h = mesh_map_hash_mix(h, 0x9E3779B97F4A7C15ull);
  }
  else if (uv_name != nullptr && uv_name[0] != '\0') {
    h = mesh_map_hash_string(h, uv_name);
    h = mesh_map_hash_attribute(h, mesh->attributes(), uv_name);
  }
  else {
    h = mesh_map_hash_mix(h, 0);
  }

  /* Material settings that shape this type's bake. Resolution and margin are used by every merge;
   * samples/denoise belong to the ray-traced maps, ao_distance to AO and edge_radius to Edge. */
  h = mesh_map_hash_mix(h, uint64_t(uint32_t(ma.mesh_map_settings.resolution)));
  h = mesh_map_hash_bytes(h, &ma.mesh_map_settings.margin, sizeof(float));
  if (ELEM(type, MA_MESH_MAP_AO, MA_MESH_MAP_CURVATURE, MA_MESH_MAP_EDGE)) {
    h = mesh_map_hash_mix(h, uint64_t(uint32_t(ma.mesh_map_settings.samples)));
    h = mesh_map_hash_mix(h, uint64_t(ma.mesh_map_settings.use_denoise != 0 ? 1 : 0));
  }
  if (type == MA_MESH_MAP_AO) {
    h = mesh_map_hash_bytes(h, &ma.mesh_map_settings.ao_distance, sizeof(float));
  }
  if (type == MA_MESH_MAP_EDGE) {
    h = mesh_map_hash_bytes(h, &ma.mesh_map_settings.edge_radius, sizeof(float));
  }

  if (ELEM(type, MA_MESH_MAP_NORMAL_WORLD, MA_MESH_MAP_NORMAL_OBJECT)) {
    /* What the evaluated normals depend on beyond positions and topology: the sharp flags and the
     * custom normals. Their presence is part of the hash, so turning split normals on or off moves
     * it. */
    h = mesh_map_hash_attribute(h, mesh->attributes(), "sharp_face");
    h = mesh_map_hash_attribute(h, mesh->attributes(), "sharp_edge");
    h = mesh_map_hash_attribute(h, mesh->attributes(), "custom_normal");
  }
  if (type == MA_MESH_MAP_NORMAL_WORLD) {
    /* World-space normals follow the object transform; object-space and the rest do not. AO is a
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
  }

  r_hash[0] = uint32_t(h & 0xFFFFFFFFu);
  r_hash[1] = uint32_t(h >> 32);
}

bool BKE_mesh_maps_object_uv_missing(const Object &ob_eval, const Material &ma)
{
  const Mesh *mesh = mesh_map_object_mesh(ob_eval);
  if (mesh == nullptr) {
    return false;
  }
  bool missing = false;
  BKE_paint_layers_uv_map_resolve(*mesh, ma, &missing);
  return missing;
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
  for (ObjectMeshMapState &state :
       *reinterpret_cast<ListBaseT<ObjectMeshMapState> *>(&ob.mesh_map_states))
  {
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
  for (const ObjectMeshMapSource &src :
       *reinterpret_cast<const ListBaseT<ObjectMeshMapSource> *>(&ob_src.mesh_map_sources))
  {
    ObjectMeshMapSource *dst = MEM_dupalloc(&src);
    dst->next = nullptr;
    dst->prev = nullptr;
    BLI_addtail(&ob_dst.mesh_map_sources, dst);
  }
}

/** \} */

}  // namespace blender
