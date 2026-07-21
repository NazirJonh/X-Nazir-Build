/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 */

#pragma once

#include "DNA_ID.h"
#include "DNA_brush_enums.h"
#include "DNA_curve_types.h"
#include "DNA_curves_types.h"
#include "DNA_defs.h"
#include "DNA_object_enums.h"
#include "DNA_scene_types.h"
#include "DNA_texture_types.h" /* for MTex */

namespace blender {

struct CurveMapping;
struct Image;
struct MTex;
struct Material;

struct BrushGpencilSettings {
  DNA_DEFINE_CXX_METHODS(BrushGpencilSettings)

  /** Amount of smoothing to apply to newly created strokes. */
  float draw_smoothfac = 0;
  /** Fill zoom factor */
  float fill_factor = 0;
  /** Amount of alpha strength to apply to newly created strokes. */
  float draw_strength = 0;
  /** Amount of jitter to apply to newly created strokes. */
  float draw_jitter = 0;
  /** Angle when the brush has full thickness. */
  float draw_angle = 0;
  /** Factor to apply when angle change (only 90 degrees). */
  float draw_angle_factor = 0;
  /** Factor of randomness for pressure. */
  float draw_random_press = 0;
  /** Factor of strength for strength. */
  float draw_random_strength = 0;
  /** Number of times to apply smooth factor to new strokes. */
  short draw_smoothlvl = 0;
  /** Number of times to subdivide new strokes. */
  short draw_subdivide = 0;
  /** Layers used for fill. */
  eGP_FillLayerModes fill_layer_mode = GP_FILL_GPLMODE_VISIBLE;
  short fill_direction = 0;

  /** Factor for transparency. */
  float fill_threshold = 0;
  short fill_solver = 0;
  /* Type of caps: eGPDstroke_Caps. */
  int8_t caps_type = 0;
  char _pad1[1] = {};

  eGPDbrush_Flag2 flag2 = {};

  /** Number of simplify steps. */
  int fill_simplylvl = 0;
  /** Type of control lines drawing mode. */
  eGP_FillDrawModes fill_draw_mode = GP_FILL_DMODE_BOTH;
  /** Type of gap filling extension to use. */
  eGP_FillExtendModes fill_extend_mode = GP_FILL_EMODE_EXTEND;

  /** Maximum distance before generate new point for very fast mouse movements. */
  int input_samples = 0;
  /** Random factor for UV rotation. */
  float uv_random = 0;
  /** Moved to 'Brush.gpencil_brush_type'. */
  DNA_DEPRECATED int brush_type = 0;
  /** Soft, hard or stroke. */
  eGP_BrushEraserMode eraser_mode = GP_BRUSH_ERASER_SOFT;
  /** Smooth while drawing factor. */
  float active_smooth = 0;
  /** Factor to apply to strength for soft eraser. */
  float era_strength_f = 0;
  /** Factor to apply to thickness for soft eraser. */
  float era_thickness_f = 0;
  /** Internal grease pencil drawing flags. */
  eGPDbrush_Flag flag = {};

  /** gradient control along y for color */
  float hardness = 0;
  /** factor xy of shape for dots gradients */
  float aspect_ratio[2] = {};
  /** Simplify adaptive factor */
  float simplify_f = 0;

  /** Mix color-factor. */
  float vertex_factor = 0;
  eGp_Vertex_Mode vertex_mode = GPPAINT_MODE_STROKE;

  eGP_Sculpt_Flag sculpt_flag = {};
  eGP_Sculpt_Mode_Flag sculpt_mode_flag = {};
  char _pad2[2] = {};
  /** Brush preselected mode (Active/Material/Vertex-color). */
  eGP_BrushMode brush_draw_mode = GP_BRUSH_MODE_ACTIVE;

  /** Randomness for Hue. */
  float random_hue = 0;
  /** Randomness for Saturation. */
  float random_saturation = 0;
  /** Randomness for Value. */
  float random_value = 0;

  eBrushColorJitterSettings_Flag color_jitter_flag = {};
  char _pad3[4] = {};

  /** Factor to extend stroke extremes using fill tool. */
  float fill_extend_fac = 0;
  /** Number of pixels to dilate fill area. */
  int dilate_pixels = 0;

  struct CurveMapping *curve_sensitivity = nullptr;
  struct CurveMapping *curve_strength = nullptr;
  struct CurveMapping *curve_jitter = nullptr;
  struct CurveMapping *curve_rand_pressure = nullptr;
  struct CurveMapping *curve_rand_strength = nullptr;
  struct CurveMapping *curve_rand_uv = nullptr;
  struct CurveMapping *curve_rand_hue = nullptr;
  struct CurveMapping *curve_rand_saturation = nullptr;
  struct CurveMapping *curve_rand_value = nullptr;

  /** Factor for external line thickness conversion to outline. */
  float outline_fac = 0;
  /** Screen space simplify threshold. Points within this margin are treated as a straight line. */
  float simplify_px = 0;
  /** Threshold distance for converting curve types. */
  float conversion_threshold = 0;
  /* #CurveType Used for converting. */
  int8_t curve_type = 0;
  char _pad4[3] = {};
  /* Fill Tool gap detection factor. */
  float fill_gap_factor = 0;
  char _pad5[4] = {};

  /* optional link of material to replace default in context */
  /** Material. */
  struct Material *material = nullptr;
  /** Material Alternative for secondary operations. */
  struct Material *material_alt = nullptr;
};

struct BrushCurvesSculptSettings {
  /** Number of curves added by the add brush. */
  int add_amount = 0;
  /** Number of control points in new curves added by the add brush. */
  int points_per_curve = 0;
  eBrushCurvesSculptFlag flag = {};
  /** When shrinking curves, they shouldn't become shorter than this length. */
  float minimum_length = 0;
  /** Length of newly added curves when it is not interpolated from other curves. */
  float curve_length = 0;
  /** Minimum distance between curve root points used by the Density brush. */
  float minimum_distance = 0;
  /** The initial radius of curve. */
  float curve_radius = 0;
  /** How often the Density brush tries to add a new curve. */
  int density_add_attempts = 0;
  eBrushCurvesSculptDensityMode density_mode = BRUSH_CURVES_SCULPT_DENSITY_MODE_AUTO;
  char _pad[7] = {};
  struct CurveMapping *curve_parameter_falloff = nullptr;
};

/** Max number of propagation steps for automasking settings. */
#define AUTOMASKING_BOUNDARY_EDGES_MAX_PROPAGATION_STEPS 20

/** One entry in a brush's Curve Patch texture list (#BrushCurvePatchSettings::texture_slots), used
 * by the Curve Patch STAMPS mode when its texture source is #MTEX_CURVE_PATCH_TEX_MULTI.
 *
 * Must stay trivially copyable: `brush_copy_data()` duplicates the list with `BLI_duplicatelist()`,
 * which copies each node's bytes through `MEM_dupallocN` and runs no constructor. Adding a member
 * with a non-trivial constructor (a `std::string`, a container) would leave that member's bytes
 * aliased between the two brushes. */
struct BrushCurvePatchTextureSlot {
  struct BrushCurvePatchTextureSlot *next = nullptr, *prev = nullptr;
  /** Sampled through a copy of #Brush::mtex with only this `tex` swapped in, so every mapping
   * setting stays shared with the brush. A null `tex` makes the stamp that drew this slot skip
   * itself entirely -- unlike the brush's own null texture, which sculpts flat. */
  struct Tex *tex = nullptr;
  /** Relative probability weight in the per-stamp draw. Zero disables the slot without deleting it;
   * a list whose weights all sum to zero falls back to the brush's own texture. */
  float weight = 1.0f;
  char _pad[4] = {};
};

/**
 * Curve Patch settings, one set per brush.
 *
 * These lived split across #Brush and #MTex until they were gathered here: the #MTex half was
 * formally per-texture-slot, but nothing ever read it from any slot other than #Brush::mtex, so the
 * split described nothing real and made the Python API address one feature through two unrelated
 * paths.
 *
 * The member order is chosen so that no explicit padding is needed: makesdna does not insert
 * alignment padding on its own (see `check_member_alignment()` in `makesdna.cc`), and the twelve
 * `char` fields sit exactly between the list's `int` and the first pointer.
 */
struct BrushCurvePatchSettings {
  /** STAMPS mode texture list, active only when #stamp_texture_source is
   * #MTEX_CURVE_PATCH_TEX_MULTI. Holds ID pointers with user counts, so brush copy, free and
   * `foreach_id` all have to walk it. */
  ListBaseT<BrushCurvePatchTextureSlot> texture_slots = {nullptr, nullptr};
  int texture_active_index = 0;

  /** When set, committing a Curve Patch (or Roll) edit assigns a new face set to the faces its
   * relief actually raised. */
  char face_set = 0;
  /** Swap which texture axis runs along the control curve's arc-length (false = V runs along the
   * curve, the default; true = U). `char`, not `bool`: makesdna has no builtin size for `bool`. */
  char swap_axis = false;
  /** How one texture tile is mapped along the arc-length. See #eMTex_CurvePatchLengthMode. */
  char length_mode = 0;
  /** REPEAT mode: number of texture repeats along the curve length (RNA-clamped 1..64). */
  char length_repeat = 1;
  /** How the relief terminates at the curve's two ends. See #eMTex_CurvePatchEndFalloff. */
  char end_falloff = MTEX_CURVE_PATCH_END_HARD;
  /** SMOOTH end falloff: length of the fade at each end, as a percentage of the curve's total
   * arc-length (RNA-clamped 0..50). The 50 ceiling keeps the two end zones from ever overlapping. */
  char end_falloff_percent = 10;
  /** Whether the texture is projected as one continuous stretched sheet along the curve (Ribbon,
   * the original behavior) or as discrete randomized stamps. See #eMTex_CurvePatchStampMode. */
  char stamp_mode = MTEX_CURVE_PATCH_STAMP_RIBBON;
  /** STAMPS mode: which coordinate frame a stamp's texture is sampled in. See
   * #eMTex_CurvePatchStampProjection. */
  char stamp_projection = MTEX_CURVE_PATCH_STAMP_PROJ_CURVE;
  /** STAMPS mode: per-stamp size randomization as a percentage (RNA-clamped 0..100). Stamps only
   * ever shrink from the brush radius, never grow past it. */
  char stamp_size_random = 0;
  /** STAMPS mode: per-stamp relief strength randomization as a percentage (RNA-clamped 0..100).
   * Like the size, it only ever reduces. */
  char stamp_strength_random = 0;
  /** Whether every stamp samples the brush's own texture or draws one at random from
   * #texture_slots. See #eMTex_CurvePatchTexSource. */
  char stamp_texture_source = MTEX_CURVE_PATCH_TEX_SINGLE;
  /** Whether the whole ribbon carries the brush's own texture or splits into the Start / Middle /
   * End textures below. See #eMTex_CurvePatchTexSource. */
  char ribbon_texture_source = MTEX_CURVE_PATCH_TEX_SINGLE;

  /** RIBBON mode Start / Middle / End textures, active only when #ribbon_texture_source is
   * #MTEX_CURVE_PATCH_TEX_MULTI. A null entry leaves its stretch of the ribbon untouched. */
  struct Tex *tex_start = nullptr;
  struct Tex *tex_middle = nullptr;
  struct Tex *tex_end = nullptr;

  /** Arc length the Start / End textures occupy, in brush DIAMETERS -- 1.0 makes a cap as long as
   * the ribbon is wide, so its texture is not distorted. Zero means no cap. */
  float cap_start_length = 1.0f;
  float cap_end_length = 1.0f;
};

/**
 * \note Any change to members that is user visible and that may make the brush differ from the one
 * saved in the asset library should be followed by a #BKE_brush_tag_unsaved_changes() call.
 */
struct Brush {
#ifdef __cplusplus
  DNA_DEFINE_CXX_METHODS(Brush)
  /** See #ID_Type comment for why this is here. */
  static constexpr ID_Type id_type = ID_BR;
#endif

  ID id;

  struct CurveMapping *curve_distance_falloff = nullptr;
  struct MTex mtex;
  struct MTex mask_mtex;

  PreviewImage *preview = nullptr;
  /** Color gradient. */
  struct ColorBand *gradient = nullptr;
  struct PaintCurve *paint_curve = nullptr;

  float normal_weight = 0.0f;
  /** Rake actual data (not texture), used for sculpt. */
  float rake_factor = 0;

  /** Blend mode. */
  short blend = 0;
  /** #eObjectMode: to see if the brush is compatible, use for display only. */
  short ob_mode = OB_MODE_ALL_PAINT;
  /** Brush weight. */
  float weight = 1.0f; /* weight of brush 0 - 1.0 */
  /** Brush diameter. */
  int size = 70; /* diameter of the brush in pixels */
  /** General purpose flags. */
  eBrushFlags flag = BRUSH_ALPHA_PRESSURE | BRUSH_SPACE_ATTEN;
  eBrushFlags2 flag2 = {};
  eBrushSamplingFlags sampling_flag = BRUSH_PAINT_ANTIALIASING;

  /**
   * How the stroke behaves when used via the modal operators.
   */
  eBrushStrokeType stroke_method = BRUSH_STROKE_SPACE;
  char _pad[7] = {};
  /** Number of samples used to smooth the stroke. */
  int input_samples = 1;

  /** Pressure influence for mask. */
  BrushMaskPressureFlags mask_pressure = {};
  /** Jitter the position of the brush. */
  float jitter = 0.0f;
  /** Absolute jitter in pixels. */
  int jitter_absolute = 0;
  eOverlayFlags overlay_flags = {};
  /** Spacing of paint operations. */
  int spacing = 10;
  /** Turning radius (in pixels) for smooth stroke. */
  int smooth_stroke_radius = 75;
  /** Higher values limit fast changes in the stroke direction. */
  float smooth_stroke_factor = 0.9f;
  /** Paint operations / second (airbrush). */
  float rate = 0.1f;

  /** Color. */
  float color[3] = {1.0f, 1.0f, 1.0f};
  eBrushColorJitterSettings_Flag color_jitter_flag = {};
  float hsv_jitter[3] = {};

  /** Color jitter pressure curves. */
  struct CurveMapping *curve_rand_hue = nullptr;
  struct CurveMapping *curve_rand_saturation = nullptr;
  struct CurveMapping *curve_rand_value = nullptr;

  struct CurveMapping *curve_size = nullptr;
  struct CurveMapping *curve_strength = nullptr;
  struct CurveMapping *curve_jitter = nullptr;

  /** Opacity. */
  float alpha = 1.0f; /* brush strength/intensity probably variable should be renamed? */
  /** Hardness */
  float hardness = 0.0f;
  /** Flow */
  float flow = 0;
  /** Wet Mix */
  float wet_mix = 0;
  float wet_persistence = 0;
  /** Density */
  float density = 0;
  ePaintBrush_flag paint_flags = {};

  /** Tip Shape */
  /* Factor that controls the shape of the brush tip by rounding the corners of a square. */
  /* 0.0 value produces a square, 1.0 produces a circle. */
  float tip_roundness = 1.0f;
  float tip_scale_x = 1.0f;

  /** Background color. */
  float secondary_color[3] = {0, 0, 0};

  /* Deprecated sRGB color for forward compatibility. */
  DNA_DEPRECATED float rgb[3] = {1.0f, 1.0f, 1.0f};
  DNA_DEPRECATED float secondary_rgb[3] = {0, 0, 0};

  /** Rate */
  float dash_ratio = 1.0f;
  int dash_samples = 20;

  /** The direction of movement for sculpt vertices. */
  eBrushSculpt_DispDir sculpt_plane = SCULPT_DISP_DIR_AREA;

  /** Offset for plane brushes (clay, flatten, fill, scrape). */
  float plane_offset = 0.0f;

  int gradient_spacing = 0;
  /** Source for stroke color gradient application. */
  eBrushGradientSourceStroke gradient_stroke_mode = BRUSH_GRADIENT_PRESSURE;
  /** Source for fill brush color gradient application. */
  eBrushGradientSourceFill gradient_fill_mode = BRUSH_GRADIENT_LINEAR;

  /**
   * Tag to indicate to the user that the brush has been changed since being imported. Only set for
   * brushes that are actually imported (must have #ID.lib set). Runtime only.
   */
  char has_unsaved_changes = 0;

  /** Projection shape (sphere, circle). */
  eBrushFalloffShape falloff_shape = PAINT_FALLOFF_SHAPE_SPHERE;
  float falloff_angle = 0;

  /** Active sculpt brush type. */
  eBrushSculptType sculpt_brush_type = SCULPT_BRUSH_TYPE_DRAW;
  /** Active vertex paint. */
  eBrushVertexPaintType vertex_brush_type = VPAINT_BRUSH_TYPE_DRAW;
  /** Active weight paint. */
  eBrushWeightPaintType weight_brush_type = WPAINT_BRUSH_TYPE_DRAW;
  /** Active image paint brush type. */
  eBrushImagePaintType image_brush_type = IMAGE_PAINT_BRUSH_TYPE_DRAW;
  /** Enum eBrushMaskTool, only used if sculpt_brush_type is SCULPT_BRUSH_TYPE_MASK. */
  BrushMaskTool mask_tool = BRUSH_MASK_DRAW;
  /** Active grease pencil brush type. */
  eBrushGPaintType gpencil_brush_type = GPAINT_BRUSH_TYPE_DRAW;
  /** Active grease pencil vertex brush type. */
  eBrushGPVertexType gpencil_vertex_brush_type = GPVERTEX_BRUSH_TYPE_DRAW;
  /** Active grease pencil sculpt brush type. */
  eBrushGPSculptType gpencil_sculpt_brush_type = GPSCULPT_BRUSH_TYPE_SMOOTH;
  /** Active grease pencil weight brush type. */
  eBrushGPWeightType gpencil_weight_brush_type = GPWEIGHT_BRUSH_TYPE_DRAW;
  /** Active curves sculpt brush type. */
  eBrushCurvesSculptType curves_sculpt_brush_type = CURVES_SCULPT_BRUSH_TYPE_COMB;

  char _pad1[2] = {};

  float autosmooth_factor = 0.0f;

  float tilt_strength_factor = 0;

  float topology_rake_factor = 0.0f;

  float crease_pinch_factor = 0.5f;

  float normal_radius_factor = 0.5f;
  float area_radius_factor = 0.5f;
  float wet_paint_radius_factor = 0.5f;

  float plane_trim = 0.5f;
  /** Affectable height of brush (layer height for layer tool, i.e.). */
  float height = 0;

  /* Plane Brush */
  float plane_height = 0;
  float plane_depth = 0;
  float stabilize_normal = 0;
  float stabilize_plane = 0;
  eBrushPlaneInversionMode plane_inversion_mode = BRUSH_PLANE_INVERT_DISPLACEMENT;

  float texture_sample_bias = 0; /* value to added to texture samples */

  /**
   * This preset is used to specify an exact function used for the distance falloff instead
   * of doing a Bezier spline evaluation via CurveMapping for performance reasons.
   * \see #eBrushCurvePreset and #eCurveMappingPreset
   */
  eBrushCurvePreset curve_distance_falloff_preset = BRUSH_CURVE_CUSTOM;

  /* Maximum distance to search fake neighbors from a vertex. */
  float disconnected_distance_max = 0.1f;

  eBrushDeformTarget deform_target = BRUSH_DEFORM_TARGET_GEOMETRY;

  /* automasking */
  DNA_DEPRECATED int automasking_flags = 0;
  DNA_DEPRECATED int automasking_boundary_edges_propagation_steps = 1;

  DNA_DEPRECATED float automasking_start_normal_limit = 0.34906585f; /* 20 degrees */
  DNA_DEPRECATED float automasking_start_normal_falloff = 0.25f;
  DNA_DEPRECATED float automasking_view_normal_limit = 1.570796; /* 90 degrees */
  DNA_DEPRECATED float automasking_view_normal_falloff = 0.25f;

  eBrushElasticDeformType elastic_deform_type = BRUSH_ELASTIC_DEFORM_GRAB;
  float elastic_deform_volume_preservation = 0;

  /* snake hook */
  eBrushSnakeHookDeformType snake_hook_deform_type = BRUSH_SNAKE_HOOK_DEFORM_FALLOFF;

  /* pose */
  eBrushPoseDeformType pose_deform_type = BRUSH_POSE_DEFORM_ROTATE_TWIST;
  float pose_offset = 0;
  int pose_smooth_iterations = 4;
  int pose_ik_segments = 1;
  eBrushPoseOriginType pose_origin_type = BRUSH_POSE_ORIGIN_TOPOLOGY;

  /* boundary */
  eBrushBoundaryDeformType boundary_deform_type = BRUSH_BOUNDARY_DEFORM_BEND;
  eBrushBoundaryFalloffType boundary_falloff_type = BRUSH_BOUNDARY_FALLOFF_CONSTANT;
  float boundary_offset = 0;

  /* cloth */
  eBrushClothDeformType cloth_deform_type = BRUSH_CLOTH_DEFORM_DRAG;
  eBrushClothForceFalloffType cloth_force_falloff_type = BRUSH_CLOTH_FORCE_FALLOFF_RADIAL;
  eBrushClothSimulationAreaType cloth_simulation_area_type = BRUSH_CLOTH_SIMULATION_AREA_LOCAL;

  float cloth_mass = 1;
  float cloth_damping = 0.01;

  float cloth_sim_limit = 2.5f;
  float cloth_sim_falloff = 0.75f;

  float cloth_constraint_softbody_strength = 0;

  /* smooth */
  eBrushSmoothDeformType smooth_deform_type = BRUSH_SMOOTH_DEFORM_LAPLACIAN;
  float surface_smooth_shape_preservation = 0;
  float surface_smooth_current_vertex = 0;
  int surface_smooth_iterations = 0;

  /* multiplane scrape */
  float multiplane_scrape_angle = 0;

  /* smear */
  eBrushSmearDeformType smear_deform_type = BRUSH_SMEAR_DEFORM_DRAG;

  /* slide/relax */
  eBrushSlideDeformType slide_deform_type = BRUSH_SLIDE_DEFORM_DRAG;

  /* Scene Project brush */
  eBrushProjectRayDirection project_ray_direction_type = BRUSH_PROJECT_RAY_DIRECTION_VIEW_NORMAL;
  char _pad2[3] = {};
  float minimum_distance = 0.0f;

  /* overlay */
  int texture_overlay_alpha = 33;
  int mask_overlay_alpha = 33;
  int cursor_overlay_alpha = 33;

  float unprojected_size = 0.10f; /* diameter of the brush in Blender units */

  /* soften/sharpen */
  float sharp_threshold = 0;
  int blur_kernel_radius = 2;
  eBlurKernelType blur_mode = KERNEL_GAUSSIAN;

  /* fill tool */
  float fill_threshold = 0.2f;

  float add_col[4] = {1.0, 0.39, 0.39, 0.9};
  float sub_col[4] = {0.39, 0.39, 1.0, 0.9};

  float stencil_pos[2] = {256, 256};
  float stencil_dimension[2] = {256, 256};

  float mask_stencil_pos[2] = {256, 256};
  float mask_stencil_dimension[2] = {256, 256};

  struct BrushGpencilSettings *gpencil_settings = nullptr;
  struct BrushCurvesSculptSettings *curves_sculpt_settings = nullptr;

  DNA_DEPRECATED int automasking_cavity_blur_steps = 0;
  DNA_DEPRECATED float automasking_cavity_factor = 1.0f;

  DNA_DEPRECATED struct CurveMapping *automasking_cavity_curve = nullptr;
  struct MeshAutomaskingSettings *mesh_automasking_settings = nullptr;

  /** Every Curve Patch setting of this brush. Embedded by value rather than held by pointer (unlike
   * #gpencil_settings and #curves_sculpt_settings): there is no optional lifetime here -- the
   * settings either belong to every brush or to none -- and by-value costs no alloc, free or
   * blend-file read of its own. */
  BrushCurvePatchSettings curve_patch;

  /** Roll stroke method (#BRUSH_STROKE_ROLL). Scale the rolled texture with the pressure-driven
   * brush radius so the pattern keeps its aspect ratio under pressure. */
  char roll_pressure_scale = 1;
  /** Roll stroke method: after the stroke finishes, hand the drawn contour off to the Curve Patch
   * editor as an editable control curve. */
  char roll_edit_after = 0;
  char _pad5[6] = {};
};

struct PaletteColor {
  struct PaletteColor *next = nullptr, *prev = nullptr;
  /* Two values, one to store color, other to store values for sculpt/weight. */
  float color[3] = {};
  float value = 0;

  /* For forward compatibility. */
  DNA_DEPRECATED float rgb[3] = {};
  float _pad = {};
};

struct Palette {
#ifdef __cplusplus
  /** See #ID_Type comment for why this is here. */
  static constexpr ID_Type id_type = ID_PAL;
#endif

  ID id;

  /** Pointer to individual colors. */
  ListBaseT<PaletteColor> colors = {nullptr, nullptr};

  int active_color = 0;
  char _pad[4] = {};
};

struct PaintCurvePoint {
  /** Bezier handle. */
  BezTriple bez = {};
  /** Pressure on that point. */
  float pressure = 0;
};

struct PaintCurve {
#ifdef __cplusplus
  /** See #ID_Type comment for why this is here. */
  static constexpr ID_Type id_type = ID_PC;
#endif

  ID id;
  /** Legacy 2D screen-space representation, no longer written or read. */
  DNA_DEPRECATED PaintCurvePoint *points = nullptr;
  DNA_DEPRECATED int tot_points = 0;
  /** Index where next point will be added. */
  int add_index = 0;

  /**
   * Authoritative 3D representation with full attribute support.
   * One bezier curve with control points stored in object space of the active object.
   */
  CurvesGeometry geometry;

  /**
   * When non-zero, `geometry` is the authoritative representation.
   * Defaults to 1 so new curves are created in 3D mode.
   */
  char use_3d_space = 1;
  /** When set, draw interactive radius handles at each control point. */
  char show_radius_handles = 1;
  char _pad0[2] = {0};
  /** Active spline index for multi-spline editing (clamped at use). */
  int active_curve = 0;
};

}  // namespace blender
