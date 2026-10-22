/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 */

#pragma once

#include "DNA_ID.h"
#include "DNA_defs.h"
#include "DNA_listBase.h"
#include "DNA_uuid_types.h"

#include "BLI_enum_flags.hh"

namespace blender {

#ifndef MAX_MTEX
#  define MAX_MTEX 18
#endif

struct AnimData;
struct Image;
struct Material;
struct bNodeTree;

/* MaterialGPencilStyle->flag */
enum eMaterialGPencilStyle_Flag : short {
  /* Fill Texture is a pattern */
  GP_MATERIAL_FILL_PATTERN = (1 << 0),
  /* don't display color */
  GP_MATERIAL_HIDE = (1 << 1),
  /* protected from further editing */
  GP_MATERIAL_LOCKED = (1 << 2),
  /* do onion skinning */
  GP_MATERIAL_HIDE_ONIONSKIN = (1 << 3),
  /* clamp texture */
  GP_MATERIAL_TEX_CLAMP = (1 << 4),
  /* mix fill texture */
  GP_MATERIAL_FILL_TEX_MIX = (1 << 5),
  /* Flip fill colors */
  GP_MATERIAL_FLIP_FILL = (1 << 6),
  /* Stroke Texture is a pattern */
  GP_MATERIAL_STROKE_PATTERN = (1 << 7),
  GP_MATERIAL_STROKE_SHOW = (1 << 8), /* Deprecated. Only used for compatibility. */
  GP_MATERIAL_FILL_SHOW = (1 << 9),   /* Deprecated. Only used for compatibility. */
  /* mix stroke texture */
  GP_MATERIAL_STROKE_TEX_MIX = (1 << 11),
  /* disable stencil clipping (overlap) */
  GP_MATERIAL_DISABLE_STENCIL = (1 << 12),
  /* Material used as stroke masking. */
  GP_MATERIAL_IS_STROKE_HOLDOUT = (1 << 13),
  /* Material used as fill masking. */
  GP_MATERIAL_IS_FILL_HOLDOUT = (1 << 14),
  /* Material use randomization. */
  GP_MATERIAL_USE_DOTS_RANDOMIZATION = static_cast<short>(1 << 15),
};
ENUM_OPERATORS(eMaterialGPencilStyle_Flag)

enum eMaterialGPencilStyle_Mode : int {
  GP_MATERIAL_MODE_LINE = 0,
  GP_MATERIAL_MODE_DOT = 1,
  GP_MATERIAL_MODE_SQUARE = 2,
};

enum eMaterialLineArtFlags : int {
  LRT_MATERIAL_MASK_ENABLED = (1 << 0),
  LRT_MATERIAL_CUSTOM_OCCLUSION_EFFECTIVENESS = (1 << 1),
  LRT_MATERIAL_CUSTOM_INTERSECTION_PRIORITY = (1 << 2),
};
ENUM_OPERATORS(eMaterialLineArtFlags)

/* maximum number of materials per material array.
 * (on object, mesh, light, etc.). limited by
 * short mat_nr in verts, faces.
 * -1 because for active material we store the index + 1 */
#define MAXMAT (32767 - 1)

/** #Material::flag */
enum eMaterial_Flag : short {
  /** For render. */
  MA_IS_USED = 1 << 0, /* UNUSED */
  /** For dope-sheet. */
  MA_DS_EXPAND = 1 << 1,
  /**
   * For dope-sheet (texture stack expander)
   * NOTE: this must have the same value as other texture stacks,
   * otherwise anim-editors will not read correctly.
   */
  MA_DS_SHOW_TEXS = 1 << 2,
};
ENUM_OPERATORS(eMaterial_Flag)

/** #Material::paint_layers_flag */
enum eMaterialPaintLayersFlag : short {
  /** The material carries a paint layer description, a stack of rows addressed by marker. */
  MA_PAINT_LAYERED = 1 << 0,
  /** The description changed; whatever is derived from it is out of date. */
  MA_PAINT_LAYERS_REGEN = 1 << 2,
  /**
   * A blend mode changed without changing the graph's shape: the generated Mix nodes take the new
   * mode in place, through the sync at the regeneration point. Carried in the file like
   * #MA_PAINT_LAYERS_REGEN, so a file saved between the edit and the sync catches up on load.
   */
  MA_PAINT_LAYERS_BLEND_SYNC = 1 << 3,
  /**
   * Some row with a bake had a value-only edit (`opacity`, `fill`, a mask value, `enabled`), or a
   * pixel edit arrived; the planner at the regeneration point re-bakes the rows whose stored hash no
   * longer matches. Set by the edit paths that cannot rebuild the tree the way
   * #MA_PAINT_LAYERS_REGEN does. Runtime only: a file never carries it.
   */
  MA_PAINT_LAYERS_BAKE_STALE = 1 << 4,
  /**
   * The user cancelled the heavy bake job; the worker is not queued again until the next edit, or
   * the same stale rows would restart it right away. Runtime only: a file never carries it.
   */
  MA_PAINT_LAYERS_BAKE_CANCELLED = 1 << 5,
  /**
   * The editor has a bake outstanding for this material: a debounce timer armed after an edit, or
   * a bake job started and not yet finished. BKE cannot see `wmJob` state, so this is the editor's
   * hand-off to #BKE_paint_layers_is_stale, set and cleared by the editor's scheduling code alone.
   * Runtime only: a file never carries it.
   */
  MA_PAINT_LAYERS_BAKE_SCHEDULED = 1 << 6,
  /**
   * An Image Editor shows this material's composite, so the maps of an edited Material row are
   * wanted while it is edited. Set by the editor's scheduling code at every scene update. Runtime
   * only: a file never carries it.
   */
  MA_PAINT_LAYERS_PREVIEW_WANTED = 1 << 7,
};
ENUM_OPERATORS(eMaterialPaintLayersFlag)

/** #Material::paint_layers_preview_flag */
enum eMaterialPaintPreviewFlag : char {
  /** The preview renders after #Material::paint_layers_preview_delay, not right after an edit. */
  MA_PAINT_PREVIEW_DELAYED = 1 << 0,
};

/** #Material::paint_layers_preview_size: the preview's side; capped by the row's own size. */
enum eMaterialPaintPreviewSize : char {
  /** Zero is 2048, so files saved before the setting read as the default. */
  MA_PAINT_PREVIEW_SIZE_2048 = 0,
  MA_PAINT_PREVIEW_SIZE_256 = 1,
  MA_PAINT_PREVIEW_SIZE_512 = 2,
  MA_PAINT_PREVIEW_SIZE_1024 = 3,
  MA_PAINT_PREVIEW_SIZE_4096 = 4,
  MA_PAINT_PREVIEW_SIZE_AUTO = 5,
};

/** #Material::paint_layers_preview_delay */
enum eMaterialPaintPreviewDelay : char {
  MA_PAINT_PREVIEW_DELAY_5S = 0,
  MA_PAINT_PREVIEW_DELAY_10S = 1,
  MA_PAINT_PREVIEW_DELAY_15S = 2,
  MA_PAINT_PREVIEW_DELAY_25S = 3,
};

/** #Material::paint_layers_composite_device: what shades the Image Editor's 2D preview. */
enum eMaterialPaintCompositeDevice : char {
  MA_PAINT_COMPOSITE_GPU = 0,
  MA_PAINT_COMPOSITE_CPU = 1,
};

/* ramps */
enum eMaterial_RampBlend : int {
  MA_RAMP_BLEND = 0,
  MA_RAMP_ADD = 1,
  MA_RAMP_MULT = 2,
  MA_RAMP_SUB = 3,
  MA_RAMP_SCREEN = 4,
  MA_RAMP_DIV = 5,
  MA_RAMP_DIFF = 6,
  MA_RAMP_DARK = 7,
  MA_RAMP_LIGHT = 8,
  MA_RAMP_OVERLAY = 9,
  MA_RAMP_DODGE = 10,
  MA_RAMP_BURN = 11,
  MA_RAMP_HUE = 12,
  MA_RAMP_SAT = 13,
  MA_RAMP_VAL = 14,
  MA_RAMP_COLOR = 15,
  MA_RAMP_SOFT = 16,
  MA_RAMP_LINEAR = 17,
  MA_RAMP_EXCLUSION = 18,
};

/** #MTex::texco */
enum eMTex_TexCo : int {
  TEXCO_ORCO = 1 << 0,
  // TEXCO_REFL = 1 << 1, /* Deprecated. */
  // TEXCO_NORM = 1 << 2, /* Deprecated. */
  TEXCO_GLOB = 1 << 3,
  TEXCO_UV = 1 << 4,
  TEXCO_OBJECT = 1 << 5,
  // TEXCO_LAVECTOR = 1 << 6, /* Deprecated. */
  // TEXCO_VIEW = 1 << 7,     /* Deprecated. */
  // TEXCO_STICKY = 1 << 8,   /* Deprecated. */
  // TEXCO_OSA = 1 << 9,      /* Deprecated. */
  TEXCO_WINDOW = 1 << 10,
  // NEED_UV = 1 << 11,       /* Deprecated. */
  // TEXCO_TANGENT = 1 << 12, /* Deprecated. */
  /** still stored in `vertex->accum`, 1 D. */
  TEXCO_STRAND = 1 << 13,
  /** strand is used for normal materials, particle for halo materials */
  TEXCO_PARTICLE = 1 << 13,
  // TEXCO_STRESS = 1 << 14, /* Deprecated. */
  // TEXCO_SPEED = 1 << 15,  /* Deprecated. */
};
ENUM_OPERATORS(eMTex_TexCo)

/** #MTex::mapto */
enum eMTex_MapTo : int {
  MAP_COL = 1 << 0,
  MAP_ALPHA = 1 << 7,
};
ENUM_OPERATORS(eMTex_MapTo)

/** #Material::pr_type */
enum ePreviewType : char {
  MA_FLAT = 0,
  MA_SPHERE = 1,
  MA_CUBE = 2,
  MA_SHADERBALL = 3,
  MA_SPHERE_A = 4, /* Used for icon renders only. */
  MA_TEXTURE = 5,
  MA_LAMP = 6,
  MA_SKY = 7,
  MA_HAIR = 10,
  MA_ATMOS = 11,
  MA_CLOTH = 12,
  MA_FLUID = 13,
};

/** #Material::pr_flag */
enum eMaterial_PreviewFlag : short {
  MA_PREVIEW_WORLD = 1 << 0,
};
ENUM_OPERATORS(eMaterial_PreviewFlag)

/** #Material::surface_render_method */
enum eMaterial_SurfaceRenderMethod : char {
  MA_SURFACE_METHOD_DEFERRED = 0,
  MA_SURFACE_METHOD_FORWARD = 1,
};

/** #Material::volume_intersection_method */
enum eMaterial_VolumeIntersectionMethod : char {
  MA_VOLUME_ISECT_FAST = 0,
  MA_VOLUME_ISECT_ACCURATE = 1,
};

/** #Material::blend_method */
enum eMaterial_BlendMethod : char {
  MA_BM_SOLID = 0,
  // MA_BM_ADD = 1, /* deprecated */
  // MA_BM_MULTIPLY = 2,  /* deprecated */
  MA_BM_CLIP = 3,
  MA_BM_HASHED = 4,
  MA_BM_BLEND = 5,
};

/** #Material::blend_flag */
enum eMaterial_BlendFlag : char {
  MA_BL_HIDE_BACKFACE = (1 << 0),
  MA_BL_SS_REFRACTION = (1 << 1),
  MA_BL_CULL_BACKFACE = (1 << 2),
  MA_BL_TRANSLUCENCY = (1 << 3),
  MA_BL_LIGHTPROBE_VOLUME_DOUBLE_SIDED = (1 << 4),
  MA_BL_CULL_BACKFACE_SHADOW = (1 << 5),
  MA_BL_TRANSPARENT_SHADOW = (1 << 6),
  MA_BL_THICKNESS_FROM_SHADOW = static_cast<char>(1 << 7),
};
ENUM_OPERATORS(eMaterial_BlendFlag)

/** #Material::blend_shadow */
enum eMaterial_BlendShadow : char {
  MA_BS_NONE = 0,
  MA_BS_SOLID = 1,
  MA_BS_CLIP = 2,
  MA_BS_HASHED = 3,
};

/** #Material::displacement_method */
enum eMaterial_DisplacementMethod : char {
  MA_DISPLACEMENT_BUMP = 0,
  MA_DISPLACEMENT_DISPLACE = 1,
  MA_DISPLACEMENT_BOTH = 2,
};

/** #Material::thickness_mode */
enum eMaterial_ThicknessMode : char {
  MA_THICKNESS_SPHERE = 0,
  MA_THICKNESS_SLAB = 1,
};

/* Grease Pencil Stroke styles */
enum eMaterialGPencilStyle_StrokeStyle : short {
  GP_MATERIAL_STROKE_STYLE_SOLID = 0,
  GP_MATERIAL_STROKE_STYLE_TEXTURE = 1,
};

/* Grease Pencil Fill styles */
enum eMaterialGPencilStyle_FillStyle : short {
  GP_MATERIAL_FILL_STYLE_SOLID = 0,
  GP_MATERIAL_FILL_STYLE_GRADIENT = 1,
  GP_MATERIAL_FILL_STYLE_CHECKER = 2, /* DEPRECATED (only for convert old files) */
  GP_MATERIAL_FILL_STYLE_TEXTURE = 3,
};

/* Grease Pencil Gradient Types */
enum eMaterialGPencilStyle_GradientType : int {
  GP_MATERIAL_GRADIENT_LINEAR = 0,
  GP_MATERIAL_GRADIENT_RADIAL = 1,
};

/* Grease Pencil Follow Drawing Modes */
enum eMaterialGPencilStyle_FollowMode : int {
  GP_MATERIAL_FOLLOW_PATH = 0,
  GP_MATERIAL_FOLLOW_OBJ = 1,
  GP_MATERIAL_FOLLOW_FIXED = 2,
};

/* Grease Pencil Placement Drawing Modes */
enum eMaterialGPencilPlacementMode : int {
  GP_MATERIAL_PLACEMENT_COUNT = 0,
  GP_MATERIAL_PLACEMENT_RADIUS = 1,
  GP_MATERIAL_PLACEMENT_DENSITY = 2,
};

struct TexPaintSlot {
  DNA_DEFINE_CXX_METHODS(TexPaintSlot)

  /** Image to be painted on. Mutual exclusive with attribute_name. */
  struct Image *ima = nullptr;
  struct ImageUser *image_user = nullptr;

  /**
   * Custom-data index for uv layer, #MAX_CUSTOMDATA_LAYER_NAME_NO_PREFIX.
   * May reference #NodeShaderUVMap::uv_name.
   */
  char *uvname = nullptr;
  /**
   * Color attribute name when painting using color attributes. Mutual exclusive with ima.
   * Points to the name of a CustomDataLayer.
   */
  char *attribute_name = nullptr;
  /** Do we have a valid image and UV map or attribute. */
  int valid = 0;
  /** Copy of node interpolation setting. */
  int interp = 0;
  /** Cached copy of #NodeTexImage.paint_slot_type. */
  char slot_type = 0;
  char _pad[7] = {};
};

/** Runtime-only resolution cache for #BKE_paint_principled_channel_image_get. Not saved to disk. */
struct MaterialPaintChannelCache {
  DNA_DEFINE_CXX_METHODS(MaterialPaintChannelCache)

  struct Image *image = nullptr;
  struct ImageUser *iuser = nullptr;
  char valid = 0;
  char resolved = 0;
  /** Pad to 8-byte alignment (pointers force align 8; size must be multiple of align). */
  char _pad[6] = {};
};

struct MaterialGPencilStyle {
  DNA_DEFINE_CXX_METHODS(MaterialGPencilStyle)

  /** Texture image for strokes. */
  struct Image *sima = nullptr;
  /** Texture image for filling. */
  struct Image *ima = nullptr;
  /** Color for paint and strokes (alpha included). */
  float stroke_rgba[4] = {};
  /** Color that should be used for drawing "fills" for strokes (alpha included). */
  float fill_rgba[4] = {};
  /** Secondary color used for gradients and other stuff. */
  float mix_rgba[4] = {};
  /** Settings. */
  eMaterialGPencilStyle_Flag flag = {};
  /** Custom index for passes. */
  short index = 0;
  /** Style for drawing strokes (used to select shader type). */
  eMaterialGPencilStyle_StrokeStyle stroke_style = GP_MATERIAL_STROKE_STYLE_SOLID;
  /** Style for filling areas (used to select shader type). */
  eMaterialGPencilStyle_FillStyle fill_style = GP_MATERIAL_FILL_STYLE_SOLID;
  /** Factor used to define shader behavior (several uses). */
  float mix_factor = 0;
  /** Angle used for gradients orientation. */
  DNA_DEPRECATED float gradient_angle = 0;
  /** Radius for radial gradients. */
  DNA_DEPRECATED float gradient_radius = 0;
  char _pad2[4] = {};
  /** UV coordinates scale. */
  DNA_DEPRECATED float gradient_scale[2] = {};
  /** Factor to shift filling in 2d space. */
  DNA_DEPRECATED float gradient_shift[2] = {};
  /** Angle used for texture orientation. */
  float texture_angle = 0;
  /** Texture scale (separated of uv scale). */
  float texture_scale[2] = {};
  /** Factor to shift texture in 2d space. */
  float texture_offset[2] = {};
  /** Texture opacity. */
  DNA_DEPRECATED float texture_opacity = 0;
  /** Pixel size for uv along the stroke. */
  float texture_pixsize = 0;
  /** Drawing mode (line or dots). */
  eMaterialGPencilStyle_Mode mode = GP_MATERIAL_MODE_LINE;

  /** Type of gradient. */
  eMaterialGPencilStyle_GradientType gradient_type = GP_MATERIAL_GRADIENT_LINEAR;

  /** Factor used to mix texture and stroke color. */
  float mix_stroke_factor = 0;
  /** Mode used to align Dots and Boxes with stroke drawing path and object rotation */
  eMaterialGPencilStyle_FollowMode alignment_mode = GP_MATERIAL_FOLLOW_PATH;
  /** Rotation for texture for Dots and Squares. */
  float alignment_rotation = 0;
  /** Placement mode for Dots and Squares. */
  eMaterialGPencilPlacementMode placement_mode = GP_MATERIAL_PLACEMENT_COUNT;
  /* Number of points per segment when placement mode is `GP_MATERIAL_PLACEMENT_COUNT` */
  int placement_count = 0;
  /* Radius factor for points when placement mode is `GP_MATERIAL_PLACEMENT_RADIUS` */
  float placement_radius_spacing = 0;
  /* Point density per unit when placement mode is `GP_MATERIAL_PLACEMENT_DENSITY` */
  float placement_density = 0;

  float random_size_factor = 0;
  float random_strength_factor = 0;
  float random_rotation_factor = 0;

  float random_hue_factor = 0;
  float random_saturation_factor = 0;
  float random_value_factor = 0;

  float random_noise_scale = 0;
  char _pad3[4] = {};
};

struct MaterialLineArt {
  eMaterialLineArtFlags flags = {};

  /* Used to filter line art occlusion edges */
  unsigned char material_mask_bits = 0;

  /** Maximum 255 levels of equivalent occlusion. */
  unsigned char mat_occlusion = 1;

  unsigned char intersection_priority = 0;

  char _pad = {};
};

/** #MaterialPaintLayer::flag */
enum eMaterialPaintLayerFlag : int16_t {
  /** The layer takes part in the stack. */
  MA_PAINT_LAYER_ENABLED = 1 << 0,
  /**
   * The mask item that is the layer's base mask. Always the first item of the owner's
   * #MaterialPaintLayer::mask_stack, and removed only together with the whole mask.
   */
  MA_PAINT_LAYER_MASK_BASE = 1 << 1,
  /**
   * A map-coverage mask item outputs its match scaled by its Base Color constant (the standard
   * gradient value) instead of the plain White/Black result. Its output is `match * value`, so a
   * value of one reads as White. Only meaningful on a map-coverage item.
   */
  MA_PAINT_LAYER_MASK_ITEM_GRADIENT = 1 << 2,
  /**
   * A Color ID mask item compares each colour with its own tolerance, and the shared tolerance and
   * softness are unused. Advanced mode; changes the generated graph's shape.
   */
  MA_PAINT_LAYER_MASK_ITEM_COLOR_ID_ADVANCED = 1 << 3,
  /**
   * The output map carries the coverage in the alpha channel, with a transparent background, rather
   * than as a filled grey. Stored for consumers that export or reuse the mask as an image.
   */
  MA_PAINT_LAYER_MASK_ITEM_ALPHA_BACKGROUND = 1 << 4,
};
ENUM_OPERATORS(eMaterialPaintLayerFlag)

/** #MaterialPaintLayer::object_mask_type */
enum eMaterialPaintLayerObjectMaskType : int8_t {
  MA_PAINT_LAYER_OBJECT_MASK_MESH = 0,
  MA_PAINT_LAYER_OBJECT_MASK_UV_TILES = 1,
};

/** #MaterialPaintLayer::object_mask_flag */
enum eMaterialPaintLayerObjectMaskFlag : int8_t {
  /**
   * The row has an object mask: set by the first hidden object or tile, and what makes the
   * generator build the Attribute nodes.
   */
  MA_PAINT_LAYER_OBJECT_MASK_ACTIVE = 1 << 0,
};

/**
 * #MaterialPaintLayer::color_tag: the display tag of a layer group (a folder).
 *
 * Zero means "no tag" rather than Grease Pencil's -1, because the field is carved out of
 * #MaterialPaintLayer's existing padding and therefore reads back as zero from every file written
 * before it existed. #eSculptLayerColorTag makes the same choice for the same reason. The eight
 * colors are drawn with the collection palette, so the tag maps to `btheme->collection_color` by
 * subtracting #MA_PAINT_LAYER_COLOR_01.
 */
enum eMaterialPaintLayerColorTag : int8_t {
  MA_PAINT_LAYER_COLOR_NONE = 0,
  MA_PAINT_LAYER_COLOR_01 = 1,
  MA_PAINT_LAYER_COLOR_02 = 2,
  MA_PAINT_LAYER_COLOR_03 = 3,
  MA_PAINT_LAYER_COLOR_04 = 4,
  MA_PAINT_LAYER_COLOR_05 = 5,
  MA_PAINT_LAYER_COLOR_06 = 6,
  MA_PAINT_LAYER_COLOR_07 = 7,
  MA_PAINT_LAYER_COLOR_08 = 8,

  /** Not a tag: the number of values the field accepts, "none" included. */
  MA_PAINT_LAYER_COLOR_TOT = 9,
};

/**
 * #MaterialPaintLayer::role. Numerically matches the reference's role table.
 */
enum eMaterialPaintLayerRole : int8_t {
  /** A row of the stack. */
  MA_PAINT_LAYER_ROLE_LAYER = 0,
  /**
   * A content correction of an owner row's #MaterialPaintLayer::effects: a map laid over the owner's
   * colour in each channel it carries, below the owner's own blend.
   */
  MA_PAINT_LAYER_ROLE_EFFECT = 1,
  /** An item of an owner row's #MaterialPaintLayer::mask_stack: coverage laid over its factor. */
  MA_PAINT_LAYER_ROLE_MASK_ITEM = 2,
};

/** #MaterialPaintLayer::blend -- how the row combines with what is below it. */
enum eMaterialPaintLayerBlend : int8_t {
  MA_PAINT_LAYER_BLEND_MIX = 0,
  MA_PAINT_LAYER_BLEND_MULTIPLY = 1,
  MA_PAINT_LAYER_BLEND_OVERLAY = 2,
  MA_PAINT_LAYER_BLEND_ADD = 3,
  /** Internal: the Normal channel forces it. Never offered as a user choice. */
  MA_PAINT_LAYER_BLEND_NORMAL_COMBINE = 4,
  MA_PAINT_LAYER_BLEND_DARKEN = 5,
  MA_PAINT_LAYER_BLEND_BURN = 6,
  MA_PAINT_LAYER_BLEND_LIGHTEN = 7,
  MA_PAINT_LAYER_BLEND_SCREEN = 8,
  MA_PAINT_LAYER_BLEND_DODGE = 9,
  MA_PAINT_LAYER_BLEND_SUBTRACT = 10,
  MA_PAINT_LAYER_BLEND_DIVIDE = 11,
  MA_PAINT_LAYER_BLEND_DIFFERENCE = 12,
  MA_PAINT_LAYER_BLEND_EXCLUSION = 13,
  MA_PAINT_LAYER_BLEND_SOFT_LIGHT = 14,
  MA_PAINT_LAYER_BLEND_LINEAR_LIGHT = 15,
  MA_PAINT_LAYER_BLEND_HUE = 16,
  MA_PAINT_LAYER_BLEND_SATURATION = 17,
  MA_PAINT_LAYER_BLEND_COLOR = 18,
  MA_PAINT_LAYER_BLEND_VALUE = 19,
  /**
   * The Normal channel only, as a per-channel override: the upper normal replaces the lower one
   * instead of being combined with it (#MA_PAINT_LAYER_BLEND_NORMAL_COMBINE). Never offered as a
   * row-level choice, and never on another channel.
   */
  MA_PAINT_LAYER_BLEND_NORMAL_REPLACE = 20,
};

/** #MaterialPaintLayer::source -- what the row paints with. */
enum eMaterialPaintLayerSource : int8_t {
  /** A painted map per channel. */
  MA_PAINT_LAYER_SOURCE_IMAGE = 0,
  /** A flat colour the row lays whatever its channels' alpha; a Fill. */
  MA_PAINT_LAYER_SOURCE_CONSTANT = 1,
  /**
   * Another material, read through #MaterialPaintLayer::material: the row lays what that material
   * shows in the paint channels. It owns no channel records and no maps of its own.
   */
  MA_PAINT_LAYER_SOURCE_MATERIAL = 2,
  /**
   * A folder: the row holds its stack in #MaterialPaintLayer::children and lays their isolated
   * result over what is below with its own blend, opacity and mask. It owns no channel records and
   * no maps. Numerically matches the reference's source table: 3 is reserved for the Node Group
   * source.
   */
  MA_PAINT_LAYER_SOURCE_STACK = 4,
  /**
   * A filter: no colour of its own, it changes the colour it reads. The member is
   * #MaterialPaintLayer::kind_subtype (#eMaterialPaintLayerFilter). Only as a correction for now.
   * 5 is reserved for the Mesh Map source, 7 for the generators.
   */
  MA_PAINT_LAYER_SOURCE_FILTER = 6,
};

/** #MaterialPaintLayer::kind_subtype of a #MA_PAINT_LAYER_SOURCE_FILTER row. */
enum eMaterialPaintLayerFilter : int16_t {
  MA_PAINT_LAYER_FILTER_INVERT = 1,
  MA_PAINT_LAYER_FILTER_LEVELS = 2,
  MA_PAINT_LAYER_FILTER_HSL = 3,
  /** A map-driven mask coverage operator: matches colours of the element's Base Color map. */
  MA_PAINT_LAYER_FILTER_COLOR_ID = 4,
  /** A map-driven mask coverage operator: matches the island mask of the element's Base Color map. */
  MA_PAINT_LAYER_FILTER_UV_ISLAND = 5,
};

/** #MaterialPaintLayerIdColor::flag */
enum eMaterialPaintLayerIdColorFlag : int8_t {
  /** The colour takes part in the Color ID match; a disabled one is skipped. */
  MA_PAINT_LAYER_ID_COLOR_ENABLED = 1 << 0,
  /** The per-colour controls are unfolded in the UI. */
  MA_PAINT_LAYER_ID_COLOR_EXPANDED = 1 << 1,
};
ENUM_OPERATORS(eMaterialPaintLayerIdColorFlag)

/**
 * One colour a Color ID mask item matches, kept in #MaterialPaintLayer::id_colors.
 *
 * The colour is stored in the ID Map's raw data space, the same values the picker reads, so the
 * CPU composite, the generated graph and the picker compare the map's texels without a conversion
 * that could drift between them.
 */
struct MaterialPaintLayerIdColor {
  DNA_DEFINE_CXX_METHODS(MaterialPaintLayerIdColor)

  struct MaterialPaintLayerIdColor *next = nullptr, *prev = nullptr;
  /** The matched colour, in the map's raw data space. */
  float color[3] = {};
  /** The colour's own tolerance in Advanced mode; the shared kind parameter in Simple mode. */
  float tolerance = 0.5f;
  /** #eMaterialPaintLayerIdColorFlag. */
  int8_t flag = MA_PAINT_LAYER_ID_COLOR_ENABLED;
  /* Pads the struct to a multiple of the pointer size, as every DNA struct must. */
  char _pad[7] = {};
};

/** #MaterialPaintLayerChannel::state */
enum eMaterialPaintLayerChannelState : int8_t {
  /** No map: the channel keeps its record but carries no coverage. */
  MA_PAINT_LAYER_CHANNEL_ABSENT = 0,
  /** A map feeds the channel and its coverage is live. */
  MA_PAINT_LAYER_CHANNEL_ENABLED = 1,
  /** The map is kept but switched off. */
  MA_PAINT_LAYER_CHANNEL_DISABLED = 2,
};

/** #MaterialPaintLayerChannel::flag */
enum eMaterialPaintLayerChannelFlag : int8_t {
  /**
   * The record's coverage is read as `1 - C`. Only a mask correction's Base Color record honours
   * it: there the map (or the flat grey) is a coverage, and a black-and-white map is as often
   * painted the wrong way round as not. Taken from the padding, so older files read as off.
   */
  MA_PAINT_LAYER_CHANNEL_INVERT = 1 << 0,
  /**
   * A Normal record's map is a DirectX tangent-space normal map (green points down). Off reads the
   * map as OpenGL, the convention of the shader's Normal Map node. Part of the graph's structure:
   * the generator adds a node flipping the green channel. Taken from the padding, so older files
   * read as OpenGL.
   */
  MA_PAINT_LAYER_CHANNEL_NORMAL_DIRECTX = 1 << 1,
};

/**
 * One channel of a #MaterialPaintLayer. A layer owns a `channels` array with one entry per channel
 * it takes part in; #channel names which #eMaterialPaintChannel the record stands for.
 */
struct MaterialPaintLayerChannel {
  DNA_DEFINE_CXX_METHODS(MaterialPaintLayerChannel)

  /** #eMaterialPaintChannel. */
  int8_t channel = 0;
  /** #eMaterialPaintLayerChannelState. */
  int8_t state = MA_PAINT_LAYER_CHANNEL_ABSENT;
  /** #eMaterialPaintLayerChannelFlag. */
  int8_t flag = 0;
  char _pad[5] = {};
  /** The channel's map, or null when it has none. */
  struct Image *image = nullptr;
  /** Constant value used while the channel carries no map. */
  float value[4] = {};
};

/**
 * The overrides of one (row, channel) pair, kept in #MaterialPaintLayer::channel_settings.
 *
 * A fixed array on the row rather than a field of the sparse #MaterialPaintLayerChannel record: the
 * value exists for every pair, record or not, which gives it one stable RNA path that survives a
 * record being created, and works for a folder that holds no records.
 */
struct MaterialPaintLayerChannelSettings {
  DNA_DEFINE_CXX_METHODS(MaterialPaintLayerChannelSettings)

  /**
   * A per-channel #eMaterialPaintLayerBlend override, -1 to inherit the row's own blend. The Normal
   * channel reads it as the combine (#MA_PAINT_LAYER_BLEND_NORMAL_COMBINE) when it inherits and
   * accepts #MA_PAINT_LAYER_BLEND_NORMAL_REPLACE alone.
   */
  int8_t blend = -1;
  char _pad[3] = {};
  /** Multiplied by the row's opacity; 1.0 when the channel has no override. */
  float opacity = 1.0f;
};

/** #MaterialPaintLayerMapping::space */
enum eMaterialPaintLayerMappingSpace : int16_t {
  MA_PAINT_LAYER_MAPPING_SPACE_UV = 0,
};

/** #MaterialPaintLayerMapping::flag */
enum eMaterialPaintLayerMappingFlag : int16_t {
  MA_PAINT_LAYER_MAPPING_ENABLED = 1 << 0,
  /**
   * The scale axes move independently. Stored inverted so that zeroed (new) rows start with the
   * axes locked together: editing one sets the other to the same value.
   */
  MA_PAINT_LAYER_MAPPING_SCALE_UNLOCKED = 1 << 1,
};
ENUM_OPERATORS(eMaterialPaintLayerMappingFlag)

/**
 * #MaterialPaintLayerMapping::wrap: what the row's maps do outside the unit UV square after the
 * mapping. Zero is Repeat, so files written before the option read back as before.
 */
enum eMaterialPaintLayerMappingWrap : int16_t {
  /** Repeat on both axes. */
  MA_PAINT_LAYER_MAPPING_WRAP_REPEAT = 0,
  /** Repeat along U; transparent outside [0, 1] along V. */
  MA_PAINT_LAYER_MAPPING_WRAP_REPEAT_X = 1,
  /** Repeat along V; transparent outside [0, 1] along U. */
  MA_PAINT_LAYER_MAPPING_WRAP_REPEAT_Y = 2,
  /** One copy; transparent outside [0, 1] on both axes. */
  MA_PAINT_LAYER_MAPPING_WRAP_CLIP = 3,
};

/**
 * UV mapping of one paint layer row, stored by value.
 *
 * Zeroes mean "disabled": old files read back as disabled, and a zero scale axis reads back as one
 * (see `BKE_paint_layers_mapping_scale_normalize`).
 */
struct MaterialPaintLayerMapping {
  DNA_DEFINE_CXX_METHODS(MaterialPaintLayerMapping)

  /** Translation in UV space. */
  float offset[2] = {};
  /** Scale in UV space; 0 reads back as 1. */
  float scale[2] = {1.0f, 1.0f};
  /** Rotation around Z, in radians. */
  float rotation = 0.0f;
  /** #eMaterialPaintLayerMappingSpace. */
  int16_t space = MA_PAINT_LAYER_MAPPING_SPACE_UV;
  /** #eMaterialPaintLayerMappingFlag. */
  int16_t flag = 0;
  /** #eMaterialPaintLayerMappingWrap. */
  int16_t wrap = MA_PAINT_LAYER_MAPPING_WRAP_REPEAT;
  /* Pads the struct to 32 bytes: it is embedded in #MaterialPaintLayer between pointer members, so
   * its size must stay a multiple of 8. The room also keeps the next mapping option from changing
   * the size. */
  char _pad[6] = {};
};

/** #MaterialPaintLayerBake::mode */
enum eMaterialPaintLayerBakeMode : int8_t {
  /** Bake only what is heavy and not being edited; everything else is evaluated live. */
  MA_PAINT_LAYER_BAKE_AUTO = 0,
  /**
   * Always bake, and show the row as stale until the bake is current. A Material row shows the
   * render of its source, never a copy of the source's graph.
   */
  MA_PAINT_LAYER_BAKE_ALWAYS = 1,
  /**
   * Never bake; the row is evaluated live. A Material row shows its source's graph through a copy
   * of it, and only what that cannot show (a graph reached through a Group node) is empty here.
   */
  MA_PAINT_LAYER_BAKE_NEVER = 2,
};

/**
 * The baked cache of one description row, or null while nothing is baked.
 *
 * Cache, not truth: the description plus its live maps are what the row *is*, and these maps are a
 * snapshot the generator and the CPU compositor may read instead of evaluating the row's subtree.
 * The #hash is the whole validity test -- there is no separate "valid" flag to save -- and the maps
 * are owned here as ID references (see the layer's `foreach_id`).
 */
struct MaterialPaintLayerBake {
  DNA_DEFINE_CXX_METHODS(MaterialPaintLayerBake)

  /**
   * One map per #eMaterialPaintChannel index, or null where the channel is not baked. The literal
   * length is #PAINT_MATERIAL_CHANNEL_NUM, which `paint_layers.cc` static_asserts.
   */
  struct Image *images[10] = {};
  /** The baked coverage map, or null. */
  struct Image *coverage = nullptr;
  /** #eMaterialPaintLayerBakeMode. */
  int8_t mode = MA_PAINT_LAYER_BAKE_AUTO;
  char _pad_a[1] = {};
  /**
   * Of a Material row: the side divided by 16 of the preview its maps were last rendered as while
   * the row was edited (scaled into the row's maps), or zero when they were rendered at the row's
   * own size. A row left behind with a preview is due for the render at its own size, and one still
   * edited is due for another when the material's preview size no longer matches.
   */
  int16_t preview_side16 = 0;
  /** Square side the maps were baked at. */
  int size = 0;
  /** The description hash at the last successful bake, low word first. */
  uint32_t hash[2] = {};
  /**
   * Of a Material row: the content hash of the source material the maps were rendered from, low
   * word first. The row's #hash is structural and cheap to test anywhere; the source is compared
   * against this on the main thread, where the source's node trees may be read.
   */
  uint32_t source_hash[2] = {};
};

/**
 * One row of a layered material's stack, the DNA description of what the material is painted with.
 *
 * The list is stored bottom-to-top. Folders nest through #children, which holds the same type.
 */
struct MaterialPaintLayer {
  DNA_DEFINE_CXX_METHODS(MaterialPaintLayer)

  struct MaterialPaintLayer *next = nullptr, *prev = nullptr;
  /** Nested layers of a folder (#MA_PAINT_LAYER_SOURCE_STACK); the same type as the parent list. */
  ListBaseT<MaterialPaintLayer> children = {nullptr, nullptr};
  char name[/*MAX_NAME*/ 64] = "";
  /** Stable identity of the row; it survives reordering and copying. */
  bUUID marker = {};
  /** #eMaterialPaintLayerBlend. */
  int8_t blend = MA_PAINT_LAYER_BLEND_MIX;
  /** #eMaterialPaintLayerSource: what the row paints with. */
  int8_t source = MA_PAINT_LAYER_SOURCE_IMAGE;
  /** #eMaterialPaintLayerFlag. */
  int16_t flag = MA_PAINT_LAYER_ENABLED;
  float opacity = 1.0f;
  struct MaterialPaintLayerChannel *channels = nullptr;
  int channels_num = 0;
  /** #eMaterialPaintLayerRole. */
  int8_t role = MA_PAINT_LAYER_ROLE_LAYER;
  char _pad1[1] = {};
  /**
   * The member of the family #source names (a filter's kind, a generator's kind); 0 for the
   * families that have no members. Together with #source it is the key of the row's kind.
   */
  int16_t kind_subtype = 0;
  /**
   * The coverage items of this row, bottom to top: they lay a factor over the row's own, so the row
   * is partly or wholly hidden. Owned by the row (freed, copied and written with it); every item
   * is a #MA_PAINT_LAYER_ROLE_MASK_ITEM row with no children of its own. The base mask, when there
   * is one, is the first.
   */
  ListBaseT<MaterialPaintLayer> mask_stack = {nullptr, nullptr};
  /**
   * The content corrections of this row, bottom to top: each lays its own maps over the row's
   * colour before the row's blend. Owned by the row like #mask_stack; every item is a
   * #MA_PAINT_LAYER_ROLE_EFFECT row with no children, mask or corrections of its own.
   */
  ListBaseT<MaterialPaintLayer> effects = {nullptr, nullptr};
  /**
   * The colours a Color ID map-coverage operator matches (#MA_PAINT_LAYER_FILTER_COLOR_ID), newest
   * first. Owned by the row (freed, copied and written with it); empty for every other kind. The
   * ordinal is not stored: the list order is the display order.
   */
  ListBaseT<MaterialPaintLayerIdColor> id_colors = {nullptr, nullptr};
  /**
   * UV mapping of this row's own maps. Stored by value, so a flat copy carries it. Zeroes read back
   * as disabled; a zero scale axis reads back as one. General to the row, not a parameter of its
   * kind: only a kind that lays a map of its own (a Fill) can use it.
   */
  MaterialPaintLayerMapping mapping = {};
  /**
   * The parameters of the row's kind, defined by the kind's schema rather than by this struct, so a
   * new kind adds no field here. Owned by the row; null while the kind has set none.
   */
  struct IDProperty *system_properties = nullptr;
  /**
   * #eMaterialPaintLayerObjectMaskType: whether the row is limited per object (Mesh) or per UDIM
   * tile of each object (UV Tiles). The per-object choices live on the objects
   * (#Object::paint_layer_masks), so the material never references the objects using it. General to
   * the row: a Layer or a folder can carry it, whatever the kind.
   */
  int8_t object_mask_type = MA_PAINT_LAYER_OBJECT_MASK_MESH;
  /** #eMaterialPaintLayerObjectMaskFlag. */
  int8_t object_mask_flag = 0;
  /** #eMaterialPaintLayerColorTag: the row's display tag, read by the UI only. */
  int8_t color_tag = MA_PAINT_LAYER_COLOR_NONE;
  char _pad_object_mask[5] = {};
  /**
   * Per (row, channel) overrides, indexed by #eMaterialPaintChannel. A fixed array so every pair has
   * a value and an RNA path whether or not the row has a #channels record for it; a folder, which
   * holds no records, uses it too. The literal length is #PAINT_MATERIAL_CHANNEL_NUM, which
   * `paint_layers.cc` static_asserts.
   */
  MaterialPaintLayerChannelSettings channel_settings[10] = {};
  /**
   * The row's baked cache, or null while nothing is baked. Owned by the row (freed, copied and
   * written with it).
   */
  struct MaterialPaintLayerBake *bake = nullptr;
  /**
   * The material a #MA_PAINT_LAYER_SOURCE_MATERIAL row reads, or null for every other source. A
   * user of the material, reported to the ID walk through the row's dependencies; it never names
   * the owner or a material that reads the owner.
   */
  Material *material = nullptr;
  /**
   * The marker of the row whose result this row's maps were last made from ("Use Row Result"), or
   * nil. Only a reference for the update button: a marker that no longer names a row means the
   * source is gone, and nothing else reads it.
   */
  bUUID row_result_source = {};
};

struct Material {
#ifdef __cplusplus
  DNA_DEFINE_CXX_METHODS(Material)
  /** See #ID_Type comment for why this is here. */
  static constexpr ID_Type id_type = ID_MA;
#endif

  ID id;
  /** Animation data (must be immediately after id for utilities to use it). */
  struct AnimData *adt = nullptr;

  eMaterial_Flag flag = {};
  /** Rendering modes for EEVEE. */
  eMaterial_SurfaceRenderMethod surface_render_method = MA_SURFACE_METHOD_DEFERRED;
  char _pad1[1] = {};

  /* Colors from Blender Internal that we are still using. */
  float r = 0.8, g = 0.8, b = 0.8, a = 1.0f;
  float specr = 1.0, specg = 1.0, specb = 1.0;
  DNA_DEPRECATED float alpha = 0;
  DNA_DEPRECATED float ray_mirror = 0;
  float spec = 0.5;
  /** Renamed and inversed to roughness. */
  DNA_DEPRECATED float gloss_mir = 0;
  float roughness = 0.4f;
  float metallic = 0;

  /** Nodes */
  DNA_DEPRECATED char use_nodes = 0;

  /** Preview render. */
  ePreviewType pr_type = MA_SPHERE;
  short pr_texture = 0;
  eMaterial_PreviewFlag pr_flag = {};

  /** Index for render passes. */
  short index = 0;

  /* #Material::use_nodes is deprecated so it's not possible to create an embedded node tree from
   * the UI or Python API by setting `use_nodes = True`. Therefore, #nodetree is required to never
   * be nullptr. */
  struct bNodeTree *nodetree = nullptr;
  struct PreviewImage *preview = nullptr;

  /* Freestyle line settings. */
  float line_col[4] = {};
  short line_priority = 0;
  short vcol_alpha = 0;

  /* Texture painting slots. */
  short paint_active_slot = 0;
  short paint_clone_slot = 0;
  short tot_slots = 0;

  /* Displacement. */
  eMaterial_DisplacementMethod displacement_method = MA_DISPLACEMENT_BUMP;

  /* Thickness. */
  eMaterial_ThicknessMode thickness_mode = MA_THICKNESS_SPHERE;

  /* Transparency. */
  float alpha_threshold = 0.5f;
  float refract_depth = 0;
  eMaterial_BlendMethod blend_method =
      MA_BM_SOLID; /* TODO(fclem): Deprecate once we remove legacy EEVEE. */
  eMaterial_BlendShadow blend_shadow =
      MA_BS_SOLID; /* TODO(fclem): Deprecate once we remove legacy EEVEE. */
  eMaterial_BlendFlag blend_flag = MA_BL_TRANSPARENT_SHADOW;

  /* Volume. */
  eMaterial_VolumeIntersectionMethod volume_intersection_method = MA_VOLUME_ISECT_FAST;

  /* Displacement. */
  float inflate_bounds = 0;

  /**
   * The active paint slot a layered material had before Mask mode moved it to Base Color, plus one;
   * zero when nothing is held. The mask lives in Base Color's slot alone, so Mask mode overrides the
   * user's choice of channel and leaving the mode has to give it back (zero in older files).
   */
  short paint_content_slot = 0;
  char _pad3[2] = {};

  /**
   * Cached slots for texture painting, must be refreshed via
   * BKE_texpaint_slot_refresh_cache before using.
   */
  struct TexPaintSlot *texpaintslot = nullptr;

  /**
   * Cached Principled-socket image resolution for material paint channels.
   * Must be refreshed via #BKE_paint_material_channel_cache_invalidate before using after edits.
   */
  MaterialPaintChannelCache paint_channel_cache[/*PAINT_MATERIAL_CHANNEL_NUM*/ 10] = {};

  /** Runtime cache for GLSL materials. */
  ListBaseT<LinkData> gpumaterial = {nullptr, nullptr};

  /** Grease pencil color. */
  struct MaterialGPencilStyle *gp_style = nullptr;
  struct MaterialLineArt lineart;

  /**
   * The DNA description of the paint layer stack, bottom-to-top. When #MA_PAINT_LAYERED is set,
   * this is the source of truth of what the material is painted with.
   */
  ListBaseT<MaterialPaintLayer> paint_layers = {nullptr, nullptr};
  /** Marker of the active layer; an index is ambiguous, a marker survives reordering. */
  bUUID active_layer_marker = {};
  /** The generated node group holding the stack, see the paint layer generator. */
  struct bNodeTree *paint_layers_tree = nullptr;
  /**
   * Identity of this material as the owner of #paint_layers_tree, stamped on the generated tree so
   * a pointer at a tree that belongs to someone else is never overwritten. Assigned the first time
   * the tree is generated and renewed on a copy.
   */
  bUUID paint_layers_owner_uid = {};
  /** #eMaterialPaintLayersFlag. */
  eMaterialPaintLayersFlag paint_layers_flag = {};
  /**
   * #eMaterialPaintChannel bitmask of the material's global paint channel set, one bit per channel
   * (`1 << channel`). Zero means "not set": BKE then derives the set from the union of every layer's
   * channel records plus the build default, so an existing file needs no versioning. Base Color is
   * an ordinary channel and may be left out of a stored set; an empty stored set is refused by the
   * setters (the derived one is never empty). Leaving a channel out of the set never removes a
   * layer's record or map; adding one never creates them (a record is created lazily).
   */
  uint16_t paint_layers_channels = 0;
  /**
   * Width and height of the maps the stack creates (256..4096). Zero in older files, which then
   * follow #PaintModeSettings::new_channel_image_size.
   */
  int16_t paint_layers_image_size = 4096;
  /** #eMaterialPaintCompositeDevice. Zero is the GPU so that files saved before it read as GPU. */
  char paint_layers_composite_device = 0;
  /**
   * The side the maps of a Material row are rendered at while the row is being edited, for the
   * Image Editor's composite (#eMaterialPaintPreviewSize). Zero is 2048, so files saved before it
   * read as the default.
   */
  char paint_layers_preview_size = 0;
  /** How long after the last edit the preview renders when it is not immediate (#eMaterialPaintPreviewDelay). */
  char paint_layers_preview_delay = 0;
  /** #eMaterialPaintPreviewFlag. Zero is immediate, which is what files saved before it read as. */
  char paint_layers_preview_flag = 0;
  char _pad_paint_layers[6] = {};
};

}  // namespace blender
