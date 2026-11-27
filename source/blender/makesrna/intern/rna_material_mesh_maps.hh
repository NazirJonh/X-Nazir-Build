/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup RNA
 *
 * Declarations for `rna_material_mesh_maps.cc`, the mesh maps RNA of a Material split out of
 * `rna_material.cc`: the definition entry the material's own definition calls, and the runtime
 * callbacks the generated `rna_material_gen.cc` resolves by name (the generated file includes
 * `rna_material.cc`, which includes this header).
 */

#pragma once

#include "DNA_material_types.h"

#include "rna_internal.hh"

namespace blender {

/* Defined in `rna_material_mesh_maps.cc`; called by `RNA_def_material` (`rna_material.cc`). */
void rna_def_material_mesh_maps(BlenderRNA *brna, StructRNA *srna_material);

#ifdef RNA_RUNTIME

/* Runtime callbacks of the mesh map properties. They are referenced directly by the generated
 * `rna_material_gen.cc` and compiled into `bf_rna` from their own file, so they cannot be
 * static, unlike the callbacks that live in `rna_material.cc` itself. */

void rna_Material_mesh_map_slots_begin(CollectionPropertyIterator *iter, PointerRNA *ptr);
int rna_Material_mesh_map_slots_length(PointerRNA *ptr);
MaterialMeshMapSlot *rna_Material_mesh_map_slots_ensure(Material *ma, int type);
MaterialMeshMapSlot *rna_Material_mesh_map_slots_find(Material *ma, int type);
bool rna_Material_mesh_map_type_is_bakeable(Material *ma, int type);
void rna_Material_mesh_map_bake_target_objects_begin(CollectionPropertyIterator *iter,
                                                     PointerRNA *ptr);
PointerRNA rna_Material_mesh_map_bake_target_objects_get(CollectionPropertyIterator *iter);
int rna_MaterialMeshMapSlot_type_get(PointerRNA *ptr);
/** The atlas Image itself, resolving the slot's map type through the material's slot list. */
PointerRNA rna_MaterialMeshMapSlot_atlas_image_get(PointerRNA *ptr);
void rna_MaterialMeshMapSlot_image_set(PointerRNA *ptr, PointerRNA value, ReportList *reports);
PointerRNA rna_Material_mesh_map_settings_get(PointerRNA *ptr);

#endif

}  // namespace blender
