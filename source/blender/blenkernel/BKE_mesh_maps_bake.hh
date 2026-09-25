/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * The render-free core of the Mesh Maps bake: how a #eMaterialMeshMapType turns into an atlas, and
 * the pure pixel/state work around the actual engine call.
 *
 * `BKE_mesh_maps.hh` is the data model (slots, states, content hash); this file is the bake side of
 * it. Everything here is deliberately free of #Render, #wmJob and the user's scene: the job that
 * drives Cycles lives in `editors/object/mesh_map_bake.cc`, and this half can be tested without an
 * engine. The joint contract is the atlas format the generated graph and the CPU composite already
 * read: a float image, `Non-Color`, a scalar map in R (spread to RGB by the writer), an RGB map as
 * stored.
 *
 * The three map families are:
 * - ray-traced maps baked through Cycles: AO, Curvature, Edge (an emission graph fed to the EMIT
 *   pass) and the two normals (the native NORMAL pass, always in world space, rotated on the CPU);
 * - CPU maps: the two ID maps, filled per covered texel from the mesh's `material_index`;
 * - an atlas shared by every object that uses the material (M5 writes only the covered texels).
 */

#include <cstddef>
#include <cstdint>

#include "BLI_span.hh"

#include "DNA_scene_enums.h"
#include "DNA_scene_types.h"

namespace blender {

struct BakePixel;
struct Depsgraph;
struct Image;
struct Main;
struct Material;
struct MaterialMeshMapSettings;
struct Mesh;
struct Object;
struct ObjectMeshMapState;
struct Scene;
struct ViewLayer;
struct bNode;
struct bNodeTree;

/**
 * The `1 - dot(bevel normal, geometry normal)` range the v1 Edge map maps onto [0, 1].
 *
 * A raw `1 - dot` is tiny for the shallow angles an edge mask has to catch (about 0.13 at 30
 * degrees), so the v1 bake divides by this constant, clamped. It is tuned by hand and validated
 * visually; there is deliberately no DNA parameter for it yet.
 */
constexpr float MESH_MAP_BAKE_EDGE_RANGE = 0.3f;

/** Whether \a type is baked by a render pass (AO, Curvature, Edge, both normals). */
bool BKE_mesh_maps_bake_type_renders(int8_t type);

/** Whether \a type is filled on the CPU (the two ID maps), with no engine involved. */
bool BKE_mesh_maps_bake_type_cpu_id(int8_t type);

/**
 * Whether \a type is a scalar atlas -- its R is the whole value -- rather than an RGB one.
 * Mirrors `paint_layer_mesh_map_is_scalar`, the classification the generator and the CPU composite
 * already share.
 */
bool BKE_mesh_maps_bake_type_scalar(int8_t type);

/**
 * The bake pass for \a type: #SCE_PASS_EMIT for the emission-graph maps, #SCE_PASS_NORMAL for the
 * normals. A type that is not rendered yields `eScenePassType(0)`.
 */
eScenePassType BKE_mesh_maps_bake_pass_for_type(int8_t type);

/**
 * The normal space for the two normal maps. Returns false for anything else, leaving \a r_space
 * untouched.
 */
bool BKE_mesh_maps_bake_normal_space_for_type(int8_t type, eBakeSpace &r_space);

/**
 * The AO ray distance for \a mesh: \a settings.ao_distance when it is positive, otherwise half the
 * diagonal of the mesh's local bounding box (the "from the object bounds" the setting documents).
 * A degenerate or empty mesh yields a small positive fallback so the AO node never reads 0, which
 * would silently mean the global radius.
 */
float BKE_mesh_maps_bake_ao_distance(const Mesh &mesh, const MaterialMeshMapSettings &settings);

/**
 * Build the emission surface for one ray-traced, non-native map (AO, Curvature, Edge) inside \a
 * tree: the value nodes, a #SH_NODE_EMISSION and the link into the material output. Returns the
 * emission node, or null for a type that has no emission graph (normals and the ID maps use another
 * path).
 *
 * \param ao_distance: the already-resolved AO ray distance; ignored unless \a type is AO.
 */
bNode *BKE_mesh_maps_bake_build_emit_tree(bNodeTree &tree,
                                          int8_t type,
                                          const MaterialMeshMapSettings &settings,
                                          float ao_distance);

/** A stable ID colour for \a ob, raw RGB in [0, 1] (no color management). */
void BKE_mesh_maps_bake_id_object_color(const Object &ob, float r_color[3]);

/** A stable ID colour for material slot index \a material_index, raw RGB in [0, 1]. */
void BKE_mesh_maps_bake_id_material_color(int material_index, float r_color[3]);

/**
 * Drop every pixel whose triangle belongs to a face that does not use \a ma.
 *
 * An object can carry several materials, but an atlas belongs to one of them, so only the triangles
 * whose face resolves -- through `material_index` and the object's slots -- to \a ma may contribute
 * to it. The rest get `primitive_id = -1`, exactly as an uncovered texel.
 */
void BKE_mesh_maps_bake_restrict_to_material(BakePixel *pixels,
                                             size_t pixels_num,
                                             const Mesh &mesh,
                                             const Object &ob,
                                             const Material &ma);

/**
 * Fill the RGBA result of a CPU ID map: every covered texel gets the object colour (ID_OBJECT) or
 * the colour of its face's `material_index` (ID_MATERIAL). Uncovered texels are left alone.
 */
void BKE_mesh_maps_bake_fill_id_pixels(const BakePixel *pixels,
                                       size_t pixels_num,
                                       const Mesh &mesh,
                                       const Object &ob,
                                       int8_t type,
                                       int channels,
                                       float *result);

/**
 * Copy the covered texels of \a result into \a atlas, leaving every uncovered texel as it was. A
 * scalar map has its R spread over G and B, so the CPU reads the same value from any channel.
 * Does nothing when \a atlas is shorter than `pixels_num * channels`.
 */
void BKE_mesh_maps_bake_write_covered(int8_t type,
                                      const BakePixel *pixels,
                                      size_t pixels_num,
                                      int channels,
                                      const float *result,
                                      MutableSpan<float> atlas);

/**
 * The atlas of \a ma for \a type, created on first use and recreated when \a resolution no longer
 * matches. The image is a float, `Non-Color`, \a resolution x \a resolution blank; the slot owns it
 * through #BKE_mesh_maps_slot_image_set. Returns null for an invalid type or resolution.
 */
Image *BKE_mesh_maps_bake_atlas_ensure(Main &bmain, Material &ma, int8_t type, int resolution);

/**
 * Resolve the UV layer a bake of \a ma on \a ob_eval must sample, through the same
 * #BKE_paint_layers_uv_map_resolve the generated graph and the content hash use.
 *
 * When the material names a layer the mesh does not have, the state is marked
 * #OB_MESH_MAP_STATUS_ERROR and null is returned: a map cannot be sampled without that layer, so
 * the caller must refuse the bake. Call after #BKE_mesh_maps_bake_state_begin so the error replaces
 * the in-flight status. The returned name points into the mesh or the material and is only valid
 * until either changes; copy it if it must outlive the call.
 */
const char *BKE_mesh_maps_bake_uv_resolve_or_fail(ObjectMeshMapState &state,
                                                  const Object &ob_eval,
                                                  const Material &ma);

/**
 * Compute the content hash the commit compares a running bake against, from the scene/view-layer
 * viewport depsgraph -- the same evaluation the RNA staleness check (`refresh`/`is_current`) uses.
 *
 * A fresh render depsgraph is deliberately not used: a modifier with different viewport and render
 * levels (Subdivision, say) evaluates differently, so a hash from it would disagree with the one
 * `refresh()` computes and turn every successful bake STALE at once. Returns false when the object
 * is not in that depsgraph.
 */
bool BKE_mesh_maps_bake_hash_from_viewport(Main &bmain,
                                           Scene &scene,
                                           ViewLayer &view_layer,
                                           const Object &ob,
                                           const Material &ma,
                                           int8_t type,
                                           uint32_t r_hash[2]);

/**
 * Whether \a image's buffer can hold the float bake result. A generated image without a float
 * buffer (a byte atlas) cannot, and a commit into one must fail instead of reporting a valid bake
 * over unwritten pixels.
 */
bool BKE_mesh_maps_bake_atlas_is_float(Image &image);

/** Whether a bake that started with \a start_hash is still current at \a now_hash. */
bool BKE_mesh_maps_bake_commit_is_current(const uint32_t start_hash[2], const uint32_t now_hash[2]);

/**
 * Enter #OB_MESH_MAP_STATUS_BAKING, returning the status to restore on cancel.
 * The prior status is kept so a cancelled bake can put it back instead of forcing an error.
 */
int8_t BKE_mesh_maps_bake_state_begin(ObjectMeshMapState &state);

/** Restore the status a bake had before #BKE_mesh_maps_bake_state_begin. */
void BKE_mesh_maps_bake_state_cancel(ObjectMeshMapState &state, int8_t previous_status);

/** Mark \a state #OB_MESH_MAP_STATUS_ERROR. */
void BKE_mesh_maps_bake_state_fail(ObjectMeshMapState &state);

/**
 * Commit a finished bake: when the content hash still matches, record it as valid
 * (#BKE_mesh_maps_object_state_mark_baked); otherwise mark #OB_MESH_MAP_STATUS_STALE and write
 * nothing. Returns the resulting status.
 */
int8_t BKE_mesh_maps_bake_state_commit(ObjectMeshMapState &state,
                                       const uint32_t start_hash[2],
                                       const uint32_t now_hash[2],
                                       int baked_time);

}  // namespace blender
