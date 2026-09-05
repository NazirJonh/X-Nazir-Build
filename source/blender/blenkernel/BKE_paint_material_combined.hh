/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * Shades every paint channel of a material into one lit image, on the CPU.
 *
 * This is a **flat tangent-space preview**: the canvas is treated as a plane facing the viewer and
 * lit by a small analytic studio rig. It is explicitly **not** a preview of the material on the
 * model -- there is no geometry, no shadowing, no environment, and no camera. What it answers is
 * "do these maps make sense together", which is the question a texture author has while painting
 * and which no per-channel pass can answer.
 *
 * Split in two on purpose:
 *
 * - The evaluator is pure. It reads #CombinedInputs and #CombinedPreviewLighting and writes float
 *   RGBA. No #Main, no #Object, no GPU, no ID lookups. Everything a GPU implementation would need
 *   is in its arguments, which is what makes a later GPU path a substitution, not a rewrite.
 * - The cache beside it owns one buffer per material and decides how much of it to recompute.
 *
 * The *gathering* -- turning a #Material into #CombinedInputs -- deliberately lives in the editors
 * (`ED_material_combined.hh`), because it needs the bake and this module must not.
 */

#include <array>
#include <cstdint>

#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"

#include "DNA_scene_types.h"
/* Complete rather than forward-declared: #CombinedCacheRequest holds #rcti by value. */
#include "DNA_vec_types.h"

namespace blender {

struct ImBuf;
struct Image;
struct Material;

/** One channel's contribution, either as pixels or as a single value. */
struct CombinedChannelInput {
  /**
   * Byte or float buffer with the combined output's dimensions, or null.
   *
   * Not owned, and not acquired: the caller holds whatever lock or `shared_ptr` keeps it alive for
   * the duration of the call. The evaluator only reads it.
   */
  const ImBuf *ibuf = nullptr;
  /** Used when #ibuf is null. RGBA for colour channels, `.x` for scalars. */
  float4 constant = float4(0.0f);
  /**
   * Whether #ibuf holds sRGB-encoded values that must be linearized before shading.
   *
   * Set by the gatherer from the source's actual colorspace, never inferred here: the evaluator
   * has no colour management and each kind of source answers this differently. Always false for
   * the data channels -- Metallic, Roughness, Specular, Normal, AO, Alpha -- whatever their
   * buffers claim.
   */
  bool is_srgb = false;
};

/** Every channel the shading reads, indexed by #eMaterialPaintChannel. */
struct CombinedInputs {
  std::array<CombinedChannelInput, PAINT_MATERIAL_CHANNEL_NUM> channels;
  int width = 0;
  int height = 0;
  /**
   * Principled's Emission Strength, which the Emission channel is multiplied by.
   *
   * A field rather than a channel because it is not one: the channel table maps
   * #PAINT_MATERIAL_CHANNEL_EMISSION to the *Emission Color* socket alone, and a preview that
   * stops there is wrong for every default material -- that socket defaults to white while the
   * strength beside it defaults to zero, so the surface would emit full white and wash out
   * everything the other channels contribute.
   *
   * Defaults to 1 so that the evaluator stays a pure function of what it is handed: a caller that
   * has already scaled its emission, or a test pinning the emission term, leaves it alone.
   */
  float emission_strength = 1.0f;
  /**
   * Resolution of the shaded result, when it differs from the inputs.
   *
   * Zero means "the same as #width and #height", which is what every caller but the viewport
   * wants. The output always covers the whole canvas: the image engine maps a display override
   * over the full image UV range using the buffer's own dimensions
   * (#ScreenSpaceDrawingMode::do_full_update_texture_slot scales by `tile_buffer.x`), so the
   * preview may be coarser than the canvas but never a window onto it.
   */
  int output_width = 0;
  int output_height = 0;

  int out_width() const
  {
    return this->output_width > 0 ? this->output_width : this->width;
  }
  int out_height() const
  {
    return this->output_height > 0 ? this->output_height : this->height;
  }
};

/** One analytic directional light of the studio rig. */
struct CombinedPreviewLight {
  /** Normalized, in the flat canvas's tangent space: `+Z` points at the viewer. */
  float3 direction = float3(0.0f, 0.0f, 1.0f);
  float3 diffuse_color = float3(1.0f);
  float3 specular_color = float3(1.0f);
  /** Wraps diffuse round the terminator, as Workbench's studio lights do. */
  float wrap = 0.0f;
};

/**
 * The whole lighting environment of the preview.
 *
 * A value rather than a global, so phase 2 can drive it from the UI and a test can pin it without
 * either reaching into the evaluator.
 */
struct CombinedPreviewLighting {
  std::array<CombinedPreviewLight, 4> lights;
  int lights_num = 4;
  float3 ambient_color = float3(0.05f);
  float exposure = 1.0f;
  /** How much the AO channel darkens ambient. 0 ignores AO entirely. */
  float ao_influence = 1.0f;

  /**
   * Every field that changes a pixel, folded into one number.
   *
   * The cache compares this rather than the struct so that a rig arriving from the UI as a fresh
   * value each redraw does not read as a change.
   */
  uint64_t hash() const;
};

/** The studio rig the preview starts from. */
CombinedPreviewLighting BKE_paint_material_combined_lighting_default();

/**
 * Turn every light of \a lighting about the canvas normal by \a angle radians.
 *
 * The flat-canvas equivalent of #View3DShading.studiolight_rot_z: `+Z` points at the viewer, so
 * rotating about it sweeps the rig across the surface and makes a normal map read as relief. Only
 * the directions move -- colours, wrap, ambient and exposure are properties of the rig, not of
 * where it is standing.
 *
 * Applied by the caller rather than stored, so #CombinedPreviewLighting stays the complete
 * description of a lighting environment and #hash therefore still covers everything that shades.
 */
void BKE_paint_material_combined_lighting_rotate_z(CombinedPreviewLighting &lighting, float angle);

/** What one evaluation actually did, for the performance work and for the cache tests. */
struct CombinedEvalStats {
  double elapsed_seconds = 0.0;
  int64_t pixels_processed = 0;
};

/**
 * The value a channel contributes when the material supplies nothing for it.
 *
 * Deliberately **not** #BKE_paint_material_channel_default_value, which defines paint *storage*
 * neutrals: it answers 0 for Base Color -- correct for a channel being painted into, and a black
 * preview for a material with no base colour map -- and it is scalar, so it cannot express the
 * flat normal (0, 0, 1) its own comment names. These are Principled preview defaults instead, and
 * the disagreement between the two tables is intended in both directions.
 */
float4 BKE_paint_material_combined_default_value(eMaterialPaintChannel channel);

/**
 * Shade \a inputs into \a dst_ibuf.
 *
 * \param dst_ibuf: **float** RGBA of `inputs.width` x `inputs.height`, in scene linear. Float
 *                  rather than byte because a lit result is scene-referred: specular highlights
 *                  exceed 1.0, and clamping them here -- before the display transform -- is the
 *                  one thing that would make the preview disagree with the viewport.
 * \param region: when given, only this rectangle is recomputed and the rest of \a dst_ibuf is left
 *                as it was. Clipped to the buffer. This is what keeps a dab's cost proportional to
 *                the dab.
 * \return false when a channel buffer's dimensions disagree with \a inputs, or when the
 *         destination is unusable.
 *
 * Pure: no #Main, no #Object, no GPU, no ID lookups, no allocation beyond scratch.
 */
bool BKE_paint_material_combined_eval(const CombinedInputs &inputs,
                                      const CombinedPreviewLighting &lighting,
                                      ImBuf *dst_ibuf,
                                      const rcti *region = nullptr,
                                      CombinedEvalStats *r_stats = nullptr);

/* -------------------------------------------------------------------- */
/** \name Combined Cache
 * \{ */

/**
 * What a cache lookup needs to know beyond the pixels themselves.
 *
 * A struct rather than four more arguments: every field is optional in its own way, and as
 * positional parameters they read at the call site as `1234, region, {}, clip` -- four values with
 * nothing to say which is which, and a fifth would be worse. Every rectangle here is in **output**
 * coordinates (see #CombinedInputs.output_width), which are the canvas's only while the output is
 * not reduced.
 */
struct CombinedCacheRequest {
  /**
   * Identifies the *structure* of the inputs -- which source each channel came from, the
   * constants, the dimensions, the identity of any bake. Deliberately not the pixels: a change
   * here means a full rebuild, and a composite whose pixels moved must not land here or every dab
   * would rebuild the canvas.
   */
  uint64_t inputs_hash = 0;
  /**
   * Pixels known to have changed since the last call. Empty means nothing did. Only consulted
   * when #inputs_hash and the lighting are unchanged.
   *
   * A caller that reports a dab in canvas texels while asking for a reduced output must scale it;
   * #combined_preview_ensure is where that happens, and it scales #clip the same way.
   */
  rcti changed_region = {0, 0, 0, 0};
  /**
   * Every #Image the inputs read directly, so the cache can subscribe to its partial-update log
   * and find out what changed in it without being told.
   *
   * Layer-stack images need not appear: their edits arrive as a #changed_region from the composite
   * that owns them, which is already precise. These are the ones nothing else would report.
   *
   * The pointers are used during the call only; the cache keeps subscriptions keyed by
   * #ID.session_uid and never an #Image pointer, so an image freed between calls is a missing
   * dependency rather than a dangling one.
   */
  Span<Image *> dependency_images;
  /**
   * The part of the buffer the caller is about to read. Empty means all of it.
   *
   * Pixels outside it may be left stale from an earlier lighting or an earlier set of inputs, so a
   * caller that reads more than it declares here will read stale pixels -- which is why every
   * caller but the viewport leaves it empty. This is what keeps a light drag proportional to the
   * editor, not to the canvas.
   */
  rcti clip = {0, 0, 0, 0};
};

/**
 * The Combined preview of \a ma, recomputing whatever part of it is out of date.
 *
 * A cache, not a resolver: \a inputs is already gathered. That split is what lets this live in BKE
 * next to the evaluator -- so #BKE_material_free can drop it directly, as it does the composite --
 * while the gathering, which needs #Main, the resolver and the bake, stays in the editors.
 *
 * The returned buffer is owned by the cache and stays valid until the next call that changes it or
 * a #BKE_paint_material_combined_cache_free_all. Do not free it.
 *
 * \param r_revision: bumped whenever the pixels are recomputed, for the display-override contract.
 * \param r_changed_region: the rectangle actually re-shaded by this call, in output coordinates,
 *                          or empty when nothing was. Exists so a consumer that keeps a copy of
 *                          these pixels -- the image engine's GPU texture -- can refresh that much
 *                          of it rather than all of it. Always consistent with \a r_revision: the
 *                          region is empty exactly when the revision did not move.
 * \return null when the inputs supply nothing, which is the caller's cue to fall back.
 */
ImBuf *BKE_paint_material_combined_cache_ensure(const Material &ma,
                                                const CombinedInputs &inputs,
                                                const CombinedPreviewLighting &lighting,
                                                const CombinedCacheRequest &request,
                                                uint64_t *r_revision = nullptr,
                                                rcti *r_changed_region = nullptr,
                                                CombinedEvalStats *r_stats = nullptr);

/**
 * Mark the Combined preview of \a ma out of date, or of every material when \a ma is null.
 *
 * Marks rather than drops, like the composite cache: a caller asking for the buffer in between
 * must get the previous pixels rather than nothing.
 *
 * For a change to the material's *description* only. An edit to the pixels of an image a channel
 * reads is found by #BKE_paint_material_combined_cache_ensure, which polls each dependency image's
 * partial-update log; reporting one here instead would turn a painted dab into a full re-shade of
 * the canvas.
 */
void BKE_paint_material_combined_cache_invalidate(const Material *ma);

/** Drop the cached preview of \a ma, freeing its buffer. Called when the material is freed. */
void BKE_paint_material_combined_cache_free_material(const Material &ma);

/** Drop every cached preview. For teardown and for file load. */
void BKE_paint_material_combined_cache_free_all();

/** Whether a preview of \a ma is currently cached. Exists for tests. */
bool BKE_paint_material_combined_cache_contains(const Material &ma);

/**
 * The dimensions of the *canvas* a cached preview of \a ma stands for, without producing one.
 *
 * Exists because a size query is on the redraw path several times a frame --
 * #ED_space_image_get_size reaches it from #image_main_region_set_view2d and from the preview's
 * own clip calculation -- and answering it by acquiring the buffer shades the whole canvas to read
 * two integers.
 *
 * The canvas, not the buffer: the preview may be shaded at a power-of-two fraction of it (see
 * #CombinedInputs.output_width), and a caller building the editor's view from that fraction would
 * draw the canvas at the wrong size.
 *
 * \return false when nothing is cached, leaving \a r_width and \a r_height untouched.
 */
bool BKE_paint_material_combined_cache_size_get(const Material &ma, int &r_width, int &r_height);

/**
 * The resolution a cached preview of \a ma is currently shaded at -- a power-of-two fraction of
 * the canvas #BKE_paint_material_combined_cache_size_get reports.
 *
 * Exists for the gather's octave choice. Deriving that octave from the zoom alone makes the
 * step-down and the step-up share one threshold, so a zoom resting on it alternates between two
 * octaves and rebuilds the whole preview every frame; starting from the octave already in use is
 * what turns that threshold into a band.
 *
 * \return false when nothing is cached, leaving \a r_width and \a r_height untouched.
 */
bool BKE_paint_material_combined_cache_output_size_get(const Material &ma,
                                                       int &r_width,
                                                       int &r_height);

/** \} */

}  // namespace blender
