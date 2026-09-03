/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup editors
 *
 * Bakes a #Material's Principled BSDF inputs into buffers a PBR Paint stroke can sample.
 *
 * Split from #BKE_paint_material_source_resolve on purpose: the resolve answers what a material
 * *could* supply and is cheap enough for every panel redraw, while a bake compiles and runs the
 * material through the render pipeline. Drawing the PBR Paint panel must never trigger that, so
 * the UI only ever calls the resolver and this header is reached from the paint path.
 *
 * The bake runs in a #wmJob, never on the caller's thread: it is a full EEVEE render, and doing it
 * inline would freeze the first dab of a stroke and take the draw lock a second time on a thread
 * that may already hold it. Callers therefore get whatever is cached right now and paint the
 * channel's own value until a bake lands, rather than blocking for one.
 */

#include <array>
#include <memory>

#include "BLI_math_vector_types.hh"

#include "BKE_paint_material_resolve.hh"

#include "DNA_brush_types.h"
#include "DNA_scene_types.h"
#include "DNA_texture_types.h"

struct bContext;

namespace blender {
struct ImBuf;
struct Image;
struct Main;
struct Material;
}  // namespace blender

namespace blender::ed::material_bake {

/**
 * One material baked onto the unit UV square.
 *
 * Immutable once constructed, and handed out as a `shared_ptr` so a stroke can hold it alive while
 * the cache behind #material_source_bake_get drops or replaces its own entry.
 */
class MaterialSourceBake {
  MaterialSourceResolve resolve_;
  /** Owned. Non-null exactly for the channels whose resolution is #ChannelResolution::Baked. */
  std::array<ImBuf *, PAINT_MATERIAL_CHANNEL_NUM> images_;

 public:
  MaterialSourceBake(const MaterialSourceResolve &resolve,
                     std::array<ImBuf *, PAINT_MATERIAL_CHANNEL_NUM> images);
  ~MaterialSourceBake();

  /* Owns #ImBuf pointers with no reference counting of its own. */
  MaterialSourceBake(const MaterialSourceBake &) = delete;
  MaterialSourceBake &operator=(const MaterialSourceBake &) = delete;

  ChannelResolution resolution(eMaterialPaintChannel channel) const;
  /** #ChannelUnavailableReason::None when the channel is usable. */
  ChannelUnavailableReason unavailable_reason(eMaterialPaintChannel channel) const;
  /**
   * The baked buffer for \a channel, or null unless #resolution is #ChannelResolution::Baked.
   * Ownership stays here; the buffer is valid for as long as this object is.
   */
  const ImBuf *channel_image(eMaterialPaintChannel channel) const;
  /** Meaningful only when #resolution is #ChannelResolution::Constant. */
  float4 channel_constant(eMaterialPaintChannel channel) const;
};

/**
 * The cached bake of \a ma, or null when this material has never been baked in this session.
 *
 * Never renders and never blocks, so the paint path can call it at stroke start.
 *
 * Falls back to another revision of the same material when \a resolution or the node-tree state
 * has moved on and the matching bake is still rendering. That window is seconds long and covers
 * ordinary actions -- selecting a different source material, editing one, changing Bake Size --
 * during which a null result would make #ChannelSourceSet mark every baked channel unusable for
 * a whole stroke. Momentarily stale pixels are the better answer than a stroke that paints
 * nothing.
 *
 * A null result still means the baked channels fall back to their own values for this stroke;
 * call #material_source_bake_ensure from a context that has one to get a bake started.
 */
std::shared_ptr<const MaterialSourceBake> material_source_bake_get(const Material &ma,
                                                                   int resolution);

/**
 * Start a background bake of \a ma when the cache holds no entry matching its current node trees.
 *
 * Cheap and idempotent: it hashes the material's node-tree state and returns immediately when that
 * bake is already cached or already running. Meant to be called from redraw-frequency code (the
 * paint cursor), which is what makes an edit to the source material pick itself up without the
 * user asking.
 */
void material_source_bake_ensure(const bContext &C, Material &ma, int resolution);

/**
 * As above, for callers that have no #bContext -- an RNA update, in particular. Starts nothing
 * when \a bmain has no window manager yet (file read, background mode).
 */
void material_source_bake_ensure(Main &bmain, Material &ma, int resolution);

/**
 * Drop every cached bake of \a ma, or of every material when \a ma is null.
 *
 * A superseded bake of the same material is dropped when its replacement lands, so this is for
 * teardown: on quit and before a file load, when every material the cache still describes is
 * about to be freed. Safe while a stroke holds its own bake, which keeps the buffers alive
 * through the `shared_ptr` it was handed.
 *
 * The caller must have stopped the bake jobs first, or a job landing afterwards would repopulate
 * the cache. Both current call sites run after #WM_jobs_kill_all.
 */
void material_source_bake_invalidate(const Material *ma);

/**
 * Mark every cached bake that sampled \a image as out of date.
 *
 * An image's pixels cannot be part of the bake's cache key -- there is no content version to hash,
 * and painting one would have to be noticed anyway -- so an edit to an image a material samples
 * would otherwise keep serving the bake made from the old pixels for the rest of the session.
 *
 * Marks rather than drops: the entry keeps answering #material_source_bake_get until a fresh bake
 * lands, so a stroke starting in between paints slightly stale pixels instead of nothing at all.
 * The next #material_source_bake_ensure starts that bake.
 */
void material_source_bake_tag_image_changed(const Image &image);

/** Whether a bake of \a ma at \a resolution is currently cached. Exists for tests. */
bool material_source_bake_cache_contains(const Material &ma, int resolution);

/**
 * What to show for a brush in Source Mode: Material, where no channel has a #Tex to sample.
 *
 * A channel is either baked into a buffer covering UV [0, 1]^2, or a plain constant. #mtex carries
 * only the placement (map mode, size, offset, rotation); its own #Tex is cleared, since the pixels
 * come from here instead.
 *
 * Not copyable: #MTex deletes its copy operations (#DNA_DEFINE_CXX_METHODS), so this must be
 * filled in place and its #mtex copied out with #dna::shallow_copy.
 */
struct MaterialSourcePreview {
  const ImBuf *ibuf = nullptr;
  float4 constant = float4(0.0f);
  MTex mtex = {};
  bool usable = false;
};

/**
 * Fill \a r_preview with the channel a user would expect to see previewed for \a brush_paint.
 *
 * Follows #BKE_paint_material_channel_preview_order, the same order the Maps-mode preview uses, so
 * both modes show the same channel; only usability is decided differently, from the bake rather
 * than from a #Tex.
 *
 * \param r_bake: keeps the bake alive for as long as #MaterialSourcePreview.ibuf points into it.
 *                The bake job may replace the cache entry from another thread at any point.
 * \return false when there is nothing to preview, leaving the caller on the brush's own texture.
 */
bool material_source_preview_get(const BrushMaterialPaint &brush_paint,
                                 const PaintModeSettings &mode_settings,
                                 int visible_material_channels,
                                 MaterialSourcePreview &r_preview,
                                 std::shared_ptr<const MaterialSourceBake> &r_bake);

}  // namespace blender::ed::material_bake
