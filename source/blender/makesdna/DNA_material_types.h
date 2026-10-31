/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 */

#pragma once

#include "DNA_ID.h"
#include "DNA_colorband_types.h"
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
struct Object;
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
  /**
   * The row is protected from painting. It changes no pixel, no topology and no value the stack is
   * evaluated with, so it only answers the paint target and the UI.
   */
  MA_PAINT_LAYER_LOCKED = 1 << 5,
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

/**
 * #MaterialPaintLayerChannelSettings::height_blend -- how a row's Height record is laid over the
 * height below it. Unlike the colour blends, every mode works on the distance from the neutral
 * level, so a neutral record is a no-op whatever the mode. Inherit follows the row's ordinary
 * blend; Mix, Add and Subtract keep their meaning there, and every other blend reads as Mix. The
 * Height channel alone reads it; the values are not #eMaterialPaintLayerBlend codes.
 */
enum eMaterialPaintLayerHeightBlend : int8_t {
  /** Follow the row's own colour blend, the way a row written before the height modes reads. */
  MA_PAINT_LAYER_HEIGHT_BLEND_INHERIT = 0,
  MA_PAINT_LAYER_HEIGHT_BLEND_MIX = 1,
  MA_PAINT_LAYER_HEIGHT_BLEND_ADD = 2,
  MA_PAINT_LAYER_HEIGHT_BLEND_SUBTRACT = 3,
  /** The greater height wins; the layer's alpha covers the other channels where it won. */
  MA_PAINT_LAYER_HEIGHT_BLEND_COMPARE = 4,
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
   * A geometry map of the owning object (ambient occlusion, curvature, normal, ...), read from the
   * material's shared UV atlas. The row stores only the abstract map type
   * (#MaterialPaintLayer::mesh_map_type); the pixels live on the material as a #MaterialMeshMapSlot,
   * so the same stack can be placed on another object without rewriting the row.
   */
  MA_PAINT_LAYER_SOURCE_MESH_MAP = 5,
  /**
   * A filter: no colour of its own, it changes the colour it reads. The member is
   * #MaterialPaintLayer::kind_subtype (#eMaterialPaintLayerFilter). Only as a correction for now.
   */
  MA_PAINT_LAYER_SOURCE_FILTER = 6,
  /**
   * An Anchor Point or an Anchor Point Reader: the marker publishes a slice of its row, the reader
   * lays that slice. The member is #MaterialPaintLayer::kind_subtype
   * (#eMaterialPaintLayerAnchorPoint). 3 stays reserved for the Node Group source named above.
   */
  MA_PAINT_LAYER_SOURCE_ANCHOR_POINT = 8,
  /**
   * A procedural generator: it produces its colour and coverage from the shading coordinates
   * instead of a map. The member is #MaterialPaintLayer::kind_subtype
   * (#eMaterialPaintLayerGenerator) and the parameters live in
   * #MaterialPaintLayer::gradient. It owns channel records for the channels it takes part in, but no
   * map: a stroke never writes into it.
   */
  MA_PAINT_LAYER_SOURCE_GENERATOR = 7,
  /**
   * A Curve Patch effect: it paints its channels from the 3D control curves of its own PaintVector
   * container, through a private snapshot of the brush it was created with. The payload is
   * #MaterialPaintLayer::curve_patch. Only as an effect of another row.
   */
  MA_PAINT_LAYER_SOURCE_CURVE_PATCH = 9,
};

/**
 * The owned payload of a #MA_PAINT_LAYER_SOURCE_CURVE_PATCH row: the curve container and the
 * brush snapshot the row was created with. 1:1 ownership (see the ownership contract in
 * `paint_layer_dependencies`): on row free the two IDs are released only while the row is their
 * last user, otherwise they are left to whoever else uses them and the pointers are cleared.
 */
struct MaterialPaintLayerCurvePatch {
  DNA_DEFINE_CXX_METHODS(MaterialPaintLayerCurvePatch)

  /** The owned container of the effect's control curves (#PaintVector). */
  struct PaintVector *vectors = nullptr;
  /** The owned brush snapshot ("`.CP Brush <row>`" naming convention). */
  struct Brush *brush = nullptr;
  /* Pads the struct to a multiple of the pointer size, as every DNA struct must. */
  char _pad[8] = {};
};

/** #MaterialPaintLayer::kind_subtype of a #MA_PAINT_LAYER_SOURCE_ANCHOR_POINT row. */
enum eMaterialPaintLayerAnchorPoint : int16_t {
  /** The inert marker: it publishes the content or mask of the row it sits in. */
  MA_PAINT_LAYER_ANCHOR_POINT_MARKER = 0,
  /** The reader: it lays a slice published by a marker. */
  MA_PAINT_LAYER_ANCHOR_POINT_READER = 1,
};

/** #MaterialPaintLayer::kind_subtype of a #MA_PAINT_LAYER_SOURCE_GENERATOR row. */
enum eMaterialPaintLayerGenerator : int16_t {
  /** A ramp along a line (`linear`) or around a point (`radial`). */
  MA_PAINT_LAYER_GENERATOR_GRADIENT = 1,
};

/** #MaterialPaintLayerGradient::space: the space the gradient's line is defined in. */
enum eMaterialPaintLayerGradientSpace : int8_t {
  /** The gradient line is in the active UV map; X/Y used, Z ignored. */
  MA_PAINT_LAYER_GRADIENT_SPACE_UV = 0,
  /** The gradient line is in object space, in object units. */
  MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT = 1,
  /** The gradient line is in generated coordinates, normalized to 0..1 by the object bounding box. */
  MA_PAINT_LAYER_GRADIENT_SPACE_GENERATED = 2,
};

/** #MaterialPaintLayerGradient::type: the shape of the gradient's iso-lines. */
enum eMaterialPaintLayerGradientType : int8_t {
  /** `t` grows along the start-to-end line. */
  MA_PAINT_LAYER_GRADIENT_LINEAR = 0,
  /** The start is the centre, `|end - start|` the radius. */
  MA_PAINT_LAYER_GRADIENT_RADIAL = 1,
};

/** How a gradient parameter outside 0..1 is folded back into the ramp. */
enum eMaterialPaintLayerGradientRepeat : int8_t {
  /** Clamp to the nearest end: the gradient fills the whole surface. */
  MA_PAINT_LAYER_GRADIENT_REPEAT_NONE = 0,
  /** `t - floor(t)`: the ramp repeats. */
  MA_PAINT_LAYER_GRADIENT_REPEAT_REPEAT = 1,
  /** Mirror at every whole number: the ramp runs forwards, backwards, forwards... */
  MA_PAINT_LAYER_GRADIENT_REPEAT_REFLECT = 2,
};

/** What decides how the colours of a gradient are spread along its line. */
enum eMaterialPaintLayerGradientDistribution : int8_t {
  /** The midpoint bends the ramp, which is the source of the colours: the easy control for two. */
  MA_PAINT_LAYER_GRADIENT_DISTRIBUTION_MIDPOINT = 0,
  /** The ramp's own stops place the colours; the midpoint is ignored: the control for several. */
  MA_PAINT_LAYER_GRADIENT_DISTRIBUTION_RAMP = 1,
};

/** When the viewports show the handles of a gradient row. */
enum eMaterialPaintLayerGradientGizmoDisplay : int8_t {
  /** Only in the paint modes (Sculpt and Texture Paint) and the Image Editor's Paint mode. */
  MA_PAINT_LAYER_GRADIENT_GIZMO_PAINT_ONLY = 0,
  /** Whenever the row is the active one. */
  MA_PAINT_LAYER_GRADIENT_GIZMO_ALWAYS = 1,
};

/**
 * The payload of a #MA_PAINT_LAYER_SOURCE_GENERATOR / #MA_PAINT_LAYER_GENERATOR_GRADIENT row.
 *
 * The row carries a pointer to this rather than the struct by value: #ColorBand is close to a
 * kilobyte, and every row, correction and mask item would otherwise pay for it. The ramp is the
 * colour source for colour channels. A scalar channel has its own value ramp, stored on its record
 * as a pointer (#MaterialPaintLayerChannel::gradient_ramp): black at 0, white at 1, with as many
 * grey stops as the user adds between them.
 *
 * The line is in #space: the active UV map (X/Y used, Z ignored), object space, or normalized
 * generated coordinates. `start` is the centre of a radial gradient and `|end - start|` its
 * radius. The two points are what the gradient tool writes; no map resolution is involved.
 */
struct MaterialPaintLayerGradient {
  DNA_DEFINE_CXX_METHODS(MaterialPaintLayerGradient)

  /** The colours a colour channel reads, evaluated at the folded ramp position. */
  struct ColorBand ramp;
  /** Start of the line / centre of a radial gradient, in #space. */
  float start[3] = {0.0f, 0.5f, 0.0f};
  /** End of the line / radius point of a radial gradient, in #space. */
  float end[3] = {1.0f, 0.5f, 0.0f};
  /**
   * Where the ramp's 0.5 lands (0..1); 0.5 is the unbent ramp. Only read in the Midpoint
   * distribution (see #BKE_paint_layers_gradient_midpoint_effective).
   */
  float midpoint = 0.5f;
  /** #eMaterialPaintLayerGradientSpace. */
  int8_t space = MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT;
  /** #eMaterialPaintLayerGradientType. */
  int8_t type = MA_PAINT_LAYER_GRADIENT_LINEAR;
  /** #eMaterialPaintLayerGradientRepeat. */
  int8_t repeat = MA_PAINT_LAYER_GRADIENT_REPEAT_NONE;
  /** Whether the viewports show the row's line as draggable handles (0 or 1). */
  int8_t gizmo_shown = 0;
  /** #eMaterialPaintLayerGradientDistribution. */
  int8_t distribution = MA_PAINT_LAYER_GRADIENT_DISTRIBUTION_MIDPOINT;
  /** #eMaterialPaintLayerGradientGizmoDisplay. */
  int8_t gizmo_display = MA_PAINT_LAYER_GRADIENT_GIZMO_PAINT_ONLY;
  /**
   * #eMaterialPaintChannel whose ramp the handles show and move: a value channel reads its own
   * value ramp, while Base Color, Emission and every colour channel share #ramp. Zero is
   * PAINT_MATERIAL_CHANNEL_BASE_COLOR, which the padding older files carry, so they open on Base
   * Color. Spelled as a number because the channel enum lives in DNA_scene_types.h, which this
   * header cannot include.
   */
  int8_t gizmo_channel = 0;
  char _pad[1] = {};
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

/**
 * #MaterialPaintLayer::mesh_map_type, and the key of #MaterialMeshMapSlot: which geometry map of
 * the owning object a #MA_PAINT_LAYER_SOURCE_MESH_MAP row reads from the material's shared atlas.
 * The reserved values keep room for later maps without renumbering.
 */
enum eMaterialMeshMapType : int8_t {
  MA_MESH_MAP_AO = 0,
  MA_MESH_MAP_CURVATURE = 1,
  MA_MESH_MAP_NORMAL_WORLD = 2,
  MA_MESH_MAP_NORMAL_OBJECT = 3,
  MA_MESH_MAP_ID_OBJECT = 4,
  MA_MESH_MAP_ID_MATERIAL = 5,
  MA_MESH_MAP_EDGE = 6,
  /** Reserved; the architecture must not exclude these. */
  MA_MESH_MAP_THICKNESS = 7,
  /** Reserved; the architecture must not exclude these. */
  MA_MESH_MAP_POSITION = 8,
  MA_MESH_MAP_TYPE_NUM = 9,
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
  /**
   * The value ramp of a scalar channel of a Gradient generator: one grey stop per value along the
   * gradient, red/green/blue equal and in 0..1. Null for a colour channel, a Normal, and every row
   * that is not a generator. Kept off the record itself because #ColorBand is close to a kilobyte
   * and only a generator's scalar channels ever need one.
   */
  struct ColorBand *gradient_ramp = nullptr;
};

/**
 * #MaterialPaintLayerChannelSettings::height_flag. Zeroed fields read as the behaviour every file
 * written before the height model had, so a flag is opt-in and old files need no versioning.
 */
enum eMaterialPaintLayerHeightFlag : int8_t {
  /**
   * The row lays nothing in #PAINT_MATERIAL_CHANNEL_HEIGHT: its Height record (a map or a flat
   * value) is kept and shown, but the height it carries stops contributing to the stack, the way
   * Ucupaint's `write_height = False` keeps a layer's height out of the displacement. The row's
   * other channels are untouched.
   *
   * Known limitation: any Height mode or a Compare record takes the material off the GPU preview
   * and onto the CPU composite (see #BKE_paint_layers_gpu_unsupported_reason), so a row that sets
   * one paints slower until the mode is cleared.
   */
  MA_PAINT_LAYER_CHANNEL_HEIGHT_WRITE_SKIP = 1 << 0,
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
  /**
   * A per-Height-channel #eMaterialPaintLayerHeightBlend override. Zero inherits the channel's
   * ordinary blend (#blend), which is what files written before the height modes and rows that take
   * no part in Height read as; only #PAINT_MATERIAL_CHANNEL_HEIGHT honours it.
   */
  int8_t height_blend = MA_PAINT_LAYER_HEIGHT_BLEND_INHERIT;
  /** #eMaterialPaintLayerHeightFlag. Zero reads as the behaviour every older file had. */
  int8_t height_flag = 0;
  /** Keeps #height_scale_delta on its float boundary. */
  char _pad_height[2] = {};
  /**
   * The Height record's own scale, stored as the distance from one: the scale is `1 + delta`, so
   * zero (what a file written before the field carries) reads as one and no versioning is needed.
   * Applied to the distance from the neutral level; only #PAINT_MATERIAL_CHANNEL_HEIGHT honours it.
   */
  float height_scale_delta = 0.0f;
};

/** #MaterialPaintLayerMapping::space */
enum eMaterialPaintLayerMappingSpace : int16_t {
  MA_PAINT_LAYER_MAPPING_SPACE_UV = 0,
};

/** #MaterialPaintLayerMapping::flag */
enum eMaterialPaintLayerMappingFlag : int16_t {
  /**
   * Formerly "mapping enabled". Files written before the inverted flag store it in bit 0, so that
   * bit is left unused: a row saved with its mapping on keeps its settings, while the flag below
   * decides whether it is off now. Kept out of the way rather than reused, so an old file's set bit
   * cannot turn the new meaning on.
   */
  MA_PAINT_LAYER_MAPPING_ENABLED_DEPRECATED = 1 << 0, /* DNA_DEPRECATED */
  /**
   * The row's mapping is switched off. Stored inverted, like #MA_PAINT_LAYER_MAPPING_SCALE_UNLOCKED:
   * zeroed rows -- every new one, and every row read from a file before this flag existed -- come
   * out enabled, so a Fill shows its Mapping panel and gizmo before the first texture is slotted;
   * a row with nothing to map keeps it inert.
   */
  MA_PAINT_LAYER_MAPPING_DISABLED = 1 << 2,
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
 * The enabled flag is stored inverted (#MA_PAINT_LAYER_MAPPING_DISABLED), so a zeroed block -- a
 * new row, or one from a file written before the flag -- reads as enabled. A zero scale axis reads
 * back as one (see `BKE_paint_layers_mapping_scale_normalize`).
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
  /** #eMaterialMeshMapType; read only when #source is #MA_PAINT_LAYER_SOURCE_MESH_MAP. */
  int8_t mesh_map_type = MA_MESH_MAP_AO;
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
  char _pad_mask_source = 0;
  /**
   * The channels of the source material a mask correction that reads a material takes its grey from,
   * as a mask of #eMaterialPaintChannel bits; 0 reads as Base Color. Stored as a mask so that taking
   * the mean of several channels later needs no new field; the lowest bit is read for now.
   */
  uint16_t mask_source_channels = 0;
  char _pad_object_mask[2] = {};
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
  /**
   * The marker of the Anchor Point whose slice the row reads, or nil. Only an Anchor Point Reader
   * uses it: the point itself is a row of the same material, and the slice it publishes is decided
   * by the list it sits in (#MaterialPaintLayer::effects for content,
   * #MaterialPaintLayer::mask_stack for the mask).
   */
  bUUID anchor_point_source = {};
  /**
   * The payload of a generator row (#MA_PAINT_LAYER_SOURCE_GENERATOR), or null for every other
   * source. Owned by the row (freed, copied and written with it); a pointer rather than a value
   * because the gradient's #ColorBand would otherwise bloat every row in the file.
   */
  struct MaterialPaintLayerGradient *gradient = nullptr;
  /**
   * The owned payload of a #MA_PAINT_LAYER_SOURCE_CURVE_PATCH row, or null for every other source.
   * Owned by the row (freed, copied and written with it); the IDs it holds are registered in
   * `paint_layer_dependencies` and the row's `foreach_id`.
   */
  MaterialPaintLayerCurvePatch *curve_patch = nullptr;
};

/**
 * One shared atlas of a material: a #MaterialMeshMapSlot per #eMaterialMeshMapType, holding the
 * Image the material's MESH_MAP rows read. The pixels are a material resource because one material
 * is one generated shader shared by every object that uses it.
 */
struct MaterialMeshMapSlot {
  DNA_DEFINE_CXX_METHODS(MaterialMeshMapSlot)

  struct MaterialMeshMapSlot *next = nullptr, *prev = nullptr;
  /** #eMaterialMeshMapType. */
  int8_t type = MA_MESH_MAP_AO;
  char _pad[7] = {};
  /** The shared atlas for this map type, or null while nothing is baked. */
  struct Image *image = nullptr;
};

/**
 * Baking and viewport settings shared by every mesh map of a material. v1 stores the bake controls
 * a per-map bake job reads; the file format and encoding of the atlas Image are decided there.
 */
struct MaterialMeshMapSettings {
  DNA_DEFINE_CXX_METHODS(MaterialMeshMapSettings)

  /** Square side the atlas is allocated at when it is first created. */
  int resolution = 2048;
  /** Cycles samples the mesh map bake renders with. */
  int samples = 32;
  /** Whether the mesh map bake asks Cycles for denoising. */
  int8_t use_denoise = 0;
  char _pad[3] = {};
  /** UV margin in pixels applied while merging a per-object bake into the atlas. */
  float margin = 16.0f;
  /** Ambient-occlusion ray distance; zero means "from the object bounds". */
  float ao_distance = 0.0f;
  /** Edge/bevel ray radius in object-space units. */
  float edge_radius = 0.01f;
  char _pad2[4] = {};
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
  /**
   * Index into #BKE_paint_layers_view_passes of the pass the 3D Viewport shows this material through
   * while painting; zero is Combined. An index rather than a pass value, so that files saved before
   * the preview existed read as Combined instead of as the first channel.
   */
  int16_t paint_layers_view_pass = 0;
  /* The four chars and the int16 above leave the pointer below off its 8-byte boundary; this is
   * the single pad the pair needs. */
  char _pad_paint_layers[4] = {};
  /**
   * Stand-in image for the Image Editor's composite when the stack holds no map at all: the
   * composite is found through an #Image, so a material made of Gradient/Fill/Material rows alone
   * needs one to be reachable. Only the size the editor starts from and the material lookup use it;
   * the composite itself is shaded from the stack. Owned by the material (one user).
   */
  struct Image *paint_layers_canvas_image = nullptr;
  /**
   * The Strength of the Bump node the stack owns, stored as the distance from one: the value is
   * `1 + delta`, so zero (what a file written before the field carries) reads as the node's own
   * default and no versioning is needed.
   */
  float paint_layers_bump_strength_delta = 0.0f;
  /**
   * The Distance of the Bump node the stack owns, in Blender metres, stored as the distance from
   * one and read back the same way as #paint_layers_bump_strength_delta.
   */
  float paint_layers_bump_distance_delta = 0.0f;
  /**
   * The Strength of the Normal Map node the stack owns, stored as the distance from one and read
   * back the same way as #paint_layers_bump_strength_delta.
   */
  float paint_layers_normal_strength_delta = 0.0f;
  /* The pointer plus three floats above are 8 + 12 bytes, so the tail is four bytes short of the
   * pointer-size multiple the file format wants. The two pads are not one: this one closes the
   * tail, #_pad_paint_layers aligns the pointer. */
  char _pad_paint_layers_tail[4] = {};
  /** The material's mesh map atlases, one #MaterialMeshMapSlot per #eMaterialMeshMapType. */
  ListBaseT<MaterialMeshMapSlot> mesh_map_slots = {nullptr, nullptr};
  /** Bake/viewport settings shared by every mesh map of this material. */
  MaterialMeshMapSettings mesh_map_settings;
  /** Explicit tail padding: the struct's pointer alignment must not leave uninitialized bytes. */
  char _pad_tail[4] = {};
};

}  // namespace blender
