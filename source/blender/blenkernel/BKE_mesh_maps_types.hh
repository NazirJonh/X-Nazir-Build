/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * The per-type descriptor table of the Mesh Maps bake (#MESH_MAP_BAKE_TYPE_INFOS) and the bake
 * limits the RNA property ranges read, split out of #BKE_mesh_maps_bake.hh.
 *
 * The header is deliberately light: `rna_material_mesh_maps.cc` is compiled twice, once without
 * #RNA_RUNTIME into the standalone `makesrna` generator, and that pass must not pull the whole
 * bake contract (`BLI_function_ref.hh` and friends) for a table of strings and flags. Only the
 * DNA definitions the table names itself are included; note that #eBakeSpace, the space the
 * normal maps claim, lives in `DNA_scene_types.h` rather than in the scene enums, which is why
 * the full scene types header is here. Everything that acts on the table stays in
 * #BKE_mesh_maps_bake.hh, which includes this one; include this header when only the registry is
 * needed.
 */

#include <cstddef>
#include <cstdint>

#include "DNA_material_types.h"
#include "DNA_scene_enums.h"
#include "DNA_scene_types.h"

namespace blender {

struct bNode;
struct bNodeSocket;
struct bNodeTree;

/* -------------------------------------------------------------------- */
/** \name Bake limits
 *
 * The bounds the bake settings clamp to, named here so the C++ clamps and the RNA ranges read
 * them from one place and cannot drift apart. In v1 only the Edge bevel samples, the Curvature
 * Normal Filter radius and the CPU ray count are clamped on the C++ side; the resolution, samples
 * and margin bounds below are only read by the RNA ranges until a C++ clamp needs them.
 * \{ */

/** Ray directions per texel the CPU Thickness and Bent Normal fills cast. */
constexpr int MESH_MAP_BAKE_CPU_RAYS_MIN = 1;
constexpr int MESH_MAP_BAKE_CPU_RAYS_MAX = 64;

/** The texel radius the Curvature Normal Filter samples along U and V. */
constexpr int MESH_MAP_BAKE_CURVATURE_RADIUS_MIN = 1;
constexpr int MESH_MAP_BAKE_CURVATURE_RADIUS_MAX = 64;

/** The samples the Edge map's bevel node clamps to (the geometry node's own sample bound). */
constexpr int MESH_MAP_BAKE_EDGE_BEVEL_SAMPLES_MIN = 2;
constexpr int MESH_MAP_BAKE_EDGE_BEVEL_SAMPLES_MAX = 128;

/** The square side the atlas is allocated at: below 16 a degenerate atlas is unreadable, and a
 * 16384 float RGBA buffer already costs 4 GiB, past which the allocation is refused. */
constexpr int MESH_MAP_BAKE_RESOLUTION_MIN = 16;
constexpr int MESH_MAP_BAKE_RESOLUTION_MAX = 16384;

/** The Cycles samples a mesh map bake may render with: one sample at least, 4096 at most. */
constexpr int MESH_MAP_BAKE_SAMPLES_MIN = 1;
constexpr int MESH_MAP_BAKE_SAMPLES_MAX = 4096;

/** The UV margin in pixels a merge may dilate an island by before it costs more than it hides. */
constexpr float MESH_MAP_BAKE_MARGIN_MAX = 256.0f;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Per-type descriptor table
 *
 * Everything that branches on a #eMaterialMeshMapType reads its answer from
 * #MESH_MAP_BAKE_TYPE_INFOS instead of keeping a switch of its own: which half fills the atlas
 * (the engine through the row's pass, the CPU through its fill entry points), what pass and space
 * the engine bakes, and the identifier and texts the RNA enum and the operators show. The
 * per-type CPU fill entry points themselves stay at their call sites until they can share one
 * signature (see #BKE_mesh_maps_bake_fill_id_pixels, #BKE_mesh_maps_bake_fill_thickness and
 * #BKE_mesh_maps_bake_fill_bent_normals); the flags describe them until then.
 * \{ */

/** What one map type needs from the bake; bitwise-or of these is #MeshMapTypeInfo::flags. */
enum MeshMapTypeInfoFlag : uint32_t {
  /** Rendered through the engine: the type turns into the row's render pass (AO, Curvature, Edge,
   * Position and the three normals). */
  MESH_MAP_TYPE_RENDERED = (1u << 0),
  /** A scalar atlas: its R carries the whole value, and the commit spreads it over G and B. */
  MESH_MAP_TYPE_SCALAR = (1u << 1),
  /** Filled on the CPU, without an engine (the ID maps). */
  MESH_MAP_TYPE_CPU_FILL = (1u << 2),
  /** The CPU fill casts rays through other surfaces (Thickness, Bent Normal). The ID maps are not
   * flagged: their base fill reads the low-poly alone, and casting onto a high-poly source is a
   * caller choice. */
  MESH_MAP_TYPE_NEEDS_SOURCES = (1u << 3),
  /** The CPU fill composes other atlases of the same material. Curvature's Normal Filter also
   * reads one, but only under a settings choice, so it stays a property of the settings, not of
   * the type. Packing, the future composing fill, would take this flag. */
  MESH_MAP_TYPE_NEEDS_OTHER_ATLAS = (1u << 4),
};

/**
 * Builds the per-type value source of an emission graph inside \a tree: the value nodes and the
 * socket they emit, which the common tail of #BKE_mesh_maps_bake_build_emit_tree feeds into an
 * #SH_NODE_EMISSION linked to the material output. Returns false when a node could not be added.
 *
 * \param ao_distance: the already-resolved AO ray distance; ignored unless the type is AO.
 * \param samples: the already-resolved bake sample count the AO and Edge nodes ray with.
 */
using MeshMapBakeEmitSourceBuilder = bool (*)(bNodeTree &tree,
                                              const MaterialMeshMapSettings &settings,
                                              float ao_distance,
                                              int samples,
                                              bNodeSocket **r_source,
                                              bNode **r_source_node);

/* The per-type builders, defined in `mesh_maps_bake.cc`; only the descriptor table below holds
 * their addresses, so they are not part of the public API beyond these declarations. Null in the
 * table for the types that have no emission graph (the normals bake through the native pass, the
 * CPU maps need no engine). */
bool mesh_map_bake_emit_source_ao(bNodeTree &tree,
                                  const MaterialMeshMapSettings &settings,
                                  float ao_distance,
                                  int samples,
                                  bNodeSocket **r_source,
                                  bNode **r_source_node);
bool mesh_map_bake_emit_source_position(bNodeTree &tree,
                                        const MaterialMeshMapSettings &settings,
                                        float ao_distance,
                                        int samples,
                                        bNodeSocket **r_source,
                                        bNode **r_source_node);
bool mesh_map_bake_emit_source_curvature(bNodeTree &tree,
                                         const MaterialMeshMapSettings &settings,
                                         float ao_distance,
                                         int samples,
                                         bNodeSocket **r_source,
                                         bNode **r_source_node);
bool mesh_map_bake_emit_source_edge(bNodeTree &tree,
                                    const MaterialMeshMapSettings &settings,
                                    float ao_distance,
                                    int samples,
                                    bNodeSocket **r_source,
                                    bNode **r_source_node);

/** Everything the bake knows about one map type; one row of #MESH_MAP_BAKE_TYPE_INFOS. */
struct MeshMapTypeInfo {
  /** The #eMaterialMeshMapType the row describes; the table is indexed by it. */
  int8_t id;
  /** The stable enum identifier ("AO", "CURVATURE", ...): the Python enum and the operators key on
   * it, so it never changes. */
  const char *idname;
  /** The UI label the RNA enum, the operators and the reports show. */
  const char *ui_name;
  /** The UI description of the map. */
  const char *ui_description;
  /** Bitwise-or of #MeshMapTypeInfoFlag. */
  uint32_t flags;
  /** The pass the engine bakes: #SCE_PASS_EMIT for the emission-graph maps, #SCE_PASS_NORMAL for
   * the native normal pass, `eScenePassType(0)` for the CPU fills. */
  eScenePassType pass;
  /** The space the normal maps claim; only read when \c pass is #SCE_PASS_NORMAL. */
  eBakeSpace space;
  /** The emission graph's per-type value source, or null for a type without one. */
  MeshMapBakeEmitSourceBuilder emit_tree_builder;
};

/**
 * One descriptor per map type, indexed by the type value, so adding an #eMaterialMeshMapType
 * without a row fails to compile instead of silently baking nothing.
 */
inline constexpr MeshMapTypeInfo MESH_MAP_BAKE_TYPE_INFOS[MA_MESH_MAP_TYPE_NUM] = {
    /* MA_MESH_MAP_AO. */
    {MA_MESH_MAP_AO,
     "AO",
     "Ambient Occlusion",
     "Ambient occlusion map",
     MESH_MAP_TYPE_RENDERED | MESH_MAP_TYPE_SCALAR,
     SCE_PASS_EMIT,
     eBakeSpace(0),
     mesh_map_bake_emit_source_ao},
    /* MA_MESH_MAP_CURVATURE. */
    {MA_MESH_MAP_CURVATURE,
     "CURVATURE",
     "Curvature",
     "Curvature map",
     MESH_MAP_TYPE_RENDERED | MESH_MAP_TYPE_SCALAR,
     SCE_PASS_EMIT,
     eBakeSpace(0),
     mesh_map_bake_emit_source_curvature},
    /* MA_MESH_MAP_NORMAL_WORLD. */
    {MA_MESH_MAP_NORMAL_WORLD,
     "NORMAL_WORLD",
     "Normal (World)",
     "World-space normal map",
     MESH_MAP_TYPE_RENDERED,
     SCE_PASS_NORMAL,
     R_BAKE_SPACE_WORLD,
     nullptr},
    /* MA_MESH_MAP_NORMAL_OBJECT. */
    {MA_MESH_MAP_NORMAL_OBJECT,
     "NORMAL_OBJECT",
     "Normal (Object)",
     "Object-space normal map",
     MESH_MAP_TYPE_RENDERED,
     SCE_PASS_NORMAL,
     R_BAKE_SPACE_OBJECT,
     nullptr},
    /* MA_MESH_MAP_ID_OBJECT. */
    {MA_MESH_MAP_ID_OBJECT,
     "ID_OBJECT",
     "Object ID",
     "Object index map",
     MESH_MAP_TYPE_CPU_FILL,
     eScenePassType(0),
     eBakeSpace(0),
     nullptr},
    /* MA_MESH_MAP_ID_MATERIAL. */
    {MA_MESH_MAP_ID_MATERIAL,
     "ID_MATERIAL",
     "Material ID",
     "Material index map",
     MESH_MAP_TYPE_CPU_FILL,
     eScenePassType(0),
     eBakeSpace(0),
     nullptr},
    /* MA_MESH_MAP_EDGE. */
    {MA_MESH_MAP_EDGE,
     "EDGE",
     "Edge",
     "Edge/bevel map",
     MESH_MAP_TYPE_RENDERED | MESH_MAP_TYPE_SCALAR,
     SCE_PASS_EMIT,
     eBakeSpace(0),
     mesh_map_bake_emit_source_edge},
    /* MA_MESH_MAP_THICKNESS. */
    {MA_MESH_MAP_THICKNESS,
     "THICKNESS",
     "Thickness",
     "Averaged interior distance map baked with CPU rays",
     MESH_MAP_TYPE_SCALAR | MESH_MAP_TYPE_CPU_FILL | MESH_MAP_TYPE_NEEDS_SOURCES,
     eScenePassType(0),
     eBakeSpace(0),
     nullptr},
    /* MA_MESH_MAP_POSITION. */
    {MA_MESH_MAP_POSITION,
     "POSITION",
     "Position",
     "World-space position map",
     MESH_MAP_TYPE_RENDERED,
     SCE_PASS_EMIT,
     eBakeSpace(0),
     mesh_map_bake_emit_source_position},
    /* MA_MESH_MAP_NORMAL_TANGENT. */
    {MA_MESH_MAP_NORMAL_TANGENT,
     "NORMAL_TANGENT",
     "Normal (Tangent)",
     "Tangent-space normal map of the active UV map",
     MESH_MAP_TYPE_RENDERED,
     SCE_PASS_NORMAL,
     R_BAKE_SPACE_TANGENT,
     nullptr},
    /* MA_MESH_MAP_BENT_NORMAL. */
    {MA_MESH_MAP_BENT_NORMAL,
     "BENT_NORMAL",
     "Bent Normal",
     "Average unoccluded direction map, world space",
     MESH_MAP_TYPE_CPU_FILL | MESH_MAP_TYPE_NEEDS_SOURCES,
     eScenePassType(0),
     eBakeSpace(0),
     nullptr},
};

/**
 * The descriptor of \a type, or null when \a type is not a map type the table describes (a file
 * saved by a newer Blender, say). Everything that branches on a type starts here.
 */
inline constexpr const MeshMapTypeInfo *BKE_mesh_maps_type_info_get(const int8_t type)
{
  if (type < 0 || type >= MA_MESH_MAP_TYPE_NUM) {
    return nullptr;
  }
  return &MESH_MAP_BAKE_TYPE_INFOS[type];
}

/**
 * Bit \a type set in a type mask for every map the bake can produce, computed from the descriptor
 * table so a type is only ever added together with its row.
 */
inline constexpr uint32_t MESH_MAP_BAKE_TYPE_MASK_SUPPORTED = []() {
  uint32_t mask = 0;
  for (const MeshMapTypeInfo &info : MESH_MAP_BAKE_TYPE_INFOS) {
    if (info.flags != 0u) {
      mask |= 1u << uint32_t(info.id);
    }
  }
  return mask;
}();

/**
 * Every map the bake can produce, render and CPU alike: operators mask against this.
 * #MESH_MAP_BAKE_TYPE_MASK_CPU stays for the tests and callers that need the subset that runs
 * without an engine.
 */
inline constexpr uint32_t MESH_MAP_BAKE_TYPE_MASK_ALL = MESH_MAP_BAKE_TYPE_MASK_SUPPORTED;

/**
 * The two CPU ID maps only: the engine-free subset scripts and tests used before the ray-marched
 * CPU maps existed. #MA_MESH_MAP_THICKNESS and #MA_MESH_MAP_BENT_NORMAL also run without an engine
 * now, but this legacy mask keeps naming just the ID pair; callers that need every CPU fill read
 * the type table instead.
 */
inline constexpr uint32_t MESH_MAP_BAKE_TYPE_MASK_CPU = (1u << MA_MESH_MAP_ID_OBJECT) |
                                                        (1u << MA_MESH_MAP_ID_MATERIAL);

/** \} */

}  // namespace blender
