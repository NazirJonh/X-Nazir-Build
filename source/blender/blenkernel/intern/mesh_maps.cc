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
#include "BLI_string_ref.hh"
#include "MEM_guardedalloc.h"

#include "BKE_attribute.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_paint_layers.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

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

/** \} */

}  // namespace blender
