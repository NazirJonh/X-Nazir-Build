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
 *   is in its arguments, which is what makes a later GPU path a substitution rather than a rewrite.
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

namespace blender {

struct ImBuf;
struct Image;
struct Material;
struct rcti;

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
 * The Combined preview of \a ma, recomputing whatever part of it is out of date.
 *
 * A cache, not a resolver: \a inputs is already gathered. That split is what lets this live in BKE
 * next to the evaluator -- so #BKE_material_free can drop it directly, as it does the composite --
 * while the gathering, which needs #Main, the resolver and the bake, stays in the editors.
 *
 * The returned buffer is owned by the cache and stays valid until the next call that changes it or
 * a #BKE_paint_material_combined_cache_free_all. Do not free it.
 *
 * \param inputs_hash: identifies the *structure* of the inputs -- which source each channel came
 *                     from, the constants, the dimensions, the identity of any bake. Deliberately
 *                     not the pixels: a change here means a full rebuild, and a composite whose
 *                     pixels moved must not land here or every dab would rebuild the canvas.
 * \param changed_region: pixels known to have changed since the last call, in output coordinates.
 *                        Empty means nothing did. Only consulted when \a inputs_hash and the
 *                        lighting are unchanged.
 * \param dependency_image_uids: #ID.session_uid of every #Image the inputs read directly, so
 *                               #BKE_paint_material_combined_cache_tag_image_region can find this
 *                               entry. Layer-stack images need not appear: their edits arrive as a
 *                               \a changed_region from the composite that owns them.
 * \param r_revision: bumped whenever the pixels are recomputed, for the display-override contract.
 * \return null when the inputs supply nothing, which is the caller's cue to fall back.
 */
ImBuf *BKE_paint_material_combined_cache_ensure(const Material &ma,
                                                const CombinedInputs &inputs,
                                                const CombinedPreviewLighting &lighting,
                                                uint64_t inputs_hash,
                                                const rcti &changed_region,
                                                Span<uint32_t> dependency_image_uids,
                                                uint64_t *r_revision = nullptr,
                                                CombinedEvalStats *r_stats = nullptr);

/**
 * Mark the Combined preview of \a ma out of date, or of every material when \a ma is null.
 *
 * Marks rather than drops, like the composite cache: a caller asking for the buffer in between
 * must get the previous pixels rather than nothing.
 */
void BKE_paint_material_combined_cache_invalidate(const Material *ma);

/**
 * Mark only \a region of every preview that reads \a image out of date.
 *
 * The direct-image counterpart of the composite's `r_changed_region`: a channel sourced from a
 * plain Image Texture has no composite behind it, so nothing else would report the rectangle a
 * stroke touched.
 */
void BKE_paint_material_combined_cache_tag_image_region(const Image &image, const rcti &region);

/** Mark every preview that reads \a image out of date, in full. */
void BKE_paint_material_combined_cache_tag_image_changed(const Image &image);

/** Drop the cached preview of \a ma, freeing its buffer. Called when the material is freed. */
void BKE_paint_material_combined_cache_free_material(const Material &ma);

/** Drop every cached preview. For teardown and for file load. */
void BKE_paint_material_combined_cache_free_all();

/** Whether a preview of \a ma is currently cached. Exists for tests. */
bool BKE_paint_material_combined_cache_contains(const Material &ma);

/** \} */

}  // namespace blender
