/* SPDX-FileCopyrightText: 2009 by Nicholas Bishop. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 */

#include <variant>

#include "BLI_array.hh"
#include "BLI_bit_vector.hh"
#include "BLI_enum_flags.hh"
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

PaintCurve *BKE_paint_curve_add(Main *bmain, const char *name);

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

/* Paint brush retrieval and assignment. */

Brush *BKE_paint_brush(Paint *paint);
const Brush *BKE_paint_brush_for_read(const Paint *paint);
Brush *BKE_paint_brush_from_essentials(Main *bmain, PaintMode paint_mode, const char *name);

/**
 * Check if brush \a brush may be set/activated for \a paint. Passing null for \a brush will return
 * true.
 */
bool BKE_paint_can_use_brush(const Paint *paint, const Brush *brush);

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
void BKE_paint_curve_clamp_endpoint_add_index(PaintCurve *pc, int add_index);

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

void BKE_paint_face_set_overlay_color_get(int face_set, int seed, uchar r_color[4]);

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

  /* Pool for texture evaluations. */
  ImagePool *tex_pool = nullptr;

  ed::sculpt_paint::StrokeCache *cache = nullptr;
  ed::sculpt_paint::filter::Cache *filter_cache = nullptr;
  ed::sculpt_paint::expand::Cache *expand_cache = nullptr;

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

  eObjectMode mode_type;

  /**
   * ID data is older than sculpt-mode data.
   * Set #Main.is_memfile_undo_flush_needed when enabling.
   */
  bool needs_flush_to_id = false;

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
std::string BKE_paint_canvas_key_get(PaintModeSettings *settings, Object *ob, const Brush *brush);

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
   * name comes from #PaintModeSettings.material_paint_custom_attr.
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
 * #PaintModeSettings.visible_material_channels (Custom is exempt — it uses the draw-time
 * `show_custom` gate instead), and a non-empty attribute name is available on \a mode_settings.
 */
bool BKE_paint_material_channel_is_enabled(const BrushMaterialPaint &brush_paint,
                                           const PaintModeSettings &mode_settings,
                                           eMaterialPaintChannel channel);

/**
 * Returns whether \a channel's painted values are written to its target map/attribute this stroke.
 * For Alpha, additionally requires #BrushMaterialPaint.use_alpha_map; other channels follow
 * #BKE_paint_material_channel_is_enabled.
 */
bool BKE_paint_material_channel_writes_to_target(const BrushMaterialPaint &brush_paint,
                                                 const PaintModeSettings &mode_settings,
                                                 eMaterialPaintChannel channel);

/**
 * Returns whether the enabled Alpha channel should mask other channels' writes this stroke
 * (#BrushMaterialPaint.use_alpha_stroke_mask).
 */
bool BKE_paint_material_channel_masks_stroke(const BrushMaterialPaint &brush_paint,
                                             const PaintModeSettings &mode_settings);

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

/** True for color-managed channels; false for data channels (Metallic, Roughness, Specular,
 * Normal). */
inline bool BKE_paint_material_channel_is_color(eMaterialPaintChannel channel)
{
  return channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR;
}

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
 * a pattern expects: Base Color if enabled and sourced, else the first of Metallic, Roughness,
 * Normal, Height, Specular that is.
 *
 * \return false when no enabled channel has a source, in which case \a r_mtex is zeroed and the
 * cursor should fall back to the brush's own texture.
 */
bool BKE_paint_material_preview_mtex_get(const BrushMaterialPaint &brush_paint,
                                         const PaintModeSettings &mode_settings,
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

/** Overwrites \a brush's #Brush.material_paint (creating it via
 *  #BKE_brush_material_paint_ensure if needed) from the preset in \a scene matching \a brush's
 *  identity, creating that preset (seeded from \a brush's current state) if none exists yet. */
void BKE_paint_material_brush_preset_apply(Scene &scene, Brush &brush);

/** Overwrites the preset in \a scene matching \a brush's identity (creating it if needed) from
 *  \a brush's current #Brush.material_paint. No-op — does not create or touch any preset — if
 *  \a brush.material_paint is null. */
void BKE_paint_material_brush_preset_snapshot(Scene &scene, const Brush &brush);

/** Frees \a preset and everything it owns. No-op if \a preset is null. The caller is responsible
 *  for unlinking it from its list first. */
void BKE_paint_material_brush_preset_free(PaintMaterialBrushPreset *preset);

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

/**
 * The #Paint that should mirror \a source while brush sync is active, or null when nothing should
 * be synced.
 *
 * Returns null unless \a source is one of the Sculpt / Image Paint pair, brush sync is enabled and
 * the canvas is #PAINT_CANVAS_SOURCE_MATERIAL. #PAINT_CANVAS_SOURCE_MATERIAL_PAINT is deliberately
 * excluded: it writes per-vertex attributes the Image Editor never paints.
 */
Paint *BKE_paint_material_sync_target_get(Scene *scene, Paint *source);

/**
 * Make the paint mode paired with \a source use the same brush, palette and cavity curve.
 *
 * The brush is shared as one ID rather than copied, so its settings cannot drift apart. Does not
 * touch the tool system: callers with a #bContext are responsible for the receiving side's tool
 * bindings.
 */
void BKE_paint_material_brush_sync(Scene *scene, Paint *source);

/**
 * Mirror \a source's #UnifiedPaintSettings (size, strength, color, jitter) onto the paired paint
 * mode. Kept separate from #BKE_paint_material_brush_sync because these change far more often than
 * the active brush and are driven from a different callback.
 */
void BKE_paint_material_unified_settings_sync(Scene *scene, Paint *source);

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
 * Returns the mesh attribute name for \a channel: the descriptor name for the fixed channels and
 * #PaintModeSettings.material_paint_custom_attr for Custom. May be empty for an unconfigured
 * Custom channel.
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
 * Effective RGB for Base Color strokes: invert uses brush secondary color;
 * otherwise #BrushMaterialPaint.base_color.
 */
float3 BKE_paint_material_base_color_get(const BrushMaterialPaint &brush_paint,
                                         const Paint &paint,
                                         const Brush &brush,
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
 * \return true when \a r_image and \a r_iuser were set.
 */
bool BKE_paint_principled_channel_image_get(Object &ob,
                                            eMaterialPaintChannel channel,
                                            Image **r_image,
                                            ImageUser **r_iuser);

/**
 * Same as #BKE_paint_principled_channel_image_get, but when \a channel's Principled
 * socket exists and has no incoming link, creates a generated blank Image with the color space
 * required by the channel, adds an Image Texture node for it, and links it to the socket, then
 * returns it.
 * Does nothing and returns false when there is no material, no node tree, no Principled
 * BSDF, no matching socket, or the socket is already driven by something other than a
 * (missing/invalid) Image Texture. Channels without a socket (Custom) always return false.
 * \param image_size: Width and height (in pixels) used for a newly created image. Ignored when
 * the channel already resolves to an existing image.
 * \return true when \a r_image and \a r_iuser were set.
 */
bool BKE_paint_principled_channel_image_ensure(Main &bmain,
                                               Object &ob,
                                               eMaterialPaintChannel channel,
                                               int image_size,
                                               Image **r_image,
                                               ImageUser **r_iuser);

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
 * Collects enabled channels that successfully resolve to a Principled Image Texture on \a ob.
 * Missing maps are skipped. Channels without a socket (Custom) are never included.
 * Order follows #BKE_paint_material_channels.
 * When \a brush_paint is null, returns an empty list (no channels enabled).
 */
Vector<PaintMaterialImageTarget> BKE_paint_material_image_targets_get(
    Object &ob, const PaintModeSettings &mode_settings, const BrushMaterialPaint *brush_paint);

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
 * Human-readable reason for a non-#MaterialPaintAttributeStatus::Ok status, for operator reports.
 * Returns an untranslated format-free message; \a attr_name is not embedded.
 */
const char *BKE_paint_material_attribute_status_message(MaterialPaintAttributeStatus status);

/** \} */

}  // namespace blender
