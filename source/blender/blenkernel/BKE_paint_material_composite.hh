/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * Flattens a layer stack -- a chain of Mix nodes over Image Textures -- into one buffer.
 *
 * This is the interactive half of the pair that turns a material into a single buffer; the other
 * is the bake in `ED_material_bake.hh`. Which one applies is not a user choice but a property of
 * the graph, decided by #BKE_paint_material_source_resolve and by whether
 * #BKE_paint_material_composite_stack_from_material can express the channel as a layer stack:
 *
 * - A plain stack of image layers composites here, on the CPU, in milliseconds, and can be
 *   refreshed for a single rectangle. That is what makes a stroke show its own result on the
 *   composite canvas within a frame.
 * - Anything else -- procedural nodes, math, ramps -- has to go through the bake, which is
 *   general but costs a shader compile and an EEVEE job, and can only ever be recomputed whole.
 *
 * The two must never both be considered authoritative for the same channel: whenever a stack is
 * derived here the composite is the answer, and the bake is not consulted.
 *
 * Free of GPU and of #Object, like the resolver it builds on.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "BLI_function_ref.hh"
#include "BLI_index_range.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "BKE_paint_material_composite_surface.hh"

#include "DNA_scene_types.h"
#include "DNA_uuid_types.h"
#include "DNA_material_types.h"

namespace blender {

namespace ocio {
class ColorSpace;
}
using ColorSpace = ocio::ColorSpace;

struct ImBuf;
struct Image;
struct Main;
struct bNode;
struct bNodeTree;
struct ImageUser;
struct Material;
struct rcti;

/**
 * How a layer combines with what is below it: every mode a Mix node offers.
 *
 * The values match the `MA_RAMP_*` codes the node itself stores, so the description's one table
 * (#BKE_paint_layers_blend_to_ramp) maps straight onto this and no second table can drift from it.
 * #NormalCombine is the one non-Mix operation and sits past the end of the ramp range.
 */
enum class CompositeBlend : int8_t {
  Mix = 0,          /* MA_RAMP_BLEND */
  Add = 1,          /* MA_RAMP_ADD */
  Multiply = 2,     /* MA_RAMP_MULT */
  Subtract = 3,     /* MA_RAMP_SUB */
  Screen = 4,       /* MA_RAMP_SCREEN */
  Divide = 5,       /* MA_RAMP_DIV */
  Difference = 6,   /* MA_RAMP_DIFF */
  Darken = 7,       /* MA_RAMP_DARK */
  Lighten = 8,      /* MA_RAMP_LIGHT */
  Overlay = 9,      /* MA_RAMP_OVERLAY */
  Dodge = 10,       /* MA_RAMP_DODGE */
  Burn = 11,        /* MA_RAMP_BURN */
  Hue = 12,         /* MA_RAMP_HUE */
  Saturation = 13,  /* MA_RAMP_SAT */
  Value = 14,       /* MA_RAMP_VAL */
  Color = 15,       /* MA_RAMP_COLOR */
  SoftLight = 16,   /* MA_RAMP_SOFT */
  LinearLight = 17, /* MA_RAMP_LINEAR */
  Exclusion = 18,   /* MA_RAMP_EXCLUSION */
  /**
   * Combine two tangent-space normal maps, rather than blending their encoded bytes.
   *
   * Encoded normals are not colours: averaging two of them channel by channel flattens the
   * relief instead of laying one over the other, which is why every layer stack that supports
   * normals has an operation of its own for it. This is the whiteout blend -- the detail map's
   * slope added to the base map's, renormalized -- which is what "overlay"/"add" means for a
   * normal layer.
   *
   * Nothing in a plain Mix chain selects this: a Mix node really does interpolate the encoded
   * values, and reproducing it any other way would make the composite disagree with the render.
   * It exists for the graph shapes that genuinely combine normals.
   */
  NormalCombine = 19,
};

/**
 * A flat colour standing in for a map, scene linear.
 *
 * A channel with no map reduces to a constant; carrying it here rather than manufacturing a
 * one-pixel image keeps the CPU path free of sampling. Its alpha is content alpha, never coverage
 * (see #PaintMaterialCompositeRowParams::color_alpha_coverage). Read by the linear evaluator only.
 */
struct PaintMaterialCompositeConstant {
  float constant_color[4] = {};
  bool has_constant_color = false;
};

/**
 * The UV mapping of a row's own map (a Fill layer, correction or mask): when #enabled, the map is
 * read through `uv' = (R * uv) * scale + offset` with Repeat edges, the transform the Vector Rotate
 * and Mapping nodes of the generated graph apply. Copied from the description when the stack is derived; the
 * resample happens while the buffers are acquired, so the evaluator reads the remapped buffer and
 * never sees these.
 */
struct PaintMaterialCompositeMapping {
  bool enabled = false;
  float offset[2] = {};
  float scale[2] = {1.0f, 1.0f};
  float rotation = 0.0f;
  /**
   * #eMaterialPaintLayerMappingWrap: what the resample does outside the unit UV square. An axis
   * that clips reads transparent there, and the map's alpha takes it out of the coverage.
   */
  int16_t wrap = 0;
  /**
   * The mapped map holds tangent-space normals (Normal channel): the resample also rotates and
   * flips the vectors, so the lighting follows the pattern.
   */
  bool tangent_normal = false;
};

/**
 * A Gradient generator's payload, as the CPU evaluator reads it.
 *
 * The row has no map: its colour comes from #ramp at the factor of the row's UV-space line, and a
 * scalar channel mixes #scalar_start and #scalar_end instead. The generated shader tree builds the
 * same factor from the shared Gradient Factor group. #width and #height are the material's map size:
 * the evaluator needs them to turn a texel into the UV the shader would see.
 */
struct PaintMaterialCompositeGradient {
  bool enabled = false;
  /** The channel reads the ramp rather than the scalar ends. */
  bool color_channel = false;
  /** The channel's own ends, for a scalar channel. */
  float scalar_start = 0.0f;
  float scalar_end = 1.0f;
  /** The material's map size, in pixels: the evaluator's UV grid for a row with no map. */
  int width = 0;
  int height = 0;
  /** The description's payload: line, midpoint, type, repeat and ramp. Its DNA type has deleted
   * copy operations, so the two methods below copy it shallowly like every other DNA value. */
  MaterialPaintLayerGradient payload;
  /**
   * The per-object surface used for the exact 2D preview of Object/Generated rows. Null for a UV
   * row and while the approximate preview is shown; the shared pointer lets a layer outlive a cache
   * replacement.
   */
  std::shared_ptr<const PaintMaterialCompositeSurface> surface;
  /** #surface's key, also carried when the pointer has already been dropped; moves the stack hash. */
  uint64_t surface_key = 0;
  /** True when the coordinate is the UV texel itself rather than a surface sample. */
  bool approximate = true;

  PaintMaterialCompositeGradient() = default;
  PaintMaterialCompositeGradient(const PaintMaterialCompositeGradient &other)
      : enabled(other.enabled),
        color_channel(other.color_channel),
        scalar_start(other.scalar_start),
        scalar_end(other.scalar_end),
        width(other.width),
        height(other.height),
        payload(dna::shallow_copy(other.payload)),
        surface(other.surface),
        surface_key(other.surface_key),
        approximate(other.approximate)
  {
  }
  PaintMaterialCompositeGradient(PaintMaterialCompositeGradient &&other) noexcept
      : enabled(other.enabled),
        color_channel(other.color_channel),
        scalar_start(other.scalar_start),
        scalar_end(other.scalar_end),
        width(other.width),
        height(other.height),
        payload(dna::shallow_copy(other.payload)),
        surface(std::move(other.surface)),
        surface_key(other.surface_key),
        approximate(other.approximate)
  {
  }
  PaintMaterialCompositeGradient &operator=(const PaintMaterialCompositeGradient &other)
  {
    if (this != &other) {
      enabled = other.enabled;
      color_channel = other.color_channel;
      scalar_start = other.scalar_start;
      scalar_end = other.scalar_end;
      width = other.width;
      height = other.height;
      payload = dna::shallow_copy(other.payload);
      surface = other.surface;
      surface_key = other.surface_key;
      approximate = other.approximate;
    }
    return *this;
  }
  PaintMaterialCompositeGradient &operator=(PaintMaterialCompositeGradient &&other) noexcept
  {
    if (this != &other) {
      enabled = other.enabled;
      color_channel = other.color_channel;
      scalar_start = other.scalar_start;
      scalar_end = other.scalar_end;
      width = other.width;
      height = other.height;
      payload = dna::shallow_copy(other.payload);
      surface = std::move(other.surface);
      surface_key = other.surface_key;
      approximate = other.approximate;
    }
    return *this;
  }
};

/**
 * What an image layer and its buffer twin share verbatim. The stack builders copy it as one value
 * (`static_cast<PaintMaterialCompositeRowParams &>(layer) = image_layer`), so a field added here
 * can never be forgotten in one of the copies. The values are plain data or owning pointers; the
 * slice pointers below are shared because the description owns them, not the cache.
 */
struct PaintMaterialCompositeRowParams : PaintMaterialCompositeConstant {
  /**
   * The layer's own map alpha multiplies into the factor, so a texel the map leaves transparent
   * shows what is below instead of covering it with the map's (black) color. Set for a painted
   * channel map; a constant covers by its opacity alone. Read by the linear evaluator only.
   */
  bool color_alpha_coverage = false;
  /** A Normal map authored in DirectX convention; convert its green channel to OpenGL before blend. */
  bool normal_directx = false;
  /**
   * The mask is the image's alpha rather than its colour.
   *
   * Which output of the Image Texture the factor was taken from, and not a detail: a layer stack
   * routes the layer's own alpha into the factor, and reading its colour there instead would
   * modulate every layer by its own brightness.
   */
  bool mask_from_alpha = false;
  CompositeBlend blend = CompositeBlend::Mix;
  float opacity = 1.0f;
  /** How much of the mask applies; 0 ignores the mask entirely. */
  float mask_influence = 1.0f;
  bool enabled = true;
  /**
   * A folder: it has no map or constant of its own, only its children composited in isolation over
   * nothing. The folder then lays that straight colour over the rows below with its own blend,
   * opacity and mask, scaled by the coverage the children accumulated.
   */
  bool is_folder = false;
  /**
   * An unpainted Paint row kept alive only for the corrections laid over it: its constant covers
   * nothing, so the corrections bring coverage in instead of the row filling everything with black.
   */
  bool empty_base = false;
  /**
   * The side of the square canvas of an #empty_base row whose corrections own no map to say it (Fill
   * and Material ones); 0 leaves the size to the first map found.
   */
  int empty_base_size = 0;
  /**
   * Whether the row's fourth component is its content alpha, raised by each correction by the over
   * model (`a = a + f * (1 - a)`), rather than the coverage it blends by. The Normal channel keeps
   * the colour's own, and the generated graph tracks the same quantity (see
   * #BKE_paint_material_channel_tracks_content_alpha).
   */
  bool tracks_content_alpha = false;
  /**
   * An adjustment (#eMaterialPaintLayerFilter, or 0 for none): the row has no colour of its own, its
   * colour is this filter applied to the stack below it, and it is laid over that stack by the
   * row's own blend, opacity and mask. Never inside a folder.
   */
  int16_t adjustment_filter = 0;
  /** The adjustment's parameters in its kind's schema order. */
  float adjustment_params[8] = {};
  /**
   * An Anchor Point slice laid as a row: straight colour and content alpha, plus the coverage it
   * lays (a grey in RGB). The description owns a reference (the slice cache may evict and clear its
   * own entry at any time), so the buffers live as long as this description; null for every other
   * row.
   */
  std::shared_ptr<ImBuf> anchor_point_color;
  std::shared_ptr<ImBuf> anchor_point_coverage;
  /** A description hash of the slice, folded into the stack hash in place of an Image uid. */
  uint64_t anchor_point_hash = 0;
  /**
   * The row's UV mapping, applied to its own map while the buffers are acquired. Shared by the
   * image layer and its buffer twin like the rest, though only the image layer's is read.
   */
  PaintMaterialCompositeMapping mapping;
};

/**
 * One content correction of a row, as data-blocks: a map laid over the row's colour before the row's
 * own blend, by `opacity * alpha(map)`; or, with #has_constant_color, a flat colour (a Fill
 * correction) laid by `opacity` alone, its alpha never part of the factor. A correction with neither
 * in the channel is not carried.
 */
struct PaintMaterialCompositeCorrection : PaintMaterialCompositeConstant {
  /**
   * A filter (#eMaterialPaintLayerFilter) instead of a map or a flat colour: the correction
   * changes the colour below it by `opacity` and lays no coverage. 0 for a laid correction.
   */
  int16_t filter = 0;
  /** The filter's parameters in its kind's schema order. */
  float filter_params[8] = {};
  Image *image = nullptr;
  /** Null selects the default #ImageUser. */
  const ImageUser *iuser = nullptr;
  CompositeBlend blend = CompositeBlend::Mix;
  /** The correction's effective opacity (enabled folded in). */
  float opacity = 1.0f;
  /** The correction's UV mapping, applied to its map while the buffers are acquired. */
  PaintMaterialCompositeMapping mapping;
  /**
   * An Anchor Point slice read as a correction: straight colour and content alpha, with the
   * coverage it lays in #anchor_point_coverage. Owned by this description (see
   * #PaintMaterialCompositeImageLayer::anchor_point_color); null otherwise.
   */
  std::shared_ptr<ImBuf> anchor_point_slice;
  std::shared_ptr<ImBuf> anchor_point_coverage;
  /** The reader this correction lays, and whether it reads content or a mask. */
  bUUID anchor_point_reader = {};
  int8_t anchor_point_target = 0;
  /** The point the reader names, the slice's producer. */
  bUUID anchor_point_point = {};
  /** A baked reader's coverage map; the live path uses #anchor_point_coverage instead. */
  Image *anchor_point_coverage_image = nullptr;
  const ImageUser *anchor_point_coverage_iuser = nullptr;
  uint64_t anchor_point_hash = 0;
  /** See #PaintMaterialCompositeImageLayer::anchor_point_images. */
  Vector<Image *> anchor_point_images;
};

/** A correction as the evaluator reads it: a buffer, or a flat colour when #ibuf is null. */
struct PaintMaterialCompositeCorrectionBuffer : PaintMaterialCompositeConstant {
  /** See #PaintMaterialCompositeCorrection::filter. */
  int16_t filter = 0;
  float filter_params[8] = {};
  ImBuf *ibuf = nullptr;
  /** Colorspace name of #ibuf's own buffer, or null for scene linear. */
  const char *colorspace_name = nullptr;
  CompositeBlend blend = CompositeBlend::Mix;
  float opacity = 1.0f;
  /** Borrowed from #PaintMaterialCompositeCorrection::anchor_point_slice, which keeps them alive. */
  ImBuf *anchor_point_slice = nullptr;
  ImBuf *anchor_point_coverage = nullptr;
  /** See #PaintMaterialCompositeCorrection::anchor_point_coverage_image. */
  ImBuf *anchor_point_coverage_ibuf = nullptr;
  const char *anchor_point_coverage_colorspace_name = nullptr;
};

/**
 * One item of a row's mask stack, as data-blocks.
 *
 * The item lays its coverage over the mask's factor `M` (one before the first item) by its own
 * blend, the Mix node the generator builds: `M = blend(M, C, A * opacity)`, where `C` is the mean of
 * the map's RGB and `A` its alpha, or #constant_gray and one when the item has no map. The base
 * mask is the Multiply item, `M * (1 - A * opacity * (1 - C))`: an unpainted (transparent) texel
 * leaves the row visible, an opaque black one hides it. A mask correction names its own blend, Mix
 * by default, which says what the coverage is where the item covers. The row's coverage is scaled
 * by the final `M`.
 */
/** One colour a Color ID map-coverage operator matches, in the map's raw data space. */
struct PaintMaterialCompositeIdColor {
  float color[3] = {};
  /** The colour's own tolerance in Advanced mode; ignored unless #own_tolerance is set. */
  float tolerance = 0.5f;
  /** A disabled colour is skipped by the match. */
  bool enabled = true;
  /** Color ID Advanced mode: use #tolerance and a hard edge instead of the shared pair. */
  bool own_tolerance = false;
};

struct PaintMaterialCompositeMaskItem {
  /** The painted map, or null while the item is still a constant. */
  Image *image = nullptr;
  /** Null selects the default #ImageUser. */
  const ImageUser *iuser = nullptr;
  /** The constant coverage, used while #image is null. */
  float constant_gray = 1.0f;
  /** The item's effective opacity (enabled folded in). */
  float opacity = 1.0f;
  /** How the item combines with the factor below it: the base mask's Multiply unless set. */
  CompositeBlend blend = CompositeBlend::Multiply;
  /** Reads the grey #C as `1 - C`, the alpha untouched; a mask correction's choice. */
  bool invert = false;
  /**
   * A filter (#eMaterialPaintLayerFilter) instead of a map or a grey: the item changes the factor
   * so far, `M = lerp(M, f(M), opacity)`. 0 for a laid item.
   */
  int16_t filter = 0;
  float filter_params[8] = {};
  /** The item's UV mapping, applied to its map while the buffers are acquired. */
  PaintMaterialCompositeMapping mapping;

  /**
   * A map-driven mask coverage operator (#MA_PAINT_LAYER_FILTER_COLOR_ID / _UV_ISLAND), or 0 for a
   * laid or filter item. It lays `match` from #coverage_image: `M = M * lerp(1, match, opacity)`.
   * A missing map is `match = 0` (the operator still takes part); the element's visibility is the
   * only switch, and its absence is folded into #opacity by the describe step.
   */
  int16_t map_coverage = 0;
  /** The operator's map (the item's Base Color record image), or null when it has none. */
  Image *coverage_image = nullptr;
  /** Null selects the default #ImageUser. */
  const ImageUser *coverage_iuser = nullptr;
  /** How far a sample may differ from a colour and still match (max-channel), 0..1. */
  float coverage_tolerance = 0.0f;
  /** The width of the falloff past the tolerance; zero is a hard edge. */
  float coverage_softness = 0.0f;
  /** The scale the match is multiplied by (the item's Gradient value, one for White/Black). */
  float coverage_output = 1.0f;
  /** The colours the Color ID operator matches; the enabled ones decide the match. */
  Vector<PaintMaterialCompositeIdColor> coverage_colors;
  /** The operator's UV mapping, applied to its map while the buffers are acquired. */
  PaintMaterialCompositeMapping coverage_mapping;
  /**
   * An Anchor Point slice read as a mask item: the scalar slice as a grey RGBA buffer, owned by this
   * description (see #PaintMaterialCompositeImageLayer::anchor_point_color). Null for every other
   * item.
   */
  std::shared_ptr<ImBuf> anchor_point_slice;
  /** The reader this mask item lays, and whether it reads content or a mask. */
  bUUID anchor_point_reader = {};
  int8_t anchor_point_target = 0;
  /** The point the reader names, the slice's producer. */
  bUUID anchor_point_point = {};
  uint64_t anchor_point_hash = 0;
  /** The channel a mask reader of a content point takes its coverage from: the first one the
   * source takes part in (#paint_layer_anchor_point_mask_read_channel). */
  int anchor_point_read_channel = 0;
  /** See #PaintMaterialCompositeImageLayer::anchor_point_images. */
  Vector<Image *> anchor_point_images;
};

/** One layer of a stack, as data-blocks. This is what a material resolves to. */
struct PaintMaterialCompositeImageLayer : PaintMaterialCompositeRowParams {
  /**
   * The maps an Anchor Point slice laid by this layer's reader (the layer itself, its corrections
   * or its mask items) was rendered from. The slice hash does not hold pixels, so the cache has to
   * subscribe to these to see a stroke on the source.
   */
  Vector<Image *> anchor_point_images;
  /** The reader this layer is, and whether it reads content or a mask. */
  bUUID anchor_point_reader = {};
  int8_t anchor_point_target = 0;
  /** The point the reader names, the slice's producer. */
  bUUID anchor_point_point = {};
  /** Markers of this row whose slices a reader lays: the GPU captures these after the matching
   * effect/mask step. Empty for every row without a read point. Not read by the GPU slots yet: the
   * slot pass captures by the reader's own #anchor_point_point. */
  Vector<bUUID> anchor_point_content_producers;
  Vector<bUUID> anchor_point_mask_producers;
  /**
   * The row's UV mapping cuts away part of the canvas, and its coverage has to follow: a Material
   * row covers by its source's transparency, not by the alpha of its map, so a map texel that a
   * clipping wrap leaves transparent would still lay its black over the rows below. When set, the
   * acquisition folds `inside(transformed_uv)` into the row's coverage (see #mapping), which is the
   * same 0/1 factor the generated graph multiplies in. Only ever set for a Material row with a
   * clipping wrap; a Fill or painted row already clips through its map's alpha.
   */
  bool mapping_clips_coverage = false;
  Image *color_image = nullptr;
  /**
   * The image node's own #ImageUser, or null. Owned by the material: copy it before acquiring a
   * buffer, since acquisition writes to it.
   */
  const ImageUser *color_iuser = nullptr;
  /** Modulates #opacity per pixel. Null when the layer's factor is a plain value. */
  Image *mask_image = nullptr;
  const ImageUser *mask_iuser = nullptr;
  /** The row's mask items, bottom to top; each multiplies the factor. */
  Vector<PaintMaterialCompositeMaskItem> mask_items;
  /** The corrections blended onto the row's colour, bottom to top. */
  Vector<PaintMaterialCompositeCorrection> content_corrections;
  /** A Gradient generator's payload; #enabled false for every row with a map or constant. */
  PaintMaterialCompositeGradient gradient;
  /**
   * A folder's own stack, bottom to top. A std::vector rather than a #Vector: the element type is
   * the one being defined here, and #Vector needs its size.
   */
  std::vector<PaintMaterialCompositeImageLayer> children;
};

/** A mask item as the evaluator reads it: a buffer, or a constant when #ibuf is null. */
struct PaintMaterialCompositeMaskBuffer {
  ImBuf *ibuf = nullptr;
  /** Colorspace name of #ibuf's own buffer, or null for scene linear. */
  const char *colorspace_name = nullptr;
  float constant_gray = 1.0f;
  float opacity = 1.0f;
  CompositeBlend blend = CompositeBlend::Multiply;
  bool invert = false;
  /** See #PaintMaterialCompositeMaskItem::filter. */
  int16_t filter = 0;
  float filter_params[8] = {};
  /** See #PaintMaterialCompositeMaskItem::map_coverage. */
  int16_t map_coverage = 0;
  /** The operator's map as a buffer, or null when it has none. */
  ImBuf *coverage_ibuf = nullptr;
  /** Colorspace name of #coverage_ibuf's own buffer, or null for scene linear. */
  const char *coverage_colorspace_name = nullptr;
  float coverage_tolerance = 0.0f;
  float coverage_softness = 0.0f;
  /** See #PaintMaterialCompositeMaskItem::coverage_output. */
  float coverage_output = 1.0f;
  Vector<PaintMaterialCompositeIdColor> coverage_colors;
  /** Borrowed from #PaintMaterialCompositeMaskItem::anchor_point_slice, which keeps it alive. */
  ImBuf *anchor_point_slice = nullptr;
};

/** One layer of a stack, as buffers. This is what the evaluator reads. */
struct PaintMaterialCompositeLayer : PaintMaterialCompositeRowParams {
  /** Byte or float RGBA, matching the stack dimensions; null for a constant layer. */
  ImBuf *color_ibuf = nullptr;
  ImBuf *mask_ibuf = nullptr;
  /**
   * Colorspace names of #color_ibuf / #mask_ibuf's own buffers, resolved once at acquisition, or
   * null for scene linear. Taken from the buffer the linear evaluator actually reads, never from
   * the #Image setting, which can disagree with the buffer that was handed over.
   */
  const char *color_colorspace_name = nullptr;
  const char *mask_colorspace_name = nullptr;
  /** The row's mask items, bottom to top. */
  Vector<PaintMaterialCompositeMaskBuffer> mask_items;
  /** The corrections blended onto the row's colour, bottom to top. */
  Vector<PaintMaterialCompositeCorrectionBuffer> content_corrections;
  /** A Gradient generator's payload, copied from the image layer. */
  PaintMaterialCompositeGradient gradient;
  /** A folder's own stack, bottom to top; see #PaintMaterialCompositeImageLayer::children. */
  std::vector<PaintMaterialCompositeLayer> children;
};

/**
 * Whether the fourth component of a row's colour in \a channel is its content alpha, which the
 * corrections laid over the row raise by the over model: every channel that shows a painted map
 * except Normal, whose fourth component is the colour chain's own.
 *
 * The CPU composite tracks it per pixel, and the generated graph tracks the same quantity as a
 * scalar chain beside the colour (`Below/Result Content Alpha <channel>` of every row group, and a
 * Compose Color Alpha at each result). The shader itself ignores the alpha of a colour socket, so
 * nothing reads the chain until the graph is evaluated into a map -- a bake, or a comparison with
 * the CPU -- which is exactly what it is kept for: without it the graph's fourth component is the
 * colour chain's own, which a Mix lerps and an isolating folder's divide drops.
 */
bool BKE_paint_material_channel_tracks_content_alpha(eMaterialPaintChannel channel);

/**
 * The match a map-driven mask coverage operator lays from one sampled texel.
 *
 * #map_coverage names the operator (#MA_PAINT_LAYER_FILTER_UV_ISLAND or _COLOR_ID); \a rgb is the
 * texel in the map's raw data space, the same space the stored colours live in. UV Island reads the
 * red channel as a scalar; Color ID matches the nearest enabled colour within \a tolerance and fades
 * over \a softness past it (max-channel distance), and no enabled colour means no match. An entry
 * with #PaintMaterialCompositeIdColor::own_tolerance uses its own tolerance and a hard edge. The one
 * rule the CPU composite here and the strokes share, so the two cannot disagree about what a mask
 * hides; a missing map is handled by the caller (\a rgb all zero reads as `match = 0` for UV Island
 * but is not relied on for Color ID).
 */
float BKE_paint_material_coverage_match(int16_t map_coverage,
                                        const float rgb[3],
                                        float tolerance,
                                        float softness,
                                        Span<PaintMaterialCompositeIdColor> colors);

/**
 * The scalar factor a row's mask items lay at one texel of the evaluation.
 *
 * The one arithmetic the linear composite and the strokes share: the composite calls it per pixel
 * while it lays a row down, and a stroke calls it to know which texels its own row's mask hides, so
 * the canvas a stroke paints and the canvas the render shows cannot disagree about what a mask
 * covers. The factor starts at one and every item combines with what the items below it left,
 * bottom to top (see #PaintMaterialCompositeMaskItem).
 *
 * \param items: the row's mask items, bottom to top.
 * \param item_samples: item \a i's decoded texel (straight scene-linear RGBA, `pixel_index * 4`
 *                      floats into it), or null when the item has no map.
 * \param coverage_samples: item \a i's decoded coverage-map texel, or null (a map-driven operator
 *                          without a readable map samples as `match = 0`).
 * \param pixel_index: the texel index (row-major) to read both sample arrays at.
 */
float BKE_paint_material_mask_factor_eval(
    Span<PaintMaterialCompositeMaskBuffer> items,
    Span<const float *> item_samples,
    Span<const float *> coverage_samples,
    int64_t pixel_index);

/**
 * Acquire \a image's map for UDIM \a tile_number, decode it to straight scene-linear float RGBA,
 * and resample it to \a ref_width x \a ref_height through \a mapping when the mapping applies.
 *
 * The pixels are the ones #BKE_paint_material_mask_factor_eval reads: the same decode and the same
 * mapping resample the linear composite applies while it acquires a stack, so a mask sampled here
 * is the mask the render hides with. The caller owns the returned buffer and frees it with
 * #IMB_freeImBuf. Null when the map cannot be read.
 */
ImBuf *BKE_paint_material_mask_map_acquire(Image *image,
                                           int tile_number,
                                           const PaintMaterialCompositeMapping *mapping,
                                           int ref_width,
                                           int ref_height);

/** Layers bottom to top: index 0 is composited first and everything else lands on top of it. */
struct PaintMaterialCompositeStack {
  Vector<PaintMaterialCompositeLayer> layers;
  int width = 0;
  int height = 0;
  /**
   * The value the linear evaluation starts from, before the first row:
   * #BKE_paint_layers_channel_bottom_color for a description-driven stack. When #has_bottom_color is
   * false the buffer starts transparent.
   */
  float bottom_color[4] = {};
  bool has_bottom_color = false;
};

struct PaintMaterialCompositeEvalStats {
  double elapsed_seconds = 0.0;
  int layers_evaluated = 0;
  int64_t pixels_processed = 0;
};

/**
 * The part of a \a width x \a height buffer an evaluation has to write into \a r_area: all of it, or
 * \a region clipped to it.
 *
 * \return false when nothing of \a region is inside the buffer, so the composite is already
 * correct and there is nothing to write.
 */
bool BKE_paint_material_composite_area_get(int width,
                                           int height,
                                           const rcti *region,
                                           rcti &r_area);

/**
 * Where a linear evaluation lands: a window of float RGBA texels.
 *
 * \a pixels starts at the first texel of the area being written (the region clipped to the frame,
 * see #BKE_paint_material_composite_area_get; the whole frame without a region) and runs to the end
 * of the storage; \a row_stride is the distance between two rows, in floats. One type covers the
 * three callers: a whole frame, a buffer that holds just the area, and the area of a frame in
 * place (an ImBuf), so the cost of refreshing a small region never depends on the frame's size.
 * The evaluation refuses a target too small for the area, it never writes out of it.
 */
struct PaintMaterialCompositeLinearTarget {
  MutableSpan<float> pixels;
  int64_t row_stride = 0;

  /** A whole `width`-wide frame, rows packed: for an evaluation without a region. */
  static PaintMaterialCompositeLinearTarget frame(MutableSpan<float> storage, int width);
  /** A buffer holding exactly \a area, rows packed: `area_width * area_height * 4` floats. */
  static PaintMaterialCompositeLinearTarget packed(MutableSpan<float> storage, const rcti &area);
  /** The \a area of a `width`-wide frame in place: offset to its corner, the frame's stride. */
  static PaintMaterialCompositeLinearTarget in_frame(MutableSpan<float> storage,
                                                     int width,
                                                     const rcti &area);
};

/**
 * Composite \a stack into \a target as straight scene-linear float RGBA.
 *
 * Every source sample is first decoded out of its own buffer's colorspace (see the layers'
 * `*_colorspace_name`), then the rows are mixed in scene linear the way the shader mixes them; a
 * premultiplied float buffer is straightened before it is read. Byte and float buffers and
 * constant layers are accepted, the evaluation starts from the stack's bottom colour and does no
 * encoding: that is the caller's job when it writes a byte map.
 *
 * \param region: when given, only this rectangle (clipped to the frame) is written and the rest of
 *                the frame is left alone; \a target has to describe exactly that area. A dab
 *                touches a few thousand pixels, so refreshing only those keeps the cost of a
 *                stroke independent of the canvas resolution.
 * \return false when the stack is empty, dimensions disagree, a buffer is unusable or \a target is
 *         too small for the area.
 */
bool BKE_paint_material_composite_eval_linear(const PaintMaterialCompositeStack &stack,
                                              const PaintMaterialCompositeLinearTarget &target,
                                              const rcti *region = nullptr,
                                              PaintMaterialCompositeEvalStats *r_stats = nullptr);

/**
 * Acquire the layer buffers, evaluate through #BKE_paint_material_composite_eval_linear, and
 * release them again: the linear counterpart of #BKE_paint_material_composite_eval_images, for a
 * caller that will encode the scene-linear result itself (an export, a test that compares against
 * the shader's own linear space).
 *
 * \param bottom_color: the value the stack starts from, or null to start transparent.
 */
bool BKE_paint_material_composite_eval_images_linear(
    Span<PaintMaterialCompositeImageLayer> image_layers,
    const PaintMaterialCompositeLinearTarget &target,
    const rcti *region = nullptr,
    PaintMaterialCompositeEvalStats *r_stats = nullptr,
    const float bottom_color[4] = nullptr);

/**
 * Render one row to straight scene-linear colour and the coverage it lays by, both before the
 * row's neighbours are involved: the content a bake of the row stores.
 *
 * \a r_color_rgba holds four floats per texel of \a region (the whole row when null) and carries
 * the row's content alpha in the fourth when the channel tracks one (always for Normal), one
 * otherwise. \a r_coverage_gray is `opacity x factor` per texel. The row is composited as an
 * isolated layer over nothing, a folder through its children, with its corrections and mask.
 *
 * \return false when the row has no content to render in \a channel or the buffers cannot be read.
 */
bool BKE_paint_material_composite_eval_row_content(const PaintMaterialCompositeImageLayer &row,
                                                   float *r_color_rgba,
                                                   float *r_coverage_gray,
                                                   const rcti *region,
                                                   int channel);

/**
 * Whether scene-linear values must be converted to or from \a colorspace: it is a known colour
 * space that is neither a data space nor scene linear. False for null.
 */
bool BKE_paint_material_composite_colorspace_is_converted(const ColorSpace *colorspace);

/**
 * Write the scene-linear \a linear result of an evaluation as bytes into \a r_dst.
 *
 * \a linear holds exactly \a area, rows packed (see #PaintMaterialCompositeLinearTarget::packed);
 * it is encoded in place into \a r_dst's own colorspace first, so a preview and a saved map are the
 * same pixels. Only \a area of \a r_dst is written. \a linear is clobbered.
 */
void BKE_paint_material_composite_linear_to_byte(MutableSpan<float> linear,
                                                 const rcti &area,
                                                 ImBuf &r_dst);

/**
 * Acquire every layer's buffer, evaluate, and release the locks again.
 *
 * The evaluation is the linear one (#BKE_paint_material_composite_eval_linear): every source is
 * decoded out of its own colorspace, float buffers and constant layers are accepted, and the result
 * is encoded into \a composite_ibuf's colorspace with #BKE_paint_material_composite_linear_to_byte.
 * The scratch is the size of the area written, never of the frame.
 *
 * The layer images are acquired for the duration of the evaluation only. Nothing is written back
 * to them, so a caller holding no lock of its own is safe here.
 *
 * \param bottom_color: the value the stack starts from, or null to start transparent.
 */
bool BKE_paint_material_composite_eval_images(
    Span<PaintMaterialCompositeImageLayer> image_layers,
    ImBuf *composite_ibuf,
    const rcti *region = nullptr,
    PaintMaterialCompositeEvalStats *r_stats = nullptr,
    const float bottom_color[4] = nullptr);

/**
 * Dimensions of the composite, taken from the bottom-most enabled layer.
 *
 * Layers of differing sizes are not composited: a stack whose layers disagree is rejected by the
 * evaluator rather than resampled, since a resample would silently change the pixels a stroke is
 * about to be compared against.
 *
 * \return false when no enabled layer has a readable buffer.
 */
bool BKE_paint_material_composite_stack_dimensions(
    Span<PaintMaterialCompositeImageLayer> image_layers, int &r_width, int &r_height);

/**
 * The layer stack \a channel of \a ma is built from, or false when it is not a layer stack.
 *
 * Walks the same graph, through the same reroutes, muted nodes and group instances, as
 * #BKE_paint_material_source_resolve: the chain of Mix nodes feeding the channel's Principled
 * input, each mixing an Image Texture over what is below it. A single Image Texture is a stack of
 * one. Anything the walk does not recognize -- an unsupported blend mode, a factor that is neither
 * a value nor an image, a node that is not a Mix -- makes the whole channel non-compositable, and
 * the caller falls back to the bake.
 *
 * Unlike the bake, this needs no group path: it only reads data-block pointers out of the nodes it
 * finds, and never has to route a socket back out to the root tree.
 *
 * A channel with no graph to walk -- Ambient Occlusion has no Principled input at all, and
 * #PAINT_LAYER_MAP_MASK is not a channel -- is instead assembled from the layers themselves: the
 * order, blending and masking come from the channel that does have a chain, and each layer's map
 * for \a channel is found by #Image.paint_layer_id and #Image.paint_layer_channel. That is the
 * whole reason those two fields exist; a user who bakes an AO map per layer has no node link that
 * could express the same thing.
 *
 * Cheap enough for a redraw, like the resolver: it allocates only \a r_layers and touches no
 * pixels -- but not free, and the fallback is the expensive half. Deriving a channel from the
 * layers costs a graph walk per channel until one resolves, plus a pass over the file's images;
 * a caller in a loop over materials should say so with \a allow_layer_map_fallback.
 *
 * \param allow_layer_map_fallback: when false, only a channel that is really wired into the graph
 *                                  resolves, and one assembled from #Image.paint_layer_id tags
 *                                  reports false instead. For a caller that is asking *which*
 *                                  material a stack belongs to rather than what it is made of:
 *                                  the wired channel already names the material, and the tagged
 *                                  maps hang off the layers it found.
 */
bool BKE_paint_material_composite_stack_from_material(
    const Main &bmain,
    const Material &ma,
    int channel,
    Vector<PaintMaterialCompositeImageLayer> &r_layers,
    bool allow_layer_map_fallback = true);

/**
 * The shader node group that lays one tangent-space normal map over another, created on demand.
 *
 * Exists so that a normal stack can be *authored* in a way the compositor is able to reproduce.
 * A plain Mix node interpolates encoded normals, which is a legitimate "replace" but flattens
 * relief when it is meant to accumulate; the correct operation is #CompositeBlend::NormalCombine,
 * and nothing in a stock node graph expresses it. Rather than guess that some Mix node "really
 * means" a normal combine -- which would make the composite disagree with the render -- the
 * engine offers this group and recognizes it again by an ID property, so the two agree by
 * construction whichever of them a viewer is looking at.
 *
 * Its inputs are `A` (below), `B` (the layer) and `Factor`; its output is `Result`. The group is
 * shared: a second call returns the one already in \a bmain.
 */
bNodeTree *BKE_paint_material_normal_combine_group_ensure(Main &bmain);

/** Whether \a node is an instance of #BKE_paint_material_normal_combine_group_ensure's group. */
bool BKE_paint_material_is_normal_combine_group(const bNode &node);

/**
 * The shader node group that blends two colours by a runtime blend mode, created on demand.
 *
 * Exists so that changing a layer's blend mode is a value edit rather than a different node type:
 * the mode is an integer input, and every mode is built once inside the group. Its inputs are
 * `Factor` (the row's coverage), `A` (below), `B` (the row) and `Mode` (the `MA_RAMP_*` code); its
 * output is `Result`. The group is shared: a second call returns the one already in \a bmain.
 */
bNodeTree *BKE_paint_material_blend_group_ensure(Main &bmain);

/** Whether \a node is an instance of #BKE_paint_material_blend_group_ensure's group. */
bool BKE_paint_material_is_blend_group(const bNode &node);

/**
 * The shared group that turns a 3D coordinate and a gradient's line into its ramp position:
 * `Factor(Coordinate, Start, End, Midpoint, Type, Repeat)`. Built once per file and shared by every
 * Gradient row, so the Hermite midpoint and the repeat folding exist in one place; the generated
 * graph and the CPU composite use the same formula. The socket's identifier stays `Socket_0` for
 * compatibility with the groups already in files; UV rows pass `(u, v, 0)`.
 */
bNodeTree *BKE_paint_material_gradient_group_ensure(Main &bmain);

/** Whether \a node is an instance of #BKE_paint_material_gradient_group_ensure's group. */
bool BKE_paint_material_is_gradient_group(const bNode &node);

/**
 * The passes a layered material offers, in the order they are listed to the user.
 *
 * Values are #eMaterialPaintChannel plus #PAINT_LAYER_MAP_MASK. A fixed list rather than one
 * derived from the material: a pass is something the material *can* have, and the user picks it
 * before the maps behind it necessarily exist. Whether a given pass resolves to anything is
 * answered by #BKE_paint_material_composite_stack_from_material.
 */
Span<int> BKE_paint_material_composite_passes();

/**
 * The passes the Image Editor offers as *display* modes, in the order they are listed.
 *
 * #BKE_paint_material_composite_passes plus #PAINT_LAYER_PASS_COMBINED at the front. A second list
 * rather than a widened one because every consumer of the role list treats its values as roles --
 * indexing a per-channel array, looking the value up in the channel descriptor table, resolving a
 * layer map -- and none of that is meaningful for a display mode. Only code that populates a
 * selector or cycles the shown pass should use this one.
 *
 * The two lists are kept in sync by hand and a test asserts that they agree.
 */
Span<int> BKE_paint_material_display_passes();

/**
 * The maps of the paint layer \a layer_id, indexed by role (channel, or #PAINT_LAYER_MAP_MASK).
 *
 * A channel wired into \a ma is answered from its stack: the layer is already identified there, so
 * its map for that channel needs no #Image.paint_layer_channel tag and works for any material a
 * stroke can paint. The tag answers the rest -- Ambient Occlusion and the mask, which no node link
 * mentions.
 *
 * Entries the layer has no map for stay null, which is what lets the canvas list show a channel
 * the active layer does not author yet without pretending it is selectable.
 */
void BKE_paint_material_layer_maps_get(const Main &bmain,
                                       const Material &ma,
                                       const bUUID &layer_id,
                                       MutableSpan<Image *> r_maps);

/**
 * Hash of everything about \a image_layers that changes the composited pixels except the pixels
 * themselves -- which images, in which order, with which blend, opacity and mask.
 *
 * Image *contents* are deliberately not in here; there is no content version to hash. An edit to a
 * layer's pixels is found instead by #BKE_paint_material_composite_cache_ensure, which polls each
 * source image's partial-update log.
 */
uint64_t BKE_paint_material_composite_stack_hash(
    Span<PaintMaterialCompositeImageLayer> image_layers);

/**
 * The composite of \a channel of \a ma, recomputing whatever part of it is out of date.
 *
 * Synchronous: unlike a bake this is a few milliseconds, and a whole-buffer rebuild only happens
 * when the stack itself changed. A stroke's own edits are refreshed as rectangles.
 *
 * Pixel changes are detected, not reported: the cache subscribes to each source image's
 * partial-update log and polls it here. A caller that edits a layer's pixels therefore has nothing
 * to tell this cache, and -- the reason it works that way -- a caller that tags the image ID for
 * unrelated reasons can no longer turn a known rectangle into "the whole canvas".
 * #BKE_paint_material_composite_cache_invalidate remains for the other kind of change: the stack's
 * own description.
 *
 * The returned buffer is owned by the cache and stays valid until the next call for a different
 * material or a #BKE_paint_material_composite_cache_free_all. Do not free it.
 *
 * \param r_revision: incremented every time the pixels are recomputed, so a consumer that uploads
 *                    them somewhere -- the image editor's GPU texture -- can tell whether its copy
 *                    is still current without comparing buffers. Never zero for a live composite.
 * \param r_changed_region: the rectangle recomputed by this call, the whole buffer when it was
 *                          rebuilt entirely, and empty when nothing was. A consumer that derives
 *                          its own pixels from this composite -- the Combined preview -- needs it
 *                          to keep its own refresh proportional to the edit.
 * \param channel: a role rather than an #eMaterialPaintChannel: #PAINT_LAYER_MAP_MASK is cached
 *                 like any other pass and is not a channel, which is also why
 *                 #BKE_paint_material_composite_stack_from_material takes an `int`.
 * \param full_layers: when given, \a image_layers is a description made *without* the pixels of its
 *                     Anchor Point slices (see `describe_only` of
 *                     #BKE_paint_layers_composite_image_layers): enough for the hash, the size
 *                     and the subscriptions, which is all an unchanged composite needs. The
 *                     function is called, and only then, when the composite has to be evaluated,
 *                     and fills the same stack with the slices rendered. Rendering them on every
 *                     call would cost a canvas-sized evaluation per frame for a composite that is
 *                     served from the cache.
 * \return null when the stack cannot be composited, which is the caller's cue to fall back.
 */
ImBuf *BKE_paint_material_composite_cache_ensure(
    const Material &ma,
    int channel,
    Span<PaintMaterialCompositeImageLayer> image_layers,
    uint64_t stack_hash,
    uint64_t *r_revision = nullptr,
    PaintMaterialCompositeEvalStats *r_stats = nullptr,
    rcti *r_changed_region = nullptr,
    FunctionRef<bool(Vector<PaintMaterialCompositeImageLayer> &)> full_layers = {});

/**
 * Mark the whole composite of \a ma out of date, or of every material when \a ma is null.
 *
 * Marks rather than drops, for the same reason the bake cache does: a caller asking for the buffer
 * in between must get the previous pixels rather than nothing.
 *
 * Called wherever the material itself changed -- its node tree was edited -- and, with a null
 * material, after an undo step that may have moved pixels under the cache without any of the
 * per-image tags being able to report it.
 */
void BKE_paint_material_composite_cache_invalidate(const Material *ma);

/**
 * Drop every cached composite of \a ma, freeing its buffers.
 *
 * Called when the material is freed. The cache is keyed on #ID.session_uid, so an entry left
 * behind would never be looked up again and would hold its buffer until the budget happened to
 * evict it; this is what a cache stored on the material itself would have got for free, and the
 * price of keeping it in one place instead -- see the note on #CompositeCache.
 */
void BKE_paint_material_composite_cache_free_material(const Material &ma);

/** Drop every cached composite. For teardown and for file load. */
void BKE_paint_material_composite_cache_free_all();

/** Whether a composite of \a channel of \a ma is currently cached. Exists for tests. */
bool BKE_paint_material_composite_cache_contains(const Material &ma, int channel);

}  // namespace blender
