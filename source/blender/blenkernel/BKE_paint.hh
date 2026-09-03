/* SPDX-FileCopyrightText: 2009 by Nicholas Bishop. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 */

#include <string>
#include <variant>

#include "BLI_array.hh"
#include "BLI_bit_vector.hh"
#include "BLI_bounds_types.hh"
#include "BLI_enum_flags.hh"
#include "BLI_map.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_ordered_edge.hh"
#include "BLI_set.hh"
#include "BLI_shared_cache.hh"
#include "BLI_string_ref.hh"
#include "BLI_utility_mixins.hh"
#include "BLI_vector.hh"

#include "DNA_brush_enums.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_enums.h"
#include "DNA_scene_enums.h"
/* For #eMaterialPaintChannel and #PAINT_MATERIAL_CHANNEL_NUM. */
#include "DNA_scene_types.h"

namespace blender {

struct AssetWeakReference;
struct BMFace;
struct BMLog;
struct BMVert;
struct BMesh;
struct BlendDataReader;
struct BlendWriter;
struct Brush;
struct BrushColorJitterSettings;
struct BrushMaterialPaint;
struct BrushMaterialPaintChannel;
struct CurveMapping;
struct Depsgraph;
struct EnumPropertyItem;
struct FaceSetColor;
namespace bke {
enum class AttrDomain : int8_t;
namespace pbvh {
class Tree;
}
}  // namespace bke
namespace ed::sculpt_paint {
namespace expand {
struct Cache;
}
namespace filter {
struct Cache;
}
struct StrokeCache;

struct VDMStampData {
  float3 location;
  float4x4 brush_local_mat;
  float4x4 brush_local_mat_inv;
  /**
   * Equivalent to `StrokeCache::plane_offset` at the time of the dab.
   *
   * Used by `sculpt_apply_texture()` to keep tiled strokes stable:
   * it samples the texture at `(brush_point - plane_offset)`.
   */
  float3 plane_offset;
  float radius;
  float bstrength;
  ePaintSymmetryFlags mirror_symmetry_pass;
  int radial_symmetry_pass;
  float4x4 symm_rot_mat;
  float4x4 symm_rot_mat_inv;
};
struct CurvePatchSession;
}  // namespace ed::sculpt_paint
struct GHash;
struct GridPaintMask;
struct Image;
struct ImagePool;
struct ImageUser;
struct KeyBlock;
struct Main;
struct Material;
struct Mesh;
struct MDeformVert;
struct MTex;
struct MultiresModifierData;
struct Object;
struct Paint;
struct PaintCurve;
enum class PaintMode : int8_t;
struct PaintModeSettings;
struct Palette;
struct PaletteColor;
struct RegionView3D;
struct ReportList;
struct Scene;
struct Sculpt;
struct SculptSession;
struct SubdivCCG;
struct Tex;
struct ToolSettings;
struct UnifiedPaintSettings;
struct View3D;
struct ViewLayer;
struct bContext;
struct bToolRef;

/* overlay invalidation */
enum ePaintOverlayControlFlags {
  PAINT_OVERLAY_INVALID_TEXTURE_PRIMARY = 1,
  PAINT_OVERLAY_INVALID_TEXTURE_SECONDARY = (1 << 2),
  PAINT_OVERLAY_INVALID_CURVE = (1 << 3),
  PAINT_OVERLAY_OVERRIDE_CURSOR = (1 << 4),
  PAINT_OVERLAY_OVERRIDE_PRIMARY = (1 << 5),
  PAINT_OVERLAY_OVERRIDE_SECONDARY = (1 << 6),
};
ENUM_OPERATORS(ePaintOverlayControlFlags);

#define PAINT_OVERRIDE_MASK \
  (PAINT_OVERLAY_OVERRIDE_SECONDARY | PAINT_OVERLAY_OVERRIDE_PRIMARY | \
   PAINT_OVERLAY_OVERRIDE_CURSOR)

/**
 * Defines 8 areas resulting of splitting the object space by the XYZ axis planes. This is used to
 * flip or mirror transform values depending on where the vertex is and where the transform
 * operation started to support XYZ symmetry on those operations in a predictable way.
 */
#define PAINT_SYMM_AREA_DEFAULT 0

enum ePaintSymmetryAreas {
  PAINT_SYMM_AREA_X = (1 << 0),
  PAINT_SYMM_AREA_Y = (1 << 1),
  PAINT_SYMM_AREA_Z = (1 << 2),
};
ENUM_OPERATORS(ePaintSymmetryAreas);

#define PAINT_SYMM_AREAS 8

void BKE_paint_invalidate_overlay_tex(const Main &bmain,
                                      Scene *scene,
                                      ViewLayer *view_layer,
                                      const Tex *tex);
void BKE_paint_invalidate_cursor_overlay(const Main &bmain,
                                         Scene *scene,
                                         ViewLayer *view_layer,
                                         CurveMapping *curve);
void BKE_paint_invalidate_overlay_all();
ePaintOverlayControlFlags BKE_paint_get_overlay_flags();
/**
 * Monotonic counter bumped whenever the active brush's primary texture is invalidated -- assigned,
 * cleared, its mapping edited, or the texture datablock itself edited -- via
 * #BKE_paint_invalidate_overlay_tex / #BKE_paint_invalidate_overlay_all. Unlike the reset-on-draw
 * overlay flags (which the paint cursor consumes), this only ever increases, so a poller can
 * detect "the brush texture changed" race-free by comparing against a previously stored value.
 * Used by the Curve Patch live editor to re-project the relief when the texture is edited
 * mid-session.
 */
uint64_t BKE_paint_get_overlay_texture_edit_count();
void BKE_paint_reset_overlay_invalid(ePaintOverlayControlFlags flag);
void BKE_paint_set_overlay_override(eOverlayFlags flag);

/* Palettes. */

Palette *BKE_palette_add(Main *bmain, const char *name);
PaletteColor *BKE_palette_color_add(Palette *palette);
bool BKE_palette_is_empty(const Palette *palette);
/**
 * Remove color from palette. Must be certain color is inside the palette!
 */
void BKE_palette_color_remove(Palette *palette, PaletteColor *color);
void BKE_palette_clear(Palette *palette);

void BKE_palette_color_set(PaletteColor *color, const float rgb[3]);
void BKE_palette_color_sync_legacy(PaletteColor *color);

/* Paint curves. */

/**
 * Number of segments a paint-curve bezier segment is tessellated into. Stored as the geometry's
 * `resolution`, so building a #PaintCurve outside the editor (versioning, the Python API) has to
 * know it too.
 */
constexpr int PAINT_CURVE_NUM_SEGMENTS = 40;

PaintCurve *BKE_paint_curve_add(Main *bmain, const char *name);

/**
 * Rebuild #PaintCurve::geometry from the legacy screen-space #PaintCurve::points array, then free
 * it. The positions stay in screen space, which is what #PaintCurve::use_3d_space false means --
 * moving them into object space needs a viewport and is left to the user.
 *
 * No-op when there is nothing to convert, so it is safe to call on every paint curve.
 */
void BKE_paint_curve_legacy_points_convert(PaintCurve &pc);

/**
 * Call when entering each respective paint mode.
 */
bool BKE_paint_ensure(ToolSettings *ts, Paint **r_paint);
/**
 * \param ensure_brushes: Call #BKE_paint_brushes_ensure().
 */
void BKE_paint_init(Main *bmain, Scene *sce, PaintMode mode, bool ensure_brushes = true);
void BKE_paint_free(Paint *paint);
/**
 * Called when copying scene settings, so even if 'src' and 'tar' are the same still do a
 * #id_us_plus(), rather than if we were copying between 2 existing scenes where a matching
 * value should decrease the existing user count as with #paint_brush_set()
 */
void BKE_paint_copy(const Paint *src, Paint *dst, int flag);

/**
 * Iterate over all paint settings in a scene.
 */
void BKE_paint_settings_foreach_mode(ToolSettings *ts, FunctionRef<void(Paint *paint)> fn);

void BKE_paint_cavity_curve_preset(Paint *paint, int preset);

void BKE_paint_mesh_automasking_settings_ensure(Paint &paint);

eObjectMode BKE_paint_object_mode_from_paintmode(PaintMode mode);
bool BKE_paint_ensure_from_paintmode(Scene *sce, PaintMode mode);
Paint *BKE_paint_get_active_from_paintmode(Scene *sce, PaintMode mode);
const EnumPropertyItem *BKE_paint_get_tool_enum_from_paintmode(PaintMode mode);
uint BKE_paint_get_brush_type_offset_from_paintmode(PaintMode mode);
std::optional<int> BKE_paint_get_brush_type_from_obmode(const Brush *brush, eObjectMode ob_mode);
std::optional<int> BKE_paint_get_brush_type_from_paintmode(const Brush *brush, PaintMode mode);
Paint *BKE_paint_get_active(const Main &bmain, Scene *sce, ViewLayer *view_layer);
Paint *BKE_paint_get_active_from_context(const bContext *C);
PaintMode BKE_paintmode_get_active_from_context(const bContext *C);
PaintMode BKE_paintmode_get_from_tool(const bToolRef *tref);
bool BKE_paint_use_unified_size(const Paint *paint);
bool BKE_paint_use_unified_strength(const Paint *paint);
bool BKE_paint_use_unified_color(const Paint *paint);

/**
 * Multi-object mirror-surface-snap search distance, in brush radii.
 *
 * Files written before #Paint.mirror_snap_distance existed store 0 in what used to be padding.
 * Mapping 0 back to the DNA default here keeps old files working without a file-subversion bump,
 * and gives RNA and the sculpt code one single source of truth.
 */
float BKE_paint_mirror_snap_distance_get(const Paint &paint);

/* Paint brush retrieval and assignment. */

Brush *BKE_paint_brush(Paint *paint);
const Brush *BKE_paint_brush_for_read(const Paint *paint);
Brush *BKE_paint_brush_from_essentials(Main *bmain, PaintMode paint_mode, const char *name);

/**
 * Check if brush \a brush may be set/activated for \a paint. Passing null for \a brush will return
 * true.
 */
bool BKE_paint_can_use_brush(const Paint *paint, const Brush *brush);

/** Groups of brush settings that can be "recorded" and forced onto every brush of a session. */
enum class BrushOverrideGroup : int8_t {
  FaceSets = 0,
  Stroke = 1,
  Falloff = 2,
};

/**
 * Carries over the brush setting groups the user is currently recording (see
 * #PaintRuntime::override_stroke and friends) from the brush that was active before a brush
 * switch onto the brush that just became active. Settings the target brush has no capability
 * for are skipped. Does nothing when no group is being recorded.
 */
void BKE_paint_brush_group_overrides_apply(Paint *paint, const Brush *src, Brush *dst);

/**
 * Reverts a single group of settings of the active brush back to the values stored in its asset
 * on disk, leaving every other setting the user has changed untouched. The brush data-block is
 * re-linked in the process, so #Paint.brush changes address.
 *
 * \return False when the active brush is not an editable asset or re-linking failed.
 */
bool BKE_paint_brush_group_reset_from_asset(Main *bmain,
                                           Scene *scene,
                                            Paint *paint,
                                            BrushOverrideGroup group,
                                            ReportList *reports);

/**
 * Activates \a brush for painting, and updates #Paint.brush_asset_reference so the brush can be
 * restored after file read. No change is done if #BKE_paint_brush_poll() returns false.
 *
 * \return True on success. If \a brush is already active, this is considered a success (the brush
 * asset reference will still be updated).
 *
 * \note #WM_toolsystem_activate_brush_and_tool() might be the preferable way to change the active
 * brush. It also lets the tool-system decide if the active tool should be changed given the type
 * of brush, and it updates the "last used brush" for the previous tool.
 * #BKE_paint_brush_set() should only be called to force a brush to be active,
 * circumventing the tool system.
 */
bool BKE_paint_brush_set(Paint *paint, Brush *brush);
/**
 * Version of #BKE_paint_brush_set() that takes an asset reference instead of a brush, importing
 * the brush if necessary.
 *
 * \return False if unable to set the brush to the provided asset reference. True otherwise.
 */
bool BKE_paint_brush_set(Main *bmain,
                         Paint *paint,
                         const AssetWeakReference &brush_asset_reference);
/**
 * Like #BKE_paint_brush_set(Paint*, Brush*), but also snapshots the outgoing brush's PBR
 * Paint state into \a scene's preset list before switching, and applies the incoming
 * brush's matching preset after. Use this at genuine user-facing brush-activation sites
 * (tool system, brush-asset revert, cross-editor brush sync); the plain
 * #BKE_paint_brush_set() remains the right call for transient Sculpt/Grease-Pencil
 * modifier-key brush toggles, which never touch material-paint-capable brushes.
 */
bool BKE_paint_brush_set_synced(Scene &scene, Paint &paint, Brush *brush);

/** Asset-reference-taking counterpart of #BKE_paint_brush_set_synced(Scene&, Paint&, Brush*). */
bool BKE_paint_brush_set_synced(Main &bmain,
                                Scene &scene,
                                Paint &paint,
                                const AssetWeakReference &brush_asset_reference);
bool BKE_paint_brush_set_default(Main *bmain, Scene *scene, Paint *paint);
bool BKE_paint_brush_set_essentials(Main *bmain, Paint *paint, const char *name);
void BKE_paint_previous_asset_reference_set(Paint *paint,
                                            AssetWeakReference &&asset_weak_reference);
void BKE_paint_previous_asset_reference_clear(Paint *paint);

std::optional<AssetWeakReference> BKE_paint_brush_type_default_reference(
    PaintMode paint_mode, std::optional<int> brush_type);
void BKE_paint_brushes_set_default_references(ToolSettings *ts);
/**
 * Make sure the active brush asset is available as active brush, importing it if necessary. If
 * there is no user set active brush, the default one is used/imported from the essentials asset
 * library.
 *
 * It's good to avoid this until the user actually shows intentions to use brushes, to avoid unused
 * brushes in files. E.g. use this when entering a paint mode, but not for versioning.
 *
 * Also handles the active eraser brush asset.
 */
void BKE_paint_brushes_ensure(Main *bmain, Scene *scene, Paint *paint);
void BKE_paint_brushes_validate(Main *bmain, Scene *scene, Paint *paint);

/* Secondary eraser brush. */

Brush *BKE_paint_eraser_brush(Paint *paint);
const Brush *BKE_paint_eraser_brush_for_read(const Paint *paint);

bool BKE_paint_eraser_brush_set(Paint *paint, Brush *brush);
Brush *BKE_paint_eraser_brush_from_essentials(Main *bmain, PaintMode paint_mode, const char *name);
bool BKE_paint_eraser_brush_set_default(Main *bmain, Paint *paint);
bool BKE_paint_eraser_brush_set_essentials(Main *bmain, Paint *paint, const char *name);

/* Paint palette. */

Palette *BKE_paint_palette(Paint *paint);
void BKE_paint_palette_set(Paint *paint, Palette *palette);
/**
 * Return true when in vertex/weight/texture paint + face-select mode?
 */
bool BKE_paint_select_face_test(const Object *ob);
/**
 * Return true when in vertex/weight paint + vertex-select mode?
 */
bool BKE_paint_select_vert_test(const Object *ob);
/**
 * Return true when in grease pencil sculpt mode.
 */
bool BKE_paint_select_grease_pencil_test(const Object *ob);
/**
 * used to check if selection is possible
 * (when we don't care if its face or vert)
 */
bool BKE_paint_select_elem_test(const Object *ob);
/**
 * Checks if face/vertex hiding is always applied in the current mode.
 * Returns true in vertex/weight paint.
 */
bool BKE_paint_always_hide_test(const Object *ob);

/* Partial visibility. */

/**
 * Returns whether any of the corners of the grid face whose inner corner is at (x, y) are hidden.
 */
bool paint_is_grid_face_hidden(BoundedBitSpan grid_hidden, int gridsize, int x, int y);
/**
 * Return true if all vertices in the face are visible, false otherwise.
 */
bool paint_is_bmesh_face_hidden(const BMFace *f);

/* Paint masks. */

float paint_grid_paint_mask(const GridPaintMask *gpm, uint level, uint x, uint y);

/**
 * Returns the overlay color for a Face Set.
 * Checks custom colors on `mesh` first; falls back to a deterministic random color.
 * Pass `mesh = nullptr` to always get the random color.
 */
void BKE_paint_face_set_overlay_color_get(int face_set,
                                          int seed,
                                          uchar r_color[4],
                                          const Mesh *mesh);

/**
 * Build a Face Set ID to custom overlay color lookup for \a mesh.
 *
 * Drawing code covers every face of a batch, so it must build this once and reuse it rather than
 * calling the #Mesh overload above per face, which scans #Mesh.face_set_colors linearly.
 */
Map<int, uchar4> BKE_paint_face_set_custom_colors_map(const Mesh *mesh);

/** Variant of #BKE_paint_face_set_overlay_color_get using a prebuilt custom color lookup. */
void BKE_paint_face_set_overlay_color_get(int face_set,
                                          int seed,
                                          uchar r_color[4],
                                          const Map<int, uchar4> &custom_colors);

/* Face Set Custom Colors */
void BKE_paint_face_set_custom_color_set(Mesh *mesh, int face_set_id, const float color[3]);
void BKE_paint_face_set_custom_color_get(const Mesh *mesh, int face_set_id, float r_color[3]);
bool BKE_paint_face_set_custom_color_exists(const Mesh *mesh, int face_set_id);
void BKE_paint_face_set_custom_color_remove(Mesh *mesh, int face_set_id);
void BKE_paint_face_set_custom_colors_clear(Mesh *mesh);
/** Read-only view of the custom color table, for callers that need to snapshot or remap it. */
Span<FaceSetColor> BKE_paint_face_set_custom_colors_get_all(const Mesh *mesh);
/** Replace the whole custom color table, freeing the previous one. */
void BKE_paint_face_set_custom_colors_set_all(Mesh *mesh, Span<FaceSetColor> colors);
/**
 * Drop entries whose Face Set ID is no longer used by any face.
 *
 * Entries for unused Face Sets are harmless while sculpting - they let a re-painted color map back
 * to the same ID - so this is only meant for session boundaries, not for use after every edit.
 */
void BKE_paint_face_set_custom_colors_remove_unused(Mesh *mesh);
/**
 * Find the Face Set ID whose custom color matches \a color within a tolerance of 1/255 per
 * channel. Returns 0 if no match is found.
 */
int BKE_paint_face_set_find_by_custom_color(const Mesh *mesh, const float color[3]);
void BKE_paint_face_set_quantize_color(const float color[3], float r_quant[3]);
uint32_t BKE_paint_face_set_quantize_color_pack(const float color[3]);
/**
 * Snap antialiased texture samples to a saturated dominant hue, then quantize to 8-bit steps.
 * Used by Face Set color-from-texture so halftones map to the same ID as pure red/green/blue.
 */
void BKE_paint_face_set_snap_texture_sample_color(const float color[3], float r_snapped[3]);
void BKE_paint_face_set_quantize_texture_color(const float color[3], float r_quant[3]);
uint32_t BKE_paint_face_set_quantize_texture_color_pack(const float color[3]);

/* Stroke related. */

namespace bke::paint {
bool supports_scene_size(PaintMode paint_mode);
bool supports_symmetry_tiling(PaintMode paint_mode);
}  // namespace bke::paint

/* Random values are generated on each new stroke so each stroke
 * gets a different starting point in the perlin noise. */
float3 seed_hsv_jitter();

bool paint_calculate_rake_rotation(Paint &paint,
                                   const Brush &brush,
                                   const float mouse_pos[2],
                                   PaintMode paint_mode,
                                   bool stroke_has_started);
void paint_update_brush_rake_rotation(Paint &paint, const Brush &brush, float rotation);

void BKE_paint_stroke_get_average(const Paint *paint, const Object *ob, float stroke[3]);

float3 BKE_paint_randomize_color(const BrushColorJitterSettings &color_jitter,
                                 const float3 &initial_hsv_jitter,
                                 const float distance,
                                 const float pressure,
                                 const float3 &color);

/* .blend I/O */

void BKE_paint_blend_write(BlendWriter *writer, Paint *paint);
void BKE_paint_blend_read_data(BlendDataReader *reader, const Scene *scene, Paint *paint);

/* Data used for displaying extra visuals while using the Pose brush */
struct SculptPoseIKChainPreview {
  Array<float3> initial_orig_coords;
  Array<float3> initial_head_coords;
};

struct SculptBoundaryInfoCache {
  /* Indexed by base mesh vertex index.
   *
   * TODO: Evaluate whether a BitVector or a Set works better for memory footprint and lookup. */
  BitVector<> verts;

  Set<OrderedEdge> edges;
};

/* Data used for displaying extra visuals while using the Boundary brush. */
struct SculptBoundaryPreview {
  Vector<std::pair<float3, float3>> edges;
  float3 pivot_position;
  float3 initial_vert_position;
};

struct SculptFakeNeighbors {
  /* Max distance used to calculate neighborhood information. */
  float current_max_distance;

  /* Indexed by vertex, stores the vertex index of its fake neighbor if available. */
  Array<int> fake_neighbor_index;
};

struct SculptTopologyIslandCache {
  /**
   * An ID for the island containing each geometry vertex. Will be empty if there is only a single
   * island.
   */
  Array<uint8_t> vert_island_ids;
};

using ActiveVert = std::variant<std::monostate, int, BMVert *>;

/* Helper return struct for associated data. */
struct PersistentMultiresData {
  Span<float3> positions;
  Span<float3> normals;
  MutableSpan<float> displacements;
};

/* Helper return struct for the layer brush's uniform depth reference surface. */
struct LayerUniformBaseData {
  Span<float3> positions;
  Span<float3> normals;
  MutableSpan<float> displacements;
};

/**
 * State of a sculpt layer mask editing session.
 *
 * While a session is open the node's sparse mask is expanded into the standard mask buffer, so the
 * whole existing toolset — Mask brush, gesture operators, flood fill, mask filters, the viewport
 * overlay and its undo steps — works on it unchanged. The user's own sculpt mask is parked here for
 * the duration.
 *
 * Runtime only. The dense buffer is never written to a .blend, so a crash costs an unfinished
 * session but cannot corrupt the file.
 *
 * Lives here rather than in the editor module's `sculpt_intern.hh` because it is held by value on
 * #SculptSession, which blenkernel owns.
 */
struct SculptLayerMaskEdit {
  /**
   * Uid of the #SculptLayerTreeNode whose mask is being edited; 0 when no session is open. Uid 0
   * names the root group, which is never drawn and therefore never editable, so the sentinel
   * collides with nothing (see #bke::sculpt_layers::node_find_by_uid).
   */
  int node_uid = 0;
  /**
   * True when the session was opened on the multires grid domain rather than on mesh vertices.
   *
   * Recorded rather than re-derived from the live #bke::pbvh::Tree on exit. The two domains park
   * the user's mask in different places — a mesh attribute against #SubdivCCG::masks — and the
   * exit path is destructive about the one it did not use (it removes the storage when the session
   * created it). Should the multires modifier be removed while a session is open, a re-derived
   * domain would make the exit restore one domain and delete the other's mask.
   */
  bool on_grids = false;
  /** True when the mesh already carried a `.sculpt_mask` attribute before the session opened. */
  bool had_vert_mask = false;
  /** The user's sculpt mask, parked. Empty when #had_vert_mask is false. */
  Array<float> saved_vert_mask;
  /**
   * True when #SubdivCCG::masks was already materialized before the session opened. The grid
   * counterpart of #had_vert_mask, kept separate because the two answer different questions: an
   * absent mesh mask is a missing attribute, an absent grid mask is an empty array.
   */
  bool had_grid_mask = false;
  /** The user's grid sculpt mask, parked. Empty when #had_grid_mask is false. */
  Array<float> saved_grid_mask;
  /**
   * `SubdivCCG::grid_area` as it was when the session opened, and therefore the block size the
   * node's mask is cut at. Zero for a vertex-domain session.
   *
   * The exit path refuses to compress a grid mask whose grid area no longer matches: a subdivision
   * level change rebuilds the CCG and re-derives #SubdivCCG::masks from the base mesh, so the
   * expanded weights the session was authoring are simply gone and compressing what replaced them
   * would store the user's own sculpt mask onto the node.
   */
  int grid_area = 0;
  /**
   * `SubdivCCG::grids_num` as it was when the session opened. Zero for a vertex-domain session.
   *
   * #grid_area alone cannot catch a base-topology change that rebuilds the CCG at the same
   * subdivision level: `grids_num` changes while `grid_area` does not, and comparing
   * `masks.size()` against a total re-derived from the *live* CCG is self-consistent by
   * construction, so it cannot detect the rebuild either. Checked alongside #grid_area in the exit
   * path for exactly the case that check exists for.
   */
  int grids_num = 0;
  /**
   * Multires top level, i.e. the level the node's mask is *stored* at. Zero for a vertex-domain
   * session.
   *
   * The session authors on #SubdivCCG::masks, which sits at the *sculpt* level, while grid masks
   * are stored at the top level alongside the layer data they weight (see #resample_grid_masks).
   * With `Sculpt Levels < Levels` the two differ and the weights are resampled at both session
   * boundaries. Recorded rather than re-read from the modifier on exit, so the buffer is always
   * mapped back out of the level it was mapped into.
   */
  int store_level = 0;
  /**
   * `SubdivCCG::id` as it was when the session opened. Zero for a vertex-domain session, and never
   * a valid id (#subdiv_ccg_next_id starts at one).
   *
   * #grid_area and #grids_num together still cannot see every rebuild: one at the *same*
   * subdivision level over the *same* base topology leaves both unchanged, and `masks.size()` with
   * them. Such a rebuild re-derives #SubdivCCG::masks from `CD_GRID_PAINT_MASK` — the user's own
   * sculpt mask, since the suspend guards make sure the session's weights never reach that layer —
   * so the exit path would find an apparently intact domain and compress the user's mask onto the
   * node as the layer's weight map, losing the painted weights without a word. It is reachable:
   * every mask operator re-evaluates the depsgraph before it runs, and anything that tagged the
   * mesh `ID_RECALC_GEOMETRY` during the session rebuilds the CCG there.
   *
   * An id rather than the #SubdivCCG address, which would be worthless: the old instance is freed
   * before the new one is allocated, so the allocator may hand back the same address.
   */
  uint64_t ccg_id = 0;
  /**
   * True when the session synthesized the node's mask on open rather than finding a usable one.
   *
   * Only the cancel path reads it, and it is what lets that path be faithful: a node that carried no
   * mask before the edit must carry none after a discarded one, or "cancel" would leave behind an
   * opaque mask the user never asked for. The open path replaces an unusable mask (missing, stale,
   * or cut for the other domain) with an opaque one, and all three cases collapse to the same
   * restoration — every consumer already ignores an unusable mask, so "no mask" is the state the
   * user was in.
   */
  bool mask_created = false;
  /**
   * Number of #bke::sculpt_layers::MaskEditSuspendGuard instances currently holding this session
   * parked. Non-zero means the session's dense weights are in #suspended_dense and the standard
   * mask storage holds the user's own mask again.
   *
   * A session keeps the node's weights in the *persistent* store — the `.sculpt_mask` attribute on
   * the original mesh, or #SubdivCCG::masks, which the multires flush copies into the base mesh's
   * `CD_GRID_PAINT_MASK` layer. Anything that serializes the file, or that flushes the CCG into the
   * base mesh, would therefore record the layer's weights as the user's own mask. Such an operation
   * is bracketed by a suspend/resume pair rather than by closing the session, because it can happen
   * without the user asking for it (auto-save runs on a timer) and closing a session the user is
   * working in would be both surprising and lossy.
   *
   * A counter rather than a flag because the brackets nest: a save parks the whole #Main and then
   * calls a flush that parks the object again. With a flag the inner bracket's exit would put the
   * layer's weights back before the outer one's operation had run — the precise corruption the
   * brackets exist to prevent. Only the transition to and from zero performs the swap, so the
   * scheme is correct at any nesting depth.
   *
   * Distinct from "no session": #node_uid stays set while suspended, so nothing else in the feature
   * has to know the difference.
   */
  int suspend_depth = 0;
  /**
   * The session's dense weights, parked while #suspend_depth is non-zero. Empty otherwise.
   *
   * The counterpart of #saved_vert_mask / #saved_grid_mask with the two roles swapped: while
   * suspended it is the *session's* buffer that is held here and the *user's* that is live.
   */
  Array<float> suspended_dense;
  /**
   * True once a refused suspend has been logged for this session.
   *
   * A refusal is reported, never silent, but it is also persistent: the condition that makes a
   * suspend impossible (a parked buffer describing a domain the object no longer has) does not
   * clear itself, so every subsequent depsgraph re-evaluation would re-report the same fact.
   * Latched here rather than on the CCG because the mesh domain has no CCG to latch on, and reset
   * with the rest of the session on close.
   */
  bool suspend_refusal_reported = false;
  /**
   * Idname of the 3D viewport's active tool as it was when the session opened, empty when it could
   * not be read (no viewport on screen).
   *
   * Restored on exit only if the active tool is still `builtin_brush.mask`: a user who switched
   * tools deliberately inside the session has made a choice that closing the session must not undo.
   *
   * The tool is the *only* thing the session parks. Entering also disarms REC, but that is not put
   * back on exit: arming carries invariants and an undo record that only its operator can supply.
   */
  std::string saved_tool_id;
};

struct SculptSession : NonCopyable, NonMovable {
  /* The current active shapekey for the mesh. Only non-null for Type::Mesh */
  KeyBlock *shapekey_active = nullptr;

  /* Edges to adjacent faces. */
  Array<int> edge_to_face_offsets;
  Array<int> edge_to_face_indices;
  GroupedSpan<int> edge_to_face_map;

  /* Vertices to adjacent edges. */
  Array<int> vert_to_edge_offsets;
  Array<int> vert_to_edge_indices;
  GroupedSpan<int> vert_to_edge_map;

  /* BMesh for dynamic topology sculpting */
  BMesh *bm = nullptr;
  /* Undo/redo log for dynamic topology sculpting */
  BMLog *bm_log = nullptr;

  MultiresModifierData *multires_modifier = nullptr;
  /* Limit surface/grids. */
  SubdivCCG *subdiv_ccg = nullptr;

  /* BVH tree acceleration structure */
  std::unique_ptr<bke::pbvh::Tree> pbvh;

  /* Object is deformed with some modifiers. */
  bool deform_modifiers_active = false;
  /* The active shape key is the only deformer (no other enabled modifier changes the drawn
   * surface), so the sculpt PBVH can be drawn directly instead of re-evaluating the mesh every
   * redraw. Only meaningful for #Type::Mesh sessions with #shapekey_active set. */
  bool shapekey_pbvh_draw = false;
  /* Coords of deformed mesh but without stroke displacement. */
  Array<float3, 0> deform_cos;
  /* Crazy-space deformation matrices. */
  Array<float3x3, 0> deform_imats;

  /**
   * Normals corresponding to the #deform_cos evaluated/deform positions. Stored as a #SharedCache
   * for consistency with mesh caches in #MeshRuntime::vert_normals_cache.
   */
  SharedCache<Vector<float3>> vert_normals_deform;
  SharedCache<Vector<float3>> face_normals_deform;

  ed::sculpt_paint::StrokeCache *cache = nullptr;
  Vector<ed::sculpt_paint::VDMStampData> vdm_stamps;
  ed::sculpt_paint::filter::Cache *filter_cache = nullptr;
  ed::sculpt_paint::expand::Cache *expand_cache = nullptr;
  ed::sculpt_paint::CurvePatchSession *curve_patch_session = nullptr;
  /**
   * Editor-owned restore+free for #curve_patch_session. Set when a session is published; invoked
   * from #BKE_sculptsession_free so object deletion and other teardowns that never go through
   * sculpt mode-exit still restore uncommitted relief and free the session.
   *
   * A function pointer, not a destructor call: `CurvePatchSession` is an editor type that
   * blenkernel only forward-declares, so ~SculptSession cannot `MEM_delete` it. No-op when the
   * session was already discarded (pointer null).
   */
  void (*free_curve_patch_session)(Object &ob) = nullptr;

  /* Cursor data and active vertex for tools */
  std::optional<int> active_face_index;
  std::optional<int> active_grid_index;

  /* When active, the cursor draws with faded colors, indicating that there is an action
   * enabled.
   */
  bool draw_faded_cursor = false;
  float cursor_radius = 0.0f;
  float3 cursor_location;
  float3 cursor_normal;
  std::optional<float3> cursor_sampled_normal;
  float3 cursor_view_normal;

  /* TODO(jbakker): Replace rv3d and v3d with ViewContext */
  RegionView3D *rv3d = nullptr;
  View3D *v3d = nullptr;

  /* Dynamic mesh preview */
  Array<int> preview_verts;

  /* Pose Brush Preview */
  std::unique_ptr<SculptPoseIKChainPreview> pose_ik_chain_preview;

  /* Boundary Brush Preview */
  std::unique_ptr<SculptBoundaryPreview> boundary_preview;

  /* "Persistent" positions and normals for multires. (For mesh the
   * ".sculpt_persistent_co" attribute is used, etc.). */
  struct {
    Array<float3> sculpt_persistent_co;
    Array<float3> sculpt_persistent_no;
    Array<float> sculpt_persistent_disp;

    /* The stored state for the SubdivCCG at the time of attribute population, used to roughly
     * determine if the topology when accessed at a current point in time is equivalent to when
     * it was originally stored. */
    int grids_num = -1;
    int grid_size = -1;
  } persistent;

  /* Reference surface for the layer brush's "Uniform Depth" option, shared by all geometry types
   * that support it. Unlike #persistent this is never written to the mesh: it only has to stay
   * stable across the strokes of a sculpt session, so keeping it out of the file also keeps it
   * out of undo steps and away from the persistent base used by "Set Persistent Base". */
  struct {
    Array<float3> positions;
    Array<float3> normals;
    Array<float> displacement;

    /* The number of elements at the time of capture, used to detect that the topology changed
     * since and the stored data can no longer be used. */
    int elements_num = -1;
  } layer_uniform_base;

  /* Sculpt layers (non-destructive sculpt edits, see #BKE_sculpt_layers.hh). Runtime state used
   * by the editor module to record strokes into the active layer. Multires (grid domain) layers
   * keep no runtime base: the composed surface is evaluated from `MDisps + sum(enabled layers)`
   * by the subdivision displacement evaluator, so grid state is fully derived from stored data. */
  struct {
    /* True between #stroke_record_begin and #stroke_record_end while a valid active layer is
     * being recorded. This is the authoritative "recording" signal. */
    bool recording = false;
    /* True while REC (record) mode is on: brush strokes are recorded into the active layer (pinned
     * to influence 1.0). When false, strokes edit the base geometry instead. Transient editing
     * state, not saved to the blend file.
     *
     * Mirrored onto the mesh as #SCULPT_LAYER_REC_ARMED so that it survives this session being
     * destroyed on the way out of sculpt mode; the session stays the authority while it exists, and
     * the mirror is read back exactly once, by #init_sculpt_mode_session. */
    bool rec_active = false;
    /* #SculptLayerTreeNode::uid of the layer whose recording has already been reported to the user
     * in this session, or 0 if nothing has been. Strokes report once rather than every time, and a
     * change of active layer reports again — both fall out of comparing against this.
     *
     * Session-scoped on purpose, and that is the whole implementation of "once per entry into sculpt
     * mode": a new session starts silent with no reset to run. Now that REC survives object mode, the
     * user can return to a mode that records without having touched the REC button, which is what
     * these two fields exist to say out loud. */
    int rec_notified_uid = 0;
    /* Whether the "REC is armed but the stroke cannot be recorded" warning has already been given in
     * this session. Cleared again by a stroke that does record, so that a cause the user fixes and
     * then re-introduces is reported anew. */
    bool rec_notified_blocked = false;
    /* For the mesh (vertex) domain: the vertex positions with every layer's contribution removed
     * (the un-layered base). Captured lazily on sculpt-mode enter; kept current by non-REC strokes.
     * Empty for the grid domain. */
    Array<float3> mesh_base;
    /* Whether #mesh_base has been initialized for this session. */
    bool state_valid = false;
    /* Per-element object-space contribution of the enabled layers ("base view" offset,
     * `combined[i] - base[i]`), valid only for the duration of one stroke. Brushes subtract it from
     * the live positions when computing surface-shape-dependent inputs (falloff, area normal,
     * smoothing targets, plane fits), so neither a base edit nor a stroke recorded into another
     * layer absorbs the residual of the layers below. While recording, the layer being authored is
     * excluded (it stays WYSIWYG). Under a shape key the offset is summed straight from the layer
     * data, which is exactly how it is composed there. Empty when no layer contributes (no enabled
     * layers, and deform-modifier sessions without a shape key, which cannot record). */
    Array<float3> base_view;
    /* The base view offset sampled at the current brush contact point, refreshed once per brush
     * action (per symmetry / tile pass). Every consumer of #base_view removes it, i.e. the brush
     * inputs use `base_view[i] - base_view_dc` rather than the raw offset.
     *
     * The raw offset cannot be used directly: the brush reference point (#StrokeCache::location_symm
     * and the radius around it) lives on the *composed* surface, so subtracting the full offset from
     * the sampled positions moves them away from the cursor by the layer height. Once that height
     * approaches the brush radius every falloff factor is zeroed (the stroke does nothing) and the
     * area-plane sampling finds no vertex at all, falling back to a plane through the composed
     * location, which yanks the still-active vertices by the layer height and tears the mesh.
     * Removing the offset *at the contact point* keeps the sampled surface anchored under the cursor
     * while still stripping the layer pattern from the brush inputs, which is what the base view is
     * for. All consumers are either differential (smoothing, plane fits) or use a matching
     * adjust / compose pair, so a constant shift is exactly invariant for them. */
    float3 base_view_dc = float3(0.0f);
    /* Bounds of the base-view offset (`base_view[i]`, without the DC) over each PBVH leaf node's
     * elements, computed once per stroke. Empty when the base view is inactive.
     *
     * The brush measures its falloff on the base view, but the nodes it processes are selected from
     * the node bounds on the *composed* surface. Those two footprints differ by the layer height, so
     * an element can earn a non-zero factor while its node was never gathered — and since a node is
     * processed as a whole, the stroke boundary then follows node borders (square tiles).
     * #layers::base_view_extend_node_mask subtracts these offset bounds from the node's position
     * bounds to box the node in base-view space, and adds every node that box brings within the
     * radius. That restores the invariant "every element with a non-zero factor belongs to a
     * gathered node" without touching the falloff itself.
     *
     * Only the offset is stored, not the base-view positions themselves: the offset is constant for
     * the whole stroke (the layer data does not change while the base is edited), while the
     * positions move under the brush. Deriving the box from #Node::bounds_ at test time therefore
     * follows the stroke, whereas a stored `position - base_view` box would go stale as soon as the
     * first dab moved anything and would drop nodes the falloff still reaches. */
    Array<Bounds<float3>> base_view_node_offset_bounds;
    /* Open weight-mask editing session, if any. See #SculptLayerMaskEdit. */
    SculptLayerMaskEdit mask_edit;
  } layers;

  /* Contains information used by tools and brushes that require different logic based on boundary
   * elements. Typically used for anything which needs to consider neighbor values.
   *
   * Not used for Dyntopo */
  std::unique_ptr<SculptBoundaryInfoCache> boundary_info_cache;
  SculptFakeNeighbors fake_neighbors = {};

  /* Transform operator */
  float3 pivot_pos = {};
  float4 pivot_rot = float4(0.0f, 0.0f, 0.0f, 1.0f);
  float3 pivot_scale = {};

  float3 init_pivot_pos = {};
  float4 init_pivot_rot = float4(0.0f, 0.0f, 0.0f, 1.0f);
  float3 init_pivot_scale = {};

  float3 prev_pivot_pos = {};
  float4 prev_pivot_rot = float4(0.0f, 0.0f, 0.0f, 1.0f);
  float3 prev_pivot_scale = {};

  /* World-space pivot position/rotation actively driven by the Transform tool's modal session
   * (see `sculpt_transform.cc`'s #createTransSculpt/#update_modal_transform). Kept separate from
   * #pivot_pos/#pivot_rot -- which stay LOCAL-space everywhere else in the codebase -- because
   * Blender's generic rotation math only produces a valid (non-sheared) result when the
   * TransData conjugation matrix is a pure rotation; for an object with non-uniform #Object.scale
   * that requires working in world space, not the object's own (anisotropic) local space. Every
   * object in a multi-object Transform session gets an identical copy of these two fields (there
   * is one shared world pivot for the whole group), converted back into that object's own
   * #pivot_pos/#pivot_rot every modal step. */
  float3 transform_pivot_pos_world = {};
  float4 transform_pivot_rot_world = float4(0.0f, 0.0f, 0.0f, 1.0f);

  eObjectMode mode_type;

  /**
   * ID data is older than sculpt-mode data.
   * Set #Main.is_memfile_undo_flush_needed when enabling.
   */
  bool needs_flush_to_id = false;

  /**
   * Sculpt-mode data was modified at least once this session (brush stroke, filter, hide/mask
   * edit, undo restore, ...), so the object's regular evaluated draw caches may be stale and the
   * viewport must draw from the PBVH. Set sticky alongside #needs_flush_to_id (never cleared for
   * the session's lifetime); until then the object can be drawn through the regular (much
   * cheaper) cached-mesh path -- see #BKE_sculptsession_use_pbvh_draw_for_display. This matters
   * for multi-object sculpt mode, where per-node PBVH drawing of every idle object in the mode
   * multiplies the per-frame draw cost.
   */
  bool pbvh_draw_required = false;

  /**
   * Some tools follows the shading chosen by the last used tool canvas.
   * When not set the viewport shading color would be used.
   *
   * NOTE: This setting is temporarily until paint mode is added.
   */
  bool sticky_shading_color = false;

  /**
   * Last used painting canvas key.
   */
  std::optional<std::string> last_paint_canvas_key = {};
  float3 last_normal;

  std::unique_ptr<SculptTopologyIslandCache> topology_island_cache;

 private:
  /* In general, this value is expected to be valid (non-empty) as long as the cursor is over the
   * mesh. Changing the underlying mesh type (e.g. enabling dyntopo, changing multires levels)
   * should invalidate this value.
   */
  ActiveVert active_vert_ = {};

  /* This value should always exist except when the cursor has never been over the mesh, or when
   * the underlying mesh type has changed and the last `active_vert_` value no longer corresponds
   * to a value that can be correctly interpreted */
  ActiveVert last_active_vert_ = {};

  /* Pool for texture evaluations. See #tex_pool_ensure. */
  ImagePool *tex_pool_ = nullptr;

 public:
  SculptSession();
  ~SculptSession();

  ActiveVert active_vert() const;

  ActiveVert last_active_vert() const;

  /**
   * Retrieves the corresponding index of the ActiveVert inside a mesh-sized array.
   *
   * Helpful in generic cases where we are unlikely to already be processing data in a backing-type
   * specific manner.
   *
   * \note For BMesh, a call to SCULPT_vertex_random_access_ensure is needed to get valid results.
   * \returns -1 if there is no currently active vertex.
   */
  int active_vert_index() const;
  int last_active_vert_index() const;

  /**
   * Retrieves the active vertex position.
   *
   * This method should be avoided if already working with the relevant position-backing structures
   * for each of the mesh types. In cases where we want more generic code, this abstraction helps
   * to remove boilerplate.
   *
   * \returns float3 at negative infinity if there is no currently active vertex
   */
  float3 active_vert_position(const Depsgraph &depsgraph, const Object &object) const;

  void set_active_vert(ActiveVert vert);
  void clear_active_elements(bool persist_last_active);

  /**
   * Retrieves the current persistent multires data.
   *
   * Potentially used for the layer and cloth brushes.
   *
   * \returns an empty optional if the current data cannot be used
   */
  std::optional<PersistentMultiresData> persistent_multires_data();

  /**
   * Retrieves the layer brush's uniform depth reference surface.
   *
   * \param elements_num: the number of vertices or grid elements the caller expects, used to
   * reject a base captured before a topology change.
   * \returns an empty optional if the current data cannot be used.
   */
  std::optional<LayerUniformBaseData> layer_uniform_base_data(int elements_num);

  /**
   * The pool caching the #ImBuf handles every texture sample goes through, created on first use
   * and living until the session ends.
   *
   * Ownership is the session's alone. A caller that samples a texture outside a stroke -- the
   * Curve Patch apply, Expand -- calls this and frees nothing. While this was a plain field, every
   * such caller had to create the pool for itself, and forgetting to do so dereferenced null in
   * the sampler for a brush WITH a texture while a brush without one passed unharmed.
   */
  ImagePool &tex_pool_ensure();

  /**
   * The pool as it stands, null when nothing has needed one yet.
   *
   * For the sampling paths inside a stroke, which ran #tex_pool_ensure at their start.
   */
  ImagePool *tex_pool() const;

  /**
   * Drop the cached #ImBuf handles so the next #tex_pool_ensure samples the images afresh.
   *
   * For when WHICH images are sampled changes, not when their parameters do.
   */
  void tex_pool_invalidate();
};

void BKE_sculptsession_free(Object *ob);
void BKE_sculptsession_free_deformMats(SculptSession *ss);
void BKE_sculptsession_free_pbvh(Object &object);
void BKE_sculptsession_bm_to_me(Object *ob);
void BKE_sculptsession_bm_to_me_for_render(Object *object);

/**
 * Create new color layer on object if it doesn't have one and if experimental feature set has
 * sculpt vertex color enabled. Returns truth if new layer has been added, false otherwise.
 */
void BKE_sculpt_color_layer_create_if_needed(Object *object);

/**
 * \warning Expects a fully evaluated depsgraph.
 */
void BKE_sculpt_update_object_for_edit(Depsgraph *depsgraph, Object *ob_orig, bool is_paint_tool);
void BKE_sculpt_update_object_before_eval(Object *ob_eval);
void BKE_sculpt_update_object_after_eval(Depsgraph *depsgraph, Object *ob_eval);

/**
 * Sculpt mode handles multi-res differently from regular meshes, but only if
 * it's the last modifier on the stack and it is not on the first level.
 */
MultiresModifierData *BKE_sculpt_multires_active(const Scene *scene, Object *ob);
int BKE_sculpt_get_grid_num_verts(const Object &object);
int BKE_sculpt_get_grid_num_faces(const Object &object);

/**
 * Ensures a mask layer exists. If depsgraph and bmain are non-null,
 * a mask doesn't exist and the object has a multi-resolution modifier
 * then the scene depsgraph will be evaluated to update the runtime
 * subdivision data.
 *
 * \note always call *before* #BKE_sculpt_update_object_for_edit.
 */
void BKE_sculpt_mask_layers_ensure(Depsgraph *depsgraph,
                                   Main *bmain,
                                   Object *ob,
                                   MultiresModifierData *mmd);
void BKE_sculpt_toolsettings_data_ensure(Main *bmain, Scene *scene);

void BKE_sculpt_sync_face_visibility_to_grids(const Mesh &mesh, SubdivCCG &subdiv_ccg);

/**
 * Test if bke::pbvh::Tree can be used directly for drawing, which is faster than
 * drawing the mesh and all updates that come with it.
 */
bool BKE_sculptsession_use_pbvh_draw(const Object *ob, const RegionView3D *rv3d);

/**
 * Variant of #BKE_sculptsession_use_pbvh_draw for the draw engines: additionally requires that
 * the object's sculpt data was actually modified this session (#SculptSession::pbvh_draw_required)
 * before routing it through the expensive per-node PBVH drawing. An untouched object's evaluated
 * draw caches are still valid, so the regular cached-mesh path both draws the same result and
 * avoids per-frame per-node work (frustum culling, batch validation, one draw call per node) --
 * which otherwise multiplies by the object count in multi-object sculpt mode.
 *
 * Editor logic (update tagging, flushing) must keep using #BKE_sculptsession_use_pbvh_draw: the
 * two predicates only diverge while an object is untouched, and every path that modifies sculpt
 * data sets #SculptSession::pbvh_draw_required first, so the display can never show stale data.
 */
bool BKE_sculptsession_use_pbvh_draw_for_display(const Object *ob, const RegionView3D *rv3d);

namespace bke::object {

pbvh::Tree &pbvh_ensure(Depsgraph &depsgraph, Object &object);

/**
 * Access the acceleration structure for ray-casting,
 * nearest queries, and spatially contiguous mesh updates and drawing.
 * The BVH tree is used by sculpt, vertex paint, and weight paint object modes.
 * This just accesses the BVH, to ensure it's built, use #pbvh_ensure.
 */
pbvh::Tree *pbvh_get(Object &object);
const pbvh::Tree *pbvh_get(const Object &object);

}  // namespace bke::object
bool BKE_object_sculpt_use_dyntopo(const Object *object);

/* paint_canvas.cc */

/**
 * Create a key that can be used to compare with previous ones to identify changes.
 * The resulting 'string' is owned by the caller.
 */
std::string BKE_paint_canvas_key_get(PaintModeSettings *settings,
                                     Object *ob,
                                     const Brush *brush,
                                     int visible_material_channels);

/**
 * Layout key for PBVH pixel encoding of \a image: seam margin and each tile's
 * (tile_number, width, height). Images that share this key can reuse the same
 * #bke::pbvh::pixels::PixelData without re-encoding.
 */
std::string BKE_paint_pixels_layout_key_get(Image &image,
                                            ImageUser &image_user,
                                            StringRef uv_map_name);

bool BKE_paint_canvas_image_get(PaintModeSettings *settings,
                                Object *ob,
                                Image **r_image,
                                ImageUser **r_image_user);
std::optional<StringRef> BKE_paint_canvas_uvmap_name_get(const PaintModeSettings *settings,
                                                         Object *ob);
CurveMapping *BKE_sculpt_default_cavity_curve();
CurveMapping *BKE_paint_default_curve();

/* -------------------------------------------------------------------- */
/** \name Material Painting (Poly Paint)
 *
 * Everything that distinguishes one material paint channel from another is described by the
 * descriptor table below. Adding a channel means adding an #eMaterialPaintChannel value, bumping
 * #PAINT_MATERIAL_CHANNEL_NUM and adding one table row; no call site should switch on the channel.
 * \{ */

/**
 * Static description of one material paint channel.
 *
 * \note The scalar range applies to the fixed channels only. #PAINT_MATERIAL_CHANNEL_CUSTOM
 * targets an arbitrary float attribute, so its range is user-defined and stored per scene in
 * #PaintModeSettings.channel_custom_range; use #BKE_paint_material_channel_range for a range that
 * accounts for both.
 */
struct MaterialPaintChannelInfo {
  eMaterialPaintChannel channel;
  /** Untranslated UI name, e.g. `"Metallic"`. */
  const char *ui_name;
  /**
   * Mesh attribute painted in #PAINT_CANVAS_SOURCE_MATERIAL_PAINT mode. Null for Custom, whose
   * name is entirely user-configured; see #BKE_paint_material_channel_attribute_name.
   */
  const char *attribute_name;
  /** Principled BSDF input socket for #PAINT_CANVAS_SOURCE_MATERIAL mode. Null when unmapped. */
  const char *socket_name;
  /** Inclusive value range of the scalar channels. */
  float value_min;
  float value_max;
  /**
   * True for channels written through the generic color-attribute path (not specifically Base
   * Color). All `is_color = true` channels share the same write path; which attribute gets written
   * is determined by `attribute_name`, not by an implicit "must be Base Color" assumption.
   */
  bool is_color;
  /**
   * True when the channel can be painted into a per-vertex mesh attribute in
   * #PAINT_CANVAS_SOURCE_MATERIAL_PAINT mode. Channels that only make sense as a texture map
   * (Normal, Height), that have no fixed storage (Custom) or that nothing displays per-vertex
   * (Emission) are map-only: they stay available for #PAINT_CANVAS_SOURCE_MATERIAL, but the
   * vertex canvas never creates, paints or snapshots an attribute for them.
   */
  bool supports_vertex_paint;
  /**
   * True when the channel can be painted into an image map in #PAINT_CANVAS_SOURCE_MATERIAL mode.
   *
   * This is the raster (Texture Paint) canvas, the primary one, and this flag is what decides
   * whether a channel participates in it. It is declared per channel rather than derived from
   * #socket_name so that lifting the current restriction is a local change: a channel that today
   * has no Principled input (Height, AO, Custom) only needs a way to resolve its image - a
   * different node, a user-assigned map - and this flag flipped, with no rule elsewhere to find
   * and update.
   *
   * NOTE: while every current entry with this set also has a #socket_name, code must go through
   * this flag and not test #socket_name, precisely so that the two can diverge.
   */
  bool supports_image_paint;
};

/**
 * The material paint channels, ordered by #eMaterialPaintChannel and indexable by it.
 * Base Color comes first so painting and undo process the color channel before the scalars.
 */
Span<MaterialPaintChannelInfo> BKE_paint_material_channels();

/** The single descriptor for \a channel. */
const MaterialPaintChannelInfo &BKE_paint_material_channel_info(eMaterialPaintChannel channel);

/**
 * Returns whether \a channel is active for painting: `use` is set, the channel is listed in
 * \a visible_material_channels (Custom is exempt — it uses the draw-time
 * `show_custom` gate instead), and a non-empty attribute name is available on \a mode_settings.
 */
bool BKE_paint_material_channel_is_enabled(const BrushMaterialPaint &brush_paint,
                                            const PaintModeSettings &mode_settings,
                                            int visible_material_channels,
                                            eMaterialPaintChannel channel);

/**
 * Returns whether \a channel's painted values are written to its target map/attribute this stroke.
 * For Alpha, additionally requires #BrushMaterialPaint.use_alpha_map; other channels follow
 * #BKE_paint_material_channel_is_enabled.
 */
bool BKE_paint_material_channel_writes_to_target(const BrushMaterialPaint &brush_paint,
                                                  const PaintModeSettings &mode_settings,
                                                  int visible_material_channels,
                                                  eMaterialPaintChannel channel);

/**
 * Returns whether the enabled Alpha channel should mask other channels' writes this stroke
 * (#BrushMaterialPaint.use_alpha_stroke_mask).
 */
bool BKE_paint_material_channel_masks_stroke(const BrushMaterialPaint &brush_paint,
                                              const PaintModeSettings &mode_settings,
                                              int visible_material_channels);

/**
 * Returns the scalar paint value for \a channel from \a brush_paint, clamped to the channel range
 * from \a mode_settings.
 * Base Color has no scalar value and returns 0.0f (use #BKE_paint_material_base_color_get).
 */
float BKE_paint_material_channel_value(const BrushMaterialPaint &brush_paint,
                                       const PaintModeSettings &mode_settings,
                                       eMaterialPaintChannel channel);

/**
 * Returns the blend mode a stroke should use for \a channel.
 *
 * Only Base Color is blendable. It is a color, which is what the #IMB_BlendMode operations are
 * defined on, so Multiply, Screen, Add and friends all mean what the user expects, and it is the
 * one channel where #BrushMaterialPaintChannel.blend is read.
 *
 * Every other channel uses a plain interpolation:
 * - The scalar channels (Metallic, Roughness, Specular, Custom) are data, not light. They are
 *   blended by expanding the value to a gray color, so a mode like Add or Screen would compute a
 *   photometric result for a quantity that has no such meaning, and Custom can hold values outside
 *   0..1 where those modes are not even bounded. Mix is the only operation that stays a valid
 *   interpolation between the old and new value.
 * - Normal always uses #IMB_BLEND_NORMAL_MIX, because the other modes operate per component and
 *   would produce non-unit tangents.
 * - When \a invert is set the stroke is erasing toward the channel default, which is only
 *   meaningful as a plain #IMB_BLEND_MIX interpolation.
 *
 * \return an #IMB_BlendMode, stored as a `short` the same way #Brush.blend and
 * #BrushMaterialPaintChannel.blend are. The enum lives in `IMB_imbuf.hh`, which no blenkernel
 * header includes; callers cast at the point of use, as they already do for #Brush.blend.
 */
short BKE_paint_material_channel_blend_mode(const BrushMaterialPaint &brush_paint,
                                            eMaterialPaintChannel channel,
                                            bool invert);

/**
 * Returns the channel's neutral/default scalar value — what a freshly created texture or
 * attribute is filled with, and what the eraser blends toward instead of an arbitrary brush
 * value. Base Color has no scalar default (0.0f); use #BrushMaterialPaint.base_color
 * for its reset value instead. For Normal, returns the Z of a flat tangent (1.0f); the full
 * flat tangent is `(0, 0, 1)`.
 */
float BKE_paint_material_channel_default_value(eMaterialPaintChannel channel);

/**
 * Pack a tangent-space normal for storage in an ImBuf.
 * Byte buffers use (n * 0.5 + 0.5); float buffers store n directly.
 */
void BKE_pbr_normal_pack(const float n[3], bool is_float, float r_packed[3]);

/**
 * Blend a tangent-space normal already stored in an ImBuf toward a target normal, then
 * renormalize. `current_packed` is the value read from the buffer in the same packed convention
 * selected by `is_float`. `t` is the MIX factor in [0, 1]. Renormalization is mandatory: a
 * non-unit tangent normal breaks shading.
 */
void BKE_pbr_normal_blend_mix(const float current_packed[3],
                              const float target_n[3],
                              float t,
                              bool is_float,
                              float r_packed[3]);

/**
 * Unpack a tangent-space normal sampled from a source normal map.
 *
 * Normal maps store `n * 0.5 + 0.5`, so the sample is mapped back to `[-1, 1]` and normalized:
 * a non-unit tangent normal breaks shading, the same reason #BKE_pbr_normal_blend_mix
 * renormalizes.
 *
 * \param rgb: the sampled color, expected in `[0, 1]`. Alpha is not used.
 * \param r_normal: the unit tangent normal, only written when this returns true.
 * \return false when the unpacked vector is degenerate (a mid-gray or black sample), in which
 * case the caller must fall back to the channel's own value rather than normalize a zero vector.
 */
bool BKE_paint_material_normal_from_sample(const float rgb[3], float r_normal[3]);

/**
 * Whether \a channel has a source texture assigned at all.
 *
 * This only reflects the pointer. Whether the source can actually be sampled (its image may be
 * missing or fail to load) is decided once per stroke by the paint code, not here.
 */
bool BKE_paint_material_channel_has_source(const BrushMaterialPaintChannel &channel);

/**
 * Fills \a r_mtex with \a channel's own source #Tex combined with \a brush_paint's mapping
 * shared by every channel (see #BrushMaterialPaint.shared_source_mapping: map_mode, size,
 * offset, angle). Sampling and cursor-preview code read mapping through this instead of
 * #BrushMaterialPaintChannel.source_mtex directly, so every channel's texture samples with
 * identical mapping and multi-channel patterns (a Base Color texture with a matching
 * Normal/Roughness texture) stay aligned.
 *
 * Out-parameter rather than a return value: #MTex disables copy/move (see
 * #DNA_DEFINE_CXX_METHODS) so callers own the storage and this only ever assigns into it via
 * #dna::shallow_copy.
 */
void BKE_paint_material_channel_effective_mtex(const BrushMaterialPaint &brush_paint,
                                               const BrushMaterialPaintChannel &channel,
                                               MTex &r_mtex);

/**
 * Fills \a r_mtex with the effective #MTex (see #BKE_paint_material_channel_effective_mtex) to
 * preview in the brush cursor overlay for \a brush_paint, chosen by the priority a user painting
 * a pattern expects: Base Color if enabled and sourced, else Alpha, else the first remaining
 * enabled sourced channel, with Normal always last.
 *
 * \a visible_material_channels is the owning #Paint's channel set, so a hidden channel is never
 * previewed; pass #PAINT_MATERIAL_CHANNELS_VISIBLE_ALL where no #Paint is in context (radial
 * control only has the #Brush).
 *
 * \return false when no enabled channel has a source, in which case \a r_mtex is zeroed and the
 * cursor should fall back to the brush's own texture.
 */
bool BKE_paint_material_preview_mtex_get(const BrushMaterialPaint &brush_paint,
                                         const PaintModeSettings &mode_settings,
                                         int visible_material_channels,
                                         MTex &r_mtex);

/**
 * Returns the valid value range for \a channel: the descriptor range for the fixed channels and
 * #PaintModeSettings.channel_custom_range for Custom.
 */
float2 BKE_paint_material_channel_range(const PaintModeSettings &settings,
                                        eMaterialPaintChannel channel);

/** Finds the preset in \a scene matching \a brush's identity, or null if none exists yet.
 *  See #PaintMaterialBrushPreset for the matching rule. */
PaintMaterialBrushPreset *BKE_paint_material_brush_preset_find(Scene &scene, const Brush &brush);

/** Like #BKE_paint_material_brush_preset_find, but creates and appends a new preset —
 *  seeded from \a brush's current #Brush.material_paint state, or from
 *  #BKE_brush_material_paint_ensure's defaults if \a brush has none yet — when none is
 *  found. Never returns null. */
PaintMaterialBrushPreset *BKE_paint_material_brush_preset_ensure(Scene &scene, const Brush &brush);

/** Overwrites \a brush's #Brush.material_paint from the preset in \a scene matching \a brush's
 *  identity. No-op if there is no preset yet — does not allocate #Brush.material_paint or create
 *  a default preset. Opt-in is #PAINT_OT_material_paint_brush_ensure. */
void BKE_paint_material_brush_preset_apply(Scene &scene, Brush &brush);

/** Overwrites the preset in \a scene matching \a brush's identity (creating it if needed) from
 *  \a brush's current #Brush.material_paint. No-op — does not create or touch any preset — if
 *  \a brush.material_paint is null. */
void BKE_paint_material_brush_preset_snapshot(Scene &scene, const Brush &brush);

/** Frees \a preset and everything it owns. No-op if \a preset is null. The caller is responsible
 *  for unlinking it from its list first.
 *  \param do_user_refcount: forwarded to #BKE_brush_material_paint_free. True for remove/purge
 *  (no ID foreach_id will run). False when freeing a Scene (foreach_id already dropped the Tex
 *  counts). */
void BKE_paint_material_brush_preset_free(PaintMaterialBrushPreset *preset,
                                          bool do_user_refcount = false);

/** Removes and frees the preset in \a scene matching \a brush's identity, if any. Call when a
 *  brush is deleted, so its preset does not outlive it and keep #Tex references alive. */
void BKE_paint_material_brush_preset_remove(Scene &scene, const Brush &brush);

/**
 * Brings every Scene's preset list in \a bmain into the state that should be written to disk:
 * snapshots the currently active brush of every material-paint-capable Paint (Sculpt, Image
 * Paint) — so edits made without ever switching away from the active brush are not lost — and
 * drops presets that can no longer belong to any brush. Intended to run immediately before a
 * blend-file write, alongside #ed::asset::pre_save_assets().
 */
void BKE_paint_material_brush_presets_prepare_for_save(Main *bmain);

/* Cross-mode brush synchronization lives in #BKE_paint_material_sync.hh. */

/**
 * Gradient shape for the scalar material-paint value ramp widget.
 * Unipolar: [0,1] black→white. Bipolar: [-1,1] white→black→white.
 */
enum class MaterialPaintValueGradientMode {
  Unipolar, /* [0,1] black→white */
  Bipolar,  /* [-1,1] white→black→white */
};

MaterialPaintValueGradientMode BKE_paint_material_value_gradient_mode(float value_min,
                                                                      float value_max);

/** t in [0,1] → RGB in [0,1]. */
void BKE_paint_material_value_gradient_color(float value_min,
                                             float value_max,
                                             float t,
                                             float r_rgb[3]);

float BKE_paint_material_value_from_t(float value_min, float value_max, float t);
float BKE_paint_material_t_from_value(float value_min, float value_max, float value);

/** value' = clamp(min+max-value, min, max) after computing mirror. */
float BKE_paint_material_value_invert(float value_min, float value_max, float value);

/**
 * Returns the mesh attribute name for \a channel via
 * #PaintModeSettings.channel_layer_bindings: an add-on-managed layer override (falling back to
 * the fixed descriptor name) for the fixed channels, or the user-configured name for Custom. May
 * be empty for an unconfigured Custom channel.
 */
StringRef BKE_paint_material_channel_attribute_name(const PaintModeSettings &settings,
                                                    eMaterialPaintChannel channel);

/**
 * Returns the channel whose fixed attribute name is \a attribute_name, if any. Never matches
 * Custom, whose name is not fixed. Used by the draw engines to decide whether a mesh attribute is
 * a material paint channel without hard-coding names.
 */
std::optional<eMaterialPaintChannel> BKE_paint_material_channel_from_attribute_name(
    StringRef attribute_name);

/**
 * Override-aware inverse of #BKE_paint_material_channel_attribute_name: the channel \a
 * attribute_name currently feeds, taking #PaintModeSettings.channel_layer_bindings into account.
 *
 * This is the form the draw engines must use. A redirected channel is fed by its bound name and
 * no longer by its built-in one, so resolving through the fixed table alone would both miss the
 * layer attribute a stroke actually writes and keep claiming an abandoned one. Unlike the
 * settings-free overload this can match Custom, whose name only exists in the bindings.
 */
std::optional<eMaterialPaintChannel> BKE_paint_material_channel_from_attribute_name(
    const PaintModeSettings &settings, StringRef attribute_name);

/**
 * Effective RGB for Base Color strokes: invert uses brush secondary color;
 * otherwise #BrushMaterialPaint.base_color.
 */
float3 BKE_paint_material_base_color_get(const BrushMaterialPaint &brush_paint,
                                         const Paint &paint,
                                         const Brush &brush,
                                         bool invert);

/**
 * Effective RGB for a color channel (#MaterialPaintChannelInfo.is_color).
 * Base Color delegates to #BKE_paint_material_base_color_get. Other color channels (Emission)
 * read #BrushMaterialPaintChannel.value; invert interpolates toward the channel default.
 */
float3 BKE_paint_material_channel_color_get(const BrushMaterialPaint &brush_paint,
                                            const Paint &paint,
                                            const Brush &brush,
                                            eMaterialPaintChannel channel,
                                            bool invert);

/**
 * Clears per-material Principled-socket image resolution cache entries.
 * Call after node-tree or material slot changes that can alter resolved targets.
 */
void BKE_paint_material_channel_cache_invalidate(Material *ma);

/**
 * Resolves the Image Texture directly linked to the Principled BSDF input for
 * \a channel on the active material of \a ob.
 * Channels without a socket (Custom) always return false. Only a direct link is followed;
 * anything routed through an intermediate node (Math, Mix, etc.) is left alone, since there is no
 * single image such a chain could be said to paint into.
 * \param mode_settings: When given and \a channel has a non-null
 * #PaintModeSettings.channel_image_bindings entry, that Image is returned directly and the
 * Principled BSDF socket is never consulted. Null (the default) preserves the socket-only
 * resolution every existing caller relied on before this override existed.
 * \return true when \a r_image and \a r_iuser were set.
 */
bool BKE_paint_principled_channel_image_get(Object &ob,
                                            eMaterialPaintChannel channel,
                                            Image **r_image,
                                            ImageUser **r_iuser,
                                            PaintModeSettings *mode_settings = nullptr);

/**
 * Image the Image Editor should show for the Material canvas when nothing is selected.
 *
 * Prefers Base Color, then other created Principled maps, with Normal and Alpha last.
 * Returns null when no paintable map is wired on the active material.
 */
Image *BKE_paint_material_preferred_display_image(Object &ob);

/**
 * Same as #BKE_paint_principled_channel_image_get, but when \a channel's Principled
 * socket exists and has no incoming link, creates a generated blank Image with the color space
 * required by the channel, adds an Image Texture node for it, and links it to the socket, then
 * returns it.
 * Used by #PAINT_OT_material_paint_images_ensure and by Material-canvas stroke init when a
 * writable channel has no map yet. Created Image IDs persist if the stroke itself is undone.
 * Does nothing and returns false when there is no material, no node tree, no Principled
 * BSDF, no matching socket, or the socket is already driven by something other than a
 * (missing/invalid) Image Texture. Channels without a socket (Custom) always return false.
 * \param image_size: Width and height (in pixels) used for a newly created image. Ignored when
 * the channel already resolves to an existing image.
 * \param mode_settings: Forwarded to #BKE_paint_principled_channel_image_get; when \a channel has
 * an image override, it is returned as-is and nothing is created or wired into the shader graph.
 * \return true when \a r_image and \a r_iuser were set.
 */
bool BKE_paint_principled_channel_image_ensure(Main &bmain,
                                               Object &ob,
                                               eMaterialPaintChannel channel,
                                               int image_size,
                                               Image **r_image,
                                               ImageUser **r_iuser,
                                               PaintModeSettings *mode_settings = nullptr);

/**
 * Create missing Principled maps for every channel this brush currently writes to. Channels with
 * a #PaintModeSettings.channel_image_bindings override never create anything - the add-on manages
 * that Image's lifetime itself.
 * \return number of newly created images.
 */
int BKE_paint_material_images_ensure_writable(Main &bmain,
                                               Object &ob,
                                               const BrushMaterialPaint &brush_paint,
                                               PaintModeSettings &mode_settings,
                                               int visible_material_channels);

/**
 * When channels are newly shown in a #Paint's visible material channels, also enable
 * their per-brush `use` flag so they can be painted and assigned a source immediately.
 */
void BKE_paint_material_enable_added_visible_channels(Paint &paint, int added_channel_bits);

/**
 * One Principled map target for Mode=`Material` multi-channel image paint.
 * Scalar \a value is written as RGB `(v, v, v)`.
 * Base Color and Normal use \a color with \a is_color_channel / \a is_normal_channel set.
 */
struct PaintMaterialImageTarget {
  eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_METALLIC;
  Image *image = nullptr;
  ImageUser *iuser = nullptr;
  float value = 0.0f;
  float color[3] = {0.0f, 0.0f, 0.0f};
  bool is_color_channel = false;
  bool is_normal_channel = false;
};

/**
 * Collects enabled channels that successfully resolve to a target Image on \a ob - either a
 * Principled Image Texture, or an add-on's #PaintModeSettings.channel_image_bindings override.
 * Missing maps are skipped. Channels without a socket (Custom) are never included.
 * Order follows #BKE_paint_material_channels.
 * When \a brush_paint is null, returns an empty list (no channels enabled).
 */
Vector<PaintMaterialImageTarget> BKE_paint_material_image_targets_get(
    Object &ob,
    PaintModeSettings &mode_settings,
    const BrushMaterialPaint *brush_paint,
    int visible_material_channels);

/**
 * Whether a face with \a face_material_index should receive image writes while painting
 * Mode=Material with the object's current active slot (#Object.actcol).
 * When the object has only one material slot, all faces match.
 */
bool BKE_paint_material_face_matches_active_slot(const Object &ob, int face_material_index);

/** Why a material paint channel could not be prepared on a mesh. */
enum class MaterialPaintAttributeStatus {
  /** The attribute exists with the required type and domain, or was just created. */
  Ok,
  /** An attribute of that name exists with an incompatible type or domain. */
  TypeMismatch,
  /** The name is empty (an unconfigured Custom channel). */
  InvalidName,
  /** Creating the attribute failed. */
  CreationFailed,
};

/**
 * Ensure a float point attribute named \a attr_name exists on \a mesh.
 * \param r_created: when non-null, set to true only if a new attribute was added by this call.
 */
MaterialPaintAttributeStatus BKE_paint_mesh_material_attribute_ensure(Mesh &mesh,
                                                                      StringRef attr_name,
                                                                      bool *r_created = nullptr);

/**
 * Ensure the color attribute of \a channel (which must have `is_color` set) exists on the mesh.
 * Accepts an existing Point- or Corner-domain ColorFloat or ColorByte; otherwise creates a
 * Point-domain ColorFloat. Only #PAINT_MATERIAL_CHANNEL_BASE_COLOR is additionally made the
 * active and default color attribute.
 * \param r_created: when non-null, set to true only if a new attribute was added by this call.
 */
MaterialPaintAttributeStatus BKE_paint_mesh_material_color_attribute_ensure(
    Mesh &mesh, eMaterialPaintChannel channel, bool *r_created = nullptr);

/**
 * Ensures \a attribute_name exists on \a mesh as a color attribute (Point or Corner domain,
 * ColorFloat or ColorByte type), creating it with default values if missing. Unlike
 * #BKE_paint_mesh_material_color_attribute_ensure, this targets an arbitrary attribute name
 * instead of a channel's fixed one, and never changes the mesh's active/default color attribute -
 * that would be wrong for an add-on-managed layer attribute, which is not "the" mesh color.
 */
MaterialPaintAttributeStatus BKE_paint_mesh_material_color_attribute_ensure_named(
    Mesh &mesh, StringRef attribute_name, bool *r_created = nullptr);

/**
 * Human-readable reason for a non-#MaterialPaintAttributeStatus::Ok status, for operator reports.
 * Returns an untranslated format-free message; \a attr_name is not embedded.
 */
const char *BKE_paint_material_attribute_status_message(MaterialPaintAttributeStatus status);

/** \} */

}  // namespace blender
