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
#include "DNA_uuid_types.h"    /* for bUUID */

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

/**
 * Tangent-space convention of a Normal channel's source image, matching the Shader Editor's
 * Normal Map node: OpenGL stores +Y (green) pointing up, DirectX stores it flipped.
 * Meaningless for any channel other than #PAINT_MATERIAL_CHANNEL_NORMAL.
 */
enum eBrushMaterialPaintNormalSpace : int8_t {
  BRUSH_MATERIAL_PAINT_NORMAL_SPACE_OPENGL = 0,
  BRUSH_MATERIAL_PAINT_NORMAL_SPACE_DIRECTX = 1,
};

/**
 * How a channel's source image is picked in the UI: through the Image Browser popover, or
 * through a grid embedded in the channel's own subpanel. Purely a UI preference; it has no
 * effect on painting.
 */
enum eBrushMaterialPaintSourceSelectMode : int8_t {
  BRUSH_MATERIAL_PAINT_SOURCE_SELECT_IMAGE_BROWSER = 0,
  BRUSH_MATERIAL_PAINT_SOURCE_SELECT_GRID = 1,
};

/**
 * Where a PBR Paint brush takes its channel textures from. The two are mutually exclusive: a
 * brush either reads the per-channel #BrushMaterialPaintChannel.source_mtex slots, or reads one
 * #Material through its Principled BSDF inputs.
 */
enum eBrushMaterialPaintSourceMode : int8_t {
  BRUSH_MATERIAL_PAINT_SOURCE_MAPS = 0,
  BRUSH_MATERIAL_PAINT_SOURCE_MATERIAL = 1,
};

/**
 * Which channel drives the scalar value written into the mask while painting in Mask mode
 * (#PAINT_LAYER_TARGET_MASK). Only read when the brush is the Mask brush.
 */
enum eBrushMaterialPaintMaskSource : int8_t {
  /** The brush's Alpha channel; falls back to #BRUSH_MATERIAL_PAINT_MASK_SOURCE_COVERAGE when off. */
  BRUSH_MATERIAL_PAINT_MASK_SOURCE_ALPHA = 0,
  /** Rec.709 luminance of the brush's Base Color, in linear space. */
  BRUSH_MATERIAL_PAINT_MASK_SOURCE_BASE_COLOR = 1,
  /**
   * Grey level of the brush's primary (or secondary when inverted) color: the plain mean of its
   * RGB, the way a color stroke has always been written into a mask. Not the luminance used for
   * #BRUSH_MATERIAL_PAINT_MASK_SOURCE_BASE_COLOR, which weighs the channels by perceived brightness.
   */
  BRUSH_MATERIAL_PAINT_MASK_SOURCE_COVERAGE = 2,
};

/** #BrushMaterialPaint.mask_flag bit: the brush is the Mask brush created by the default Mask
 * setup, not any brush the user picked. */
#define BRUSH_MATERIAL_PAINT_MASK_DEFAULT_BRUSH (1 << 0)

/** How a source material's channels are laid out onto the paint target. */
enum eBrushMaterialPaintSourceLayout : int8_t {
  /** Through the shared brush mapping, like the Maps slots. */
  BRUSH_MATERIAL_PAINT_SOURCE_LAYOUT_BRUSH = 0,
  /** One to one with the target's UV. */
  BRUSH_MATERIAL_PAINT_SOURCE_LAYOUT_TARGET_UV = 1,
};

/** When a Layer brush source picks up edits made to its material or row. */
enum eBrushMaterialPaintSourceRefreshMode : int8_t {
  /** Re-bake automatically, after #BrushMaterialPaint.source_refresh_delay. */
  BRUSH_MATERIAL_PAINT_SOURCE_REFRESH_AUTO = 0,
  /** Keep the current bake until the user runs Update Now. */
  BRUSH_MATERIAL_PAINT_SOURCE_REFRESH_MANUAL = 1,
};

/**
 * Stroke parameter preset applied by an Erase brush (#BrushMaterialPaint.erase_mode), mirroring
 * the stock texture paint Erase brushes. The mode is a one-shot preset (falloff, antialiasing):
 * not a runtime branch, so user tweaks made afterwards are kept.
 */
enum ePaintMaterialEraseMode : int8_t {
  BRUSH_MATERIAL_PAINT_ERASE_HARD = 0,
  /* 1 was "Hard Pressure" (removed): pressure is the brush's own pressure toggles. Keep unused so
   * Soft and Pixel Art keep their stored values. */
  BRUSH_MATERIAL_PAINT_ERASE_SOFT = 1,
  BRUSH_MATERIAL_PAINT_ERASE_PIXEL_ART = 2,
};

/**
 * Bits of #BrushMaterialPaintCorrection.flag. Additive on purpose: a new effect takes a free bit
 * instead of a new field, so the struct keeps its size and a file written by an older build keeps
 * its meaning.
 */
enum eBrushMaterialPaintCorrectionFlag {
  /** Master switch; clear leaves the sample untouched whatever the parameters say. */
  BRUSH_MATERIAL_PAINT_CORRECTION_ENABLED = (1 << 0),
  /** `1 - v` over the channel range (color channels: `|c - 1|` per component). */
  BRUSH_MATERIAL_PAINT_CORRECTION_INVERT = (1 << 1),
  /** Remap Input Black..Input White through Gamma onto Output Black..Output White. */
  BRUSH_MATERIAL_PAINT_CORRECTION_LEVELS = (1 << 2),
  /** Hue/Saturation/Value adjustment like the Hue/Saturation/Value shader node. */
  BRUSH_MATERIAL_PAINT_CORRECTION_HSV = (1 << 3),
};

/**
 * Pointwise correction of a channel's source image ("Correction Image"): adjustments applied to
 * every sampled texel of #BrushMaterialPaintChannel.source_mtex before the stroke uses it, so
 * the texture arrives prepared for projection. The order is fixed: Levels, then
 * Hue/Saturation/Value (color channels only), then Invert. Parameters are kept when
 * #BRUSH_MATERIAL_PAINT_CORRECTION_ENABLED is cleared, so switching the feature off and back on
 * restores them.
 */
struct BrushMaterialPaintCorrection {
  DNA_DEFINE_CXX_METHODS(BrushMaterialPaintCorrection)

  /** #eBrushMaterialPaintCorrectionFlag bits. */
  short flag = 0;
  char _pad[2] = {};
  float levels_in_black = 0.0f;
  float levels_in_white = 1.0f;
  float levels_gamma = 1.0f;
  float levels_out_black = 0.0f;
  float levels_out_white = 1.0f;
  /** Neutral values: hue 0.5, saturation 1, value 1 (the node's own identity). */
  float hsv_hue = 0.5f;
  float hsv_saturation = 1.0f;
  float hsv_value = 1.0f;
};

/**
 * Which part of the sampled Mask slot texture the correction reads. The Mask slot always reduces
 * its texture to one scalar; the channel decides how that scalar is taken from the RGBA the
 * texture engine returns.
 */
enum eBrushMaskCorrectionChannel : int8_t {
  /** Rec.709 luminance of the RGB, the texture engine's own `intensity`. */
  BRUSH_MASK_CORRECTION_CHANNEL_LUMINANCE = 0,
  BRUSH_MASK_CORRECTION_CHANNEL_RED = 1,
  BRUSH_MASK_CORRECTION_CHANNEL_GREEN = 2,
  BRUSH_MASK_CORRECTION_CHANNEL_BLUE = 3,
  BRUSH_MASK_CORRECTION_CHANNEL_ALPHA = 4,
};

/** Kernel of the Mask correction's spatial Blur stage. */
enum eBrushMaskCorrectionBlurShape : int8_t {
  BRUSH_MASK_CORRECTION_BLUR_GAUSSIAN = 0,
  BRUSH_MASK_CORRECTION_BLUR_BOX = 1,
};

/**
 * Direction of the spatial morph stage. Erode takes the darkest value of the neighborhood (the
 * mask opens up), Dilate the brightest (the mask spreads out).
 */
enum eBrushMaskCorrectionMorphMode : int8_t {
  BRUSH_MASK_CORRECTION_MORPH_ERODE = 0,
  BRUSH_MASK_CORRECTION_MORPH_DILATE = 1,
};

/**
 * Bits of #BrushMaskCorrection.flag. Additive on purpose: a new stage takes a free bit instead of
 * a field, so the struct keeps its size and an older build keeps the meaning of what it does know.
 */
enum eBrushMaskCorrectionFlag {
  /** Master switch; clear leaves the sample untouched whatever the parameters say. */
  BRUSH_MASK_CORRECTION_ENABLED = (1 << 0),
  /** Remap Input Black..Input White through Gamma onto Output Black..Output White. */
  BRUSH_MASK_CORRECTION_LEVELS = (1 << 1),
  /** Soft compression of the range around Min..Max by Softness. */
  BRUSH_MASK_CORRECTION_SMOOTHSTEP = (1 << 2),
  /** Apply the attached #CurveMapping to the value. */
  BRUSH_MASK_CORRECTION_CURVE = (1 << 3),
  /** Binary threshold at #BrushMaskCorrection.threshold. */
  BRUSH_MASK_CORRECTION_THRESHOLD = (1 << 4),
  /** Quantize to #BrushMaskCorrection.posterize_steps levels. */
  BRUSH_MASK_CORRECTION_POSTERIZE = (1 << 5),
  /** Clamp into #BrushMaskCorrection.clamp_min..clamp_max. */
  BRUSH_MASK_CORRECTION_CLAMP = (1 << 6),
  /** Spatial Gaussian/Box blur, image textures only. */
  BRUSH_MASK_CORRECTION_BLUR = (1 << 7),
  /** Spatial unsharp mask, image textures only. */
  BRUSH_MASK_CORRECTION_SHARPEN = (1 << 8),
  /** Spatial erode/dilate, image textures only. */
  BRUSH_MASK_CORRECTION_MORPH = (1 << 9),
  /** Spatial median filter, image textures only. */
  BRUSH_MASK_CORRECTION_MEDIAN = (1 << 10),
};

/**
 * Extended correction of the Mask slot's scalar coverage ("Correction Image"), the counterpart of
 * #BrushMaterialPaintCorrection for #Brush.mask_mtex.
 *
 * Two groups share one struct because they describe one map:
 * - pointwise stages (Levels, Smoothstep, Curve, Threshold, Posterize, Clamp), applied to every
 *   sample of any texture, at any mapping;
 * - spatial stages (Blur, Sharpen, Erode/Dilate, Median), applied only to image textures with a
 *   pixel buffer, in that order, before the pointwise stack.
 *
 * Parameters are kept when #BRUSH_MASK_CORRECTION_ENABLED is cleared, so switching the feature off
 * and back on restores them. The #curve pointer is owned by the brush and serialized with it.
 */
struct BrushMaskCorrection {
  DNA_DEFINE_CXX_METHODS(BrushMaskCorrection)

  /** #eBrushMaskCorrectionFlag bits. */
  short flag = 0;
  /** #eBrushMaskCorrectionChannel. */
  int8_t channel = BRUSH_MASK_CORRECTION_CHANNEL_LUMINANCE;
  /** #eBrushMaskCorrectionBlurShape. */
  int8_t blur_shape = BRUSH_MASK_CORRECTION_BLUR_GAUSSIAN;
  /** #eBrushMaskCorrectionMorphMode. */
  int8_t morph_mode = BRUSH_MASK_CORRECTION_MORPH_ERODE;
  char _pad[3] = {};

  float levels_in_black = 0.0f;
  float levels_in_white = 1.0f;
  float levels_gamma = 1.0f;
  float levels_out_black = 0.0f;
  float levels_out_white = 1.0f;

  /** Smoothstep: input range and how much of the smooth curve to mix in (0 = linear). */
  float smoothstep_min = 0.0f;
  float smoothstep_max = 1.0f;
  float smoothstep_softness = 1.0f;

  /** Threshold stage: values >= this become 1, the rest 0. */
  float threshold = 0.5f;
  /** Posterize stage: number of output levels, RNA-clamped 1..64. */
  int posterize_steps = 4;
  float clamp_min = 0.0f;
  float clamp_max = 1.0f;

  /** Pointwise curve, evaluated on the combined channel. Null until the block is allocated. */
  struct CurveMapping *curve = nullptr;

  /**
   * Spatial radii, in image texels. The RNA setters raise a zero radius to one when a stage is
   * switched on; only a zero left in a damaged or pre-versioned file disables the stage.
   */
  float blur_radius = 1.0f;
  float sharpen_radius = 1.0f;
  float sharpen_amount = 0.5f;
  float morph_radius = 1.0f;
  float median_radius = 1.0f;

  /** Keeps the struct 8-byte aligned (makesdna inserts no tail padding of its own). */
  char _pad_tail[4] = {};
};

struct BrushMaterialPaintChannel {
  DNA_DEFINE_CXX_METHODS(BrushMaterialPaintChannel)

  /**
   * Paint value. Scalar channels use \a value[0]; Normal uses the full XYZ tangent
   * (default flat tangent). Unused for Base Color (Base Color follows
   * #BrushMaterialPaint.base_color).
   */
  float value[3] = {0.0f, 0.0f, 1.0f};
  /** #IMB_BlendMode. */
  short blend = 0;
  char use = 0;
  /** #eBrushMaterialPaintNormalSpace. Only read for the Normal channel. */
  char normal_space = 0;
  /** #eBrushMaterialPaintSourceSelectMode. UI-only; zero keeps the Image Browser popover. */
  char source_select_mode = 0;
  /** UI-only: collapses the inline preview grid shown when #source_select_mode is Grid. */
  char show_source_grid = 1;
  /**
   * Inverts the sampled #source_mtex value (`1 - v` over the channel range). Only read for the
   * Alpha and Height channels: the inversion is a property of the texture, so it applies wherever
   * the channel is sampled, not only for a Mask stroke. For Height, `1 - v` over the stored range
   * mirrors the relief around its neutral (a bump becomes a dent).
   */
  char source_invert = 0;
  char _pad1 = 0;
  /**
   * Second paint value of the Alpha channel. Only read in Mask mode, where Alpha primary is
   * \a value[0] and this is the one used for an inverted (secondary) stroke.
   */
  float value_secondary = 0.0f;
  /* #source_mtex is 8-byte aligned; the fields above end exactly at its offset (24). */

  /**
   * Source texture sampled instead of `value` / base color. Inactive when `tex` is null.
   * Sampled through the same brush mapping machinery as #Brush.mtex, so all brush map modes
   * work without extra coordinate handling.
   */
  struct MTex source_mtex;

  /**
   * Source-image correction ("Correction Image") applied to every sample of #source_mtex,
   * see #BrushMaterialPaintCorrection. Null in files that predate it: the feature is off.
   * Not read for the Normal channel: corrections would break the tangent-space encoding (its
   * green-flip toggle is the dedicated correction there). Stored in the brush, so presets and
   * brush assets carry the settings along.
   */
  struct BrushMaterialPaintCorrection *correction = nullptr;
};

struct BrushMaterialPaint {
  DNA_DEFINE_CXX_METHODS(BrushMaterialPaint)

  /** Indexed by #eMaterialPaintChannel. */
  BrushMaterialPaintChannel channels[/*PAINT_MATERIAL_CHANNEL_NUM*/ 18] = {};
  /** RGB written by the Base Color channel when not synced with the brush color. */
  float base_color[3] = {1.0f, 1.0f, 1.0f};
  char use_sync_base_color_with_brush = 1;
  /** When the Alpha channel is enabled, write stroke values into the Alpha map. */
  char use_alpha_map = 0;
  /** When the Alpha channel is enabled, sample Alpha to mask other channels' writes this stroke. */
  char use_alpha_stroke_mask = 1;
  /** #ePaintMaterialEraseMode. */
  char erase_mode = 0;
  /**
   * Mapping (map_mode, size, angle) shared by every channel's source texture, so multi-channel
   * patterns (e.g. a Base Color texture with a matching Normal/Roughness texture) stay aligned
   * instead of drifting apart because one channel's mapping was tweaked independently.
   * #MTex.tex is never set here; each channel keeps its own source texture identity in
   * #BrushMaterialPaintChannel.source_mtex.tex, only reading mapping from here at paint time.
   */
  struct MTex shared_source_mapping;
  /** #eBrushMaterialPaintSourceMode. */
  char source_mode = 0;
  /** #eBrushMaterialPaintSourceLayout. */
  char source_layout = 0;
  /** #eBrushMaterialPaintMaskSource. Only read when the brush paints the Mask. */
  char mask_source = 0;
  /** Bit flags, see #BRUSH_MATERIAL_PAINT_MASK_DEFAULT_BRUSH. */
  char mask_flag = 0;
  /**
   * Image Transform Settings: edit #shared_source_mapping's Size X and Size Y together, keeping
   * them equal. Lives in former padding, so the struct layout is unchanged; files that predate it
   * are switched to the locked default by `versioning_520.cc`.
   */
  char shared_mapping_scale_lock = 1;
  /**
   * Inverts the Mask Source texture's coverage (`1 - c`) while the brush paints the Mask. Lives in
   * former padding, so the struct layout is unchanged. Independent of the channels' own
   * #BrushMaterialPaintChannel.source_invert, so it covers every source the Mask can select (Alpha
   * and Base Color alike) without ever touching ordinary - non-Mask - strokes: the mask paths read
   * it, the per-channel scalar reads never do. Files written before the field existed carry the
   * Alpha channel's flag here, see `versioning_520.cc`.
   */
  char mask_source_invert = 0;
  /**
   * #eBrushMaterialPaintSourceSelectMode. UI-only; how the Mask texture is picked in the Mask
   * panel: Image Browser popover (0) or the inline preview grid. Lives in former padding, so the
   * struct layout is unchanged.
   */
  char mask_source_select_mode = 0;
  /**
   * UI-only: collapses the inline preview grid shown when #mask_source_select_mode is Grid.
   * Stored inverted (hide) so files that predate the field read 0 = shown without a doversion.
   * Lives in former padding, so the struct layout is unchanged.
   */
  char hide_mask_source_grid = 0;
  /* #source_material is pointer-aligned: 2 (modes) + 2 (mask flag) + 1 (scale lock) + 1 (mask
   * source invert) + 1 (mask source select mode) + 1 (hide mask source grid) + 4 (_pad_modes)
   * keep the int + pointer pair below on their natural boundaries without hidden compiler
   * padding. */
  char _pad_modes[4] = {};
  /**
   * Side of the square bake buffer in pixels. Deliberately smaller than
   * #PaintModeSettings.new_channel_image_size: this is a scratch buffer several channels of which
   * are held at once, not a paint target.
   */
  int source_bake_size = 2048;
  /** Channel source when #source_mode is #BRUSH_MATERIAL_PAINT_SOURCE_MATERIAL. */
  struct Material *source_material = nullptr;
  /** #eBrushMaterialPaintMaterialSourceKind. Only read in Material source mode. */
  char material_source_kind = 0;
  /** #eBrushMaterialPaintLayerSourceScope. Only read for the Layer kind. */
  char layer_source_scope = 0;
  /** #eBrushMaterialPaintSourceRefreshMode. Only read for the Layer kind. */
  char source_refresh_mode = 0;
  char _pad_layer_source = 0;
  /**
   * Seconds an automatic refresh waits after the last edit, so a burst of edits bakes once.
   * Zero refreshes immediately.
   */
  float source_refresh_delay = 0.5f;
  /**
   * Layered material the Layer kind reads from. Separate from #source_material so the Default
   * and Layer selections survive switching between them, and it may be a different material
   * than the one being painted (the canvas material).
   */
  struct Material *source_layer_owner = nullptr;
  /**
   * Marker of the stack row isolated by #BRUSH_MATERIAL_PAINT_LAYER_SOURCE_SCOPE_ROW. Nil in
   * Full scope; a marker that no longer resolves makes the source stale.
   */
  bUUID source_layer_marker = {};
  /**
   * Deprecated: per-stroke size randomization moved to #Brush::size_random, which every stroke
   * consumer reads now. Kept in DNA only so `versioning_520.cc` can migrate old files' amounts
   * into the brush field, which then zeroes this; nothing else may read or write it. The old
   * #mask_strength_random (the Mask stroke's separate amount) is not kept: the shared
   * #Brush::strength_random serves Mask strokes too.
   */
  float size_random = 0.0f;
  /** Deprecated: per-stroke strength randomization moved to #Brush::strength_random, see
   * #size_random. */
  float strength_random = 0.0f;
};

/** Max number of propagation steps for automasking settings. */
#define AUTOMASKING_BOUNDARY_EDGES_MAX_PROPAGATION_STEPS 20

/** What one Curve Patch texture slot holds (#BrushCurvePatchTextureSlot::kind). */
enum eBrushCurvePatchSlotKind : int8_t {
  /** A single texture (#BrushCurvePatchTextureSlot::tex): the pre-PBR behavior. */
  BRUSH_CURVE_PATCH_SLOT_TEXTURE = 0,
  /**
   * A PBR set: the slot declares which material paint channels it covers
   * (#BrushCurvePatchTextureSlot::channels) and carries one image per channel. A stamp that drew
   * this slot paints every enabled channel whose image is set, so one slot stands for a whole
   * Base Color + Normal + Roughness ... bundle the brush stores for it.
   */
  BRUSH_CURVE_PATCH_SLOT_PBR = 1,
  /**
   * A live reference to a Material (#BrushCurvePatchTextureSlot::material): the images linked to
   * its Principled BSDF inputs are the per-channel images, resolved on every re-stamp, so edits to
   * the material reach the stamps without copying anything into the slot.
   */
  BRUSH_CURVE_PATCH_SLOT_MATERIAL = 2,
};

/** One channel of a PBR-kind Curve Patch texture slot (#BrushCurvePatchTextureSlot::channels),
 * indexed by #eMaterialPaintChannel. Modelled on #BrushMaterialPaintChannel's own DNA shape so
 * the RNA collection, the UI drop rows and the sampling plumbing all read the same way.
 *
 * Must stay trivially copyable: the slot is duplicated by `BLI_duplicatelist()`, which copies the
 * node's bytes and runs no constructor. */
struct BrushCurvePatchSlotChannel {
  /**
   * Whether the PBR slot DECLARES this channel. Declaring is INTENT, not data: an enabled channel
   * without an #image is skipped by the stamps (and the UI warns), so a half-filled set never
   * paints a channel from the brush's own sources by surprise.
   */
  char enabled = 0;
  /** Keeps #image pointer-aligned; makesdna inserts no padding of its own. */
  char _pad[7] = {};
  /**
   * The channel's stamp image. Null until the user drops one. The slot list holds a user on every
   * non-null image, same as it does on the TEXTURE kind's `tex`.
   */
  struct Image *image = nullptr;
};

/** One entry in a brush's Curve Patch texture list (#BrushCurvePatchSettings::texture_slots), used
 * by the Curve Patch STAMPS mode when its texture source is #BRUSH_CURVE_PATCH_TEX_MULTI.
 *
 * Must stay trivially copyable: `brush_copy_data()` duplicates the list with
 * `BLI_duplicatelist()`, which copies each node's bytes through `MEM_dupallocN` and runs no
 * constructor. Adding a member with a non-trivial constructor (a `std::string`, a container) would
 * leave that member's bytes aliased between the two brushes. */
struct BrushCurvePatchTextureSlot {
  struct BrushCurvePatchTextureSlot *next = nullptr, *prev = nullptr;
  /** (TEXTURE kind) Sampled through a copy of #Brush::mtex with only this `tex` swapped in, so
   * every mapping setting stays shared with the brush. A null `tex` makes the stamp that drew this
   * slot skip itself entirely -- unlike the brush's own null texture, which sculpts flat. A PBR
   * slot may still carry a `tex`: it then serves as the stamp's coverage mask (a PBR set without
   * one falls back to its Base Color image's alpha, when Base Color is enabled and set). */
  struct Tex *tex = nullptr;
  /** Relative probability weight in the per-stamp draw. Zero disables the slot without deleting
   * it; a list whose weights all sum to zero falls back to the brush's own texture. */
  float weight = 1.0f;
  /** #eBrushCurvePatchSlotKind. */
  char kind = 0;
  char _pad[1] = {};
  /** (MATERIAL kind) Resolution of the cached bake of #material, as for
   * #BrushMaterialPaint::source_bake_size. Zero (a file written before the field existed) reads as
   * the default 2048. Also keeps #material pointer-aligned; makesdna inserts no padding. */
  int16_t bake_size = 2048;
  /** (MATERIAL kind) The material whose Principled image inputs feed the stamps. The list holds a
   * user on it, as it does on `tex`. Kept when the kind flips away, so a flip back restores it. */
  struct Material *material = nullptr;
  /**
   * (PBR kind) The slot's channels, indexed by #eMaterialPaintChannel (array size 18 =
   * #PAINT_MATERIAL_CHANNEL_NUM, spelled as a literal per the DNA convention -- see
   * #BrushMaterialPaint.channels). Unused for the TEXTURE kind, whose entries stay disabled.
   */
  struct BrushCurvePatchSlotChannel channels[/*PAINT_MATERIAL_CHANNEL_NUM*/ 18] = {};
};

/**
 * Curve Patch settings, one set per brush.
 *
 * These lived split across #Brush and #MTex until they were gathered here: the #MTex half was
 * formally per-texture-slot, but nothing ever read it from any slot other than #Brush::mtex, so
 * the split described nothing real and made the Python API address one feature through two
 * unrelated paths.
 *
 * Makesdna does not insert alignment padding on its own (see `check_member_alignment()` in
 * `makesdna.cc`), so `_pad[6]` sits between `end_point_shape` and `stamp_mode` below to align the
 * following pointer-containing members to eight bytes, and `_pad1[4]` sits after the deprecated
 * stamp randoms to do the same for `tex_start`; new `char` settings should be appended after
 * `stamp_mode`'s group, not before the padding.
 */
struct BrushCurvePatchSettings {
  /** STAMPS mode texture list, active only when #stamp_texture_source is
   * #BRUSH_CURVE_PATCH_TEX_MULTI. Holds ID pointers with user counts, so brush copy, free and
   * `foreach_id` all have to walk it. */
  ListBaseT<BrushCurvePatchTextureSlot> texture_slots = {nullptr, nullptr};
  int texture_active_index = 0;

  /** When set, committing a Curve Patch (or Roll) edit assigns a new face set to the faces its
   * relief actually raised. */
  char face_set = 0;
  /** Swap which texture axis runs along the control curve's arc-length (false = V runs along the
   * curve, the default; true = U). `char`, not `bool`: makesdna has no builtin size for `bool`. */
  char swap_axis = false;
  /** How one texture tile is mapped along the arc-length. See #eBrushCurvePatchLengthMode. */
  char length_mode = 0;
  /** REPEAT mode: number of texture repeats along the curve length (RNA-clamped 1..64). */
  char length_repeat = 1;
  /** How the relief terminates at the curve's two ends. See #eBrushCurvePatchEndFalloff. */
  char end_falloff = BRUSH_CURVE_PATCH_END_HARD;
  /** SMOOTH end falloff: length of the fade at each end, as a percentage of the curve's total
   * arc-length (RNA-clamped 0..50). The 50 ceiling keeps the two end zones from ever overlapping.
   */
  char end_falloff_percent = 10;
  /** Profile of the relief at the start/end of an open curve. See #eBrushCurvePatchPointShape. */
  char start_point_shape = BRUSH_CURVE_PATCH_POINT_SQUARE;
  char end_point_shape = BRUSH_CURVE_PATCH_POINT_SQUARE;
  char _pad[6] = {};
  /** Whether the texture is projected as one continuous stretched sheet along the curve (Ribbon,
   * the original behavior) or as discrete randomized stamps. See #eBrushCurvePatchStampMode. */
  char stamp_mode = BRUSH_CURVE_PATCH_STAMP_RIBBON;
  /** STAMPS mode: which coordinate frame a stamp's texture is sampled in. See
   * #eBrushCurvePatchStampProjection. */
  char stamp_projection = BRUSH_CURVE_PATCH_STAMP_PROJ_CURVE;
  /** Whether every stamp samples the brush's own texture or draws one at random from
   * #texture_slots. See #eBrushCurvePatchTexSource. */
  char stamp_texture_source = BRUSH_CURVE_PATCH_TEX_SINGLE;
  /** STAMPS mode: where the stamps land. #BRUSH_CURVE_PATCH_STAMP_LAYOUT_FILL spaces them every
   * Spacing along the arc length; #BRUSH_CURVE_PATCH_STAMP_LAYOUT_POINTS centers one stamp on
   * every control point, which makes Spacing irrelevant (see #eBrushCurvePatchStampLayout). */
  char stamp_layout = BRUSH_CURVE_PATCH_STAMP_LAYOUT_FILL;
  /** POINTS layout: show handles to edit each point's own stamp size (the per-point `radius`
   * attribute) in the curve editor. Only a view toggle: the attribute exists either way. */
  char show_point_radius_handles = false;
  /** POINTS layout: show handles to edit each point's own stamp strength (the
   * `paintcurve_stamp_strength` attribute) in the curve editor. */
  char show_point_strength_handles = false;
  /** How the per-point `radius` attribute transitions between neighboring control points when
   * the ribbon's width is tessellated. See #eBrushCurvePatchRadiusInterp. Mostly meaningful in
   * RIBBON mode, where the width follows the interpolated radii. */
  char radius_interp = BRUSH_CURVE_PATCH_RADIUS_INTERP_SPLINE;
  /**
   * Deprecated: STAMPS mode per-stamp size randomization as a percentage (RNA-clamped 0..100),
   * moved to #Brush::size_random, which stores a fraction instead. Kept in DNA only so
   * `versioning_520.cc` can migrate old files' amounts (divided by 100) into the brush field,
   * which then zeroes this; nothing else may read or write it.
   */
  char stamp_size_random = 0;
  /** Deprecated: STAMPS mode per-stamp strength randomization, moved to #Brush::strength_random,
   * see #stamp_size_random. */
  char stamp_strength_random = 0;
  /** STAMPS FILL layout: per-stamp spacing randomization as a percentage (RNA-clamped 0..100).
   * Each stamp's position shifts along the curve by up to +-Spacing Random of the stamp step, on
   * top of the world-space Jitter scatter. */
  char stamp_spacing_random = 0;
  /** STAMPS mode: chance in percent that a stamp's texture mirrors horizontally (per stamp, a
   * hash draw; random flip). */
  char stamp_flip_x_random = 0;
  /** STAMPS mode: chance in percent that a stamp's texture mirrors vertically. */
  char stamp_flip_y_random = 0;
  /* Keeps `tex_start` on its 8-byte boundary. */
  char _pad1[1] = {};
  /** Whether the whole ribbon carries the brush's own texture or splits into the Start / Middle /
   * End textures below. See #eBrushCurvePatchTexSource. */
  char ribbon_texture_source = BRUSH_CURVE_PATCH_TEX_SINGLE;

  /** RIBBON mode Start / Middle / End textures, active only when #ribbon_texture_source is
   * #BRUSH_CURVE_PATCH_TEX_MULTI. A null entry leaves its stretch of the ribbon untouched. */
  struct Tex *tex_start = nullptr;
  struct Tex *tex_middle = nullptr;
  struct Tex *tex_end = nullptr;

  /** Arc length the Start / End textures occupy, in brush DIAMETERS -- 1.0 makes a cap as long as
   * the ribbon is wide, so its texture is not distorted. Zero means no cap. */
  float cap_start_length = 1.0f;
  float cap_end_length = 1.0f;
};

/** Settings of the Clone Stamp Mesh sculpt brush (#SCULPT_BRUSH_TYPE_CLONE_MESH). Embedded by
 * value for the same reasons as #BrushCurvePatchSettings: no optional lifetime, no extra
 * alloc or blend-file read. */
struct BrushCloneStampMeshSettings {
  /** How the sampled source form is applied to the target surface. See
   * #eCloneStampMeshApplyMode. */
  char apply_mode = CLONE_STAMP_MESH_APPLY_IMPRINT;
  /** What happens to a vertex whose source probe found no surface. See
   * #eCloneStampMeshMissingSource. */
  char missing_source = CLONE_STAMP_MESH_MISSING_IGNORE;
  char _pad[2] = {};
  /** Scales the tangential extent of the patch read from the source relative to the brush
   * radius (1.0 samples a source patch of the brush's own world size). RNA-clamped. */
  float sample_scale = 1.0f;
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
  /** Shape used to clip the brush texture when stamping. */
  eBrushTextureClipShape texture_clip_shape = BRUSH_TEXTURE_CLIP_SPHERE;
  char _pad[6] = {};
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
  /**
   * Per-dab size randomization (#bke::PaintRuntime::size_random_value), the single field shared
   * by dab strokes, PBR paint and Curve Patch stamps. Dab strokes multiply the radius by
   * `1 + size_random * (rand - 0.5)`, a symmetric range of +-(size_random / 2) around the base
   * size; Curve Patch stamps apply their own shrink-only factor instead, because a stamp growing
   * past the radius would clip against the patch's bounds.
   */
  float size_random = 0.0f;
  /**
   * Per-dab strength randomization (#bke::PaintRuntime::strength_random_value), the single field
   * shared by dab strokes, PBR paint (Mask strokes included) and Curve Patch stamps: the strength
   * is multiplied by `1 - strength_random * rand`, i.e. only lowered by up to this fraction;
   * 0 keeps the strength exact.
   */
  float strength_random = 0.0f;
  /**
   * Per-dab spacing randomization (#bke::PaintRuntime::spacing_random_value): the spacing between
   * dabs is multiplied by `1 - spacing_random * rand`, so spacing only shrinks. Drawn by the
   * stroke step and consumed by #paint_space_stroke_spacing.
   */
  float spacing_random = 0.0f;
  /* The three random floats above add 12 bytes; this keeps the members that follow on their 8-byte
   * boundary (makesdna does not insert padding itself). */
  char _pad_stroke_random[4] = {};
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

  /** #eBrushDragKind. Kept in sync with #sculpt_brush_type / #stroke_method / #cloth_deform_type
   * by #BKE_brush_drag_kind_update. */
  eBrushDragKind drag_kind = BRUSH_DRAG_KIND_NONE;

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

  /** Grid-resolution level for the VDM insert-mesh stamp. See #eBrushVDMInsertQuality. */
  eBrushVDMInsertQuality vdm_insert_quality = BRUSH_VDM_INSERT_QUALITY_MEDIUM;
  /** Base-shape / black-cropping algorithm for the VDM insert-mesh stamp. See
   * #eBrushVDMInsertMethod. */
  eBrushVDMInsertMethod vdm_insert_method = BRUSH_VDM_INSERT_METHOD_CONFORM;

  /** How texture affects brush behavior. #eBrushTextureDataMode. */
  int texture_data_mode = BRUSH_TEXTURE_DATA_MODE_NONE;
  /**
   * Last Face Sets-from-texture sub-mode picked explicitly (#eBrushTextureDataMode, never
   * #BRUSH_TEXTURE_DATA_MODE_NONE): #texture_data_mode is cleared when the feature is turned off,
   * so the choice has to live separately to survive toggling it back on.
   *
   * The padding keeps the following members (pointer-bearing `MTex` fields among them) on their
   * 8-byte aligned offsets, so inserting this field does not shift the rest of the struct.
   */
  int face_sets_texture_mode = BRUSH_TEXTURE_DATA_MODE_FACE_SETS_FROM_TEXTURE;
  char _pad_fstm[4] = {};
  /** Alpha threshold for binary texture mode. Range [0..1], default 0.5. */
  float texture_threshold = 0.5f;
  /** Explicit Face Set ID to assign (0 = auto-generate on first stroke). */
  int face_set_id = 0;
  char write_vcol = 0;
  char vcol_channel = BRUSH_VCOL_CHANNEL_RGB;
  char vcol_mode = 0;
  char _pad_vcol[1] = {};

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

  /**
   * Random per-dab weakening of the Mask texture's influence, 0..1: each dab scales the slot's
   * coverage toward "no clipping" by `1 - amount * rand` (0 keeps the mask untouched). The
   * transient factor lives in #bke::PaintRuntime::mask_influence_random_value. Older files read 0.
   */
  float mask_texture_strength_random = 0.0f;
  /* Keeps the pointers below on their 8-byte boundary. */
  char _pad_mask_strength_random[4] = {};

  struct BrushGpencilSettings *gpencil_settings = nullptr;
  struct BrushCurvesSculptSettings *curves_sculpt_settings = nullptr;
  /** Lazily allocated by #BKE_brush_material_paint_ensure. */
  struct BrushMaterialPaint *material_paint = nullptr;

  DNA_DEPRECATED int automasking_cavity_blur_steps = 0;
  DNA_DEPRECATED float automasking_cavity_factor = 1.0f;

  DNA_DEPRECATED struct CurveMapping *automasking_cavity_curve = nullptr;
  struct MeshAutomaskingSettings *mesh_automasking_settings = nullptr;

  /** Draw Face Sets color assignment mode (#eSculptFaceSetDrawMode). */
  int face_set_draw_mode = 0;
  /** Last sampled Face Set ID via Ctrl+LMB; -1 if unset. */
  int face_set_sample_id = -1;
  /** Primary color used to identify or create a Face Set when draw mode is Custom (linear RGB). */
  float face_set_color[3] = {144.0f / 255.0f, 178.0f / 255.0f, 218.0f / 255.0f};
  char _pad_fsc[4] = {};
  /** Secondary color, swapped with the primary via the X key (linear RGB). */
  float face_set_secondary_color[3] = {144.0f / 255.0f, 178.0f / 255.0f, 218.0f / 255.0f};
  char _pad_fssc[4] = {};
  /** RGB texture for per-face Face Set color assignment (#eBrushTextureDataMode). */
  struct MTex face_set_color_mtex;
  /** Channels that brush writes to in vertex paint mode (#eBrushVertexPaintChannel). */
  int vertex_paint_channel_flag = (BRUSH_VPAINT_CHANNEL_R | BRUSH_VPAINT_CHANNEL_G |
                                   BRUSH_VPAINT_CHANNEL_B);
  char _pad4[4] = {};

  /** #eImagePaint_SelectionExpand — Fill pick mode. */
  char fill_expand = IMAGE_PAINT_SELECT_EXPAND_FACE;
  /** Draw the mesh wireframe in the viewport while this Fill brush is the active tool. */
  char fill_show_wire = 1;
  /* Keep #Brush 8-byte aligned: #face_set_color_mtex above ends on an 8-byte
   * boundary, so the two chars plus their padding must add up to 8. */
  char _pad_fill[6] = {};

  /** Every Curve Patch setting of this brush. Embedded by value rather than held by pointer
   * (unlike #gpencil_settings and #curves_sculpt_settings): there is no optional lifetime here --
   * the settings either belong to every brush or to none -- and by-value costs no alloc, free or
   * blend-file read of its own. */
  BrushCurvePatchSettings curve_patch;

  /** Clone Stamp Mesh brush settings (#SCULPT_BRUSH_TYPE_CLONE_MESH). Embedded by value like
   * #curve_patch; see #BrushCloneStampMeshSettings. */
  BrushCloneStampMeshSettings clone_stamp_mesh;

  /** Roll stroke method (#BRUSH_STROKE_ROLL). Scale the rolled texture with the pressure-driven
   * brush radius so the pattern keeps its aspect ratio under pressure. */
  char roll_pressure_scale = 1;
  /** Roll stroke method: after the stroke finishes, hand the drawn contour off to the Curve Patch
   * editor as an editable control curve. */
  char roll_edit_after = 0;
  /** Preserve Layers: when Smooth / Surface Smooth records into a sculpt layer, smooth the active
   * layer's own delta over a reference with the layer removed, instead of the composed surface,
   * so the layers below are not flattened into the recorded delta. */
  char use_smooth_preserve_layers = 0;
  char _pad5[5] = {};
  /**
   * Extended correction of the Mask slot's scalar coverage ("Correction Image"), see
   * #BrushMaskCorrection. Lazily allocated by #BKE_brush_mask_correction_ensure; null means the
   * feature is off, and files that predate it read it as null. Appended after `_pad5`, which keeps
   * the new pointer on its 8-byte boundary without shifting any earlier member.
   */
  struct BrushMaskCorrection *mask_correction = nullptr;
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
