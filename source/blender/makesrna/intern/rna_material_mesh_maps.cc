/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup RNA
 *
 * The mesh maps RNA of a Material, split out of `rna_material.cc`: the shared atlas slots and the
 * shared bake and viewport settings.
 *
 * Unlike `rna_material_projection.hh`, which is included by `rna_material.cc`, this file is
 * compiled on its own, twice: without #RNA_RUNTIME into the `makesrna` generator (the definition
 * pass in the `#else` below) and with #RNA_RUNTIME into `bf_rna` (the callbacks in the `#ifdef`
 * below). makesrna emits the callbacks of a property into the generated file of the file that
 * defines it, so the generated `rna_material_gen.cc` -- which includes `rna_material.cc`, which
 * includes `rna_material_mesh_maps.hh` -- references the callbacks below by name and resolves
 * them at link time, which is why they cannot be static. The same holds for
 * `rna_enum_material_mesh_map_type_items`, which `rna_object.cc` and the generated property
 * tables reference through #RNA_enum_types.hh.
 */

#include <algorithm>
#include <cfloat>

#include "DNA_material_types.h"

#include "BKE_mesh_maps_types.hh"

#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "rna_internal.hh"
#include "rna_material_mesh_maps.hh"

#include "WM_types.hh"

namespace blender {

/* Values match #eMaterialMeshMapType. One descriptor per type lives in #MESH_MAP_BAKE_TYPE_INFOS
 * (BKE_mesh_maps_types.hh), and every item below is built from it at compile time: the
 * identifiers, names and descriptions live only in the table. The order is pinned by this list,
 * because the Python enum's order is stable API and differs from the table's type-value order.
 * Defined here, outside the makesrna-only section, because rna_object.cc reuses the table at
 * runtime (see #DEF_ENUM): the Object mesh map states key on the same map types. */
constexpr EnumPropertyItem rna_material_mesh_map_type_item(const int8_t type)
{
  const blender::MeshMapTypeInfo *info = blender::BKE_mesh_maps_type_info_get(type);
  return {info->id, info->idname, 0, info->ui_name, info->ui_description};
}

const EnumPropertyItem rna_enum_material_mesh_map_type_items[] = {
    rna_material_mesh_map_type_item(MA_MESH_MAP_AO),
    rna_material_mesh_map_type_item(MA_MESH_MAP_CURVATURE),
    rna_material_mesh_map_type_item(MA_MESH_MAP_NORMAL_WORLD),
    rna_material_mesh_map_type_item(MA_MESH_MAP_NORMAL_OBJECT),
    rna_material_mesh_map_type_item(MA_MESH_MAP_NORMAL_TANGENT),
    rna_material_mesh_map_type_item(MA_MESH_MAP_ID_OBJECT),
    rna_material_mesh_map_type_item(MA_MESH_MAP_ID_MATERIAL),
    rna_material_mesh_map_type_item(MA_MESH_MAP_EDGE),
    rna_material_mesh_map_type_item(MA_MESH_MAP_POSITION),
    {0, nullptr, 0, nullptr, nullptr},
};

}  // namespace blender

#ifdef RNA_RUNTIME

#  include "DNA_image_types.h"
#  include "DNA_object_types.h"

#  include "BLI_listbase.h"
#  include "BLI_vector.hh"

#  include "BKE_global.hh"
#  include "BKE_main.hh"
#  include "BKE_mesh_maps.hh"
#  include "BKE_mesh_maps_bake.hh"

#  include "MEM_guardedalloc.h"

#  include "RNA_access.hh"
#  include "RNA_prototypes.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Mesh map RNA helpers
 * \{ */

void rna_Material_mesh_map_slots_begin(CollectionPropertyIterator *iter, PointerRNA *ptr)
{
  Material *ma = id_cast<Material *>(ptr->owner_id);
  rna_iterator_listbase_begin(iter, ptr, &ma->mesh_map_slots, nullptr);
}

int rna_Material_mesh_map_slots_length(PointerRNA *ptr)
{
  Material *ma = id_cast<Material *>(ptr->owner_id);
  return BLI_listbase_count(&ma->mesh_map_slots);
}

MaterialMeshMapSlot *rna_Material_mesh_map_slots_ensure(Material *ma, int type)
{
  return BKE_mesh_maps_slot_ensure(*ma, int8_t(type));
}

MaterialMeshMapSlot *rna_Material_mesh_map_slots_find(Material *ma, int type)
{
  return BKE_mesh_maps_slot_find(*ma, int8_t(type));
}

bool rna_Material_mesh_map_type_is_bakeable(Material * /*ma*/, int type)
{
  return BKE_mesh_maps_bake_type_is_supported(int8_t(type));
}

void rna_Material_mesh_map_bake_target_objects_begin(CollectionPropertyIterator *iter,
                                                     PointerRNA *ptr)
{
  Material *ma = id_cast<Material *>(ptr->owner_id);
  /* The objects are resolved from the whole file, which a property setter is not handed: fall back
   * to #G_MAIN, guarded the same way other introspection callbacks guard it. */
  if (ma == nullptr || G_MAIN == nullptr) {
    iter->valid = false;
    return;
  }
  Vector<Object *> objects;
  BKE_mesh_maps_bake_target_objects(*G_MAIN, *ma, objects);
  /* The iterator owns the array and frees it at the end, so the objects outlive this call. */
  Object **data = MEM_new_array<Object *>(size_t(std::max<int64_t>(objects.size(), 1)), __func__);
  for (const int64_t i : objects.index_range()) {
    data[i] = objects[i];
  }
  rna_iterator_array_begin(iter, ptr, data, sizeof(Object *), objects.size(), true, nullptr);
}

PointerRNA rna_Material_mesh_map_bake_target_objects_get(CollectionPropertyIterator *iter)
{
  /* The items are IDs, so each pointer is self-owned rather than a child of the material. */
  Object *ob = static_cast<Object *>(rna_iterator_array_dereference_get(iter));
  return RNA_id_pointer_create(&ob->id);
}

int rna_MaterialMeshMapSlot_type_get(PointerRNA *ptr)
{
  return static_cast<const MaterialMeshMapSlot *>(ptr->data)->type;
}

/** The atlas Image itself, resolving the slot's map type through the material's slot list. */
PointerRNA rna_MaterialMeshMapSlot_atlas_image_get(PointerRNA *ptr)
{
  const MaterialMeshMapSlot *slot = static_cast<const MaterialMeshMapSlot *>(ptr->data);
  Image *image = (slot != nullptr) ? slot->image : nullptr;
  if (image == nullptr) {
    return PointerRNA_NULL;
  }
  return RNA_id_pointer_create(&image->id);
}

void rna_MaterialMeshMapSlot_image_set(PointerRNA *ptr, PointerRNA value, ReportList *)
{
  MaterialMeshMapSlot *slot = static_cast<MaterialMeshMapSlot *>(ptr->data);
  Material *ma = id_cast<Material *>(ptr->owner_id);
  BKE_mesh_maps_slot_image_set(*ma, slot->type, static_cast<Image *>(value.data));
}

PointerRNA rna_Material_mesh_map_settings_get(PointerRNA *ptr)
{
  Material *ma = id_cast<Material *>(ptr->owner_id);
  return RNA_pointer_create_with_parent(
      *ptr, RNA_MaterialMeshMapSettings, &ma->mesh_map_settings);
}

/** \} */

}  // namespace blender

#else

namespace blender {

/* Values match #eMaterialMeshMapCurvatureMethod. */
static const EnumPropertyItem rna_material_mesh_map_curvature_method_items[] = {
    {MA_MESH_MAP_CURVATURE_POINTINESS,
     "POINTINESS",
     0,
     "Pointiness",
     "Cycles' Pointiness attribute; fast, but it follows the low-poly's density"},
    {MA_MESH_MAP_CURVATURE_NORMAL_FILTER,
     "NORMAL_FILTER",
     0,
     "Normal Filter",
     "The divergence of the baked Tangent Normal atlas; density independent, and it needs that "
     "map baked first"},
    {0, nullptr, 0, nullptr, nullptr},
};

/* Values match #eMaterialMeshMapPositionNormalization. */
static const EnumPropertyItem rna_material_mesh_map_position_normalization_items[] = {
    {MA_MESH_MAP_POSITION_RAW,
     "RAW",
     0,
     "Raw",
     "Keep the raw world-space position; also the way to share one range across several objects"},
    {MA_MESH_MAP_POSITION_BOX,
     "BOX",
     0,
     "Bounding Box",
     "Remap the object's world bounding box to 0..1 per axis; each object is fitted separately, "
     "so objects sharing the material's atlas get different ranges"},
    {MA_MESH_MAP_POSITION_SPHERE,
     "SPHERE",
     0,
     "Bounding Sphere",
     "Remap the object's world bounding sphere to 0..1; each object is fitted separately, so "
     "objects sharing the material's atlas get different ranges"},
    {0, nullptr, 0, nullptr, nullptr},
};

/* The mesh map settings and their slot overrides all end the same way: a UI text and the common
 * material update, because any of them changes what the next bake renders or how the atlas
 * merges. One helper keeps the many definitions from drifting apart. */
static void rna_def_mesh_map_ui_update(PropertyRNA *prop,
                                       const char *ui_name,
                                       const char *ui_description)
{
  RNA_def_property_ui_text(prop, ui_name, ui_description);
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");
}

void rna_def_material_mesh_maps(BlenderRNA *brna, StructRNA *srna)
{
  StructRNA *coll_srna;
  StructRNA *slot_srna;
  PropertyRNA *prop;
  FunctionRNA *func;
  PropertyRNA *parm;

  prop = RNA_def_property(srna, "mesh_map_slots", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_struct_type(prop, "MaterialMeshMapSlot");
  RNA_def_property_collection_funcs(prop,
                                    "rna_Material_mesh_map_slots_begin",
                                    "rna_iterator_listbase_next",
                                    "rna_iterator_listbase_end",
                                    "rna_iterator_listbase_get",
                                    "rna_Material_mesh_map_slots_length",
                                    nullptr,
                                    nullptr,
                                    nullptr);
  RNA_def_property_ui_text(
      prop, "Mesh Map Slots", "The material's shared mesh map atlases, one per map type");

  RNA_def_property_srna(prop, "MaterialMeshMapSlots");
  coll_srna = RNA_def_struct(brna, "MaterialMeshMapSlots", nullptr);
  RNA_def_struct_sdna(coll_srna, "Material");
  RNA_def_struct_ui_text(coll_srna, "Mesh Map Slots", "Collection of mesh map atlas slots");

  func = RNA_def_function(coll_srna, "ensure", "rna_Material_mesh_map_slots_ensure");
  RNA_def_function_ui_description(func, "Get the slot for a map type, creating it if absent");
  parm = RNA_def_enum(func,
                      "type",
                      rna_enum_material_mesh_map_type_items,
                      MA_MESH_MAP_AO,
                      "Type",
                      "The mesh map type");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_pointer(func, "slot", "MaterialMeshMapSlot", "", "The mesh map slot");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(coll_srna, "find", "rna_Material_mesh_map_slots_find");
  RNA_def_function_ui_description(func, "Get the slot for a map type, or none");
  parm = RNA_def_enum(func,
                      "type",
                      rna_enum_material_mesh_map_type_items,
                      MA_MESH_MAP_AO,
                      "Type",
                      "The mesh map type");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_pointer(func, "slot", "MaterialMeshMapSlot", "", "The mesh map slot, or none");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(
      srna, "mesh_map_type_is_bakeable", "rna_Material_mesh_map_type_is_bakeable");
  RNA_def_function_ui_description(
      func, "Whether a bake can produce this map type in this build");
  parm = RNA_def_enum(func,
                      "type",
                      rna_enum_material_mesh_map_type_items,
                      MA_MESH_MAP_AO,
                      "Type",
                      "The mesh map type");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_boolean(func, "result", false, "", "Whether the map type can be baked");
  RNA_def_function_return(func, parm);

  prop = RNA_def_property(srna, "mesh_map_bake_target_objects", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_struct_type(prop, "Object");
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_collection_funcs(prop,
                                    "rna_Material_mesh_map_bake_target_objects_begin",
                                    "rna_iterator_array_next",
                                    "rna_iterator_array_end",
                                    "rna_Material_mesh_map_bake_target_objects_get",
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    nullptr);
  RNA_def_property_ui_text(
      prop,
      "Bake Target Objects",
      "The objects a 'Bake All Objects' run over this material covers, sources excluded");

  /* slot */
  /* `srna` stays the Material: the settings below belong to it, not to the slot. */
  slot_srna = RNA_def_struct(brna, "MaterialMeshMapSlot", nullptr);
  RNA_def_struct_sdna(slot_srna, "MaterialMeshMapSlot");
  RNA_def_struct_ui_text(slot_srna, "Mesh Map Slot", "One shared mesh map atlas");

  prop = RNA_def_property(slot_srna, "type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_material_mesh_map_type_items);
  RNA_def_property_enum_funcs(prop, "rna_MaterialMeshMapSlot_type_get", nullptr, nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Type", "The mesh map this slot holds");

  prop = RNA_def_property(slot_srna, "image", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "Image");
  RNA_def_property_pointer_sdna(prop, nullptr, "image");
  RNA_def_property_pointer_funcs(
      prop, nullptr, "rna_MaterialMeshMapSlot_image_set", nullptr, nullptr);
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Image", "The shared atlas for this map type, or none");

  /* Read-only alias of `image`, named for the UI a mesh map mask reads: same atlas, but drawn as
   * a preview rather than a Drop image picker (a mesh map mask never assigns a map of its own). */
  prop = RNA_def_property(slot_srna, "atlas_image", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "Image");
  RNA_def_property_pointer_funcs(
      prop, "rna_MaterialMeshMapSlot_atlas_image_get", nullptr, nullptr, nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Atlas Image", "The shared atlas this slot holds, or none");

  /* settings */
  prop = RNA_def_property(srna, "mesh_map_settings", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "MaterialMeshMapSettings");
  RNA_def_property_pointer_funcs(
      prop, "rna_Material_mesh_map_settings_get", nullptr, nullptr, nullptr);
  RNA_def_property_ui_text(
      prop, "Mesh Map Settings", "Baking and viewport settings shared by the mesh maps");

  srna = RNA_def_struct(brna, "MaterialMeshMapSettings", nullptr);
  RNA_def_struct_sdna(srna, "MaterialMeshMapSettings");
  RNA_def_struct_ui_text(srna, "Mesh Map Settings", "Baking and viewport settings of mesh maps");

  prop = RNA_def_property(srna, "resolution", PROP_INT, PROP_NONE);
  RNA_def_property_int_sdna(prop, nullptr, "resolution");
  RNA_def_property_range(prop, MESH_MAP_BAKE_RESOLUTION_MIN, MESH_MAP_BAKE_RESOLUTION_MAX);
  rna_def_mesh_map_ui_update(prop, "Resolution", "Square side the atlas is allocated at");

  prop = RNA_def_property(srna, "samples", PROP_INT, PROP_NONE);
  RNA_def_property_int_sdna(prop, nullptr, "samples");
  RNA_def_property_range(prop, MESH_MAP_BAKE_SAMPLES_MIN, MESH_MAP_BAKE_SAMPLES_MAX);
  rna_def_mesh_map_ui_update(prop, "Samples", "Cycles samples the mesh map bake renders with");

  prop = RNA_def_property(srna, "use_denoise", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "use_denoise", 1);
  rna_def_mesh_map_ui_update(prop, "Denoise", "Ask Cycles to denoise the mesh map bake");

  prop = RNA_def_property(srna, "position_normalization", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "position_normalization");
  RNA_def_property_enum_items(prop, rna_material_mesh_map_position_normalization_items);
  rna_def_mesh_map_ui_update(prop,
                             "Position Normalization",
                             "What the Position map stores: the raw world position or the "
                             "object's bounds remapped to 0..1");

  prop = RNA_def_property(srna, "use_directx_normal", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "use_directx_normal", 1);
  rna_def_mesh_map_ui_update(
      prop,
      "DirectX Normal",
      "Flip the tangent-space normal's green channel (the DirectX convention); leave off for the "
      "OpenGL convention the Normal Map node reads");

  prop = RNA_def_property(srna, "margin", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, nullptr, "margin");
  RNA_def_property_range(prop, 0.0f, MESH_MAP_BAKE_MARGIN_MAX);
  rna_def_mesh_map_ui_update(prop, "Margin", "UV margin in pixels applied while merging a bake");

  prop = RNA_def_property(srna, "ao_distance", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, nullptr, "ao_distance");
  RNA_def_property_range(prop, 0.0f, FLT_MAX);
  rna_def_mesh_map_ui_update(
      prop, "AO Distance", "Ambient occlusion ray distance; zero is automatic");

  prop = RNA_def_property(srna, "edge_radius", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, nullptr, "edge_radius");
  RNA_def_property_range(prop, 0.0f, FLT_MAX);
  rna_def_mesh_map_ui_update(prop, "Edge Radius", "Edge/bevel ray radius in object-space units");

  prop = RNA_def_property(srna, "curvature_method", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "curvature_method");
  RNA_def_property_enum_items(prop, rna_material_mesh_map_curvature_method_items);
  rna_def_mesh_map_ui_update(prop,
                             "Curvature Method",
                             "Where the Curvature map's values come from");

  prop = RNA_def_property(srna, "curvature_radius", PROP_INT, PROP_NONE);
  RNA_def_property_int_sdna(prop, nullptr, "curvature_radius");
  RNA_def_property_range(prop,
                         MESH_MAP_BAKE_CURVATURE_RADIUS_MIN,
                         MESH_MAP_BAKE_CURVATURE_RADIUS_MAX);
  rna_def_mesh_map_ui_update(
      prop,
      "Curvature Radius",
      "Texel radius the Normal Filter samples along U and V; wider radii catch smoother bends");

  prop = RNA_def_property(srna, "curvature_scale", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, nullptr, "curvature_scale");
  RNA_def_property_range(prop, 0.0f, FLT_MAX);
  rna_def_mesh_map_ui_update(
      prop,
      "Curvature Scale",
      "Multiplier of the Normal Filter's divergence before it maps into the map's range");
}

}  // namespace blender

#endif
