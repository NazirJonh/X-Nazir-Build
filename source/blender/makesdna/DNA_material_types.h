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
  /** The material carries a paint layer description; its tree is generated from it. */
  MA_PAINT_LAYERED = 1 << 0,
  /**
   * The generator owns the tree and overwrites manual edits. Cleared for debugging, where the
   * tree is left untouched until an explicit regenerate.
   */
  MA_PAINT_LAYERS_LOCKED = 1 << 1,
  /** The tree is out of date and will be rebuilt from the description. */
  MA_PAINT_LAYERS_REGEN = 1 << 2,
  /**
   * The cached texture-paint slots (`Material::texpaintslot`) no longer match the description: the
   * active layer, a channel map, the mask or the target mode changed. Rebuilt on the main thread
   * at the K-1 regeneration point.
   */
  MA_PAINT_LAYERS_SLOTS_STALE = 1 << 3,
  /**
   * Some row with a bake had a value-only edit (`opacity`, `fill`, a mask value, `enabled`), or a
   * pixel edit arrived; the planner at the K-1 point re-bakes the rows whose stored hash no longer
   * matches. Set by the value-only edit path, which cannot rebuild the tree the way REGEN does.
   */
  MA_PAINT_LAYERS_BAKE_STALE = 1 << 4,
  /**
   * The active row moved, so the row left behind is due for its deferred bake. Read by the
   * editor's Material-row planner after the depsgraph update.
   *
   * Separate from #MA_PAINT_LAYERS_BAKE_STALE on purpose: that one belongs to the CPU
   * planner, which clears it at the K-1 point -- before the editor update runs -- and never
   * counts a Material row as pending, so it cannot carry this signal.
   */
  MA_PAINT_LAYERS_MATERIAL_BAKE_DUE = 1 << 5,
};
ENUM_OPERATORS(eMaterialPaintLayersFlag)

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

/** #MaterialPaintLayer::blend */
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
};

/**
 * #MaterialPaintLayer::source. Numerically matches #blender::PaintLayerSourceType
 * (`BKE_paint_layers.hh`); kept in step with a static_assert in `paint_layers.cc`.
 */
enum eMaterialPaintLayerSource : int8_t {
  MA_PAINT_LAYER_SOURCE_IMAGE = 0,
  MA_PAINT_LAYER_SOURCE_CONSTANT = 1,
  MA_PAINT_LAYER_SOURCE_MATERIAL = 2,
  MA_PAINT_LAYER_SOURCE_NODE_GROUP = 3,
  MA_PAINT_LAYER_SOURCE_STACK = 4,
};

/**
 * #MaterialPaintLayer::role. Numerically matches #blender::PaintLayerRole
 * (`BKE_paint_layers.hh`); kept in step with a static_assert in `paint_layers.cc`.
 */
enum eMaterialPaintLayerRole : int8_t {
  MA_PAINT_LAYER_ROLE_LAYER = 0,
  MA_PAINT_LAYER_ROLE_EFFECT = 1,
  MA_PAINT_LAYER_ROLE_MASK_ITEM = 2,
};

/** #MaterialPaintLayerChannel::state */
enum eMaterialPaintLayerChannelState : int8_t {
  /** No map: the channel keeps its row but carries no coverage. */
  MA_PAINT_LAYER_CHANNEL_ABSENT = 0,
  /** A map feeds the channel and its coverage is live. */
  MA_PAINT_LAYER_CHANNEL_ENABLED = 1,
  /** The map is kept but switched off; coverage is unlinked and zero. */
  MA_PAINT_LAYER_CHANNEL_DISABLED = 2,
};

/** #MaterialPaintLayer::flag */
enum eMaterialPaintLayerFlag : int16_t {
  /** The layer takes part in the stack. */
  MA_PAINT_LAYER_ENABLED = 1 << 0,
};
ENUM_OPERATORS(eMaterialPaintLayerFlag)

/**
 * One channel of a #MaterialPaintLayer. A layer owns a `channels` array with one entry per wired
 * channel; #channel names which #eMaterialPaintChannel the record stands for.
 */
struct MaterialPaintLayerChannel {
  DNA_DEFINE_CXX_METHODS(MaterialPaintLayerChannel)

  /** #eMaterialPaintChannel. */
  int8_t channel = 0;
  /** #eMaterialPaintLayerChannelState. */
  int8_t state = MA_PAINT_LAYER_CHANNEL_ABSENT;
  char _pad[6] = {};
  /** The channel's map, or null when it has none. */
  struct Image *image = nullptr;
  /** Constant value used when the channel carries no map. */
  float value[4] = {};
  /**
   * Deprecated per-channel blend/opacity override, superseded by
   * #MaterialPaintLayer::channel_settings. Kept only so a file written between the two layouts can
   * be migrated; the runtime never reads these.
   */
  DNA_DEPRECATED int8_t blend = -1;
  char _pad_deprecated[3] = {};
  DNA_DEPRECATED float opacity = 1.0f;
};

/**
 * The per (row, channel) blend and opacity override of a #MaterialPaintLayer.
 *
 * A fixed array on the row rather than a field of a sparse #MaterialPaintLayerChannel record: the
 * value exists for every pair, record or not, which gives it one stable RNA path that survives a
 * record being created, and lets a keyframe address it. The sentinels are the inherit defaults.
 */
struct MaterialPaintLayerChannelSettings {
  DNA_DEFINE_CXX_METHODS(MaterialPaintLayerChannelSettings)

  /** #eMaterialPaintLayerBlend, or -1 to inherit the row's blend. */
  int8_t blend = -1;
  char _pad[3] = {};
  /** Multiplied by the row's opacity; 1.0 when the channel has no override. */
  float opacity = 1.0f;
};

/** #MaterialPaintLayerBake::mode */
enum eMaterialPaintLayerBakeMode : int8_t {
  /** Bake only the heavy inactive subgraphs; the active node is evaluated live. */
  MA_PAINT_LAYER_BAKE_AUTO = 0,
  /** Always bake and show "stale" until the bake is current. */
  MA_PAINT_LAYER_BAKE_ALWAYS = 1,
  /** Never bake; the node is evaluated live. */
  MA_PAINT_LAYER_BAKE_NEVER = 2,
};

/**
 * The baked cache of one description row, or null while nothing is baked.
 *
 * Cache, not truth: the description plus its live maps are what the row *is*, and these are a
 * snapshot the generator and the CPU compositor may read instead of evaluating the subtree. The
 * `hash` is the whole validity test -- there is no separate "valid" flag to save -- and the maps
 * are owned here as ID references (see the layer's `foreach_id`).
 */
struct MaterialPaintLayerBake {
  DNA_DEFINE_CXX_METHODS(MaterialPaintLayerBake)

  /** One map per #eMaterialPaintChannel index, or null where the channel is not baked.
   * Sized by #PAINT_MATERIAL_CHANNEL_NUM; `paint_layers.cc` static_asserts the 10 stays honest. */
  struct Image *images[10] = {};
  /** The baked coverage map, or null. */
  struct Image *coverage = nullptr;
  /** #eMaterialPaintLayerBakeMode. */
  int8_t mode = MA_PAINT_LAYER_BAKE_AUTO;
  char _pad[3] = {};
  /** Square side the maps were baked at. */
  int size = 0;
  /**
   * Description hash at the last successful bake, low word first; see #MaterialPaintLayer's old
   * note for why two 32-bit words.
   */
  uint32_t hash[2] = {};
};

/**
 * One row of a layered material's stack, the DNA description the node tree is generated from.
 *
 * The list is stored bottom-to-top. Folders nest through #children, which holds the same type.
 */
struct MaterialPaintLayer {
  DNA_DEFINE_CXX_METHODS(MaterialPaintLayer)

  struct MaterialPaintLayer *next = nullptr, *prev = nullptr;
  /** Nested folder layers; the same type as the parent list. */
  ListBase children = {nullptr, nullptr};
  char name[/*MAX_NAME*/ 64] = "";
  /** Stable identity of the row, shared by the nodes the generator builds for it. */
  bUUID marker = {};
  /** #eMaterialPaintLayerBlend. */
  int8_t blend = MA_PAINT_LAYER_BLEND_MIX;
  char _pad0[1] = {};
  /** #eMaterialPaintLayerFlag. */
  int16_t flag = MA_PAINT_LAYER_ENABLED;
  float opacity = 1.0f;
  float fill_color[4] = {};
  /** Display color tag, interpreted by the UI only. -1 means no color (default). */
  int8_t color_tag = -1;
  /**
   * #eMaterialPaintLayerSource: what the row paints with (image, constant, material, node group
   * or stack). Read-only from the outside; changed only through #BKE_paint_layers_source_change.
   */
  int8_t source = MA_PAINT_LAYER_SOURCE_IMAGE;
  /**
   * #eMaterialPaintLayerRole: a stack layer, a content correction (effect) or a mask item.
   * Changed only through #BKE_paint_layers_role_set.
   */
  int8_t role = MA_PAINT_LAYER_ROLE_LAYER;
  char _pad[5] = {};
  /** Node group for a #MA_PAINT_LAYER_SOURCE_NODE_GROUP layer. */
  struct bNodeTree *custom_group = nullptr;
  /** Source material of a #MA_PAINT_LAYER_SOURCE_MATERIAL layer. Phase 3. */
  struct Material *material = nullptr;
  /**
   * The row's baked cache, or null while nothing is baked. A snapshot the generator and the CPU
   * compositor may read instead of evaluating the subtree; the description stays the truth.
   */
  struct MaterialPaintLayerBake *bake = nullptr;
  /** Custom group input values and add-on data; the core never reads structure from here. */
  IDProperty *properties = nullptr;
  struct MaterialPaintLayerChannel *channels = nullptr;
  int channels_num = 0;
  char _pad2[4] = {};
  /**
   * Per (row, channel) blend/opacity overrides, indexed by #eMaterialPaintChannel. A fixed array so
   * every pair has a value and an RNA path, whether or not it has a #channels record yet. The
   * literal length is #PAINT_MATERIAL_CHANNEL_NUM; `paint_layers.cc` static_asserts the 10.
   */
  MaterialPaintLayerChannelSettings channel_settings[10] = {};
  /**
   * Effects that adjust what the row paints with, bottom to top. Same type as a layer, linked by
   * their own markers.
   */
  ListBase effects = {nullptr, nullptr};
  /**
   * Mask items that limit where the row applies, bottom to top. A mask is a stack like any other;
   * its coverage starts at one. Same type as a layer, linked by their own markers.
   */
  ListBase mask_stack = {nullptr, nullptr};
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

  char _pad3[4] = {};

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
   * the node tree is generated from this and the description is the source of truth.
   */
  ListBase paint_layers = {nullptr, nullptr};
  /** Marker of the active layer or mask; an index is ambiguous across nesting. */
  bUUID active_layer_marker = {};
  /** The generated node group holding the stack, see the paint layer generator. */
  struct bNodeTree *paint_layers_tree = nullptr;
  /**
   * Identity of this material as the owner of #paint_layers_tree, stamped on the generated tree so
   * a pointer at a tree that belongs to someone else is never overwritten. Assigned the first time
   * the material becomes layered (or the tree is generated) and regenerated on a copy.
   */
  bUUID paint_layers_owner_uid = {};
  /** #eMaterialPaintLayersFlag. */
  eMaterialPaintLayersFlag paint_layers_flag = {};
  char _pad2[6] = {};
};

}  // namespace blender
