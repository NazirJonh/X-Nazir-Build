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
 * drives Cycles lives in `editors/object/mesh_map/mesh_map_bake.cc`, and this half can be tested
 * without an engine. The joint contract is the atlas format the generated graph and the CPU
 * composite already read: a float image, `Non-Color`, a scalar map in R (spread to RGB by the
 * writer), an RGB map as stored.
 *
 * The three map families are:
 * - ray-traced maps baked through Cycles: AO, Curvature, Edge and Position (an emission graph fed
 *   to the EMIT pass) and the three normals (the native NORMAL pass, always in world space,
 *   rotated into the claimed space on the CPU); this file turns a type into a pass/space/emission
 *   graph, but never calls the engine itself;
 * - CPU maps: the two ID maps, filled per covered texel from the mesh's `material_index`;
 * - an atlas shared by every object that uses the material (only the covered texels are written).
 */

#include <cstddef>
#include <cstdint>

#include "BKE_mesh_maps_types.hh"
#include "BLI_function_ref.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include "DNA_material_types.h"
#include "DNA_scene_enums.h"
#include "DNA_scene_types.h"

namespace blender {

struct BakePixel;
struct Depsgraph;
struct Image;
struct Main;
struct Material;
struct Mesh;
struct Object;
struct ObjectMeshMapState;
struct Scene;
struct ViewLayer;
struct bNode;
struct bNodeSocket;
struct bNodeTree;

/**
 * The `1 - dot(bevel normal, geometry normal)` range the v1 Edge map maps onto [0, 1].
 *
 * A raw `1 - dot` is tiny for the shallow angles an edge mask has to catch (about 0.13 at 30
 * degrees), so the v1 bake divides by this constant, clamped. It is tuned by hand and validated
 * visually; there is deliberately no DNA parameter for it yet.
 */
constexpr float MESH_MAP_BAKE_EDGE_RANGE = 0.3f;

/** Whether \a type is baked by a render pass (AO, Curvature, Edge, Position, the three normals). */
bool BKE_mesh_maps_bake_type_renders(int8_t type);

/**
 * Whether \a type is a simple CPU fill (the two ID maps): filled with no engine, and neither
 * casting through other surfaces nor composing other atlases.
 */
bool BKE_mesh_maps_bake_type_cpu_simple(int8_t type);

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
 * The normal space for the three normal maps. Returns false for anything else, leaving \a r_space
 * untouched.
 */
bool BKE_mesh_maps_bake_normal_space_for_type(int8_t type, eBakeSpace &r_space);

/**
 * Whether \a type bakes through the engine for \a ma's settings. The static classification
 * (#BKE_mesh_maps_bake_type_renders) answers without settings; the Curvature map switches to a
 * CPU filter over the baked Tangent Normal atlas when
 * #MaterialMeshMapSettings::curvature_method is #MA_MESH_MAP_CURVATURE_NORMAL_FILTER.
 */
bool BKE_mesh_maps_bake_pair_renders(const Material &ma, int8_t type);

/**
 * The AO ray distance for \a mesh: \a settings.ao_distance when it is positive, otherwise half the
 * diagonal of the mesh's local bounding box (the "from the object bounds" the setting documents).
 * A degenerate or empty mesh yields a small positive fallback so the AO node never reads 0, which
 * would silently mean the global radius.
 */
float BKE_mesh_maps_bake_ao_distance(const Mesh &mesh, const MaterialMeshMapSettings &settings);

/**
 * Build the emission surface for one ray-traced, non-native map (AO, Curvature, Edge, Position)
 * inside \a tree: the value nodes, a #SH_NODE_EMISSION and the link into the material output.
 * Returns the emission node, or null for a type that has no emission graph (normals and the ID
 * maps use another path).
 *
 * \param ao_distance: the already-resolved AO ray distance; ignored unless \a type is AO.
 * \param samples: the already-resolved bake sample count the AO and Edge nodes ray with.
 */
bNode *BKE_mesh_maps_bake_build_emit_tree(bNodeTree &tree,
                                          int8_t type,
                                          const MaterialMeshMapSettings &settings,
                                          float ao_distance,
                                          int samples);

/**
 * Remap a raw world-space Position result in place. \a mode decides the mapping
 * (#eMaterialMeshMapPositionNormalization): #MA_MESH_MAP_POSITION_RAW leaves the buffer alone,
 * #MA_MESH_MAP_POSITION_BOX remaps \a bb_min..\a bb_max per axis into [0, 1], and
 * #MA_MESH_MAP_POSITION_SPHERE remaps the bounding sphere around the box's center into [0, 1].
 * Only the RGB channels are touched; \a result holds \a texels_num RGBA texels.
 */
void BKE_mesh_maps_bake_normalize_position(float *result,
                                           size_t texels_num,
                                           const float bb_min[3],
                                           const float bb_max[3],
                                           int8_t mode);

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

/** Whether \a type is the scalar half of the ray-marched CPU fills: interior distance rays. */
bool BKE_mesh_maps_bake_type_cpu_rays_scalar(int8_t type);

/** Whether \a type is the vector half of the ray-marched CPU fills: outward hemisphere rays. */
bool BKE_mesh_maps_bake_type_cpu_rays_vector(int8_t type);

/**
 * One interior surface the Thickness map's rays march through, with its local-to-world matrix.
 * The ray cast runs in the source's local space and reads the distance back in world units, so
 * \a matrix may carry scale.
 */
struct MeshMapBakeThicknessSource {
  const Mesh *mesh;
  float4x4 matrix;
};

/**
 * Fill the RGBA result of a CPU Thickness map: every covered texel gets the averaged distance
 * the interior rays travel before they leave the solid, written to R (G and B are spread by the
 * commit, as for every scalar map).
 *
 * The texel's origin and flat normal come from \a low_mesh and \a low_matrix (the surface the
 * bake samples); each of \a rays directions is cast into the surfaces of \a sources through a
 * BVH, marched through entering hits, and the first exiting hit's distance is the thickness of
 * that direction. A texel whose rays all leave without an exit hit (an open surface, or another
 * part covering the exit) writes black (0,0,0) with alpha 1, so its atlas coverage stays as
 * opaque as every other map's.
 *
 * Uncovered texels are left alone; \a result holds `pixels_num * 4` floats.
 *
 * \param rays: directions per texel (1 traces straight down the normal). Clamped to
 * #MESH_MAP_BAKE_CPU_RAYS_MIN..#MESH_MAP_BAKE_CPU_RAYS_MAX.
 */
void BKE_mesh_maps_bake_fill_thickness(const BakePixel *pixels,
                                       size_t pixels_num,
                                       const Mesh &low_mesh,
                                       const float4x4 &low_matrix,
                                       Span<MeshMapBakeThicknessSource> sources,
                                       int rays,
                                       float *result);

/**
 * Whether a custom \a cage can be cast through for a low-poly \a low: the cage must have the same
 * face and corner counts, so each low-poly face has a matching cage face (mirrors the check the
 * full bake does before it accepts a cage object).
 */
bool BKE_mesh_maps_bake_cage_matches(const Mesh &low, const Mesh &cage);

/**
 * Fill a CPU ID map when the texels were cast onto high-poly objects:
 * - #MA_MESH_MAP_ID_OBJECT: the colour of the hit source object
 *   (\a pixels[i].object_id indexes \a objects);
 * - #MA_MESH_MAP_ID_MATERIAL: the colour of the material slot of the hit triangle, read from the
 *   source mesh's `material_index` (\a pixels[i].primitive_id is the triangle, `object_id` the
 *   source).
 *
 * A texel with no hit (`object_id < 0`) is left alone. \a objects and \a meshes are parallel and
 * must both be at least as long as the largest `object_id`, or the texel is skipped.
 */
void BKE_mesh_maps_bake_fill_id_from_high_poly(const BakePixel *pixels,
                                               size_t pixels_num,
                                               const Object *const *objects,
                                               size_t objects_num,
                                               const Mesh *const *meshes,
                                               size_t meshes_num,
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

/* -------------------------------------------------------------------- */
/** \name Per-object coverage and guarded atlas merge
 *
 * A bake belongs to one object, but the atlas is shared by every object using the material. The
 * margin that hides UV seams therefore must not overwrite the islands another object already
 * baked. These helpers separate the three steps: a coverage mask from the rasterized
 * #BakePixel array, the union of the other objects' coverage, and a merge that always writes the
 * object's own pixels but only the margin where no other object owns the texel.
 * \{ */

/**
 * Mark every texel \a pixels covers (its `primitive_id` is not -1) in \a r_coverage, as 1, and
 * every other texel as 0. Does nothing when \a r_coverage is shorter than \a pixels_num.
 */
void BKE_mesh_maps_bake_coverage_from_pixels(const BakePixel *pixels,
                                             size_t pixels_num,
                                             MutableSpan<uint8_t> r_coverage);

/** OR \a source into \a r_destination; the shorter of the two bounds the write. */
void BKE_mesh_maps_bake_coverage_union(Span<uint8_t> source, MutableSpan<uint8_t> r_destination);

/** How many texels both masks mark. The shorter of the two bounds the count. */
size_t BKE_mesh_maps_bake_coverage_overlap(Span<uint8_t> a, Span<uint8_t> b);

/**
 * Fill the RGBA result of a CPU Bent Normal map: every covered texel gets the normalized average
 * of the hemisphere directions around its surface normal that escape \a sources without a hit,
 * written in world space (`0.5 + 0.5 * direction`, a texel whose rays all hit stays black). The
 * origins and flat normals come from \a low_mesh and \a low_matrix; \a rays directions per
 * texel are clamped to #MESH_MAP_BAKE_CPU_RAYS_MIN..#MESH_MAP_BAKE_CPU_RAYS_MAX. Uncovered texels
 * are left alone.
 */
void BKE_mesh_maps_bake_fill_bent_normals(const BakePixel *pixels,
                                          size_t pixels_num,
                                          const Mesh &low_mesh,
                                          const float4x4 &low_matrix,
                                          Span<MeshMapBakeThicknessSource> sources,
                                          int rays,
                                          float *result);

/**
 * Fill the RGBA result of a Curvature map through the Normal Filter: the divergence of the
 * baked Tangent Normal atlas, sampled \a radius texels along U and V of the same tile the
 * curvature raster covers. The center and the available neighbors decode from the atlas's raw
 * map space (the DirectX green flip \a directx decodes), the divergence maps through \a scale
 * into a signed value written as `0.5 + 0.5 * clamp(value, -1, 1)` in R. Texels without enough
 * covered neighbors stay neutral; uncovered texels are left alone.
 *
 * \a normal_rgba is one tile's `resolution * resolution * 4` floats of the Tangent Normal
 * atlas; \a pixels is the curvature raster of the same tile, whose coverage gates the samples.
 * A \a pixels_num larger than that tile (a whole-atlas count, say) is clamped to the tile, so the
 * walk never leaves its block of the shared buffers.
 *
 * The caller resolves that Tangent Normal atlas through #BKE_mesh_maps_slot_find.
 */
void BKE_mesh_maps_bake_fill_curvature_from_normal(const BakePixel *pixels,
                                                   size_t pixels_num,
                                                   int resolution,
                                                   Span<float> normal_rgba,
                                                   int radius,
                                                   float scale,
                                                   bool directx,
                                                   float *result);

/**
 * Merge one object's bake into the shared atlas with a margin that respects the other objects.
 *
 * \param own_coverage: texels the object's own rasters cover. Always written.
 * \param own_margin: texels the margin fill added for this object. Written only where
 *   \a foreign_coverage is zero, so the margin of one object never lands on another's island.
 * \param foreign_coverage: union of the coverage of every other object sharing the atlas.
 * \param result: `pixels_num * channels` floats of this object's bake; a scalar map has its R
 *   spread over G and B, exactly as #BKE_mesh_maps_bake_write_covered does.
 */
void BKE_mesh_maps_bake_write_with_margin(int8_t type,
                                          Span<uint8_t> own_coverage,
                                          Span<uint8_t> own_margin,
                                          Span<uint8_t> foreign_coverage,
                                          int channels,
                                          const float *result,
                                          MutableSpan<float> atlas);

/**
 * Union into \a r_coverage the coverage of every object in \a bmain other than \a ob that uses
 * \a ma in a slot and carries a usable UV map (resolved through #BKE_mesh_maps_uv_resolve, so an
 * object without one is skipped. The atlas belongs to the material, and a material is shared across
 * every scene of the file, so the scope is the whole file rather than one view layer. No render
 * happens: the texels come from \a populate_pixels on each object's evaluated mesh plus
 * #BKE_mesh_maps_bake_restrict_to_material, on the viewport depsgraph. An object that the viewport
 * depsgraph does not evaluate (one in another scene, say) is taken from its own data mesh, without
 * modifiers: a proximity estimate, not an exact one.
 *
 * \param populate_pixels: fills the `resolution x resolution` #BakePixel array of a mesh for the UV
 * layer of the given name (the caller passes #RE_bake_pixels_populate), which keeps blenkernel free
 * of the render module. The pass is one UDIM tile's: \a tile_number shifts the UVs into it.
 * \param tiles: the UDIM tiles of \a own_coverage and \a r_coverage, in order; each is one
 * `resolution x resolution` block of them.
 *
 * When \a r_overlap_name is non-empty and some foreign coverage intersects \a own_coverage, it
 * receives the name (without the ID prefix) of the first such object; it is cleared otherwise.
 *
 * \return how many objects contributed coverage.
 */
int BKE_mesh_maps_bake_foreign_coverage(
    Main &bmain,
    Scene &scene,
    ViewLayer &view_layer,
    const Object &ob,
    const Material &ma,
    int resolution,
    Span<int> tiles,
    Span<uint8_t> own_coverage,
    FunctionRef<void(Mesh &mesh,
                     MutableSpan<BakePixel> pixels,
                     int resolution,
                     StringRef uv_name,
                     int tile_number)>
        populate_pixels,
    MutableSpan<uint8_t> r_coverage,
    MutableSpan<char> r_overlap_name);

/**
 * The atlas of \a ma for \a type, created on first use and recreated when \a resolution no longer
 * matches. The image is a float, `Non-Color`, \a resolution x \a resolution blank; the slot owns it
 * through #BKE_mesh_maps_slot_image_set. Returns null for an invalid type or resolution.
 */
Image *BKE_mesh_maps_bake_atlas_ensure(Main &bmain, Material &ma, int8_t type, int resolution);

/**
 * The indices of \a tiles -- one `pixels_per_tile` block of \a pixels each, in order -- that hold
 * at least one used texel (#BakePixel::primitive_id != -1), the raster's own answer to which tiles
 * the bake covers. A tile the UV-bounds pre-filter (#BKE_paint_layers_tiles_from_uv_bounds) could
 * not rule out but the raster left empty is dropped here, so no atlas tile and no render pass is
 * paid for it. The indices come back in order.
 */
Vector<int> BKE_mesh_maps_bake_tiles_with_pixels(Span<BakePixel> pixels,
                                                 int64_t pixels_per_tile,
                                                 Span<int> tiles);

/**
 * Resolve the UV layer a bake on \a ob_eval must sample: the mesh's active UV map, through the
 * same #BKE_mesh_maps_uv_resolve the generated graph and the content hash use.
 *
 * When the evaluated mesh has no usable UV map, the state is marked #OB_MESH_MAP_STATUS_ERROR and
 * null is returned: a map cannot be sampled without that layer, so the caller must refuse the bake.
 * Call after #BKE_mesh_maps_bake_state_begin so the error replaces the in-flight status. The
 * returned name points into the mesh and is only valid until the mesh's UV layers change; copy it
 * if it must outlive the call.
 */
const char *BKE_mesh_maps_bake_uv_resolve_or_fail(ObjectMeshMapState &state,
                                                  const Object &ob_eval);

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

/* -------------------------------------------------------------------- */
/** \name Multi-pair planning, rollback and clearing
 *
 * A "bake all" job is a sequence of (object, material, map type) pairs. The plan is pure data so
 * the operator and the tests can agree on which objects and types a run touches without any
 * rendering.
 * \{ */

/** One (object, map type) of a bake-all run. */
struct MeshMapBakePair {
  Object *object;
  Material *material;
  int8_t type;
};

/** Which objects a bake-all run targets. */
enum eMeshMapBakeObjectScope : int8_t {
  /** Only the active object. */
  MA_MESH_MAP_BAKE_ACTIVE_OBJECT = 0,
  /** Every object in the file that uses the material in a slot. */
  MA_MESH_MAP_BAKE_ALL_OBJECTS = 1,
};

/** Whether \a type is one the bake can produce. */
bool BKE_mesh_maps_bake_type_is_supported(int8_t type);

/**
 * Whether \a ob is part of the bake plan for \a ma in \a object_scope. This is the one rule the
 * Python panel and the "bake all" job share, so the two cannot disagree on which objects a run
 * covers.
 *
 * For #MA_MESH_MAP_BAKE_ACTIVE_OBJECT the predicate only accepts \a active_object. For
 * #MA_MESH_MAP_BAKE_ALL_OBJECTS it requires a mesh that uses \a ma in a slot and is not a resolved
 * high-poly source of another object using \a ma (a source is an input, never a low-poly to bake).
 *
 * \param source_objects: the result of #BKE_mesh_maps_source_collect_for_material, when the caller
 * already has it for a whole sweep over many objects. nullptr makes the predicate collect the
 * sources itself for this one call: convenient for a single query, but it scans every object, so a
 * caller that asks about many objects must collect once and pass the set here instead.
 */
bool BKE_mesh_maps_bake_object_in_plan(Main &bmain,
                                       Object &ob,
                                       const Material &ma,
                                       const Object *active_object,
                                       int8_t object_scope,
                                       const Set<const Object *> *source_objects);

/**
 * Append to \a r_objects, in `bmain.objects` order, every object a "bake all" run over \a ma covers
 * (#MA_MESH_MAP_BAKE_ALL_OBJECTS): a mesh that uses \a ma in a slot and is not a resolved high-poly
 * source of another object using \a ma. The sources are collected once for the whole sweep, so a
 * caller that wants the whole list must use this rather than asking the per-object predicate for
 * each candidate.
 */
void BKE_mesh_maps_bake_target_objects(Main &bmain,
                                       Material &ma,
                                       Vector<Object *> &r_objects);

/**
 * Append to \a r_pairs the (object, type) pairs a run over \a ma should bake, in a stable order:
 * objects in `bmain.objects` order (or just \a active_object for
 * #MA_MESH_MAP_BAKE_ACTIVE_OBJECT), types ascending. A type whose bit is unset in \a type_mask, or
 * that is reserved, is skipped. Only objects that use \a ma in a slot are considered for the
 * all-objects scope; \a active_object may be null for it.
 *
 * Exception to the ascending order: when \a ma's Curvature method is
 * #MA_MESH_MAP_CURVATURE_NORMAL_FILTER, each object's Tangent Normal pair is moved in front of its
 * Curvature pair, because the filter fills the Curvature pair from the Tangent Normal atlas baked
 * in that same run. Otherwise the order stays types ascending.
 */
void BKE_mesh_maps_bake_plan_pairs(Main &bmain,
                                   Object *active_object,
                                   Material &ma,
                                   int8_t object_scope,
                                   uint32_t type_mask,
                                   Vector<MeshMapBakePair> &r_pairs);

/**
 * Restore, for a cancelled run, the statuses of the pairs that were begun but never committed.
 * \a previous_statuses is paired with \a states; the shorter of the two bounds the loop.
 */
void BKE_mesh_maps_bake_states_rollback(Span<ObjectMeshMapState *> states,
                                        Span<int8_t> previous_statuses);

/**
 * Clear \a ma's atlas for \a type: every texel becomes opaque black, and every object state for
 * (\a ma, \a type) in \a bmain goes back to #OB_MESH_MAP_STATUS_NONE with its hash and time reset.
 * Returns how many object states were reset.
 *
 * The whole atlas is cleared, not one object's pixels: the atlas is a shared material resource, and
 * a partial clear would leave other objects' states claiming VALID pixels that are now half gone.
 */
int BKE_mesh_maps_bake_clear_type(Main &bmain, Material &ma, int8_t type);

/** \} */

}  // namespace blender
