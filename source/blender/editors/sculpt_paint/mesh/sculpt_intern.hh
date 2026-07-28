/* SPDX-FileCopyrightText: 2006 by Nicholas Bishop. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#pragma once

#include <optional>

#include "BKE_brush.hh"
#include "BKE_bvhutils.hh"
#include "BKE_image_wrappers.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_subdiv_ccg.hh"

#include "BLI_array.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_quaternion_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "IMB_colormanagement.hh"

#include "DNA_brush_enums.h"
#include "DNA_brush_types.h"

#include "ED_view3d.hh"

namespace blender {

namespace ed::sculpt_paint {
namespace auto_mask {
struct Cache;
}
namespace boundary {
struct SculptBoundary;
}
namespace cloth {
struct SimulationData;
}
namespace pose {
struct IKChain;
}
namespace undo {
struct Node;
enum class Type : int8_t;
}  // namespace undo
}  // namespace ed::sculpt_paint
struct bContext;
struct BMLog;
struct Dial;
struct DistRayAABB_Precalc;
struct Image;
struct ImageUser;
struct Key;
struct KeyBlock;
struct Object;
struct PaintModeSettings;
struct SculptLayer;
struct SculptLayerGroup;
struct SculptLayerTreeNode;
struct ReportList;
struct wmKeyConfig;
struct wmKeyMap;
struct wmOperator;
struct wmOperatorType;

/* -------------------------------------------------------------------- */
/** \name Sculpt Types
 * \{ */

namespace ed::sculpt_paint {

/** Contains shape key array data for quick access for deformation. */
struct ShapeKeyData {
  MutableSpan<float3> active_key_data;
  bool basis_key_active;
  Vector<MutableSpan<float3>> dependent_keys;

  static std::optional<ShapeKeyData> from_object(Object &object);
};

/**
 * This class represents an API to deform original positions based on translations created from
 * evaluated positions. It should be constructed once outside of a parallel context.
 */
class PositionDeformData {
 public:
  /**
   * Positions from after procedural deformation from modifiers, used to build the
   * pbvh::Tree. Translations are built for these values, then applied to the original positions.
   * When there are no deforming modifiers, this will reference the same array as #orig.
   */
  Span<float3> eval;

 private:
  /**
   * In some cases deformations must also apply to the evaluated positions (#eval) in case the
   * changed values are needed elsewhere before the object is reevaluated (which would update the
   * evaluated positions).
   */
  std::optional<MutableSpan<float3>> eval_mut_;

  /**
   * Transforms from deforming modifiers, used to convert translations of evaluated positions to
   * "original" translations.
   */
  std::optional<Span<float3x3>> deform_imats_;

  /**
   * Positions from the original mesh. Not the same as #eval if there are deform modifiers.
   */
  MutableSpan<float3> orig_;

  std::optional<ShapeKeyData> shape_key_data_;

  /**
   * When a mesh sculpt-layer stroke is being recorded, the active layer's per-vertex offset buffer.
   * The translation applied to #orig_ is accumulated into it per dab (see #deform), so the
   * end-of-stroke recording does not have to rewrite the layer with a full random-access scatter
   * of the whole brushed area. Empty when no mesh layer is being recorded.
   */
  MutableSpan<float3> layer_record_data_;

  /** Accumulate \a translations into #layer_record_data_ at \a verts. No-op when not recording. */
  void record_layer_offsets(Span<int> verts, Span<float3> translations) const;

 public:
  PositionDeformData(const Depsgraph &depsgraph, Object &object_orig);
  void deform(MutableSpan<float3> translations, Span<int> verts) const;
};

/**
 * Temporarily override #ViewContext.obact for the duration of a scope; restores the previous value
 * on destruction.
 *
 * Used by multi-object ("global") sculpt code paths that must call brush / sculpt helpers which
 * internally read #vc.obact (e.g. #paint_calc_object_space_radius, the `raycast_init` BVH path in
 * #stroke_get_location_object) and need to be redirected to a per-iteration object for one stroke
 * step. Without an RAII guard, a `continue` / early `return` in the protected scope would leave
 * `#vc.obact` pointing at the wrong object for the rest of the frame; the compiler and any
 * `#BLI_assert` (which is a no-op in Release) cannot catch that.
 *
 * \note This is for **temporary** per-iteration overrides only. The permanent per-stroke switch of
 *       `PaintStroke::object` + `#vc.obact` performed by `SculptPaintStroke::get_location` when
 *       the cursor moves over a different mesh is a deliberate state change of the stroke, not
 *       an override, and must NOT be wrapped in this guard.
 */
class ScopedObactOverride {
  ViewContext &vc_;
  Object *const saved_obact_;

 public:
  ScopedObactOverride(ViewContext &vc, Object &new_obact) : vc_(vc), saved_obact_(vc.obact)
  {
    vc_.obact = &new_obact;
  }
  ~ScopedObactOverride()
  {
    vc_.obact = saved_obact_;
  }

  /* The dtor must undo the constructor's swap; forbid copies and moves so we never end up with a
   * guard whose saved pointer became stale or whose swap was performed into a different lifetime.
   */
  ScopedObactOverride(const ScopedObactOverride &) = delete;
  ScopedObactOverride &operator=(const ScopedObactOverride &) = delete;
  ScopedObactOverride(ScopedObactOverride &&) = delete;
  ScopedObactOverride &operator=(ScopedObactOverride &&) = delete;
};

enum class UpdateType {
  Position,
  Mask,
  Visibility,
  Color,
  Image,
  FaceSet,
};

static constexpr int face_set_none_id = 0;

/* Factor of brush to have rake point following behind
 * (could be configurable but this is reasonable default). */
#define SCULPT_RAKE_BRUSH_FACTOR 0.25f

struct SculptRakeData {
  float follow_dist = 0.0f;
  float3 follow_co = float3(0);
  float angle = 0.0f;
};

enum class TransformDisplacementMode {
  /* Displaces the elements from their original coordinates. */
  Original = 0,
  /* Displaces the elements incrementally from their previous position. */
  Incremental = 1,
};
/* Defines how transform tools are going to apply its displacement. */

static constexpr int plane_brush_max_rolling_average_num = 20;

struct ProjectBrushTarget {
  bke::BVHTreeFromMesh tree_data;
  float4x4 active_to_target_matrix;
};

namespace paint::image {

struct TileColorspaceProcessor : NonCopyable {
  ColormanageProcessor buffer_to_linear_processor = {};
  ColormanageProcessor linear_to_buffer_processor = {};
  bool is_noop = true;
};

struct ImageData : NonCopyable {
  Image *image = nullptr;
  ImageUser *image_user = nullptr;

  Map<bke::image::TileNumber, ImBuf *> buffers = {};
  Map<bke::image::TileNumber, TileColorspaceProcessor> processors = {};

  ~ImageData();

  static std::unique_ptr<ImageData> init_active_image(Object &ob,
                                                      PaintModeSettings &paint_mode_settings);
};

}  // namespace paint::image

struct StrokeToggleSettings {
  /**
   * Whether the modifier key that controls inverting brush behavior is active currently.
   *
   * \see BrushStrokeMode::Invert.
   */
  bool invert = false;

  /**
   * Whether the modifier key that controls smoothing is active currently.
   *
   * \see BrushSwitchMode::Smooth.
   */
  bool alt_smooth = false;

  /**
   * Whether the modifier key that controls masking is active currently.
   * Switches the active brush to the mask brush during the stroke.
   *
   * \see BrushSwitchMode::Mask.
   */
  bool alt_mask = false;

  Brush *original_active_brush = nullptr;
  BrushMaskTool original_brush_mask_tool = BRUSH_MASK_DRAW;
  int original_brush_size = 0;
};

/**
 * This structure contains all the temporary data
 * needed for individual brush strokes.
 */
struct StrokeCache {
  /* Invariants */
  float initial_radius = 0.0f;
  float3 scale = float3(0);
  /**
   * Per-axis correction for local-space POSITION DIFFERENCES (falloff distance, slide
   * direction) under non-uniform #Object.scale: `ob.scale[axis] / mat4_to_scale(world matrix)`.
   * This is the opposite relationship from #scale (`max_scale / ob.scale[axis]`), which corrects
   * NORMAL/direction vectors via the inverse-transpose rule. Distances/positions transform
   * directly by the object's scale, not its inverse; using #scale here would invert the
   * correction. #cache.radius is defined as `screen_radius / mat4_to_scale(world matrix)`
   * (#paint_calc_object_space_radius), so this factor makes `length((p - center) *
   * position_scale) < cache.radius` match a true isotropic world-space sphere test.
   */
  float3 position_scale = float3(1);
  /**
   * True when the stroke spans more than one object (#MultiObjectStrokeContext::mode_objects).
   * Used by multi-object-only mechanisms unrelated to scale (pooled area sampling, shared
   * symmetry/texture frames); for the non-uniform-scale compensation gate see
   * #non_uniform_scale_active instead.
   */
  bool multi_object_stroke = false;
  /**
   * True when the non-uniform-scale compensation in
   * #scale_normalized()/#position_scale_normalized() should engage: either #multi_object_stroke,
   * or a single object whose own #Object.scale is anisotropic (#object_has_non_uniform_scale). A
   * uniformly-scaled (or unscaled) single-object stroke stays bit-exact with its pre-correction
   * behavior. Seeded once per object in #stroke_cache_init.
   */
  bool non_uniform_scale_active = false;
  struct {
    uint8_t flag = 0;
    float3 tolerance = float3(0);
    float4x4 mat = float4x4::identity();
    float4x4 mat_inv = float4x4::identity();
  } mirror_modifier_clip;
  float2 initial_mouse = float2(0);

  /**
   * Some brushes change behavior drastically depending on the directional value (i.e. the smooth
   * and enhance details functionality being bound to the Smooth brush).
   *
   * Storing the initial direction allows discerning the behavior without checking the sign of the
   * brush direction at every step, which would have ambiguity at 0.
   */
  bool initial_direction_flipped = false;

  StrokeToggleSettings toggle_settings = {};

  /* Variants */
  float radius = 0.0f;
  float radius_squared = 0.0f;
  float3 location = float3(0);
  float3 last_location = float3(0);
  float3 location_symm = float3(0);
  float3 last_location_symm = float3(0);
  float stroke_distance = 0.0f;

  /* Multi-object ("global") sculpt: shared surface sampling for area-/plane-based brushes.
   *
   * When a stroke spans more than one mesh object, #calc_area_normal, #calc_area_center and
   * #calc_area_normal_and_center pool the vertices of every object in #multi_object_sample_objects
   * into #multi_object_sample_reference's local space and convert the resulting normal/center back
   * into the requesting object's space. This makes Draw (area), Clay, Clay Strips, Plane, Flatten,
   * Multiplane Scrape, etc. see one shared surface like a single joined mesh, instead of each
   * object sampling only itself. Empty / null in single-object mode and only honored for
   * #bke::pbvh::Type::Mesh. Refreshed every #update_step; the span is owned by the paint stroke.
   */
  Span<Object *> multi_object_sample_objects;
  const Object *multi_object_sample_reference = nullptr;

  /**
   * Maps this object's local coordinates into the shared texture-sampling space (the primary
   * object's local space) so 3D brush textures (#MTEX_MAP_MODE_3D) read the same values across
   * all objects of a multi-object stroke, like on a joined mesh. Identity in single-object mode
   * and for the primary object itself.
   */
  float4x4 texture_sample_from_object = float4x4::identity();

  /**
   * Shared symmetry origin (multi-object sculpt, #PAINT_SYMMETRY_SHARED_ORIGIN).
   *
   * When mirroring the brush across a single symmetry plane shared by the whole stroke, the
   * mirror must happen in the primary (reference) object's local space and the result be brought
   * back into this object's space. #symm_ref_from_cur maps this object's local coordinates into
   * the reference object's local space; #symm_cur_from_ref is its inverse. Both are identity for
   * the primary object, in single-object mode, and when the option is disabled, which keeps the
   * per-object symmetry path bit-exact.
   */
  float4x4 symm_ref_from_cur = float4x4::identity();
  float4x4 symm_cur_from_ref = float4x4::identity();
  /* True only for objects other than the symmetry reference in a multi-object stroke while
   * #PAINT_SYMMETRY_SHARED_ORIGIN is on. When false (reference object, single-object stroke,
   * option off) brush data is mirrored around this object's own origin exactly as in single-object
   * mode. */
  bool symm_shared_origin_active = false;
  /* Reference object whose local space defines the single shared symmetry plane for the whole
   * stroke (#PAINT_SYMMETRY_SHARED_ORIGIN). This is the active object — the one a #Join would
   * merge everything into — so the plane stays fixed instead of following the cursor between
   * meshes. The symmetry flag set and radial counts are read from its mesh. Null when the option
   * is off or in single-object mode. */
  const Object *symm_reference_object = nullptr;

  /**
   * Used for alternating between deformations in brushes that need to apply different ones to
   * achieve certain effects.
   */
  int iteration_count = 0;

  /* Original pixel radius with the pressure curve applied for dyntopo detail size */
  float dyntopo_pixel_radius = 0.0f;

  bool is_last_valid = false;

  float pressure = 0.0f;
  float hardness = 0.0f;
  /**
   * Depending on the mode, can either be the raw brush strength, or a scaled (possibly negative)
   * value.
   *
   * \see #brush_strength for Sculpt Mode.
   */
  float bstrength = 0.0f;
  float2 tilt = float2(0);

  /**
   * Position of the mouse corresponding to the stroke location, modified by the paint_stroke
   * operator according to the stroke type.
   */
  float2 mouse = float2(0);
  /* Position of the mouse event in screen space, not modified by the stroke type. */
  float2 mouse_event = float2(0);

  struct {
    Array<float3> prev_displacement;
    Array<float3> limit_surface_co;
  } displacement_smear;

  /* Erase Layer (grid domain only): the active sculpt layer's own object-space contribution
   * (`BKE_multires_sculpt_layer_object_contribution`, at influence 1.0 with its mask honored)
   * computed once on first use and reused for every dab of the stroke — the call walks every
   * grid in the CCG, so recomputing it per dab would be a whole-object cost paid every brush
   * update. Empty until first used; sized to `subdiv_ccg.positions.size()` once computed. */
  struct {
    Array<float3> object_space_contribution;
  } layer_eraser;

  /* The rest is temporary storage that isn't saved as a property */

  /* Store initial starting points for perlin noise on the beginning of each stroke when using
   * color jitter. */
  std::optional<float3> initial_hsv_jitter;
  /* Beginning of stroke may do some things special. */
  bool first_time = false;

  /* from ED_view3d_ob_project_mat_get(). */
  float4x4 projection_mat = float4x4::identity();

  /* TODO: Clean this up! */
  ViewContext *vc = nullptr;
  const Brush *brush = nullptr;
  const Paint *paint = nullptr;

  float special_rotation = 0.0f;
  float3 grab_delta = float3(0);
  float3 grab_delta_symm = float3(0);
  float3 old_grab_location = float3(0);
  float3 orig_grab_location = float3(0);

  /* Screen-space rotation defined by mouse motion. */
  std::optional<math::Quaternion> rake_rotation;
  std::optional<math::Quaternion> rake_rotation_symm;
  SculptRakeData rake_data;

  /* The face set being painted. */
  int paint_face_set = face_set_none_id;

  /* The symmetry pass we are currently on between 0 and 7. */
  ePaintSymmetryFlags mirror_symmetry_pass = ePaintSymmetryFlags(0);
  float3 view_normal = float3(0);
  float3 view_normal_symm = float3(0);
  float3 view_origin = float3(0);
  float3 view_origin_symm = float3(0);

  /**
   * The primary direction of influence for a brush stroke.
   *
   * May be unused for some brushes (e.g. Smooth)
   * May be only calculated at the beginning of a stroke (e.g. Grab)
   *
   * Calculated by either #calc_sculpt_normal or #calc_brush_plane.
   */
  float3 sculpt_normal = float3(0);
  float3 sculpt_normal_symm = float3(0);
  /**
   * Surface-aligned stamp plane normal for rectangle texture clip (area-averaged, with tilt).
   * Independent of #Brush.sculpt_plane which controls displacement, not mask projection.
   */
  float3 texture_plane_normal = float3(0);
  float3 texture_plane_normal_symm = float3(0);

  /**
   * Used for area texture mode, local_mat gets calculated by
   * calc_brush_local_mat() and used in sculpt_apply_texture().
   * Transforms from model-space coords to local area coords.
   */
  float4x4 brush_local_mat = float4x4::identity();
  /**
   * The matrix from local area coords to model-space coords is used to calculate the vector
   * displacement in area plane mode.
   */
  float4x4 brush_local_mat_inv = float4x4::identity();

  /**
   * Multi-object Area-texture parity: the brush-frame-to-WORLD transform used to build every
   * object's #brush_local_mat in a multi-object stroke (see #calc_brush_area_texture_mat). The
   * primary object (the sampling reference under the cursor) computes it from the pooled area
   * normal; every other object reuses this exact world frame so the Area-mapped texture reads
   * continuously across the seam between meshes (a joined mesh has a single such frame). Unused
   * for single-object strokes (each object keeps its own #calc_brush_local_mat frame).
   */
  float4x4 area_texture_frame_to_world = float4x4::identity();
  bool area_texture_frame_valid = false;

  /* used to shift the plane around when doing tiled strokes */
  float3 plane_offset = float3(0);
  int tile_pass = 0;

  float3 last_center = float3(0);
  int radial_symmetry_pass = 0;
  float4x4 symm_rot_mat = float4x4::identity();
  float4x4 symm_rot_mat_inv = float4x4::identity();

  /**
   * Accumulate mode.
   * \note inverted for #SCULPT_BRUSH_TYPE_DRAW_SHARP.
   */
  bool accum = false;

  /* Paint Brush. */
  struct {
    float flow = 0.0f;

    float4 wet_mix_prev_color = float4(0);
    float wet_mix = 0.0f;
    float wet_persistence = 0.0f;

    std::optional<float> density_seed;
    float density = 0.0f;

    /**
     * Used by the color attribute paint brush tool to store the brush color during a stroke and
     * composite it over the original color.
     */
    Array<float4> mix_colors;
    Array<float4> prev_colors;
  } paint_brush;

  /* Pose brush */
  std::unique_ptr<pose::IKChain> pose_ik_chain;

  /* Enhance Details. */
  Array<float3> detail_directions;

  /* Clay Thumb brush */
  struct {
    /* Angle of the front tilting plane of the brush to simulate clay accumulation. */
    float front_angle = 0.0f;
    /* Stores the last 10 pressure samples to get a stabilized strength and radius variation. */
    std::array<float, 10> pressure_stabilizer;
    int stabilizer_index = 0;

  } clay_thumb_brush;

  /* Plane Brush */
  struct {
    std::optional<float3> last_normal;
    std::optional<float3> last_center;
    Array<float3> normals;
    Array<float3> centers;
    int normal_index = 0;
    int center_index = 0;

    /**
     * True if the current step is the first time the Plane brush is being evaluated.
     *
     * We cannot use the generic `first_time` variable used by other brushes because
     * the Plane brush uses `grab_delta` to compute its local matrix. Since `grab_delta` requires
     * at least two stroke steps, the first step (and successive steps if the user does not move
     * the cursor) of the Plane brush is always skipped.
     */
    bool first_time = false;
  } plane_brush;

  /* Scene Project brush */
  Vector<ProjectBrushTarget> project_targets;

  /* Cloth brush */
  std::unique_ptr<cloth::SimulationData> cloth_sim;
  float3 initial_location_symm = float3(0);
  float3 initial_location = float3(0);
  float3 initial_normal_symm = float3(0);
  float3 initial_normal = float3(0);

  /* Boundary brush */
  std::array<std::unique_ptr<boundary::SculptBoundary>, PAINT_SYMM_AREAS> boundaries;

  /* Surface Smooth Brush */
  /* Stores the displacement produced by the laplacian step of HC smooth. */
  Array<float3> surface_smooth_laplacian_disp;

  /* Layer brush */
  Array<float> layer_displacement_factor;

  /* Amount to rotate the vertices when using rotate brush. */
  float vertex_rotation = 0.0f;
  Dial *dial = nullptr;

  float plane_trim_squared = 0.0f;

  bool supports_gravity = false;
  float3 gravity_direction = float3(0);
  float3 gravity_direction_symm = float3(0);

  std::unique_ptr<auto_mask::Cache> automasking;

  float4x4 stroke_local_mat = float4x4::identity();
  float multiplane_scrape_angle = 0.0f;

  std::unique_ptr<paint::image::ImageData> image_data;

  StrokeCache();
  ~StrokeCache();
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sculpt Poll Functions
 * \{ */

bool sculpt_mode_poll(bContext *C);
bool sculpt_mode_poll_view3d(bContext *C);
/**
 * Checks for a brush, not just sculpt mode.
 */
bool sculpt_mode_and_brush_poll(bContext *C);

/**
 * Determines whether or not the brush cursor should be shown in the viewport
 */
bool brush_cursor_poll(bContext *C);

/**
 * Returns true if the current Mesh type can handle color attributes. If false an error message
 * will be shown to the user.  Operators should return OPERATOR_CANCELLED in this case.
 *
 * NOTE: Does not check if a color attribute actually exists. Calling code must handle this itself;
 * in most cases a call to BKE_sculpt_color_layer_create_if_needed() is sufficient.
 */
bool color_supported_check(const Scene &scene, Object &object, ReportList *reports);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sculpt Update Functions
 * \{ */

/**
 * Triggers redraws, updates, and dependency graph tags as necessary after each brush calculation.
 */
void flush_update_step(bContext *C, UpdateType update_type);
void flush_update_step(ViewContext &vc, Object &object, UpdateType update_type);
/**
 * Triggers redraws, updates, and dependency graph tags as necessary when a brush stroke finishes.
 */
void flush_update_done(bContext *C, Object &ob, UpdateType update_type);
void flush_update_done(ViewContext &vc,
                       const wmWindowManager &wm,
                       Object &ob,
                       UpdateType update_type);

/**
 * Should be used after modifying the mask or face set IDs.
 */
void tag_update_overlays(bContext *C);
/** \} */

/* -------------------------------------------------------------------- */
/** \name Stroke Functions
 * \{ */

/**
 * Do a ray-cast in the tree to find the 3d brush location
 * (This allows us to ignore the GL depth buffer)
 *
 * TODO: This should be updated to return std::optional<float3>
 */
bool stroke_get_location_bvh(bContext *C, float out[3], const float mval[2], bool force_original);

struct ActiveElementInfo {
  ActiveVert vert = {};
  int active_face_idx = -1;
  int active_grid_idx = -1;
};

/**
 * Retrieve the active vertex and active grid or face index.
 *
 * \note This API assumes that we are only interested in the current bounds of the BVH tree. */
std::optional<ActiveElementInfo> active_element_info_get(ViewContext &vc, const float2 &mval);

struct CursorGeometryInfo {
  float3 location = float3(0);
  float3 normal = float3(0);
};

/**
 * Gets the normal, location and active vertex location of the geometry under the cursor. This also
 * updates the active vertex and cursor related data of the SculptSession using the mouse position
 */
bool stroke_get_location_bvh(Depsgraph &depsgraph,
                             ViewContext &vc,
                             const Sculpt *sd,
                             const Brush *brush,
                             float out[3],
                             const float mval[2],
                             const bool force_original,
                             Object **r_hit_ob = nullptr);
bool stroke_get_location_bvh(Depsgraph &depsgraph,
                             ViewContext &vc,
                             const Paint &paint,
                             const Brush *brush,
                             float out[3],
                             const float mval[2],
                             const bool force_original,
                             Object **r_hit_ob = nullptr);

std::optional<CursorGeometryInfo> cursor_geometry_info_update(bContext *C,
                                                              const float2 &mval,
                                                              bool use_sampled_normal,
                                                              Object **r_hit_ob = nullptr);
/**
 * \param resolve_hit_object: when false, skip the multi-object raycast that redirects the lookup
 * to the front-most sculpt-mode object and trust `vc.obact` instead. Pass false only when the
 * caller has already resolved the object under the cursor (e.g. the paint cursor, which raycasts
 * in #paint_cursor_context_init) -- this runs on every cursor redraw, so the redundant second
 * multi-object raycast is measurable with several objects in the mode.
 */
std::optional<CursorGeometryInfo> cursor_geometry_info_update(Depsgraph &depsgraph,
                                                              const Paint &paint,
                                                              const Sculpt *sd,
                                                              ViewContext &vc,
                                                              const Base *base,
                                                              const float2 &mval,
                                                              bool use_sampled_normal,
                                                              bool resolve_hit_object = true,
                                                              Object **r_hit_ob = nullptr);

void geometry_preview_lines_update(Depsgraph &depsgraph,
                                   Object &object,
                                   SculptSession &ss,
                                   float radius);

void stroke_modifiers_check(
    Depsgraph &depsgraph, RegionView3D *rv3d, const Sculpt &sd, Object &ob, const Brush *brush);
void stroke_modifiers_check(const bContext *C, Object &ob, const Brush *brush);
float raycast_init(ViewContext *vc,
                   const float2 &mval,
                   float3 &ray_start,
                   float3 &ray_end,
                   float3 &ray_normal,
                   bool original);

/**
 * Ray-cast \a ob's current (deformed) surface along \a view_axis through \a location -- both in
 * \a ob's LOCAL space -- and return the signed distance from \a location to the front-most
 * FRONT-FACING hit, measured along \a view_axis (positive when the surface is in front of
 * \a location). Only the span reaching \a max_distance to either side of \a location is searched.
 *
 * `std::nullopt` when the ray misses that span, or when the first hit is a back face -- which
 * means the ray started inside the mesh, i.e. its front surface is farther than \a max_distance in
 * front of \a location and therefore out of reach.
 *
 * \a view_axis must be normalized and point towards the viewer (like
 * #StrokeCache.view_normal_symm).
 */
std::optional<float> raycast_front_facing_surface_offset(const Depsgraph &depsgraph,
                                                         Object &ob,
                                                         const float3 &location,
                                                         const float3 &view_axis,
                                                         float max_distance);

/* Symmetry */
ePaintSymmetryFlags mesh_symmetry_xyz_get(const Object &object);

/**
 * Returns true when the step belongs to the stroke that is directly performed by the brush and
 * not by one of the symmetry passes.
 */
bool stroke_is_main_symmetry_pass(const ed::sculpt_paint::StrokeCache &cache);
/**
 * Return true only once per stroke on the first symmetry pass, regardless of the symmetry passes
 * enabled.
 *
 * This should be used for functionality that needs to be computed once per stroke of a particular
 * tool (allocating memory, updating random seeds...).
 */
bool stroke_is_first_brush_step(const ed::sculpt_paint::StrokeCache &cache);
/**
 * Returns true on the first brush step of each symmetry pass.
 */
bool stroke_is_first_brush_step_of_symmetry_pass(const ed::sculpt_paint::StrokeCache &cache);

float3 grab_delta_get(const Brush &brush, const StrokeCache &cache);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sculpt mesh accessor API
 * \{ */

/** Ensure random access; required for bke::pbvh::Type::BMesh */
void vert_random_access_ensure(Object &object);

/**
 * Return all mesh objects currently in sculpt mode in the view layer of \a vc, active object
 * first.
 */
Vector<Object *> sculpt_mode_objects(const ViewContext &vc);

/**
 * Ensure the grid paint-mask layer exists on every object in \a objects, not just one -- brush
 * strokes and gesture tools that touch masks index #SubdivCCG::masks unconditionally for a Grids
 * PBVH, which is left empty for a multires object that has never had a mask layer created.
 */
void ensure_mask_layers(Depsgraph *depsgraph,
                        Main *bmain,
                        const Scene *scene,
                        Span<Object *> objects);

int vertex_count_get(const Object &object);

bool vertex_is_occluded(const Depsgraph &depsgraph,
                        const Object &object,
                        const float3 &position,
                        bool original);

/**
 * Coordinates used for manipulating the base mesh when Grab Active Vertex is enabled.
 */
Span<float3> vert_positions_for_grab_active_get(const Depsgraph &depsgraph, const Object &object);

using BMeshNeighborVerts = Vector<BMVert *, 64>;
Span<BMVert *> vert_neighbors_get_bmesh(BMVert &vert, BMeshNeighborVerts &r_neighbors);
Span<BMVert *> vert_neighbors_get_interior_bmesh(BMVert &vert, BMeshNeighborVerts &r_neighbors);

Span<int> vert_neighbors_get_mesh(OffsetIndices<int> faces,
                                  Span<int> corner_verts,
                                  GroupedSpan<int> vert_to_face,
                                  Span<bool> hide_poly,
                                  int vert,
                                  Vector<int> &r_neighbors);

/* Fake Neighbors */

#define FAKE_NEIGHBOR_NONE -1

/**
 * This allows the sculpt brushes to work on meshes with multiple connected components as if they
 * had only one connected component. These neighbors are calculated for each vertex using the
 * minimum distance to a vertex that is in a different connected component.
 */
Span<int> fake_neighbors_ensure(const Depsgraph &depsgraph, Object &ob, float max_dist);
void fake_neighbors_free(Object &ob);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Brush Utilities.
 * \{ */

float brush_plane_offset_get(const Brush &brush, const SculptSession &ss);

/**
 * \warning This call is *not* idempotent and changes values inside the StrokeCache.
 *
 * Brushes may behave incorrectly if preserving original plane / normal when this
 * method is not called.
 */
void calc_brush_plane(const Depsgraph &depsgraph,
                      const Brush &brush,
                      Object &ob,
                      const IndexMask &node_mask,
                      float3 &r_area_no,
                      float3 &r_area_co);

std::optional<float3> calc_area_normal(const Depsgraph &depsgraph,
                                       const Brush &brush,
                                       const Object &ob,
                                       const IndexMask &node_mask);

/**
 * This calculates flatten center and area normal together,
 * amortizing the memory bandwidth and loop overhead to calculate both at the same time.
 */
void calc_area_normal_and_center(const Depsgraph &depsgraph,
                                 const Brush &brush,
                                 const Object &ob,
                                 const IndexMask &node_mask,
                                 float r_area_no[3],
                                 float r_area_co[3]);
void calc_area_center(const Depsgraph &depsgraph,
                      const Brush &brush,
                      const Object &ob,
                      const IndexMask &node_mask,
                      float r_area_co[3]);

std::optional<int> nearest_vert_calc_mesh(const bke::pbvh::Tree &pbvh,
                                          Span<float3> vert_positions,
                                          Span<bool> hide_vert,
                                          const float3 &location,
                                          float max_distance,
                                          bool use_original);
std::optional<SubdivCCGCoord> nearest_vert_calc_grids(const bke::pbvh::Tree &pbvh,
                                                      const SubdivCCG &subdiv_ccg,
                                                      const float3 &location,
                                                      float max_distance,
                                                      bool use_original);
std::optional<BMVert *> nearest_vert_calc_bmesh(const bke::pbvh::Tree &pbvh,
                                                const float3 &location,
                                                float max_distance,
                                                bool use_original);

ePaintSymmetryAreas get_vertex_symm_area(const float co[3]);
bool check_vertex_pivot_symmetry(const float vco[3], const float pco[3], char symm);
/**
 * Checks if a vertex is inside the brush radius from any of its mirrored axis.
 */
bool is_vertex_inside_brush_radius_symm(const float vertex[3],
                                        const float br_co[3],
                                        float radius,
                                        char symm);
float3 flip_v3_by_symm_area(const float3 &vector,
                            ePaintSymmetryFlags symm,
                            ePaintSymmetryAreas symmarea,
                            const float3 &pivot);
void flip_quat_by_symm_area(float quat[4],
                            ePaintSymmetryFlags symm,
                            ePaintSymmetryAreas symmarea,
                            const float pivot[3]);

/**
 * Utility functions to get the closest vertices after flipping an original vertex position for
 * all symmetry passes. The returned vector is sorted.
 */
Vector<int> find_symm_verts_mesh(const Depsgraph &depsgraph,
                                 const Object &object,
                                 int original_vert,
                                 float max_distance = std::numeric_limits<float>::max());
Vector<int> find_symm_verts_grids(const Object &object,
                                  int original_vert,
                                  float max_distance = std::numeric_limits<float>::max());
Vector<int> find_symm_verts_bmesh(const Object &object,
                                  int original_vert,
                                  float max_distance = std::numeric_limits<float>::max());
Vector<int> find_symm_verts(const Depsgraph &depsgraph,
                            const Object &object,
                            int original_vert,
                            float max_distance = std::numeric_limits<float>::max());

/**
 * Similar to `find_symm_verts`, but returns an unsorted set of all vertex indices.
 *
 * \note If a symmetry pass is invalid, the corresponding vertex index is set to -1
 */
std::array<int, PAINT_SYMM_AREAS> find_all_symm_verts_mesh(
    const Depsgraph &depsgraph,
    const Object &object,
    int original_vert,
    float max_distance = std::numeric_limits<float>::max());
std::array<int, PAINT_SYMM_AREAS> find_all_symm_verts_grids(
    const Object &object,
    int original_vert,
    float max_distance = std::numeric_limits<float>::max());
std::array<int, PAINT_SYMM_AREAS> find_all_symm_verts_bmesh(
    const Object &object,
    int original_vert,
    float max_distance = std::numeric_limits<float>::max());
std::array<int, PAINT_SYMM_AREAS> find_all_symm_verts(
    const Depsgraph &depsgraph,
    const Object &object,
    int original_vert,
    float max_distance = std::numeric_limits<float>::max());

bool node_fully_masked_or_hidden(const bke::pbvh::Node &node);
bool node_in_sphere(const bke::pbvh::Node &node,
                    const float3 &location,
                    float radius_sq,
                    bool original);
bool node_in_cylinder(const DistRayAABB_Precalc &ray_dist_precalc,
                      const bke::pbvh::Node &node,
                      float radius_sq,
                      bool original);
IndexMask gather_nodes(const bke::pbvh::Tree &pbvh,
                       eBrushFalloffShape falloff_shape,
                       bool use_original,
                       const float3 &location,
                       float radius_sq,
                       const std::optional<float3> &ray_direction,
                       IndexMaskMemory &memory);

const float *brush_frontface_normal_from_falloff_shape(const SculptSession &ss,
                                                       char falloff_shape);
void cube_tip_init(const Sculpt &sd, const Object &ob, const Brush &brush, float mat[4][4]);

/**
 * Sample the brush's texture value.
 *
 * \param brush_point: must be the composed (evaluated) position of the element, never the
 * sculpt-layer base view. The texture is anchored to the surface the user sees and aims at: the
 * screen-projected mapping modes must land the stamp where the cursor is, and the 3D mapping mode
 * must keep the pattern stuck to the visible geometry.
 */
void sculpt_apply_texture(const SculptSession &ss,
                          const Brush &brush,
                          const float brush_point[3],
                          int thread_id,
                          float *r_value,
                          float4 &r_rgba);

/**
 * Calculates the vertex offset for a single vertex depending on the brush setting rgb as vector
 * displacement.
 */
void calc_vertex_displacement(const SculptSession &ss, const Brush &brush, float translation[3]);

/**
 * Tilts a normal by the x and y tilt values using the view axis.
 */
float3 tilt_apply_to_normal(const Object &object,
                            const float4x4 &view_inverse,
                            const float3 &normal,
                            const float2 &tilt,
                            float tilt_strength);
float3 tilt_apply_to_normal(const float3 &normal, const StrokeCache &cache, float tilt_strength);

/**
 * Get effective surface normal with pen tilt and tilt strength applied to it.
 */
float3 tilt_effective_normal_get(const SculptSession &ss, const Brush &brush);

/**
 * The brush uses translations calculated at the beginning of the stroke. They can't be calculated
 * dynamically because changing positions will influence neighboring translations. However we can
 * reduce the cost in some cases by skipping initializing values for vertices in hidden or masked
 * nodes.
 */
void calc_smooth_translations(const Depsgraph &depsgraph,
                              const Object &object,
                              const IndexMask &node_mask,
                              MutableSpan<float3> translations);

/**
 * Flip all the edit-data across the axis/axes specified by \a symm.
 * Used to calculate multiple modifications to the mesh when symmetry is enabled.
 */
void cache_calc_brushdata_symm(ed::sculpt_paint::StrokeCache &cache,
                               ePaintSymmetryFlags symm,
                               char axis,
                               float angle);

struct OrigPositionData {
  Span<float3> positions;
  Span<float3> normals;
};
/**
 * Retrieve positions from the latest undo state. This is often used for modal actions that depend
 * on the initial state of the geometry from before the start of the action.
 */
std::optional<OrigPositionData> orig_position_data_lookup_mesh_all_verts(
    const Object &object, const bke::pbvh::MeshNode &node);
std::optional<OrigPositionData> orig_position_data_lookup_mesh(const Object &object,
                                                               const bke::pbvh::MeshNode &node);
inline OrigPositionData orig_position_data_get_mesh(const Object &object,
                                                    const bke::pbvh::MeshNode &node)
{
  const std::optional<OrigPositionData> result = orig_position_data_lookup_mesh(object, node);
  BLI_assert(result.has_value());
  return *result;
}

std::optional<OrigPositionData> orig_position_data_lookup_grids(const Object &object,
                                                                const bke::pbvh::GridsNode &node);
inline OrigPositionData orig_position_data_get_grids(const Object &object,
                                                     const bke::pbvh::GridsNode &node)
{
  const std::optional<OrigPositionData> result = orig_position_data_lookup_grids(object, node);
  BLI_assert(result.has_value());
  return *result;
}

void orig_position_data_gather_bmesh(const BMLog &bm_log,
                                     const Set<BMVert *, 0> &verts,
                                     MutableSpan<float3> positions,
                                     MutableSpan<float3> normals);

std::optional<Span<float4>> orig_color_data_lookup_mesh(const Object &object,
                                                        const bke::pbvh::MeshNode &node);
inline Span<float4> orig_color_data_get_mesh(const Object &object, const bke::pbvh::MeshNode &node)
{
  return *orig_color_data_lookup_mesh(object, node);
}

std::optional<Span<int>> orig_face_set_data_lookup_mesh(const Object &object,
                                                        const bke::pbvh::MeshNode &node);

std::optional<Span<int>> orig_face_set_data_lookup_grids(const Object &object,
                                                         const bke::pbvh::GridsNode &node);

std::optional<Span<float>> orig_mask_data_lookup_mesh(const Object &object,
                                                      const bke::pbvh::MeshNode &node);

std::optional<Span<float>> orig_mask_data_lookup_grids(const Object &object,
                                                       const bke::pbvh::GridsNode &node);

inline bool brush_type_is_paint(const int tool)
{
  return ELEM(tool, SCULPT_BRUSH_TYPE_PAINT, SCULPT_BRUSH_TYPE_SMEAR, SCULPT_BRUSH_TYPE_BLUR);
}

inline bool brush_type_is_mask(const int tool)
{
  return ELEM(tool, SCULPT_BRUSH_TYPE_MASK);
}

BLI_INLINE bool brush_type_is_attribute_only(const int tool)
{
  return brush_type_is_paint(tool) || brush_type_is_mask(tool) ||
         ELEM(tool, SCULPT_BRUSH_TYPE_DRAW_FACE_SETS);
}

inline bool brush_uses_vector_displacement(const Brush &brush)
{
  return brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_DRAW &&
         brush.flag2 & BRUSH_USE_COLOR_AS_DISPLACEMENT &&
         brush.mtex.brush_map_mode == MTEX_MAP_MODE_AREA;
}

void ensure_valid_pivot(const Object &ob, Paint &paint);

/** Retrieve or calculate the object space radius depending on brush settings. */
float object_space_radius_get(const ViewContext &vc,
                              const Paint &paint,
                              const Brush &brush,
                              const float3 &location,
                              float scale_factor = 1.0);

/* In these brushes the grab delta is calculated always from the initial stroke location, which is
 * generally used to create grab deformations.
 *
 * The classification itself lives in #Brush.drag_kind (declarative, single source of truth kept
 * in sync by #BKE_brush_drag_kind_update from #Brush.sculpt_brush_type / #stroke_method /
 * #cloth_deform_type) instead of the two parallel `ELEM` lists this function and
 * #need_delta_for_tip_orientation used to duplicate. */
bool need_delta_from_anchored_origin(const Brush &brush);

/**
 * Test whether any PBVH node of \a ob intersects the brush volume centered at \a world_center
 * (given in world space), projecting it into the object's local space first. Does not modify the
 * cache.
 *
 * The volume matches #Brush.falloff_shape, so that this gate and the brush's own node gathering
 * (#pbvh_gather_generic) agree on what "inside the brush" means: a sphere for
 * #PAINT_FALLOFF_SHAPE_SPHERE, a cylinder along the view axis for #PAINT_FALLOFF_SHAPE_TUBE
 * (Projected), where depth is ignored.
 *
 * \param world_view_direction: the axis of that cylinder, in world space. Ignored for Sphere
 * falloff. It is per-daub rather than per-stroke because mirroring a daub reflects its view axis
 * along with its center -- see #MirroredDaub.
 * \param radius_multiplier: scales the brush radius used for the test. Values above 1 are used for
 * MIRRORED daub centers when the mirror surface snap is active, so an object whose surface the
 * snap could still reach is not rejected here first. Defaults to 1.0, which keeps every existing
 * call byte-identical.
 */
bool object_geometry_intersects_world_sphere(Object &ob,
                                             const StrokeCache &cache,
                                             Paint &paint,
                                             const Brush &brush,
                                             const float3 &world_center,
                                             const float3 &world_view_direction,
                                             float radius_multiplier = 1.0f);

/**
 * Set a secondary sculpt object's brush location and radius from the world-space brush center,
 * projecting it into the object's local space. Unconditional: the caller decides whether the
 * object should be processed at all (see #object_geometry_intersects_world_sphere).
 */
void stroke_cache_apply_world_center(
    Object &ob, StrokeCache &cache, Paint &paint, const Brush &brush, const float3 &world_center);

/**
 * Sets the brush location for a secondary sculpt object by projecting the world-space brush
 * center into the object's local space and testing whether any PBVH nodes intersect the brush
 * volume. Used in multi-object sculpt mode for objects that are NOT directly under the cursor.
 *
 * Unlike the primary object, which keeps the framework-provided RNA "location", this function
 * accepts any object whose geometry overlaps the brush volume in 3D world space.
 *
 * \param world_view_direction: see #object_geometry_intersects_world_sphere; only used by
 * Projected falloff.
 * \return true if any PBVH node of \a ob intersects the brush volume and the cache was updated.
 */
bool stroke_cache_set_location_from_world_sphere(Object &ob,
                                                 StrokeCache &cache,
                                                 Paint &paint,
                                                 const Brush &brush,
                                                 const float3 &world_center,
                                                 const float3 &world_view_direction);
}  // namespace ed::sculpt_paint

/** \} */

/* -------------------------------------------------------------------- */
/** \name 3D Texture Paint (Experimental)
 * \{ */

void SCULPT_do_paint_brush_image(const Depsgraph &depsgraph,
                                 const Sculpt &sd,
                                 Object &ob,
                                 const IndexMask &node_mask);
bool SCULPT_use_image_paint_brush(PaintModeSettings &settings, Object &ob);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operators
 * \{ */

namespace ed::sculpt_paint {

void SCULPT_OT_brush_stroke(wmOperatorType *ot);

}

namespace ed::sculpt_paint::expand {

void SCULPT_OT_expand(wmOperatorType *ot);
void modal_keymap(wmKeyConfig *keyconf);

}  // namespace ed::sculpt_paint::expand

namespace ed::sculpt_paint::project {
void SCULPT_OT_project_line_gesture(wmOperatorType *ot);
}

namespace ed::sculpt_paint::trim {
void SCULPT_OT_trim_lasso_gesture(wmOperatorType *ot);
void SCULPT_OT_trim_box_gesture(wmOperatorType *ot);
void SCULPT_OT_trim_line_gesture(wmOperatorType *ot);
void SCULPT_OT_trim_polyline_gesture(wmOperatorType *ot);
}  // namespace ed::sculpt_paint::trim

namespace ed::sculpt_paint::face_set {

void SCULPT_OT_face_sets_randomize_colors(wmOperatorType *ot);
void SCULPT_OT_face_set_change_visibility(wmOperatorType *ot);
void SCULPT_OT_face_sets_init(wmOperatorType *ot);
void SCULPT_OT_face_sets_create(wmOperatorType *ot);
void SCULPT_OT_face_sets_edit(wmOperatorType *ot);

void SCULPT_OT_face_set_lasso_gesture(wmOperatorType *ot);
void SCULPT_OT_face_set_box_gesture(wmOperatorType *ot);
void SCULPT_OT_face_set_line_gesture(wmOperatorType *ot);
void SCULPT_OT_face_set_polyline_gesture(wmOperatorType *ot);

}  // namespace ed::sculpt_paint::face_set

namespace ed::sculpt_paint {

void mask_overlay_check(bContext &C, wmOperator &op);
void face_set_overlay_check(bContext &C, wmOperator &op);

void SCULPT_OT_set_pivot_position(wmOperatorType *ot);
void SCULPT_OT_paint_mask_extract(wmOperatorType *ot);
void SCULPT_OT_face_set_extract(wmOperatorType *ot);
void SCULPT_OT_paint_mask_slice(wmOperatorType *ot);

}  // namespace ed::sculpt_paint

namespace ed::sculpt_paint::filter {

void SCULPT_OT_mesh_filter(wmOperatorType *ot);
wmKeyMap *modal_keymap(wmKeyConfig *keyconf);

}  // namespace ed::sculpt_paint::filter

namespace ed::sculpt_paint::cloth {
void SCULPT_OT_cloth_filter(wmOperatorType *ot);
}

namespace ed::sculpt_paint::color {
void SCULPT_OT_color_filter(wmOperatorType *ot);
}

namespace ed::sculpt_paint::mask {

void SCULPT_OT_mask_filter(wmOperatorType *ot);
void SCULPT_OT_mask_init(wmOperatorType *ot);

}  // namespace ed::sculpt_paint::mask

namespace ed::sculpt_paint::dyntopo {

void SCULPT_OT_detail_flood_fill(wmOperatorType *ot);
void SCULPT_OT_sample_detail_size(wmOperatorType *ot);
void SCULPT_OT_dyntopo_detail_size_edit(wmOperatorType *ot);
void SCULPT_OT_dynamic_topology_toggle(wmOperatorType *ot);

}  // namespace ed::sculpt_paint::dyntopo

namespace ed::sculpt_paint::layers {

/* Sculpt layers integration (non-destructive sculpt edits, see also #BKE_sculpt_layers.hh). */

/** True when sculpt layers are available for this object (regular mesh or multires, not dyntopo). */
bool is_supported(const Object &object);
/**
 * True when the layer system actually shapes this object's surface right now: recording is armed,
 * or at least one layer is enabled and not hidden by a disabled folder (so the composed surface
 * differs from the base). Brushes that cannot work with a composed surface — the cloth
 * simulation, whose constraints and simulation-area falloff are solved on it — are rejected in
 * this state (see #sculpt_brush_stroke_invoke).
 */
bool in_use(const Object &object);
/** Element domain (#SCULPT_LAYER_DOMAIN_VERT / #SCULPT_LAYER_DOMAIN_GRID) for the sculpt target. */
short domain_for(const Object &object);
/**
 * Number of layer elements for the object (mesh vertices, or total multires grid points).
 *
 * 64-bit for the grid case, which counts `grids_num * grid_size(level)^2` — see
 * #bke::sculpt_layers::data_ensure.
 */
int64_t element_count(const Object &object);

/** Initialize per-session layer state. Call when entering sculpt mode. */
void session_state_ensure(Object &object);

/**
 * Re-derive #SCULPT_LAYER_REC_EXEMPT across \a object's layer tree from its live REC state, so the
 * composite stops honoring the recording layer's mask (and its folder chain's) while REC is armed.
 *
 * The single writer of that bit, and idempotent, so every path that can change either half of the
 * answer — REC arming and disarming, a change of active layer — simply calls it. Passing a null
 * session or an object that is not in sculpt mode clears the exemption, which is what makes it safe
 * to call from the mode-exit path.
 *
 * Also the repair for a bit that undo moved, in either direction, and this is what makes the
 * re-derive mandatory rather than merely tidy. #SCULPT_LAYER_REC_EXEMPT lives in
 * #SculptLayerTreeNode::flag, which the sculpt undo system snapshots and restores as a whole word
 * (#push_sculpt_layer_flags_batch swaps it outright), so a restore can just as easily resurrect the
 * bit from a snapshot taken while REC was armed as drop the one that should be set. The bit is also
 * deliberately not persisted (see #bke::sculpt_layers::rec_exempt_set), and memfile undo goes
 * through that same writer and reader, so a memfile restore leaves the tree unexempted.
 *
 * Re-deriving from the live session settles all of those the same way. #commit_layers_change calls
 * this on every path that can restore an undo step, and #stroke_record_begin calls it again at the
 * one point where being wrong is destructive: a dropped exemption followed by a recorded dab is the
 * `D / mask` condition the exemption exists to prevent, not a display artifact.
 *
 * Returns whether the bit actually moved. Repairing it changes what every composite resolves for
 * that layer, so the surface a caller is holding becomes stale exactly when this returns true — and
 * a recompose must run against a runtime base derived *before* the repair, never after it (see the
 * load-bearing constraint on #bke::sculpt_layers::rec_exempt_set). Callers that cannot recompose
 * say so at their call site.
 *
 * Follows #SculptSession::layers::rec_active exactly, including the cases where a dab would not in
 * fact be recorded (a layer hidden by a disabled folder, Solo Base). That is deliberate: REC's other
 * pinning, #SculptLayer::influence, behaves the same way, and one rule the user can see in the REC
 * button beats two rules that disagree in states the UI does not distinguish.
 */
bool rec_exemption_refresh(Object &object);

/**
 * Set #SculptSession::layers::rec_active to \a armed, running the whole state change the flag is
 * part of. A no-op when it already holds that value.
 *
 * The single writer of that flag, because it is not a UI toggle: its mirror
 * #SCULPT_LAYER_REC_EXEMPT decides whether the composite honors the active layer's weight mask, so
 * flipping the flag by hand moves the composed surface without recomposing it. The order below is
 * load-bearing and is why this is one function rather than a convention:
 *
 * - The "is the active layer masked" question is asked with the exemption lifted, since while REC
 *   is armed the exemption is precisely what makes #node_mask_for_composite answer "unmasked".
 * - A masked layer drains the multires base first. The lazy CCG flush subtracts each layer's
 *   contribution with the weights in force at flush time, so a flush landing after the flip would
 *   subtract an unmasked contribution the evaluator had composed masked, denting the base by the
 *   difference — permanently.
 * - The runtime base is derived from the still-consistent pre-change state, and only then does the
 *   flag move, the exemption follow it and the surface recompose.
 *
 * Arming also pins the active layer to enabled with influence 1.0, which is REC's contract with the
 * user: what is recorded is what is seen. Undo pushes, notifiers and any refusal to arm belong to
 * the caller — this function performs a decision that has already been made.
 */
void rec_active_set(Object &object, bool armed);

/* Stroke recording into the active layer. */
void stroke_record_begin(const Depsgraph &depsgraph, Object &object);
void stroke_record_end(const Depsgraph &depsgraph, Object &object);
/** Revert a cancelled stroke's recording, restoring the pre-stroke influence/visibility state. */
void stroke_record_cancel(const Depsgraph &depsgraph, Object &object);

/**
 * Give an armed REC a layer to record into when the mesh has none, so the stroke about to start does
 * not silently edit the base under a lit REC button. A no-op whenever REC is off or a layer is
 * already active, which is every ordinary stroke.
 *
 * Must be called BEFORE the stroke's own undo step is opened, and is the reason it is a separate
 * entry point rather than part of #stroke_record_begin: the layer creation is recorded with
 * #undo::push_sculpt_layer_list_change, which claims #StepData::type for the layer machinery, while
 * the stroke's dabs claim the same field for positions. Sharing one step would drop one of the two
 * restores; the creation therefore gets its own step, pushed first.
 *
 * The common source of the armed-but-empty state is handled at the source instead — arming REC
 * creates the layer (see #SCULPT_OT_layer_toggle_rec). This covers what happens afterwards: the last
 * layer removed, or an undo restoring a layer-less tree, while REC stays armed.
 */
void stroke_ensure_rec_layer(const Scene &scene, Object &object);

/**
 * The active mesh-domain recording layer's per-vertex offset buffer, or an empty span when no mesh
 * layer is currently being recorded. Used by #PositionDeformData to accumulate a stroke per dab.
 */
MutableSpan<float3> active_record_data(Object &object);

/**
 * Per-element object-space contribution of the enabled sculpt layers ("base view" offset
 * `O = combined - base`) for the current stroke, or an empty span when the mode is inactive.
 * Indexed like the PBVH positions (mesh vertex index / CCG element index) and constant for the
 * stroke's duration. Brushes subtract it from the live positions when computing
 * surface-shape-dependent inputs (falloff distances, area normal/center, smoothing targets, plane
 * fits) so the edit does not absorb the layer residual; the resulting translations are still
 * applied to the live (composed) positions.
 *
 * Only strokes that edit THE BASE build one. A stroke recorded into a layer works on the composed
 * surface — what is drawn is what was seen — so this span is empty while recording and every helper
 * below degenerates to the plain, layer-less path (see #stroke_record_begin for the trade-off).
 *
 * The offset must always be taken relative to #stroke_base_view_dc — never raw. The brush reference
 * point (#StrokeCache::location_symm and the radius around it) stays on the composed surface, so
 * removing the raw offset shifts the sampled positions away from the cursor by the layer height:
 * once that height reaches the brush radius every falloff factor is zeroed and the area-plane
 * sampling degenerates. The helpers below already do this; a raw consumer must subtract the DC
 * itself (or be strictly differential, like the smooth brush's neighbor averaging, where a constant
 * offset cancels).
 */
Span<float3> stroke_base_view(const Object &object);

/**
 * The base-view offset sampled at the current brush contact point (see #stroke_base_view). Zero
 * when the base view is inactive. Refreshed once per brush action, so it follows the symmetry and
 * tile passes.
 */
float3 stroke_base_view_dc(const Object &object);

/**
 * Refresh #stroke_base_view_dc for the current brush action. Called once per symmetry / tile pass,
 * before any brush computation reads the base view.
 */
void base_view_dc_update(const Depsgraph &depsgraph, Object &object);

/**
 * Add the nodes the brush reaches in base-view space to \a node_mask, and return the union.
 *
 * The brush measures its falloff on the base view, but the gather selects nodes from their bounds on
 * the composed surface. The two footprints differ by the layer height, so without this an element
 * can earn a non-zero factor while its node was never gathered; a node is processed as a whole, so
 * the stroke boundary then follows node borders (square tiles). The returned mask is a superset —
 * the added nodes' elements simply get their (usually zero) factor like any other. Returns \a
 * node_mask unchanged when the base view is inactive. \a radius must cover the brush footprint (pass
 * the same scaled radius the gather used).
 */
IndexMask base_view_extend_node_mask(const Object &object,
                                     const IndexMask &node_mask,
                                     float radius,
                                     IndexMaskMemory &memory);

/**
 * Base-view adjustment helpers for brush computations. Each returns the input span unchanged
 * when the base view is inactive; otherwise the adjusted copy (with the DC offset removed) lives in
 * \a r_storage.
 */
/** Compact node positions (one per element of \a verts) minus the base-view offset. */
Span<float3> base_view_adjust_compact_mesh(const Object &object,
                                           Span<int> verts,
                                           Span<float3> positions,
                                           Vector<float3> &r_storage);
/**
 * Gather base-view positions for \a verts from the full \a vert_positions array. Returns an
 * EMPTY span when the base view is inactive (the caller keeps its indexed code path).
 */
Span<float3> base_view_gather_mesh(const Object &object,
                                   Span<int> verts,
                                   Span<float3> vert_positions,
                                   Vector<float3> &r_storage);
/** Compact node grid positions (CCG node layout) minus the base-view offset. */
Span<float3> base_view_adjust_compact_grids(const Object &object,
                                            const SubdivCCG &subdiv_ccg,
                                            Span<int> grids,
                                            Span<float3> positions,
                                            Vector<float3> &r_storage);
/**
 * Inverse of #base_view_adjust_compact_mesh: add the base-view offset back into \a positions
 * in place, lifting brush results computed in base space up to the live (composed) space. No-op
 * when the base view is inactive. Used by brushes (Pose, Boundary) that build absolute new
 * positions from the base so the layer residual is carried instead of baked into the base.
 */
void base_view_compose_mesh(const Object &object,
                            Span<int> verts,
                            MutableSpan<float3> positions);
/** Grid (CCG node layout) counterpart of #base_view_compose_mesh. */
void base_view_compose_grids(const Object &object,
                             const SubdivCCG &subdiv_ccg,
                             Span<int> grids,
                             MutableSpan<float3> positions);
/**
 * Undo the per-dab layer accumulation of an in-progress stroke that is being cancelled. Must run
 * before the sculpt undo restores the pre-stroke positions, because the offset is recomputed as
 * `current_position - pre_stroke_position` from the still-available per-node undo data. Under a
 * shape key the stroke lives in the evaluated positions (the basis is untouched), so the depsgraph
 * is needed to diff against them.
 */
void cancel_recorded_offsets(const Depsgraph &depsgraph, Object &object);

/**
 * Bring the live positions in sync after a layer change (influence, visibility, data edit, list
 * change): canonical recompute from `mesh_base + layers` with a lightweight PBVH refresh for the
 * mesh domain, honest geometry re-evaluation for multires (the CCG is rebuilt from
 * `MDisps + sum(enabled layers)`).
 */
void commit_layers_change(const Depsgraph &depsgraph, Object &object);
/**
 * #commit_layers_change for callers that have no depsgraph to hand.
 *
 * The two are the same operation: the recompute reads the object's own session state and the
 * multires branch only tags the mesh, so nothing here ever consults a depsgraph. The overload
 * taking one is kept because most callers are operators that already hold it, but the exit paths
 * that must close a mask editing session (#BKE_sculptsession_free, a mode or object switch) run
 * without any context at all and would otherwise have no way to commit.
 */
void commit_layers_change(Object &object);

/**
 * Multires: reshape any pending (lazily flushed) base sculpt edits from the live CCG into the
 * base MDisps while the CCG and the stored layer set are still consistent. MUST be called before
 * any change to the grid layer set or influences — a later flush would reshape the stale composed
 * CCG against the changed layer set and leak the difference into the base. No-op when nothing is
 * pending or the object has no live grids session.
 */
void flush_pending_multires_base(Object &object);

/* -------------------------------------------------------------------- */
/** \name Weight mask editing session
 *
 * A node's sparse weight mask (#SculptLayerMask) is authored by expanding it into the mesh's
 * standard `.sculpt_mask` attribute for the duration of a session, so the whole existing mask
 * toolset — Mask brush, gesture operators, flood fill, mask filters, the viewport overlay — edits it
 * with no changes of its own. The user's own sculpt mask is parked in
 * #SculptSession::layers::mask_edit and restored on exit.
 * \{ */

/**
 * Open a mask editing session on \a node, expanding its mask into `.sculpt_mask`. Returns false
 * (changing nothing) when the session cannot be opened, which the caller must report to the user:
 *
 * - The object has no sculpt session, or its PBVH is not the mesh (vertex) kind. The grid domain is
 *   a separate path and is not handled here.
 * - A session is already open, or \a node is the root group (uid 0 is the "no session" sentinel).
 * - REC is armed. Recording writes the full stroke delta into the layer regardless of its mask (see
 *   #bke::sculpt_layers::node_mask_for_composite), so the two running together would show the user a
 *   surface that does not match what is being stored.
 *
 * A node whose stored mask is missing, or stale against the live vertex count
 * (#bke::sculpt_layers::is_stale_mask), starts the session from a fully opaque mask. A stale mask is
 * already inert — every consumer fails open on it — so this reproduces exactly the surface the user
 * currently sees, rather than expanding a buffer that does not describe this topology.
 *
 * Marks the PBVH mask summaries and their draw buffers dirty, but sends no notifier: as everywhere
 * else in this module — the layer operators in `sculpt_layers.cc`, the mask operators in
 * `paint_mask.cc` — the redraw belongs to the calling operator. A caller that forgets it opens a
 * session the user cannot see.
 *
 * \a depsgraph and \a bmain are needed only on the multires grid domain, where the base mesh's
 * `CD_GRID_PAINT_MASK` layer must be materialized *before* the session opens; see the body.
 */
bool mask_edit_begin(Depsgraph &depsgraph,
                     Main &bmain,
                     Object &object,
                     SculptLayerTreeNode &node);

/**
 * Open a session on \a node the way the user's entry points do: disarm REC first (through
 * #rec_active_set, so the exemption mirror and the composite follow), then #mask_edit_begin.
 * Returns what #mask_edit_begin returned.
 *
 * The disarm is one-way on *both* outcomes, a refused open included. Arming is not the inverse of
 * disarming: it pins the active layer to enabled with `influence = 1.0f`, so replaying it as a
 * rollback would silently overwrite an influence the user set, with no undo record. A user who
 * wants REC back goes through the operator that owns those invariants; see the note in
 * #mask_edit_end.
 *
 * The entry every caller should use, including the undo restore: #mask_edit_begin refuses outright
 * while REC is armed, and nothing clears REC on the user's behalf anywhere else.
 */
bool mask_edit_enter(Depsgraph &depsgraph, Main &bmain, Object &object, SculptLayerTreeNode &node);

/**
 * Close the open mask editing session: compress `.sculpt_mask` back onto the node and restore the
 * user's own mask. REC is left as it stands, disarmed. A no-op when no session is open, so it is
 * safe on every exit path.
 *
 * Deliberately takes no #bContext: the paths that must not leave a session open
 * (#BKE_sculptsession_free, a mode or object switch) have none to give. As with #mask_edit_begin
 * the notifier is the caller's to send.
 *
 * Returns whether the painted weights actually reached the node. False means they were salvaged
 * from nothing — the domain the session opened on is gone (the multires modifier removed under it,
 * the CCG rebuilt at another level), so there was nothing left to compress. Callers that tell the
 * user the edit was applied must check this; the session is closed and cleared either way.
 */
bool mask_edit_end(Object &object);

/**
 * Put back the viewport tool a mask editing session replaced with the Mask brush, when the user has
 * not since chosen another one. A no-op when no session is open, so it is safe to call
 * unconditionally next to #mask_edit_end.
 *
 * Must run *before* #mask_edit_end, which clears the session struct the parked idname lives on.
 *
 * There is no REC half to undo. Entering a session disarms REC and leaving does not put it back —
 * arming is a heavyweight operation with its own refusals and its own undo record, so it stays the
 * operator's to perform; see the note in #mask_edit_end.
 *
 * Unlike #mask_edit_end this needs a #bContext, because only a context can reach the tool system.
 * The exit paths that have none (#BKE_sculptsession_free, a mode switch, an undo restore) therefore
 * close the session without it and leave the Mask brush active — which #mask_edit_enter_ui accounts
 * for by refusing to park the mask tool's own idname on the next entry.
 */
void mask_edit_exit_ui(bContext *C, Object &object);

/** Uid of the node whose mask is being edited, or 0 when no session is open. */
int mask_edit_active_uid(const SculptSession &ss);

/**
 * Overload for callers that hold an #Object rather than its session — the tree view rows above all.
 * Answers 0 for an object with no sculpt session, so it is safe outside Sculpt Mode.
 */
int mask_edit_active_uid(const Object &object);

/**
 * How a weight mask must be cut for a given sculpt target: the number of domain elements it covers
 * and the number of elements per block.
 *
 * \note A wrong block size on the grid domain is not rejected, it is *silently ignored*:
 * #bke::sculpt_layers::grid_masks_for_composite drops a grid mask whose `block_size` is not the
 * grid area, so the layer contributes fully with no crash and no warning. That failure mode is why
 * the choice lives in one pure function rather than at each producer.
 */
struct MaskLayout {
  /** 64-bit to match #SculptLayerMask::totelem; a grid layout counts `grids_num * grid_area`. */
  int64_t totelem = 0;
  int block_size = 0;
};

/**
 * The mask layout for a sculpt target, or a zeroed #MaskLayout when the target carries no elements
 * to mask (which every caller must treat as a refusal).
 *
 * A folder has no domain of its own, so it takes the same layout as a layer on the same object:
 * #bke::sculpt_layers::chain_mask folds folder masks into the layers below through
 * #bke::sculpt_layers::mask_multiply, which returns null unless `totelem` *and* `block_size` agree,
 * and #bke::sculpt_layers::node_mask_for_composite then gates the product against the live element
 * count. A folder mask cut any other way is therefore inert. Hence the parameters describe the
 * object, not the node.
 *
 * Pure, so the choice can be tested without a live #SubdivCCG — see the note on #MaskLayout for why
 * that matters more here than the usual amount.
 */
MaskLayout mask_layout_for(bool on_grids, int verts_num, int grids_num, int grid_area);

/**
 * The layout every mask on \a object must be cut at, or a zeroed #MaskLayout when the object carries
 * no elements to mask (which every caller must treat as a refusal).
 *
 * Defined in `sculpt_layer_mask_edit.cc`. Exported because the merges in `sculpt_layers.cc` size and
 * cut masks too, and a second derivation of the block size there would be free to drift: a grid mask
 * cut at any other size is *silently* dropped by #bke::sculpt_layers::grid_masks_for_composite, so
 * the two would disagree with no crash and no warning.
 */
MaskLayout mask_layout_for_object(Object &object);

/**
 * Dense per-element weights of \a node's own weight mask, or false when it has none usable.
 *
 * Defined in `sculpt_layers.cc`; declared here because the mask operators
 * (`sculpt_layer_mask_edit.cc`) and the merges both fold masks into layer data and must do it from
 * one implementation.
 */
bool gather_node_weight_mask(const SculptLayerTreeNode &node,
                             int64_t elem_num,
                             Array<float> &r_dense);

/** Dense product of \a node's mask and every folder mask strictly below \a stop_above (exclusive). */
bool gather_fold_mask(const SculptLayerTreeNode &node,
                      const SculptLayerGroup *stop_above,
                      int64_t elem_num,
                      Array<float> &r_dense);

/** The first stale mask in the range #gather_fold_mask folds, or null when all are usable. */
const SculptLayerTreeNode *find_stale_mask_in_fold(const SculptLayerTreeNode &node,
                                                   const SculptLayerGroup *stop_above,
                                                   int64_t elem_num);

/**
 * True when \a sculpt_brush_type must be refused because a weight-mask editing session is open.
 *
 * Pure, so the decision is testable without a #bContext or a live session: pass
 * #mask_edit_active_uid for \a mask_edit_uid.
 *
 * A session is entered to paint a mask, so the brushes that only write attributes — the Mask brush
 * above all, but Paint/Smear/Blur and Draw Face Sets equally — must keep working; they are what the
 * session exists to borrow. Everything else moves vertices, and while a session is open the
 * standard mask storage does not hold the user's mask, so a moved vertex would be displaced by the
 * *layer's weights* wherever a brush consults the mask (automasking, the mask factor every brush
 * multiplies its strength by). The stroke would be authored against a mask the user cannot see and
 * did not paint, and no exit path can undo a stroke that was already applied.
 *
 * Note that this is *not* a "keeps the CCG clean" rule: the attribute-only brushes tag multires
 * just as the position brushes do (#flush_update_step calls #multires_mark_as_modified for every
 * dab on a multires object, before it branches on the update type), so `SubdivCCG::dirty.coords` is
 * set throughout a grid session either way. Keeping the layer's weights out of the base mesh is the
 * job of #bke::sculpt_layers::MaskEditSuspendGuard inside the flush primitives, not of this test.
 *
 * Refused rather than closing the session for them: ending it silently would throw away an
 * in-progress mask edit as a side effect of an unrelated action.
 *
 * Keyed on what the brush *writes* (#brush_type_is_attribute_only), not on its name or category, so
 * a new position brush is refused by default rather than by being remembered.
 */
inline bool mask_edit_blocks_brush(const int mask_edit_uid, const int sculpt_brush_type)
{
  return mask_edit_uid != 0 && !brush_type_is_attribute_only(sculpt_brush_type);
}

/**
 * Refill both sculpt layer overlays — the weight mask and the displacement preview. Which node
 * the overlays show has changed (active layer switched, a mask was added/removed/filled/inverted),
 * so no node keeps a valid value — unlike a brush stroke, where only the touched nodes go stale.
 *
 * Defined in `sculpt_layers.cc` because that file already shares the PBVH/object helpers the
 * #active_set call sites need; `sculpt_layer_mask_edit.cc` calls it for its out-of-session mask
 * operators through this declaration.
 *
 * Mirrored in `ED_sculpt.hh` for the RNA setter of the active layer, exactly as
 * #rec_exemption_refresh is and for the same reason.
 */
void tag_layer_overlays_dirty(Object &object);

/* #mask_edit_refuse_deform lives in `ED_sculpt.hh`: the transform system calls it from
 * `editors/transform/`, which cannot see this module-internal header. */

/**
 * Refuse (reporting) a layer-tree operator whose commit rebuilds the multires CCG while a weight-mask
 * editing session is open on the grid domain. True when it was refused.
 *
 * #commit_layers_change reaches `DEG_id_tag_update(..., ID_RECALC_GEOMETRY)` on the multires path,
 * which *rebuilds* the CCG rather than flushing it. A grid session keeps its live weights in
 * #SubdivCCG::masks while #SculptLayerTreeNode::mask still holds the pre-session snapshot, so the
 * rebuild regenerates the array from `CD_GRID_PAINT_MASK`, #SculptLayerMaskEdit::ccg_id stops
 * matching, and the close then refuses to compress — the edit is gone with nothing said. Refusing up
 * front is what turns that silent loss into a message.
 *
 * Scoped to the grid domain deliberately: the mesh path recomposes in place
 * (#recompute_mesh_canonical) and leaves the session's expanded weights untouched, so refusing there
 * would restrict the UI for nothing.
 */
bool mask_edit_refuse_ccg_rebuild(wmOperator *op, const Object &object);

/** \} */

/** Where #SCULPT_OT_layer_move_to places the moved items relative to its anchor. Values are the
 * operator's `location` enum property, set from #ui::DropLocation by the tree view. */
enum class MoveLocation : int {
  Before = 0,
  After = 1,
  Into = 2,
};

/* Operators. */
void SCULPT_OT_layer_add(wmOperatorType *ot);
void SCULPT_OT_layer_remove(wmOperatorType *ot);
void SCULPT_OT_layer_move(wmOperatorType *ot);
void SCULPT_OT_layer_move_to(wmOperatorType *ot);
void SCULPT_OT_layer_duplicate(wmOperatorType *ot);
void SCULPT_OT_layer_merge_down(wmOperatorType *ot);
void SCULPT_OT_layer_merge_selected(wmOperatorType *ot);
void SCULPT_OT_layer_bake(wmOperatorType *ot);
void SCULPT_OT_layer_bake_to_shape_key(wmOperatorType *ot);
void SCULPT_OT_layer_bake_and_editmode_enter(wmOperatorType *ot);
void SCULPT_OT_layer_clear(wmOperatorType *ot);
void SCULPT_OT_layer_invert(wmOperatorType *ot);
void SCULPT_OT_layer_validate(wmOperatorType *ot);
void SCULPT_OT_layer_mask_isolate(wmOperatorType *ot);
void SCULPT_OT_layer_mask_add(wmOperatorType *ot);
void SCULPT_OT_layer_mask_remove(wmOperatorType *ot);
void SCULPT_OT_layer_mask_invert(wmOperatorType *ot);
void SCULPT_OT_layer_mask_apply(wmOperatorType *ot);
void SCULPT_OT_layer_mask_clear(wmOperatorType *ot);
void SCULPT_OT_layer_mask_fill(wmOperatorType *ot);
void SCULPT_OT_layer_mask_edit_toggle(wmOperatorType *ot);
void SCULPT_OT_layer_mask_edit_finish(wmOperatorType *ot);
void SCULPT_OT_layer_mask_edit_cancel(wmOperatorType *ot);
void SCULPT_OT_layer_mask_toggle(wmOperatorType *ot);
void SCULPT_OT_layer_set_influence(wmOperatorType *ot);
void SCULPT_OT_layer_influence_drag(wmOperatorType *ot);
void SCULPT_OT_layer_toggle_visibility(wmOperatorType *ot);
void SCULPT_OT_layer_select(wmOperatorType *ot);
void SCULPT_OT_layer_toggle_rec(wmOperatorType *ot);
void SCULPT_OT_layer_solo_base(wmOperatorType *ot);
void SCULPT_OT_layer_group_add(wmOperatorType *ot);
void SCULPT_OT_layer_group_remove(wmOperatorType *ot);
void SCULPT_OT_layer_group_merge(wmOperatorType *ot);
void SCULPT_OT_layer_group_delete(wmOperatorType *ot);
void SCULPT_OT_layer_group_toggle_visibility(wmOperatorType *ot);
void SCULPT_OT_layer_group_color_tag(wmOperatorType *ot);

}  // namespace ed::sculpt_paint::layers

/** \} */

}  // namespace blender
