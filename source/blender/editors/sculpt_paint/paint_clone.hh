/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 * \brief Clone Stamp dab kernel: surface frames, CPU sampler, stroke targets.
 *
 * Scope:
 * - 3D Viewport (sculpt + projection paint) and Image Editor.
 * - Absolute and Relative source tracking.
 * - CPU ImBuf sampling; no GPU path.
 * - No dyntopo, no multi-tile UDIM.
 * - Undo is always owned by the outer stroke; this module never opens one.
 * - True-clone offset: source_uv = source_center + (dest_uv - dab_center_uv).
 */

#pragma once

#include <array>
#include <optional>
#include <string>

#include "BLI_array.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_string_ref.hh"
#include "BLI_uuid.h"
#include "BLI_vector.hh"
#include "BLI_virtual_array.hh"

#include "BKE_paint_types.hh"

#include "DNA_scene_types.h"

/* NOTE: all Blender DNA/data types live in namespace blender. */
namespace blender {
struct ARegion;
struct Brush;
struct Depsgraph;
struct Image;
struct ImBuf;
struct ImageUser;
struct Main;
struct Material;
struct Mesh;
struct Object;
struct RegionView3D;
struct ViewContext;
struct bContext;
struct wmEvent;
struct wmOperator;
struct wmOperatorType;
}  // namespace blender

namespace blender::ed::sculpt_paint::clone {

/**
 * Everything about one triangle a clone dab needs beyond its UV: the screen-aligned in-surface
 * basis, the surface's own tangent basis, and how a UV offset maps into the screen basis.
 *
 * The screen basis is what lets the stamp be oriented rather than locked to the UV layout. Built
 * the same way the material paint builds its Normal-channel write basis -- see
 * #blender::ed::sculpt_paint::material::build_normal_write_basis.
 */
struct CloneSurfaceFrame {
  /** Screen right/up flattened onto the surface, object space. */
  float3 t_screen = float3(1.0f, 0.0f, 0.0f);
  float3 b_screen = float3(0.0f, 1.0f, 0.0f);
  /** The surface's own tangent basis, the frame a Normal map is stored in. */
  float3 n_m = float3(0.0f, 0.0f, 1.0f);
  float3 t_m = float3(1.0f, 0.0f, 0.0f);
  float3 b_m = float3(0.0f, 1.0f, 0.0f);
  /** UV offset -> offset in (#t_screen, #b_screen), i.e. `[dP/du, dP/dv]` in the screen basis. */
  float2x2 uv_to_screen = float2x2::identity();
  bool valid = false;
};

/* -------------------------------------------------------------------- */
/** \name Stroke targets (built once per stroke).
 * \{ */

/**
 * One writable (layer, channel) destination, holding its acquired #ImBuf for the stroke.
 *
 * Move-only and self-releasing: the acquire/release pair is long-lived (a whole stroke) and used
 * from several stroke owners, so the release cannot be left to any one of their exit paths --
 * cancel included. Moving from a target leaves it inert, so the moved-from copy releases nothing.
 */
struct CloneChannelTarget {
  eMaterialPaintChannel channel = eMaterialPaintChannel(0);
  Image *image = nullptr;
  ImageUser iuser = {};
  ImBuf *ibuf = nullptr;
  void *lock = nullptr;
  /**
   * Pristine copy of #ibuf taken when the stroke starts, and the only buffer the clone reads.
   *
   * Source and destination are the same image, so sampling #ibuf would feed already-painted
   * pixels back into the next dab: with a short offset the stamp copies its own output and the
   * stroke smears instead of cloning. Owned by the target and freed with it.
   */
  ImBuf *source_ibuf = nullptr;
  /** The channel's pixels are non-color data, so partial blends must skip sRGB decoding. */
  bool is_data = false;

  CloneChannelTarget() = default;
  ~CloneChannelTarget();
  CloneChannelTarget(CloneChannelTarget &&other) noexcept;
  CloneChannelTarget &operator=(CloneChannelTarget &&other) noexcept;
  CloneChannelTarget(const CloneChannelTarget &) = delete;
  CloneChannelTarget &operator=(const CloneChannelTarget &) = delete;

 private:
  void release();
};

struct CloneLayerTarget {
  bUUID layer_id = {};
  std::array<std::optional<CloneChannelTarget>, PAINT_MATERIAL_CHANNEL_NUM> channels;
};

struct CloneStrokeTargets {
  Vector<CloneLayerTarget> layers;

  /* Releasing is #CloneChannelTarget's own job, so the defaults are correct here. */
  CloneStrokeTargets() = default;
  ~CloneStrokeTargets() = default;
  CloneStrokeTargets(CloneStrokeTargets &&) noexcept = default;
  CloneStrokeTargets &operator=(CloneStrokeTargets &&) noexcept = default;
  CloneStrokeTargets(const CloneStrokeTargets &) = delete;
  CloneStrokeTargets &operator=(const CloneStrokeTargets &) = delete;

  bool is_valid() const
  {
    return !layers.is_empty();
  }
};

/**
 * Build frozen target map once at stroke start.
 * - Enumerates layer IDs from material images' paint_layer_id tags.
 * - Resolves each (layer, channel) via BKE_paint_material_layer_maps_get.
 * - Acquires each ImBuf once; released in destructor (also on failure paths).
 * Returns empty (invalid) targets when nothing writable is found.
 */
CloneStrokeTargets clone_stroke_targets_build(const Main &bmain,
                                              const Material &material,
                                              int visible_material_channels);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Dab transform (oriented stamp + Normal channel re-planting).
 * \{ */

/**
 * The source-to-destination relation of one dab, resolved once per dab and constant across every
 * layer, channel and texel of it.
 */
struct CloneDabTransform {
  /**
   * A destination-side UV offset from the dab center, expressed as a UV offset from the source
   * point: `source_frame.uv_to_screen^-1 * dest_frame.uv_to_screen`.
   *
   * The source half is #CloneSourcePoint.frame, frozen when the source was picked, so this
   * carries how far the view has turned since -- see that field for why both halves being live
   * would cancel out.
   *
   * Identity is the degenerate fallback (no view, or an edge-on / UV-degenerate triangle), which
   * reproduces the plain UV-locked stamp rather than dropping the dab.
   */
  float2x2 dest_uv_to_source_uv = float2x2::identity();
  /**
   * The destination's `uv_to_screen` with its scale divided out, so it is pure rotation and
   * shear. It turns a UV offset into a direction in the destination's screen-aligned basis, which
   * is the frame the footprint is shaped in: it keeps a round brush round while giving a
   * rectangular one (#BRUSH_TEXTURE_CLIP_RECTANGLE) edges aligned with the view rather than with
   * the UV axes. Identity when #has_frames is false.
   */
  float2x2 dest_uv_to_footprint_dir = float2x2::identity();
  /** Only read for #PAINT_MATERIAL_CHANNEL_NORMAL, and only when #has_frames. */
  CloneSurfaceFrame source_frame;
  CloneSurfaceFrame dest_frame;
  bool has_frames = false;
};

/**
 * The UV map a dab reads, resolved the same way #clone_pick_uv resolves it so that a frame and
 * the UV it is built for can never disagree.
 */
VArraySpan<float2> clone_uv_map_get(const Mesh &mesh_eval,
                                    ePaintCanvasSource canvas_mode,
                                    Object *ob_for_material,
                                    int tri_index);

/**
 * UV Jacobian of one triangle: the 3D directions the surface travels per UV unit (`dP/du`,
 * `dP/dv`). False when the triangle is unwrapped to a point or a line, so the map does not exist.
 *
 * \param r_denom: the UV pair's orientation determinant, the sign the tangent-handedness reads.
 * Optional because only the frame build needs it.
 */
bool clone_tri_uv_jacobian(const Mesh &mesh_eval,
                           Span<float2> uv_map,
                           int tri_index,
                           float3 &r_dp_du,
                           float3 &r_dp_dv,
                           float *r_denom = nullptr);

/**
 * Build the screen-aligned and tangent bases of one triangle.
 *
 * \param region, projection_mat, view_right: the view the stamp is oriented by, in the object
 * space the evaluated mesh positions live in. A null \a region leaves the frame invalid, which
 * the caller reads as "keep the UV-locked stamp".
 */
bool clone_surface_frame_build(const Mesh &mesh_eval,
                               Span<float2> uv_map,
                               int tri_index,
                               const ARegion *region,
                               const float4x4 &projection_mat,
                               const float3 &view_right,
                               CloneSurfaceFrame &r_frame);

/**
 * Camera right in \a ob's object space -- the in-plane axis #build_normal_write_basis falls back
 * to when a triangle's screen projection is degenerate. Zero without a view, which that function
 * also handles.
 */
float3 clone_view_right_get(const ViewContext &vc, const Object &ob);

/**
 * Compose a dab transform with a symmetry Jacobian so a mirrored pass stamps the mirror image of
 * the main pass's stamp: both matrices are post-multiplied by `jacobian^-1`, which maps the
 * mirrored destination's UV offsets back into the main destination's frame before the source or
 * the footprint reads them.
 *
 * Returns false (transform untouched, un-mirrored stamp) when the Jacobian is singular.
 */
bool clone_symmetry_transform_apply(CloneDabTransform &transform, const float2x2 &jacobian);

/**
 * Resolve the source-to-destination relation of one dab.
 *
 * Both frames come from the CURRENT view, so the mapping between them carries the relative
 * orientation of the destination patch relative to the frame the source was picked in. Only the
 * destination half is live, and that is deliberate: with both halves rebuilt from the current
 * view, a viewport rotation turns `uv_to_screen` at both ends by the same in-plane angle and the
 * two cancel in the product, so the stamp never turns. Against the frozen source frame the
 * rotation survives, which is the relation a clone tool has between the moment the source was set
 * and the moment the stroke is drawn. It also makes the stamp survive a destination island whose
 * UV parametrization is rotated, mirrored or scaled against the source.
 *
 * A frame that could not be built leaves the transform at identity -- the plain UV-locked stamp
 * -- rather than dropping the dab.
 */
CloneDabTransform clone_dab_transform_build(const Mesh &mesh_eval,
                                            ePaintCanvasSource canvas_mode,
                                            Object *ob_for_material,
                                            const CloneSurfaceFrame &source_frame,
                                            int dest_tri_index,
                                            const ARegion *region,
                                            const float4x4 &projection_mat,
                                            const float3 &view_right);

/** \} */

/* -------------------------------------------------------------------- */
/** \name CPU sampler (works on already-acquired ImBuf, never reacquires).
 * \{ */

/**
 * Bilinear sample of \a ibuf at \a uv. Out-of-range UVs wrap, matching the paint sampler.
 * False when the buffer carries neither byte nor float pixels.
 */
bool sample_channel_at_uv_cpu(const ImBuf &ibuf, const float2 &uv, float4 &r_color);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Per-channel blend policy.
 * \{ */

/**
 * Blend a sampled source value over the destination at \a strength.
 *
 * \param is_data: the channel's byte pixels are non-color data (Roughness, Metallic, AO ...).
 * Color channels stored as bytes are sRGB-encoded, and mixing two encoded values is not the mix
 * of the colors they stand for -- a half-strength blend comes out lighter than the brush's own.
 * So a color byte channel is decoded, mixed and re-encoded; a data channel is mixed as stored,
 * because its numbers are the values themselves. Float buffers are already linear either way.
 * \param channel: #PAINT_MATERIAL_CHANNEL_NORMAL at near-full strength copies the encoded RGB
 * unchanged, so tangent-space values survive exactly.
 */
void clone_blend_channel_values(const float4 &src,
                                const float4 &dst,
                                float strength,
                                eMaterialPaintChannel channel,
                                bool is_data,
                                float4 &r_result);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Dab application.
 * \{ */

struct CloneStrokeRuntime;

/**
 * Apply one dab to all targets (outer loop over layers/channels), stamping the full brush
 * footprint in UV space.
 * \param dest_uv: dab center destination UV.
 * \param dab_center_uv: the point the source-side offset is measured from -- \a dest_uv in
 *                       Absolute mode (so the source is pinned), the Relative anchor otherwise.
 * \param transform: how this dab maps its footprint back onto the source, and the two surface
 *                   frames a Normal-channel write needs. Built once per dab by the caller.
 * \param uv_radius: brush footprint radius in UV units (caller computes it from the brush
 *                   radius and the local surface-to-UV scale, or the canvas pixel size).
 * \param channel_mask: bit set of #eMaterialPaintChannel values to stamp in this call. Every
 *                      channel of one layer shares the footprint geometry and the falloff, which
 *                      is resolved once per texel rather than once per channel.
 * \param strength: brush strength at this dab (falloff * pressure * opacity already applied by
 *                  caller); the per-pixel brush falloff curve is applied here on top.
 * Every destination pixel samples source_uv = source_center + (dest_pixel - dab_center) so the
 * stamp copies image detail, not a flat color. The pristine undo tile is captured through
 * #ED_image_paint_tile_push before a tile is first modified, so one stroke-level image undo
 * step restores the whole stroke.
 */
void clone_stroke_apply_dab(CloneStrokeRuntime *runtime,
                            const float2 &dest_uv,
                            const float2 &dab_center_uv,
                            const CloneDabTransform &transform,
                            float uv_radius,
                            int channel_mask,
                            float strength);

/** \} */

}  // namespace blender::ed::sculpt_paint::clone
