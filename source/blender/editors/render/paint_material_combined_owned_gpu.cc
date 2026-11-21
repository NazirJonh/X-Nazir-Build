/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edrend
 *
 * The editors' owned-effect GPU provider for BKE's paint layers: the
 * #BKE_paint_layers_owned_effect_gpu_provider_set hook. A bake of an effect that owns its result
 * asks the hook before it recomputes the effect on the CPU, and this file answers with what the
 * combined preview's effect stage computes: the row is described the way the GPU path lays it, the
 * preview's own effect run walks the row's effects in order (the ones before the asked-for effect
 * feed its input; a cache hit costs no dispatch), and the asked-for effect's result texture is read
 * back into a float #ImBuf with the CPU effect buffer's semantics. Every refusal answers null,
 * which the caller answers with the CPU compute. The mask-effect hook this module also answers
 * lives in paint_material_combined_mask_gpu.cc, registered by the one register below.
 */

#include "paint_material_combined_owned_gpu.hh"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>

#include "MEM_guardedalloc.h"

#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_perf_debug.hh"

#include "BLI_index_range.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_memory_utils.hh"
#include "BLI_rect.h"
#include "BLI_span.hh"
#include "BLI_threads.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "DNA_material_types.h"
#include "DNA_vec_types.h"

#include "DRW_engine.hh"

#include "GPU_context.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"
#include "GPU_texture_pool.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "ED_material_combined.hh"

#include "paint_material_combined_effects.hh"

namespace blender::ed::material_combined {

/**
 * The owned-effect provider's refusal stage, printed once per stage change under
 * `BLENDER_PBR_PERF`: a null answer is silent, and a user whose effect landed on the CPU has no
 * line naming the gate that refused it. The provider always runs on the main thread (the hook's
 * own contract), so a single static slot is a thread-safe dedup, and the stage strings are
 * literals, so pointer equality tests the change.
 */
static void own_bake_refuse(const char *stage)
{
  static const char *last_stage = nullptr;
  if (!bke::paint_perf::enabled() || last_stage == stage) {
    return;
  }
  last_stage = stage;
  printf("[PBR-PERF] gpu_effect.own_bake.refuse stage=%s\n", stage);
  fflush(stdout);
}

/**
 * Compute an owned effect's result on the GPU, the way the combined preview's effect stage does.
 *
 * The result is a float RGBA #ImBuf at the effect input's own size -- the same buffer the CPU effect
 * cache computes for the same description: premultiplied by the input's content alpha where the
 * channel tracks one, a one elsewhere, and the kernel's own alpha for a kind whose plan writes one
 * (#PAINT_LAYER_KIND_RESULT_HAS_ALPHA). The texture the read back takes is the GPU effect cache's
 * own, keyed exactly as the preview keys it, so a bake and the screen share one computed result.
 *
 * Null on any refusal, and the caller computes on the CPU: not the main thread (the hook's own
 * gate, repeated as a guard), the material set to the CPU composite or the `BLENDER_PBR_GPU_COMBINED`
 * comparison switch, a tile the GPU cache does not key, a row the GPU path cannot describe, no GPU
 * context, an effect whose input the stage refuses (no dimensions, a plan that names no slot image,
 * an Anchor Point slice nothing captured), or a texture that could not be allocated or read back.
 * Each refusal names its stage in the `BLENDER_PBR_PERF` log (see #own_bake_refuse), so a user
 * whose effect landed on the CPU can read which gate refused it.
 *
 * \param changed_region: the area of the effect's sources the caller already knows moved (the bake's
 * subscription rectangle over the input's maps, in the input's own texels), or null for a full
 * recompute. The provider reads nothing of it and computes nothing extra: it hands the rectangle to
 * the effect stage, whose GPU result cache already tracks a dirty region of its own and narrows the
 * pass dispatches to dirty + halo -- the caller's rectangle joins that one, so the dispatched band
 * covers both views of the change and never less than the cache's own (the texture is shared with
 * the screen, and the stage publishes it valid as a whole). A cache Hit dispatches nothing and
 * the read back below takes the whole texture, which the Hit guarantees current; the buffer handed
 * back is therefore always the effect's full, fresh result whatever the rectangle named.
 */
static std::shared_ptr<ImBuf> owned_gpu_result_compute(const Material &ma,
                                                       const MaterialPaintLayer &row,
                                                       const MaterialPaintLayer &effect,
                                                       const int channel,
                                                       const int tile_number,
                                                       const rcti *changed_region)
{
  /* The provider's own gates, each refusing with its stage named: the hook's gate ahead of it
   * repeats some of these, so the stages are what a future caller's refusal shows too. */
  if (!BLI_thread_is_main()) {
    own_bake_refuse("not_main");
    return nullptr;
  }
  if (!combined_preview_gpu_enabled(&ma)) {
    own_bake_refuse("not_enabled");
    return nullptr;
  }
  if (tile_number != PAINT_TILE_NUMBER_BASE) {
    own_bake_refuse("tile");
    return nullptr;
  }
  if (!BKE_paint_layers_effect_is_gpu_computed(effect)) {
    /* The kind has no pass plan of its own, so the stage below would refuse it the same way. */
    own_bake_refuse("plan");
    return nullptr;
  }

  /* The row as the GPU path lays it, whole and described only: the effect stage walks its
   * corrections and collects every effect of the row in the row's own order. The ones before the
   * asked-for effect are part of its input, so they are computed too; the ones after it are cut
   * off by the marker the stage is handed below -- a bake reads no frame after itself, and their
   * dispatches would only warm a preview the bake does not serve. */
  PaintMaterialCompositeImageLayer entry;
  if (!BKE_paint_layers_gpu_row_entry(ma, row, channel, entry)) {
    own_bake_refuse("describe");
    return nullptr;
  }

  /* The bake asks outside a draw, where no GPU context is current: the frame enables-and-restores
   * the context, ensures the shaders and acquires the placeholders the dispatches bind, the row
   * render's own terms (see #CombinedGpuFrame). */
  gpu::TexturePool &pool = gpu::TexturePool::get();
  /* No HOST_READ here: the flag's own rule is that only the textures a data read takes carry it
   * (gpu_texture.cc), and this provider's read back takes the effect cache's result texture, which
   * the cache's acquire hook (#effect_gpu_acquire) flags for the read -- the scratch this usage
   * covers is sampled by the dispatches alone. */
  const eGPUTextureUsage usage = GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_SHADER_WRITE;
  CombinedGpuFrame frame(pool);
  if (!frame.ok) {
    own_bake_refuse(frame.fail_stage);
    return nullptr;
  }
  Vector<gpu::Texture *> filter_luts;
  Vector<GpuSliceSlot> slice_slots;
  Vector<GpuMaskSlot> mask_slots;
  Vector<GpuEffectResult> effect_results;
  BLI_SCOPED_DEFER([&]() {
    for (gpu::Texture *texture : filter_luts) {
      pool.release_texture(texture);
    }
  });

  CombinedPasses passes(pool,
                        frame.dummy,
                        frame.slot_dummy,
                        frame.shaders.layer,
                        frame.shaders.mask,
                        frame.shaders.correction,
                        frame.shaders.folder,
                        frame.shaders.effect,
                        filter_luts,
                        slice_slots,
                        mask_slots,
                        effect_results,
                        usage);

  /* The single-row, single-channel stack the stage walks. No Anchor Point captures run before it:
   * the preview captures its slots first, so an input that reads a live slice runs into the missing
   * slot below and refuses there, to the CPU. */
  const Span<const PaintMaterialCompositeImageLayer> stack(&entry, 1);
  const Span<Span<const PaintMaterialCompositeImageLayer>> stack_view(&stack, 1);
  const int channel_enum = channel;
  const bool has_stack = true;
  Vector<GpuEffectJob> jobs;
  BLI_SCOPED_DEFER([&]() {
    /* The stage takes a reference of every result it hands out -- including on the failure paths it
     * returns through -- and the provider holds none past the read back below. */
    for (const GpuEffectJob &job : jobs) {
      if (job.result != nullptr) {
        GPU_texture_free(job.result);
      }
    }
  });
  if (!combined_preview_gpu_effects_run(ma,
                                        stack_view,
                                        Span<int>(&channel_enum, 1),
                                        Span<bool>(&has_stack, 1),
                                        /*wanted_channel=*/-1,
                                        passes,
                                        jobs,
                                        /*painted_object=*/nullptr,
                                        tile_number,
                                        /*effects_region=*/changed_region,
                                        /*effect_limit=*/&effect.marker))
  {
    own_bake_refuse("job");
    return nullptr;
  }

  /* The job the bake asked for, found by its marker. A missing one is an effect whose effective
   * opacity laid nothing in this channel -- the walk skipped it, and the CPU compute answers it as
   * it always has. */
  const GpuEffectJob *target = nullptr;
  for (const GpuEffectJob &job : jobs) {
    if (job.channel == channel && BLI_uuid_equal(job.correction->effect_marker, effect.marker)) {
      target = &job;
      break;
    }
  }
  if (target == nullptr || target->result == nullptr) {
    /* The stage refuses nothing here -- a missing job is the marker's own cut (an effect that laid
     * nothing in this channel), and the CPU compute answers it as it always has. A stage that
     * refused already named its own stage; this one is the missing job, not a refusal. */
    own_bake_refuse("no_job");
    return nullptr;
  }

  /* The read back: the result texture holds the CPU effect buffer's own semantics (the tracks-alpha
   * rule the stage computed from the description, a plan that writes its own alpha carried through),
   * so the pixels move over as they are. The texture is the effect cache's own, at the input's
   * resolution -- the grid the cache's acquire keyed -- so the frame's buffer answers the same
   * float RGBA the CPU effect cache computes. */
  std::shared_ptr<ImBuf> result = frame.read_back(target->result);
  if (result == nullptr) {
    own_bake_refuse("readback");
    return nullptr;
  }
  return result;
}

void paint_material_combined_owned_gpu_register()
{
  BKE_paint_layers_owned_effect_gpu_provider_set(&owned_gpu_result_compute);
  /* The mask provider's own registration, the second hook this module answers: the provider
   * itself lives in paint_material_combined_mask_gpu.cc, split out of this file so each provider
   * holds its own terms. */
  paint_material_combined_mask_gpu_register();
  /* The compute shaders are made the first time a provider runs, which needs the context it
   * enables itself; a caller that already holds one current warms them here, so the first bake of
   * an owned effect -- or the first mask effect's grey -- does not pay the compile. */
  if (GPU_context_active_get() != nullptr) {
    paint_material_combined_gpu_shaders_ensure();
  }
}

}  // namespace blender::ed::material_combined
