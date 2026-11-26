/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edrend
 *
 * The mask-effect GPU provider: the editors' registration of the effect pass plan as
 * #BKE_paint_layers_mask_effect_gpu_provider_set's answer, split out of
 * paint_material_combined_owned_gpu.cc so each provider holds its own terms. A mask stack's
 * effect item (a Blur, a Noise, a Drop Shadow laid as a mask) hands the grey the mask has built up
 * to it, and the effect's own GPU pass plan runs over that canvas directly -- no row render, no
 * result cache; the grey is uploaded once, the plan dispatches full-canvas, and the last pass is
 * read back into the caller's buffer with the CPU kernel's own grey semantics. Every refusal
 * answers false, which the caller answers with the CPU kernel.
 */

#include "paint_material_combined_owned_gpu.hh"

#include <algorithm>
#include <cstdio>

#include "MEM_guardedalloc.h"

#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_perf_debug.hh"

#include "BLI_index_range.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_span.hh"
#include "BLI_threads.h"
#include "BLI_vector.hh"

#include "DNA_material_types.h"

#include "GPU_state.hh"
#include "GPU_texture.hh"
#include "GPU_texture_pool.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf_types.hh"

#include "ED_material_combined.hh"

#include "paint_material_combined_effects.hh"

namespace blender::ed::material_combined {

/** The mask provider's own refusal stage, the same terms as #own_bake_refuse. */
static void mask_bake_refuse(const char *stage)
{
  static const char *last_stage = nullptr;
  if (!bke::paint_perf::enabled() || last_stage == stage) {
    return;
  }
  last_stage = stage;
  printf("[PBR-PERF] mask_effect.own_bake.refuse stage=%s\n", stage);
  fflush(stdout);
}

/* -------------------------------------------------------------------- */
/** \name Mask Effect GPU Provider
 *
 * The editors' answer to #BKE_paint_layers_mask_effect_gpu_provider_set: a mask stack's effect item
 * (a Blur, a Noise, a Drop Shadow laid as a mask) hands the grey the mask has built up to it, and
 * the effect's own GPU pass plan runs over that canvas directly. No row render and no GPU result
 * cache stand behind it -- the input is the caller's buffer, uploaded once before any dispatch (the
 * contract lets the result alias the input, which this order keeps safe), the plan dispatches
 * full-canvas, and the last pass is read back into the caller's buffer.
 *
 * The CPU kernel the provider replaces runs with `for_mask` set, and the kinds it special-cases are
 * matched on the GPU by their parameters, not by new kernels:
 * - Drop Shadow, Mask Outline, Fill Area, Bevel read their shape from the luminance rather than the
 *   alpha (`for_mask` in #paint_layer_effect_kernel_apply). The source slot is overridden in the
 *   packed parameters before the plan is built, so the seed / silhouette / border passes read the
 *   grey and the tail's under-mode folds off, exactly the CPU branch takes.
 * - The kinds whose result is a layer of its own (#PAINT_LAYER_KIND_RESULT_HAS_ALPHA) leave a
 *   premultiplied colour with the weight in alpha from their GPU tails, where the CPU `for_mask`
 *   branch mixes into the grey and keeps it grey. The readback below folds those per kind: the Drop
 *   Shadow over-composites the premultiplied tail onto the grey it was given (the caller's grey
 *   normalize -- the kind is #PAINT_LAYER_KIND_MASK_NEEDS_LUMA -- then reads the mixed grey exactly
 *   as the CPU's does), and the Mask Outline reads the band back out of the alpha its tail wrote
 *   (not a luma kind, so the caller touches nothing; the band is the CPU's grey result).
 * - Every other kind runs grey-in-grey-out, the CPU's own answer.
 *
 * What is not answered here falls back to the CPU kernel with `false`: the thread, context, shader
 * and gate refusals, a kind whose plan is empty (the plan builder's own refusals -- a picked Color
 * Match source, the iteration-less source mode), a Fill Area (the CPU mask branch blurs the mixed
 * grey, the GPU plan blurs the coverage -- not the same filter, see
 * #mask_gpu_result_compute), a cheap plan whose fixed provider cost outweighs the CPU kernel (see
 * the pass-count gate in #mask_gpu_result_compute), a Channel Split (its slot fetch reads the row's
 * slot image through the effect stage's describe, and the provider holds no material to describe
 * with or tile to key the slot texture by), and every allocation or read-back that came up short.
 * \{ */

/**
 * The scratch the mask provider's plan runs over is routed by the shared #EffectPassBuffers
 * (paint_material_combined_effects.hh) -- the class the preview's effect stage walks the same roles
 * through, so the two dispatchers cannot drift apart again. The provider's one difference is pinned
 * by the class's strict-rectangles flag: its grey canvas has no region of its own, so every
 * ordinary pass dispatches whole.
 */

/**
 * Compute a mask effect's kernel on the GPU. The grey in \a grey_input is uploaded whole into a
 * texture before any dispatch, the effect's pass plan runs over it full-canvas, and the last pass's
 * texture is read back into \a r_result -- which may alias \a grey_input, the per-texel write below
 * reading its grey before it writes it. \a r_result leaves with the CPU kernel's `for_mask` result:
 * RGB grey, alpha one, and no grey normalize -- the caller lays that for the kinds the kind table
 * flags #PAINT_LAYER_KIND_MASK_NEEDS_LUMA.
 *
 * False on any refusal, and the caller runs the CPU kernel as it always has. Each refusal names
 * its stage in the `BLENDER_PBR_PERF` log (see #mask_bake_refuse).
 */
static bool mask_gpu_result_compute(const MaterialPaintLayer &effect,
                                    const ImBuf &grey_input,
                                    ImBuf &r_result)
{
  /* The provider's own gates, all cheap and none of them needing a GPU, each refusing with its
   * stage named. */
  if (!BLI_thread_is_main()) {
    mask_bake_refuse("not_main");
    return false;
  }
  if (!combined_preview_gpu_enabled()) {
    mask_bake_refuse("not_enabled");
    return false;
  }
  if (!BKE_paint_layers_effect_is_gpu_computed(effect)) {
    /* The kind has no pass plan of its own, the same terms the owned provider refuses one by. */
    mask_bake_refuse("plan");
    return false;
  }
  const int width = grey_input.x;
  const int height = grey_input.y;
  if (width <= 0 || height <= 0 || grey_input.float_buffer.data == nullptr ||
      r_result.x != width || r_result.y != height || r_result.float_buffer.data == nullptr)
  {
    /* The caller's grey canvas is the provider's whole input contract, so an unusable one is the
     * describe refusing. */
    mask_bake_refuse("describe");
    return false;
  }

  bke::paint_perf::ScopedTimer perf_pass("mask_effect.gpu.passes");

  /* The parameters, packed the way the describe packs a correction's, with the for-mask source
   * slots forced to the luminance before the plan reads them: BKE's one answer for both. */
  const eMaterialPaintLayerFilter filter = eMaterialPaintLayerFilter(effect.kind_subtype);
  if (filter == MA_PAINT_LAYER_FILTER_FILL_AREA) {
    /* The CPU mask branch blurs the mixed grey, while the GPU plan blurs the coverage in alpha and
     * mixes the texel's own grey after the read-back. `blur(mix(grey, fill, cover))` and
     * `mix(grey, fill, blur(cover))` are not the same filter, so the mask path stays on the CPU
     * until an exact GPU plan exists. The owned (non-mask) path is the other branch and keeps its
     * GPU result: its CPU kernel blurs the same coverage the plan does. */
    mask_bake_refuse("fill_area");
    return false;
  }
  PaintMaterialCompositeCorrection correction;
  correction.filter = int16_t(effect.kind_subtype);
  BKE_paint_layers_effect_mask_filter_params(effect, correction.filter_params);

  /* The plan, and the halo with it. The dispatches run full-canvas, so the halo is bookkeeping
   * here -- a plan the builder refused (a picked Color Match source, the Fill Area's
   * largest-region classification, its debug ramp, the iteration-less source mode) comes back
   * empty, and the CPU kernel answers the kind as it always has. */
  Vector<PaintLayerEffectPass> passes;
  BKE_paint_layers_effect_pass_plan(filter, correction.filter_params, width, height, passes);
  if (passes.is_empty()) {
    mask_bake_refuse("plan");
    return false;
  }
  const int halo = BKE_paint_layers_effect_halo(filter, correction.filter_params, width, height);
  /* The provider's fixed cost -- the grey upload, the full-canvas dispatches, GPU_finish and the
   * whole-canvas read-back -- only pays for a plan that makes the CPU kernel walk many passes.
   * Measured on 2K masks: Bevel (15 passes) 134 ms CPU -> 92 ms GPU, while Blur (2) 41 -> 69 and
   * Drop Shadow (6) 31 -> 90. A cheap plan stays on the CPU; a future asynchronous read-back may
   * lower the bar, and the threshold is the one place to change then. */
  if (passes.size() < 10) {
    mask_bake_refuse("cheap");
    return false;
  }
  /* A Channel Split's slot fetch reads the row's slot image through the effect stage's describe;
   * the provider holds no material to describe with and no tile to key the slot texture by, so the
   * CPU kernel answers the kind. */
  for (const PaintLayerEffectPass &pass : passes) {
    if (pass.aux == ePaintLayerEffectPassAux::Slot) {
      mask_bake_refuse("slot");
      return false;
    }
  }

  /* The bake and the composite ask outside a draw, where no GPU context is current: the frame
   * enables-and-restores the context, ensures the shaders and acquires the placeholders the pass
   * binds when it reads no aux, the row render's own terms (see #CombinedGpuFrame). */
  gpu::TexturePool &pool = gpu::TexturePool::get();
  /* Only the result is read back, so only it carries HOST_READ (the flag names the usage a data
   * read takes; a scratch canvas the dispatches alone sample has no reason to). */
  const eGPUTextureUsage usage = GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_SHADER_WRITE;
  const eGPUTextureUsage readback_usage = usage | GPU_TEXTURE_USAGE_HOST_READ;
  CombinedGpuFrame frame(pool);
  if (!frame.ok) {
    mask_bake_refuse(frame.fail_stage);
    return false;
  }
  Vector<gpu::Texture *> scratch;
  BLI_SCOPED_DEFER([&]() {
    for (gpu::Texture *texture : scratch) {
      pool.release_texture(texture);
    }
  });
  const auto acquire_with_usage = [&](const char *name,
                                      const eGPUTextureUsage texture_usage) -> gpu::Texture * {
    gpu::Texture *texture = pool.acquire_texture_2d(
        int2(width, height), 1, gpu::TextureFormat::SFLOAT_16_16_16_16, texture_usage, name);
    if (texture != nullptr) {
      scratch.append(texture);
    }
    return texture;
  };
  const auto acquire = [&](const char *name) -> gpu::Texture * {
    return acquire_with_usage(name, usage);
  };
  const auto acquire_pair = [&](const char *name,
                                const gpu::TextureFormat format,
                                gpu::Texture *(&pair)[2]) -> bool {
    for (gpu::Texture *&half : pair) {
      half = pool.acquire_texture_2d(int2(width, height), 1, format, usage, name);
      if (half == nullptr) {
        return false;
      }
      scratch.append(half);
    }
    return true;
  };

  gpu::Texture *input = acquire("combined_mask_input");
  gpu::Texture *result = acquire_with_usage("combined_mask_result", readback_usage);
  const int n = int(passes.size());
  /* The luminance a grayscale High Pass pre-pass keeps, and the ring the window passes ping-pong
   * through: each acquired only when the plan uses it, like the preview's job scratch. */
  gpu::Texture *luminance = (passes[0].kind == ePaintLayerEffectPass::Luminance) ?
                                acquire("combined_mask_luminance") :
                                nullptr;
  gpu::Texture *ring[2] = {nullptr, nullptr};
  if (n > 1) {
    ring[0] = acquire("combined_mask_ring");
    ring[1] = acquire("combined_mask_ring");
  }
  bool plan_reduces = false;
  bool plan_floods = false;
  for (const PaintLayerEffectPass &pass : passes) {
    plan_reduces |= ELEM(pass.kind,
                         ePaintLayerEffectPass::ReduceStart,
                         ePaintLayerEffectPass::ReduceStep);
    plan_floods |= ELEM(pass.kind,
                        ePaintLayerEffectPass::FloodInit,
                        ePaintLayerEffectPass::FloodStep);
  }
  gpu::Texture *reduce[2] = {nullptr, nullptr};
  gpu::Texture *field[2] = {nullptr, nullptr};
  bool acquired = input != nullptr && result != nullptr;
  if (acquired && passes[0].kind == ePaintLayerEffectPass::Luminance) {
    acquired = luminance != nullptr;
  }
  if (acquired && n > 1) {
    acquired = ring[0] != nullptr && ring[1] != nullptr;
  }
  if (acquired && plan_reduces) {
    acquired = acquire_pair(
        "combined_mask_reduce", gpu::TextureFormat::SFLOAT_32_32_32_32, reduce);
  }
  if (acquired && plan_floods) {
    /* The label field is a 2D coordinate: RG32F, half the bytes of the reduction's RGBA32F pair. */
    acquired = acquire_pair("combined_mask_field", effect_field_texture_format, field);
  }
  if (!acquired) {
    mask_bake_refuse("scratch");
    return false;
  }

  /* The grey goes whole into its texture before any dispatch: the contract lets the result alias
   * the input, and this order is what keeps every texel's source fixed before the first write. */
  GPU_texture_update(input, GPU_DATA_FLOAT, grey_input.float_buffer.data);
  /* The upload is read by the dispatches that follow. */
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE);

  /* The job stub the dispatch helper reads its pushes from: the input's size, the packed
   * parameters, and whether the plan writes an alpha of its own. That last one is the preview's
   * own rule for the same situation -- a tail that names its alpha (a Drop Shadow's weight, a Mask
   * Outline's band, a Fill Area's coverage) has the passes after it carry that alpha through the
   * standard store -- and the Fill Area's blur pair needs it: the coverage it softens rides in the
   * alpha the tail wrote. A grey input carries one everywhere, so the carried alpha stays one on
   * every kind that writes none. */
  GpuEffectJob job;
  job.correction = &correction;
  job.input_size = int2(width, height);
  const bool pass_writes_alpha = std::any_of(passes.begin(),
                                             passes.end(),
                                             [](const PaintLayerEffectPass &pass) {
                                               return pass.alpha_from_kernel;
                                             });
  job.tracks_alpha = pass_writes_alpha;
  /* The dispatch helper binds the float32 slots of every pass, not only the flooding's own
   * destination: a pass that reads no field gets these placeholders, the way the preview's own job
   * scratch does (#GpuEffectJob::field_placeholder / #GpuEffectJob::flood_placeholder), so a null
   * binding never reaches an image slot of the effect shader. The reduction slot is RGBA32F, the
   * label-field slot RG32F (#effect_field_texture_format). */
  job.field_placeholder = pool.acquire_texture_2d(
      int2(1), 1, gpu::TextureFormat::SFLOAT_32_32_32_32, usage, "combined_mask_field_placeholder");
  if (job.field_placeholder == nullptr) {
    mask_bake_refuse("scratch");
    return false;
  }
  scratch.append(job.field_placeholder);
  job.flood_placeholder = pool.acquire_texture_2d(int2(1),
                                                  1,
                                                  effect_field_texture_format,
                                                  usage,
                                                  "combined_mask_flood_placeholder");
  if (job.flood_placeholder == nullptr) {
    mask_bake_refuse("scratch");
    return false;
  }
  scratch.append(job.flood_placeholder);

  Vector<gpu::Texture *> filter_luts;
  Vector<GpuSliceSlot> slice_slots;
  Vector<GpuMaskSlot> mask_slots;
  Vector<GpuEffectResult> effect_results;
  CombinedPasses pass_dispatcher(pool,
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

  /* The dispatches: one pass of the plan, the roles routed by the shared #EffectPassBuffers. The
   * window runs over the whole canvas -- there is no dirty region to narrow to, and the halo the
   * plan computes is what a partial dispatch would have grown the band by. Two kinds cannot take the
   * whole canvas: the flooding steps walk each line sequentially through a read-write field, one
   * thread to a line (more would race the walk against itself), and the reduction levels dispatch
   * their own pyramid size, the same rectangles the preview's effect stage clips these two to. */
  rcti empty_region;
  BLI_rcti_init(&empty_region, 0, 0, 0, 0);
  /* The field the flooding last wrote lands in this local, the preview job's own slot's terms. */
  gpu::Texture *field_current = nullptr;
  EffectPassBuffers buffers(int2(width, height),
                            input,
                            luminance,
                            ring[0],
                            ring[1],
                            result,
                            /*slot=*/nullptr,
                            reduce[0],
                            reduce[1],
                            field[0],
                            field[1],
                            field_current);
  float luma[3];
  /* The working space's luminance weights, the ones the CPU kernels read through
   * `IMB_colormanagement_get_luminance` -- the same push the preview's effect stage makes. */
  IMB_colormanagement_get_luminance_coefficients(luma);
  const float4 luma_coefficients(luma[0], luma[1], luma[2], 0.0f);

  for (const PaintLayerEffectPass &pass : passes) {
    gpu::Texture *dst = buffers.destination(pass);
    /* The reduction and the flooding write their float32 texture through the field slot; the 16F
     * result slot keeps a valid binding through the input canvas, which those kernels never
     * write. */
    gpu::Texture *field_dst = nullptr;
    if (ELEM(pass.kind,
             ePaintLayerEffectPass::ReduceStart,
             ePaintLayerEffectPass::ReduceStep,
             ePaintLayerEffectPass::FloodInit,
             ePaintLayerEffectPass::FloodStep))
    {
      field_dst = dst;
      dst = input;
    }
    /* The Color Match's grade is graded by the pyramid's one-texel sum: its aux is the level the
     * reduction last wrote, whatever role the plan names the pass. */
    gpu::Texture *aux = (pass.kind == ePaintLayerEffectPass::MatchGrade) ?
                            buffers.reduce_final() :
                            buffers.aux(pass.aux);
    pass_dispatcher.effect_pass_dispatch(
        {job,
         pass,
         buffers.source(pass.source),
         aux,
         dst,
         field_dst,
         /* No dirty region here: the strict flag makes every ordinary pass whole, the same way
          * the preview's region-exact rule narrows its own. */
         buffers.rect_for(pass, /*region_exact=*/false, empty_region, {})},
        luma_coefficients);
    buffers.commit(pass, field_dst != nullptr ? field_dst : dst);
  }

  /* The read-back: the last pass wrote the result texture, the one canvas acquired with the
   * HOST_READ flag the data read takes (see #readback_usage above). */
  GPU_finish();
  const float *grey = grey_input.float_buffer.data;
  if (grey == nullptr) {
    /* Refused rather than dereferenced below: without the grey the fold has nothing to read. */
    mask_bake_refuse("no grey");
    return false;
  }
  /* Decide whether the two buffers alias *before* the write handle is taken. `float_data_for_write`
   * may clone a shared buffer, so comparing against the pointer it returns would call an aliased
   * grey distinct; comparing the raw buffers keeps the decision independent of the implicit-sharing
   * semantics. A grey that aliases the result is folded from a separate read-back buffer -- the
   * read would otherwise overwrite the very grey the fold mixes in. */
  const bool aliases = grey_input.float_buffer.data == r_result.float_buffer.data;
  float *out = r_result.float_data_for_write();
  const int64_t texels = int64_t(width) * height;
  /* The read lands in the caller's buffer when the grey does not alias it: the fold's sources are
   * each texel's own, so it reads in place, and the whole-canvas MEM copy (256 MiB at 4K) is
   * saved. */
  float *pixels = out;
  bool pixels_owned = false;
  if (aliases) {
    pixels = static_cast<float *>(GPU_texture_read(result, GPU_DATA_FLOAT, 0));
    if (pixels == nullptr) {
      mask_bake_refuse("readback");
      return false;
    }
    pixels_owned = true;
  }
  else {
    GPU_texture_read(result, GPU_DATA_FLOAT, 0, out);
  }
  BLI_SCOPED_DEFER([&]() {
    if (pixels_owned) {
      MEM_delete(pixels);
    }
  });

  /* The readback lands in the caller's buffer with the CPU kernel's own `for_mask` semantics. The
   * kinds whose GPU tail leaves a premultiplied colour and a weight in alpha are folded here:
   * - Drop Shadow, Fill Area: over-composite the tail onto the grey it was given. The tail's RGB
   *   is the tint premultiplied by the weight (`rgb = color * weight`, the weight clamped into
   *   [0, 1], which the silhouette and the opacity stay inside of), so `grey * (1 - alpha) + rgb`
   *   is the CPU branch's `grey + (color - grey) * weight` per channel. The caller's grey
   *   normalize (both kinds are #PAINT_LAYER_KIND_MASK_NEEDS_LUMA) then reads the same mixed grey
   *   the CPU's does. The Fill Area's soft edge differs by the grey's own blur -- the CPU blurs
   *   the mixed buffer, the GPU folds the coverage's blur and mixes the texel's own grey back --
   *   the same class of approximation the pass plan's other kernels accept.
   * - Mask Outline: the tail wrote the band as its own alpha over the tint, and the band *is* the
   *   CPU branch's result (the band, blurred, in the grey). Not a luma kind, so the caller's
   *   normalize would leave a premultiplied tint in place -- the band comes back out of the alpha
   *   here.
   * Every other kind leaves the RGB as the kernel wrote it: a grey in, a grey out, and the kinds
   * that can colour one (a Noise, a Glow, a Color Match) are the ones the caller normalizes. */
  /* Unreachable under the current gates: Fill Area refuses above, and Drop Shadow's plan is six
   * passes, below the cheap-plan gate. Kept so lowering the gate keeps the CPU mask branch's fold;
   * a test must land with that change. */
  const bool over_grey = ELEM(filter,
                              MA_PAINT_LAYER_FILTER_DROP_SHADOW,
                              MA_PAINT_LAYER_FILTER_FILL_AREA);
  const bool band_from_alpha = filter == MA_PAINT_LAYER_FILTER_MASK_OUTLINE;
  for (const int64_t i : IndexRange(texels)) {
    /* The grey reads before the write: the two buffers may alias, and each texel's source is its
     * own texel alone. */
    const float grey_rgb[3] = {grey[i * 4 + 0], grey[i * 4 + 1], grey[i * 4 + 2]};
    const float4 texel(pixels[i * 4 + 0], pixels[i * 4 + 1], pixels[i * 4 + 2], pixels[i * 4 + 3]);
    if (over_grey) {
      const float keep = 1.0f - texel.w;
      out[i * 4 + 0] = grey_rgb[0] * keep + texel.x;
      out[i * 4 + 1] = grey_rgb[1] * keep + texel.y;
      out[i * 4 + 2] = grey_rgb[2] * keep + texel.z;
    }
    else if (band_from_alpha) {
      out[i * 4 + 0] = texel.w;
      out[i * 4 + 1] = texel.w;
      out[i * 4 + 2] = texel.w;
    }
    else {
      out[i * 4 + 0] = texel.x;
      out[i * 4 + 1] = texel.y;
      out[i * 4 + 2] = texel.z;
    }
    out[i * 4 + 3] = 1.0f;
  }
  perf_pass.note("kind=%d size=%dx%d passes=%d halo=%d tracks_alpha=%d",
                 int(filter),
                 width,
                 height,
                 n,
                 halo,
                 job.tracks_alpha ? 1 : 0);
  return true;
}

/** \} */

/**
 * Register the mask-effect GPU provider with BKE, the owned-effect provider's own terms: called
 * by #paint_material_combined_owned_gpu_register, the one register the module's caller holds.
 */
void paint_material_combined_mask_gpu_register()
{
  BKE_paint_layers_mask_effect_gpu_provider_set(&mask_gpu_result_compute);
}

}  // namespace blender::ed::material_combined
