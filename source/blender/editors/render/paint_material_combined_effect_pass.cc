/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edrend
 *
 * The render half of the GPU effect stage: each collected job's input laid into a canvas of its
 * own resolution, the job's pass plan dispatched over it through the shared #EffectPassBuffers,
 * and the result published into the GPU effect cache. Split out of
 * #paint_material_combined_effects.cc (#combined_preview_gpu_effects_run) so the collection and
 * the cache walk it feeds stay readable beside their own refusals.
 */

#include "paint_material_combined_effects.hh"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>

#include "MEM_guardedalloc.h"

#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_perf_debug.hh"
#include "BKE_paint_material_composite.hh"

#include "BLI_index_range.hh"
#include "BLI_math_base.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_material_types.h"
#include "DNA_object_types.h"

#include "GPU_state.hh"
#include "GPU_texture.hh"

#include "IMB_colormanagement.hh"


namespace blender::ed::material_combined {

bool combined_effect_jobs_render(const Material &ma,
                                 CombinedPasses &passes,
                                 Vector<GpuEffectJob> &r_jobs,
                                 const rcti *effects_region,
                                 int tile_number)
{
  /* The scope of the pass timer, kept as its own block the way the stage held it: the dispatches
   * below are what it measures, and a refusal out of them still ends the scope before the
   * return. */
  {
    bke::paint_perf::ScopedTimer perf_pass("gpu_effect.pass");
    /* The working space's luminance weights, which `IMB_colormanagement_get_luminance` reads: the
     * shader's own Rec.709 triple would grain a different grey in another space. */
    float luma[3];
    IMB_colormanagement_get_luminance_coefficients(luma);
    const float4 luma_coefficients(luma[0], luma[1], luma[2], 0.0f);

    /* The textures one job's plan runs over, routed by the shared #EffectPassBuffers: the rendered
     * input, the luminance a grayscale High Pass keeps, the two ring slots the window passes
     * ping-pong through, and the two float32 pairs the reduction and the flooding ping-pong
     * through on their own. */
    int rendered = 0;
    int last_kind = 0;
    int last_radius = 0;
    int last_passes = 0;
    rcti last_region;
    BLI_rcti_init(&last_region, 0, 0, 0, 0);
    bool last_full = true;

    /* Effects render in the order they were collected: an effect's input is laid from the results of
     * the ones before it, which are published by the time its own input is rendered. */
    for (GpuEffectJob &job : r_jobs) {
      if (!job.needs_render) {
        continue;
      }
      /* The scratch of this job alone, returned to the pool when the job ends -- the pass results
       * the later effects read live in the GPU result cache, so nothing here has to outlive the
       * publish below, and the next job of the same size reuses the textures instead of the frame
       * growing by a set of canvases per effect. */
      Vector<gpu::Texture *> job_scratch;
      BLI_SCOPED_DEFER([&]() {
        for (gpu::Texture *texture : job_scratch) {
          passes.pool.release_texture(texture);
        }
      });
      const auto job_acquire = [&](const int2 canvas_size) -> gpu::Texture * {
        gpu::Texture *texture = passes.pool.acquire_texture_2d(canvas_size,
                                                               1,
                                                               gpu::TextureFormat::SFLOAT_16_16_16_16,
                                                               passes.accum_usage,
                                                               "combined_effect_scratch");
        if (texture != nullptr) {
          job_scratch.append(texture);
        }
        return texture;
      };
      const int2 canvas = job.input_size;
      const int n = int(job.passes.size());

      /* The slot map a Channel Split's slot fetch reads as its aux: the image's own GPU texture,
       * the same acquisition every row's map goes through. A cache hit never dispatches, so its
       * slot map is not uploaded again, and the texture is borrowed from the image's cache rather
       * than released with the job's scratch -- the maps the input render below reads are borrowed
       * the same way. */
      for (const PaintLayerEffectPass &pass : job.passes) {
        if (pass.aux != ePaintLayerEffectPassAux::Slot) {
          continue;
        }
        bool premultiplied = false;
        job.slot_texture = gpu_map_texture(job.input_entry.slot_image, nullptr, premultiplied);
        if (job.slot_texture == nullptr) {
          return gpu_refuse("a Channel Split slot map has no GPU texture");
        }
        break;
      }

      /* The content the effect reads, laid into a canvas of the input's own resolution and texel
       * space. The input is rendered whole (no region): a capture has no region of its own, and its
       * cost is measured against the effect passes below -- if it dominates, a regional input is the
       * next step. */
      {
        bke::paint_perf::ScopedTimer perf_input("gpu_effect.input");
        CombinedTarget effect_target;
        /* The folder accumulators this target grows are its own: released when the job's scope ends, on
         * every path out of it, because the pool keeps a texture that is never released until the
         * context's pool is destroyed. */
        BLI_SCOPED_DEFER([&]() {
          for (gpu::Texture *texture : effect_target.folder_premul) {
            if (texture != nullptr) {
              passes.pool.release_texture(texture);
            }
          }
          for (gpu::Texture *texture : effect_target.folder_cover) {
            if (texture != nullptr) {
              passes.pool.release_texture(texture);
            }
          }
        });
        effect_target.size = canvas;
        effect_target.groups = math::divide_ceil(canvas, int2(16));
        effect_target.texel_to_uv = math::from_scale<float3x3, 2>(1.0f / float2(canvas));
        effect_target.texel_to_uv_4x4 = float4x4::identity();
        effect_target.texel_to_uv_4x4[0] = float4(
            effect_target.texel_to_uv[0].x, effect_target.texel_to_uv[0].y, 0.0f, 0.0f);
        effect_target.texel_to_uv_4x4[1] = float4(
            effect_target.texel_to_uv[1].x, effect_target.texel_to_uv[1].y, 0.0f, 0.0f);
        effect_target.texel_to_uv_4x4[3] = float4(
            effect_target.texel_to_uv[2].x, effect_target.texel_to_uv[2].y, 0.0f, 1.0f);
        /* The four textures the input's row_emit reads, held only while it lays: the defer
         * returns them the moment this block ends, so the passes below run against the input and
         * the ping-pong alone. */
        gpu::Texture *target_scratch[4] = {nullptr, nullptr, nullptr, nullptr};
        BLI_SCOPED_DEFER([&]() {
          for (gpu::Texture *texture : target_scratch) {
            if (texture != nullptr) {
              passes.pool.release_texture(texture);
            }
          }
        });
        const auto target_acquire = [&](const int2 canvas_size) -> gpu::Texture * {
          gpu::Texture *texture = passes.pool.acquire_texture_2d(canvas_size,
                                                                 1,
                                                                 gpu::TextureFormat::SFLOAT_16_16_16_16,
                                                                 passes.accum_usage,
                                                                 "combined_effect_scratch");
          for (gpu::Texture *&slot : target_scratch) {
            if (slot == nullptr) {
              slot = texture;
              break;
            }
          }
          return texture;
        };
        effect_target.row_color = target_acquire(canvas);
        effect_target.row_factor = target_acquire(canvas);
        effect_target.mask_accum = target_acquire(canvas);
        /* The capture returns before anything blends onto the destination, but the layer pass binds it
         * as an image all the same: a placeholder of its own keeps it from being the texture the same
         * dispatch samples. */
        effect_target.dest = target_acquire(int2(1));
        job.input = job_acquire(canvas);
        if (effect_target.row_color == nullptr || effect_target.row_factor == nullptr ||
            effect_target.mask_accum == nullptr || effect_target.dest == nullptr ||
            job.input == nullptr)
        {
          return gpu_refuse("an effect input texture could not be allocated");
        }
        /* An effect's input has no mask to build (the describe clears the row's): refuse rather than
         * rely on that, since the capture's store step would write a canvas into the one-texel
         * placeholder. */
        if (!job.input_entry.mask_items.is_empty() || job.input_entry.mask_image != nullptr) {
          return gpu_refuse("an effect input carries a mask");
        }
        if (!passes.row_emit(effect_target, job.input_entry, job.channel, -1, job.input, nullptr)) {
          return gpu_refuse("an effect input's row render failed");
        }
        /* The alpha the CPU buffer holds when the kernel is done: the content alpha where the channel
         * tracks one, one elsewhere (`eval_row_content`'s rule), because the correction pass divides the
         * result by it as the CPU evaluator straightens a float buffer. */
        /* A pass whose plan writes its own alpha (a Fill Area's coverage, a Drop Shadow's weight)
         * has the passes after it carry that alpha through the standard store -- a blur pair after
         * a tail keeps the coverage it softened, exactly the CPU buffer's own rule. */
        const bool pass_writes_alpha = std::any_of(job.passes.begin(),
                                                   job.passes.end(),
                                                   [](const PaintLayerEffectPass &pass) {
                                                     return pass.alpha_from_kernel;
                                                   });
        job.tracks_alpha = job.input_entry.tracks_content_alpha ||
                           job.channel == PAINT_MATERIAL_CHANNEL_NORMAL || pass_writes_alpha;
        GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);
        perf_input.note("kind=%d size=%dx%d full=1", int(job.correction->filter), canvas.x, canvas.y);
      }

      /* The luminance buffer a grayscale High Pass keeps, and the two ring slots the window
       * passes ping-pong through. Each is acquired only when the plan uses it: a box blur needs no
       * luminance buffer, a pointwise Noise needs neither, and the job's scratch defer above hands
       * every one of them back to the pool when the job's scope ends. */
      gpu::Texture *scratch[3] = {nullptr, nullptr, nullptr};
      if (!job.passes.is_empty() && job.passes[0].kind == ePaintLayerEffectPass::Luminance) {
        scratch[0] = job_acquire(canvas);
        if (scratch[0] == nullptr) {
          return gpu_refuse("an effect scratch texture could not be allocated");
        }
      }
      if (n > 1) {
        scratch[1] = job_acquire(canvas);
        scratch[2] = job_acquire(canvas);
        if (scratch[1] == nullptr || scratch[2] == nullptr) {
          return gpu_refuse("an effect scratch texture could not be allocated");
        }
      }

      /* The float32 pairs the two whole-canvas effects run through, acquired only when the plan
       * names the passes that use them: the Color Match's reduction pyramid sums its levels up
       * from the input -- sums a 4K canvas overflows a half float with -- and the Fill Area's
       * label field stamps coordinates the width of the canvas, past what a half float numbers
       * exactly. Both are canvas-sized, so every pyramid level and every field fits into one, and
       * the job's scratch defer above hands them back to the pool with the rest. */
      bool plan_reduces = false;
      bool plan_floods = false;
      for (const PaintLayerEffectPass &pass : job.passes) {
        plan_reduces |= pass.kind == ePaintLayerEffectPass::ReduceStart ||
                        pass.kind == ePaintLayerEffectPass::ReduceStep;
        plan_floods |= pass.kind == ePaintLayerEffectPass::FloodInit ||
                       pass.kind == ePaintLayerEffectPass::FloodStep;
      }
      const auto job_acquire_pair = [&](gpu::Texture *(&pair)[2], const char *name) -> bool {
        for (gpu::Texture *&half : pair) {
          half = passes.pool.acquire_texture_2d(canvas,
                                                1,
                                                gpu::TextureFormat::SFLOAT_32_32_32_32,
                                                passes.accum_usage,
                                                name);
          if (half == nullptr) {
            return false;
          }
          job_scratch.append(half);
        }
        return true;
      };
      BLI_SCOPED_DEFER([&]() {
        /* The pairs went back to the pool with the scratch above; the job keeps no pointers past
         * this scope. */
        job.reduce_pair[0] = nullptr;
        job.reduce_pair[1] = nullptr;
        job.field_pair[0] = nullptr;
        job.field_pair[1] = nullptr;
        job.field_texture = nullptr;
        job.field_placeholder = nullptr;
      });
      job.field_placeholder = passes.pool.acquire_texture_2d(int2(1),
                                                             1,
                                                             gpu::TextureFormat::SFLOAT_32_32_32_32,
                                                             passes.accum_usage,
                                                             "combined_effect_field_placeholder");
      if (job.field_placeholder == nullptr) {
        return gpu_refuse("an effect field placeholder could not be allocated");
      }
      job_scratch.append(job.field_placeholder);
      if (plan_reduces && !job_acquire_pair(job.reduce_pair, "combined_effect_reduce")) {
        return gpu_refuse("an effect reduction texture could not be allocated");
      }
      if (plan_floods && !job_acquire_pair(job.field_pair, "combined_effect_field")) {
        return gpu_refuse("an effect field texture could not be allocated");
      }

      /* The dispatch rectangle of every pass: the dirty region grown by the reach the passes
       * after it still read, per axis, and by the halo of the effects before this one in the
       * row. The last pass writes the cache over the whole dirty + halo band. A render that is
       * not region exact -- or a first render, whose dirty region is the whole canvas -- runs
       * over the canvas instead. */
      Vector<rcti> pass_rects(n);
      {
        bke::paint_perf::ScopedTimer perf_region("gpu_effect.region");
        BKE_paint_layers_effect_pass_regions(
            Span<const PaintLayerEffectPass>(job.passes.data(), job.passes.size()),
            job.region,
            job.predecessor_halo,
            canvas.x,
            canvas.y,
            MutableSpan<rcti>(pass_rects.data(), pass_rects.size()));
        perf_region.note("kind=%d region=%dx%d full=%d halo=%d provider=%d",
                         int(job.correction->filter),
                         BLI_rcti_size_x(&pass_rects[n - 1]),
                         BLI_rcti_size_y(&pass_rects[n - 1]),
                         job.region_exact ? 0 : 1,
                         job.halo,
                         /* The caller's rectangle widened this job's dirty band (it is part of the
                          * region printed above, unioned into the cache's own). */
                         (effects_region != nullptr && !BLI_rcti_is_empty(effects_region)) ? 1 : 0);
      }
      EffectPassBuffers buffers(canvas,
                                job.input,
                                scratch[0],
                                scratch[1],
                                scratch[2],
                                job.result,
                                job.slot_texture,
                                job.reduce_pair[0],
                                job.reduce_pair[1],
                                job.field_pair[0],
                                job.field_pair[1],
                                job.field_texture);
      for (const PaintLayerEffectPass &pass : job.passes) {
        gpu::Texture *src = buffers.source(pass.source);
        /* A plan that names a source role the buffers hold no texture for would bind null at the
         * dispatch and crash the backend on it (the Blur Slope's driver fell here once, its source
         * read after the pass it is). The correct plans never reach this, so the host refuses the
         * preview instead of relying on a future plan edit to keep holding. */
        if (src == nullptr) {
          return gpu_refuse("an effect pass has no source texture");
        }
        gpu::Texture *dst = buffers.destination(pass);
        /* The reduction and the flooding write their float32 texture through the field slot: the
         * pass's own destination goes there, and the 16F result slot keeps a valid binding through
         * the input canvas, which those kernels never write. */
        gpu::Texture *field_dst = nullptr;
        if (ELEM(pass.kind,
                 ePaintLayerEffectPass::ReduceStart,
                 ePaintLayerEffectPass::ReduceStep,
                 ePaintLayerEffectPass::FloodInit,
                 ePaintLayerEffectPass::FloodStep))
        {
          field_dst = dst;
          dst = job.input;
        }
        /* The Color Match's grade is graded by the pyramid's one-texel sum: its aux is the level
         * the reduction last wrote, whatever role the plan names the pass. */
        gpu::Texture *aux = (pass.kind == ePaintLayerEffectPass::MatchGrade) ?
                                buffers.reduce_final() :
                                buffers.aux(pass.aux);
        passes.effect_pass_dispatch(
            {job,
             pass,
             src,
             aux,
             dst,
             field_dst,
             buffers.rect_for(pass, job.region_exact, job.region, pass_rects)},
            luma_coefficients);
        buffers.commit(pass, field_dst != nullptr ? field_dst : dst);
      }

      /* The versions are stamped after the render, so an edit that landed while it ran is newer than
       * this entry and costs one more update rather than a result that never clears. The hashes are
       * the ones the entry was keyed on above. */
      BKE_paint_layers_effect_gpu_cache_publish(ma,
                                                *job.effect,
                                                job.channel,
                                                job.input_size.x,
                                                job.input_size.y,
                                                job.images,
                                                PaintLayerEffectKeyExtras{job.chain_hash,
                                                                          job.input_scalar_hash},
                                                tile_number);
      rendered++;
      last_kind = int(job.correction->filter);
      last_radius = job.halo;
      last_passes = n;
      last_region = buffers.rect_for(job.passes[n - 1], job.region_exact, job.region, pass_rects);
      last_full = !job.region_exact || (BLI_rcti_size_x(&job.region) >= canvas.x &&
                                        BLI_rcti_size_y(&job.region) >= canvas.y);
    }
    if (rendered > 0) {
      /* The correction pass samples the results next. */
      GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH);
    }
    perf_pass.note("effects=%d kind=%d halo=%d passes=%d region=%dx%d full=%d",
                   rendered,
                   last_kind,
                   last_radius,
                   last_passes,
                   BLI_rcti_size_x(&last_region),
                   BLI_rcti_size_y(&last_region),
                   last_full ? 1 : 0);
  }

  return true;
}

}  // namespace blender::ed::material_combined
