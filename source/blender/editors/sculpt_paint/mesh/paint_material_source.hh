/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 */

#include <array>
#include <cstdint>

#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"

#include "DNA_scene_types.h"
#include "DNA_texture_types.h"

struct ARegion;
struct Brush;
struct Paint;
struct BrushMaterialPaint;
struct ImagePool;
struct ImBuf;
struct Object;
struct PaintModeSettings;
struct SculptSession;

namespace blender::ocio {
class ColorSpace;
}

namespace blender::bke {
struct PaintRuntime;
}

namespace blender::ed::sculpt_paint::material {

struct TexelSampleContext;

/** Which #MTex::brush_map_mode a #DirectSampleLayout was built for. */
enum class DirectSampleKind : int8_t {
  /** The source cannot be sampled directly; callers must fall back to the texture engine. */
  Disabled = 0,
  Area,
  View,
  Tiled,
  Random,
  Stencil,
};

/**
 * Everything needed to sample one image source without #RE_texture_evaluate, resolved once per
 * stroke (or per dab, where the mapping moves with the cursor) instead of per texel.
 *
 * Built by #make_direct_sample_layout, consumed by #sample_direct_layout. The Sculpt path and the
 * Image Editor 2D path share both, so the two cannot drift apart on where a brush texture lands.
 * #Disabled is the "not eligible" state; every consumer must keep a texture-engine fallback.
 */
struct DirectSampleLayout {
  DirectSampleKind kind = DirectSampleKind::Disabled;
  const float4x4 *local_mat = nullptr;
  float size_x = 1.0f;
  float size_y = 1.0f;
  float ofs_x = 0.0f;
  float ofs_y = 0.0f;
  float rotation = 0.0f;
  float invradius = 1.0f;
  float tex_mouse_x = 0.0f;
  float tex_mouse_y = 0.0f;
  float stencil_pos_x = 0.0f;
  float stencil_pos_y = 0.0f;
  float stencil_dim_x = 1.0f;
  float stencil_dim_y = 1.0f;
  float sample_bias = 0.0f;
  bool rotate = false;
  /** Pinned source image, filled once per chunk when #kind is not Disabled. */
  const float *float_pixels = nullptr;
  const uchar *byte_pixels = nullptr;
  int ibuf_x = 0;
  int ibuf_y = 0;
  bool wrap = false;
  bool clip = false;
  float eval_size_x = 1.0f;
  float eval_size_y = 1.0f;
  float eval_ofs_x = 0.0f;
  float eval_ofs_y = 0.0f;
};

/**
 * Which #MTex each material paint channel samples from, and whether it is usable.
 *
 * Resolved once per stroke and read-only afterwards. Owns the #ImagePool used to probe image
 * sources, because that probe is part of deciding usability: keeping it here is what makes the
 * answer stable for the whole stroke, so the UI warning can never disagree with what actually gets
 * painted.
 *
 * Has no dependency on #SculptSession, so both the Sculpt path (through #ChannelSourceSampler) and
 * the Image Editor 2D path can use it.
 */
class ChannelSourceSet {
 public:
  struct ChannelSource {
    /** #BKE_paint_material_channel_effective_mtex for this channel: its own #Tex combined with
     * the mapping shared by every channel. Owned here (not a pointer into the channel's DNA
     * #source_mtex) since it is a value the sampler builds, not the raw per-channel data. */
    MTex effective_mtex{};
    const MTex *mtex = nullptr;
    /** False when the slot has a texture but it cannot be sampled (no image, or load failure). */
    bool usable = false;
    /**
     * True when the source is a byte image whose pixels are not already in scene-linear space
     * (e.g. an sRGB Base Color texture). #BKE_brush_sample_tex_3d only auto-decodes the brush's
     * single legacy `Brush.mtex` texture, not per-channel sources, so this sampler has to redo
     * that decode itself or every enabled channel with a byte source ends up double-encoded
     * (too bright) once its sampled color is written into the (correctly sRGB-tagged) target.
     */
    bool do_linear_conversion = false;
    const ocio::ColorSpace *colorspace = nullptr;
    /**
     * True for the Normal channel when its source is authored in the DirectX tangent-space
     * convention (green channel flipped relative to OpenGL). Resolved once here from
     * #BrushMaterialPaintChannel.normal_space so #color does not re-read the DNA flag on every
     * texel/vertex sample of a stroke.
     */
    bool flip_green_channel = false;
    /**
     * Pinned #ImBuf for a #TEX_IMAGE source, acquired from the stroke pool at construct.
     * Null when the source is not a loadable image. Released in #ChannelSourceSet's destructor.
     * Read-only for the rest of the stroke.
     */
    ImBuf *ibuf = nullptr;
    /**
     * True when this source is a plain image texture that #sample_image_direct can sample
     * without #RE_texture_evaluate (no nodes, no UDIM, default crop/filter/color).
     */
    bool image_direct_sample = false;
  };

 private:
  std::array<ChannelSource, PAINT_MATERIAL_CHANNEL_NUM> sources_;
  ImagePool *pool_ = nullptr;
  bool active_ = false;

 public:
  ChannelSourceSet(const BrushMaterialPaint &brush_paint,
                   const PaintModeSettings &settings,
                   int visible_material_channels);
  ~ChannelSourceSet();

  bool is_active() const;
  bool channel_source_failed(eMaterialPaintChannel channel) const;
  const ChannelSource &source(int channel) const;
  ImagePool *pool() const;

  /**
   * Sample \a source at the same (\a tex_x, \a tex_y) that #paint_get_tex_pixel would receive
   * after Area Plane #local_mat * size/ofs. Matches #RE_texture_evaluate placement and
   * #MTEX_FLAT mapping, then bilinear-samples the pinned #ImBuf.
   *
   * \param r_value: Optional. Intensity is only computed when non-null (scalar channels).
   * \return false when this source is not eligible; the caller must use the texture engine.
   */
  bool sample_image_direct(const ChannelSource &source,
                           float tex_x,
                           float tex_y,
                           float *r_value,
                           float4 &r_rgba) const;
};

/**
 * Per-stroke source texture sampling for material paint channels.
 *
 * Owns the image pool shared by every channel, so a stroke acquires each source ImBuf once
 * instead of once per dab per channel. Callers never branch on whether a channel has a usable
 * source: the sampler falls back to the channel's own value.
 *
 * Only channels that are enabled at construction time are registered, so a disabled channel can
 * never make the sampler active or cause an image to be acquired.
 */
class ChannelSourceSampler {
  const SculptSession &ss_;
  const Brush &brush_;
  const PaintModeSettings &settings_;
  const BrushMaterialPaint &brush_paint_;
  ChannelSourceSet sources_;
  /** Per-channel #MTEX_MAP_MODE_AREA local matrix; only valid for channels whose source uses
   * Area Plane mapping (see #update_area_local_mats). Unlike #sources_, recomputed every dab. */
  std::array<float4x4, PAINT_MATERIAL_CHANNEL_NUM> area_local_mats_;

 public:
  ChannelSourceSampler(const SculptSession &ss,
                       const Brush &brush,
                       const BrushMaterialPaint &brush_paint,
                       const PaintModeSettings &settings,
                       int visible_material_channels);
  ~ChannelSourceSampler() = default;

  /** Whether any enabled channel has a usable source, so callers can skip the sampling path. */
  bool is_active() const;

  /**
   * Recompute #area_local_mats_ for every channel whose source uses #MTEX_MAP_MODE_AREA. Must be
   * called once per dab (not per stroke), from the same place that rebuilds
   * #StrokeCache.brush_local_mat, since both depend on the current dab's location and motion
   * direction. A no-op when #is_active is false.
   */
  void update_area_local_mats(const Object &ob);

  /**
   * True when \a channel has a source assigned that cannot be sampled. The UI uses this to warn
   * instead of silently painting the slider value.
   */
  bool channel_source_failed(eMaterialPaintChannel channel) const;

  /** True when \a channel has a source that can be sampled this stroke. */
  bool has_usable_source(eMaterialPaintChannel channel) const;

  /**
   * Target scalar for \a channel at \a position (object space), clamped to the channel range.
   * Falls back to the channel's slider value when there is no usable source.
   */
  float scalar(eMaterialPaintChannel channel, const float3 &position, int thread) const;

  /**
   * Same as above, but reusing a #TexelSampleContext already built for this position instead of
   * recomputing it. Callers sampling more than one channel at the same texel/vertex (the normal
   * case for multi-channel PBR paint) should build the context once per position and use this
   * overload for every channel, instead of calling the \a position overload once per channel.
   */
  float scalar(eMaterialPaintChannel channel, const TexelSampleContext &ctx, int thread) const;

  /**
   * Target RGB for \a channel at \a position (object space): the sampled color for Base Color,
   * and the unpacked unit tangent normal for Normal.
   *
   * \param decode_linear: When false, a Base Color sample whose source needs a colorspace
   * decode (see #needs_linear_conversion) is returned still encoded. Callers that gather many
   * samples into a contiguous buffer should do this and decode the whole buffer in one
   * #decode_linear_batch call afterward — #IMB_colormanagement_colorspace_to_scene_linear is
   * documented as "much higher performance" than converting pixels one by one, and at brush
   * scale (hundreds of thousands of samples per dab) that per-pixel call dominates the cost.
   * Ignored for Normal, which is tangent data and is never colorspace-decoded either way.
   */
  float3 color(eMaterialPaintChannel channel,
               const float3 &position,
               int thread,
               bool decode_linear = true) const;

  /** Same as above, but reusing a #TexelSampleContext already built for this position instead of
   * recomputing it; see the \a ctx overload of #scalar. */
  float3 color(eMaterialPaintChannel channel,
               const TexelSampleContext &ctx,
               int thread,
               bool decode_linear = true) const;

  /**
   * Target scalar for \a channel at explicit 2D coordinates in the CALLER's own
   * parametrization, instead of through the brush's view / area mapping.
   *
   * A Curve Patch ribbon has a frame of its own -- `u` across the ribbon, `v` along it -- and
   * its own zone textures have always been sampled in it. A channel source sampled through the
   * brush mapping instead would stay put while the curve turns, which is not what "paint along
   * this curve" means. The channel's Size / Offset still apply, so its mapping controls keep
   * working; what is replaced is only WHERE the coordinates come from.
   *
   * Falls back to the channel's slider value when there is no usable source, exactly as the
   * position-based overloads do.
   */
  float scalar_at_uv(eMaterialPaintChannel channel, const float2 &uv, int thread) const;

  /** #color counterpart of #scalar_at_uv; see there for why an explicit frame exists. */
  float3 color_at_uv(eMaterialPaintChannel channel,
                     const float2 &uv,
                     int thread,
                     bool decode_linear = true) const;

  /**
   * Single-sample counterpart of #gather_tangent_normals_packed: sample the Normal channel, remap
   * the unpacked decal normal into the destination surface's tangent basis and pack it to 0..1
   * RGB.
   *
   * \param t_decal, b_decal: World-space directions in which the decal's own x and y grow. For a
   * brush-mapped sample they are the screen right/up projected onto the surface, which is what
   * the batched overload derives from `t_screen`/`b_screen`; a caller with a frame of its own (a
   * Curve Patch ribbon) passes that frame's axes instead, and the decal then turns with it.
   * \param n_m, t_m, b_m: The destination surface's own tangent basis, as built per UV primitive.
   */
  float3 tangent_normal_packed(eMaterialPaintChannel channel,
                               const TexelSampleContext &ctx,
                               int thread,
                               const float3 &t_decal,
                               const float3 &b_decal,
                               const float3 &n_m,
                               const float3 &t_m,
                               const float3 &b_m) const;

  /** #tangent_normal_packed sampled at explicit 2D coordinates; see #scalar_at_uv for why an
   * explicit frame exists. */
  float3 tangent_normal_packed_at_uv(eMaterialPaintChannel channel,
                                     const float2 &uv,
                                     int thread,
                                     const float3 &t_decal,
                                     const float3 &b_decal,
                                     const float3 &n_m,
                                     const float3 &t_m,
                                     const float3 &b_m) const;

  /**
   * Same meaning as calling #color once per element of \a contexts. Zero-factor slots are
   * written as zero so callers can skip them the same way as the per-pixel path.
   * Mapping (Area/View/…) is resolved once for the chunk instead of once per texel.
   */
  void gather_colors(eMaterialPaintChannel channel,
                     Span<TexelSampleContext> contexts,
                     Span<float> factors,
                     int thread,
                     bool decode_linear,
                     MutableSpan<float3> r_colors) const;

  /**
   * Same meaning as calling #scalar once per element of \a contexts. Zero-factor slots are
   * written as zero.
   */
  void gather_scalars(eMaterialPaintChannel channel,
                      Span<TexelSampleContext> contexts,
                      Span<float> factors,
                      int thread,
                      MutableSpan<float> r_values) const;

  /**
   * Sample the Normal channel and remap each unpacked decal normal into the destination
   * surface tangent basis, then pack to 0..1 RGB. Same math as gathering #color and applying the
   * row TBN in the caller, in one pass so bilinear stays in this translation unit.
   */
  void gather_tangent_normals_packed(eMaterialPaintChannel channel,
                                     Span<TexelSampleContext> contexts,
                                     Span<float> factors,
                                     int thread,
                                     const float3 &t_screen,
                                     const float3 &b_screen,
                                     const float3 &n_m,
                                     const float3 &t_m,
                                     const float3 &b_m,
                                     MutableSpan<float3> r_packed) const;

  /** Whether a Base Color sample from \a channel needs #decode_linear_batch applied afterward
   * when it was sampled with `decode_linear = false`. Always false for non-color channels. */
  bool needs_linear_conversion(eMaterialPaintChannel channel) const;

  /** Colorspace to decode with, valid whenever #needs_linear_conversion returns true. */
  const ocio::ColorSpace *colorspace(eMaterialPaintChannel channel) const;

  /** Batched counterpart to the per-pixel decode #color skips when `decode_linear = false`. */
  static void decode_linear_batch(MutableSpan<float3> colors, const ocio::ColorSpace *colorspace);

 private:
  using ChannelSource = ChannelSourceSet::ChannelSource;

  /** The stroke's #Paint. The only thing the channel-value fallbacks need from the session, and
   * factoring it out here is what lets the UV path below be shared with the Image Editor, which
   * has a #Paint but no #SculptSession. */
  const Paint &paint() const;

  /** Null unless \a source uses Area Plane mapping, in which case it is the channel's own
   * #area_local_mats_ entry rather than the brush's shared local matrix. */
  const float4x4 *area_local_mat_for(int channel, const ChannelSource &source) const;
};

/**
 * Channel sampling in a frame the CALLER supplies, with no dependency on a sculpt session.
 *
 * #ChannelSourceSampler is tied to a #SculptSession because its brush-mapped sampling needs the
 * stroke's view and symmetry state. Sampling at explicit 2D coordinates needs none of that -- the
 * caller has already decided where the texel sits -- so the Image Editor's flat canvas, which has
 * no session at all, can use exactly the same code as the Sculpt viewport. The Sculpt sampler's
 * own `*_at_uv` methods delegate here, so the two cannot drift.
 *
 * Holds references only; it is built per use, not stored.
 */
class ChannelUvSampler {
  const ChannelSourceSet &sources_;
  const BrushMaterialPaint &brush_paint_;
  const PaintModeSettings &settings_;
  const Paint &paint_;
  const Brush &brush_;

 public:
  ChannelUvSampler(const ChannelSourceSet &sources,
                   const BrushMaterialPaint &brush_paint,
                   const PaintModeSettings &settings,
                   const Paint &paint,
                   const Brush &brush)
      : sources_(sources),
        brush_paint_(brush_paint),
        settings_(settings),
        paint_(paint),
        brush_(brush)
  {
  }

  /** True when \a channel has a source that can be sampled. */
  bool has_usable_source(eMaterialPaintChannel channel) const;

  /** See #ChannelSourceSampler::scalar_at_uv. */
  float scalar_at_uv(eMaterialPaintChannel channel, const float2 &uv, int thread) const;

  /** See #ChannelSourceSampler::color_at_uv. */
  float3 color_at_uv(eMaterialPaintChannel channel,
                     const float2 &uv,
                     int thread,
                     bool decode_linear = true) const;

  /** See #ChannelSourceSampler::tangent_normal_packed_at_uv. */
  float3 tangent_normal_packed_at_uv(eMaterialPaintChannel channel,
                                     const float2 &uv,
                                     int thread,
                                     const float3 &t_decal,
                                     const float3 &b_decal,
                                     const float3 &n_m,
                                     const float3 &t_m,
                                     const float3 &b_m) const;
};

/**
 * Resolve how \a source maps onto the brush for the current dab.
 *
 * \a paint_runtime supplies the view-relative mapping state (#tex_mouse, #pixel_radius,
 * #brush_rotation) that #BKE_brush_sample_tex_3d reads, so a layout is only valid for the dab it
 * was built from. \a area_local_mat is required by #MTEX_MAP_MODE_AREA and ignored otherwise.
 *
 * \param mtex: The mapping to plan for. Normally #ChannelSource.mtex, but the Image Editor 2D path
 * passes its own copy with #MTex.brush_map_mode rewritten, so the layout follows the mode that
 * will actually be sampled rather than the one stored on the brush.
 * \return a layout whose #DirectSampleLayout.kind is #DirectSampleKind::Disabled when \a source
 * is not eligible for direct sampling; the caller must then use the texture engine.
 */
DirectSampleLayout make_direct_sample_layout(const ChannelSourceSet::ChannelSource &source,
                                             const MTex &mtex,
                                             const bke::PaintRuntime *paint_runtime,
                                             const Brush &brush,
                                             const float4x4 *area_local_mat);

/**
 * Sample \a layout at a point in the space its #DirectSampleLayout.kind expects: region
 * coordinates for View / Tiled / Random / Stencil, and object space for Area (which also needs
 * #DirectSampleLayout.local_mat).
 *
 * Applies #Brush.texture_sample_bias the same way #BKE_brush_sample_tex_3d and #sculpt_apply_texture
 * do for the respective mapping modes. Does not apply any colorspace decode: the caller owns that,
 * so color channels can batch it.
 *
 * \param r_value: Optional. Intensity (image luminance) is only computed when non-null.
 * \return false when \a layout is #DirectSampleKind::Disabled, i.e. nothing was written.
 */
bool sample_direct_layout(const DirectSampleLayout &layout,
                          const float3 &symm_point,
                          const float2 &view_point_2d,
                          float *r_value,
                          float4 &r_rgba);

/**
 * Build the two bases a Normal-channel write needs for one UV primitive.
 *
 * A normal map sample is a direction in the frame the DECAL is laid out in, while the destination
 * map stores directions in the frame the SURFACE's UV parametrization defines. Both have to be
 * known before one can be expressed in the other, and both are constant across a UV primitive --
 * so this is called once per pixel row, not once per texel.
 *
 * \param tri_tangent, tri_bitangent_sign: The primitive's UV tangent and handedness, from
 * #UVPrimitives.
 * \param tri_positions: The primitive's three object-space corners, in winding order.
 * \param view_right: Fallback in-plane direction for a primitive whose screen projection is
 * degenerate (edge-on), where the screen basis cannot be derived.
 * \param region, projection_mat: The view the decal is laid out in for a brush-mapped source.
 * \a region may be null, in which case the \a view_right fallback is used throughout; a caller
 * whose decal has a frame of its own does not need the screen basis at all.
 * \param r_t_screen, r_b_screen: Screen right/up projected onto the surface -- the decal frame a
 * brush-mapped normal source is authored in.
 * \param r_n_m, r_t_m, r_b_m: The surface's own tangent basis, the frame the map is written in.
 */
void build_normal_write_basis(const float3 &tri_tangent,
                              float tri_bitangent_sign,
                              Span<float3> tri_positions,
                              const float3 &view_right,
                              const ARegion *region,
                              const float4x4 &projection_mat,
                              float3 &r_t_screen,
                              float3 &r_b_screen,
                              float3 &r_n_m,
                              float3 &r_t_m,
                              float3 &r_b_m);

}  // namespace blender::ed::sculpt_paint::material
