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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>

#include "MEM_guardedalloc.h"

#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_perf_debug.hh"
#include "BKE_paint_material_composite.hh"

#include "BLI_index_range.hh"
#include "BLI_fileops.h"
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

/**
 * `BLENDER_PBR_EFFECT_REGION=1` (or `true`) enables the regional effect input render: a pass plan
 * whose region can be narrowed lays only the stage the plan reads, instead of the whole canvas. A
 * developer switch read once per process, off by default, so the whole-canvas path is the one
 * every ordinary run takes and the region arithmetic stays a comparison.
 */
static bool combined_effect_input_region_enabled()
{
  static const bool enabled = []() {
    const char *env = std::getenv("BLENDER_PBR_EFFECT_REGION");
    if (env == nullptr) {
      return false;
    }
    return std::strcmp(env, "1") == 0 || std::strcmp(env, "true") == 0;
  }();
  return enabled;
}

/**
 * A developer A/B of the regional input render: `BLENDER_PBR_EFFECT_REGION_TEST` names a rectangle
 * `xmin,ymin,xmax,ymax` in canvas texels and `BLENDER_PBR_EFFECT_REGION_TEST_FILE` a control file.
 * While that file exists every plan job is forced to re-render, with its dirty region replaced by
 * the rectangle, so a first draw (file absent) is the whole canvas and a second (file present) is
 * the narrowed stage -- the two must show the same pixels. Read once; off unless both are set.
 */
struct EffectRegionTest {
  bool configured = false;
  const char *file = nullptr;
  rcti rect = {};

  bool active() const
  {
    return configured && BLI_exists(file);
  }
};

static const EffectRegionTest &effect_region_test()
{
  static const EffectRegionTest test = []() {
    EffectRegionTest value;
    const char *rect_env = std::getenv("BLENDER_PBR_EFFECT_REGION_TEST");
    const char *file_env = std::getenv("BLENDER_PBR_EFFECT_REGION_TEST_FILE");
    if (rect_env == nullptr || file_env == nullptr) {
      return value;
    }
    int xmin = 0, ymin = 0, xmax = 0, ymax = 0;
    if (std::sscanf(rect_env, "%d,%d,%d,%d", &xmin, &ymin, &xmax, &ymax) != 4) {
      return value;
    }
    value.configured = true;
    value.file = file_env;
    BLI_rcti_init(&value.rect, xmin, xmax, ymin, ymax);
    return value;
  }();
  return test;
}

/**
 * Whether the described input lays a correction whose result is a field over the canvas rather
 * than a function of its own texel: a Deband's grain is hashed from the canvas coordinates, so a
 * stage-local texel would shift it and the narrowed render would differ from the whole one. The
 * region guard cannot see the input description, only the pass plan, so the caller refuses the
 * region here.
 */
static bool combined_effect_input_has_canvas_correction(
    const PaintMaterialCompositeImageLayer &layer)
{
  /* The row's own adjustment filter runs through the same texel-keyed branch: a Deband/dither
   * adjustment hashes its grain from the canvas texel, so a stage-local texel would shift it. */
  if (layer.adjustment_filter == MA_PAINT_LAYER_FILTER_DEBAND) {
    return true;
  }
  for (const PaintMaterialCompositeCorrection &correction : layer.content_corrections) {
    if (correction.filter == MA_PAINT_LAYER_FILTER_DEBAND) {
      return true;
    }
  }
  for (const PaintMaterialCompositeImageLayer &child : layer.children) {
    if (combined_effect_input_has_canvas_correction(child)) {
      return true;
    }
  }
  return false;
}

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

    /* The developer A/B of the regional stage: while its control file exists, every plan job
     * re-renders with the test rectangle as its dirty region. The first draw (no file) is the
     * whole canvas, the second one the narrowed stage; the project's pixel check compares them. */
    const EffectRegionTest &region_test = effect_region_test();
    const bool region_test_active = region_test.active();
    if (region_test_active) {
      for (GpuEffectJob &job : r_jobs) {
        if (job.baked_result || job.input_size.x <= 0 || job.input_size.y <= 0) {
          continue;
        }
        rcti rect = region_test.rect;
        rcti canvas_rect;
        BLI_rcti_init(&canvas_rect, 0, job.input_size.x, 0, job.input_size.y);
        if (!BLI_rcti_isect(&rect, &canvas_rect, &rect) || BLI_rcti_is_empty(&rect)) {
          continue;
        }
        job.region = rect;
        /* The A/B is only meaningful on an input the scene can describe in the canvas' own texel
         * grid; the probe turns the rows' identity mappings off, so the test says so in place of
         * whatever predicate refused the band. */
        job.region_exact = true;
        job.needs_render = true;
      }
    }

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
      if (job.baked_result) {
        /* A baked job runs no plan -- its passes are empty, so the ping-pong, the input render and
         * the region arithmetic below are all bypassed: the CPU effect cache's buffer is the whole
         * result, and it goes into the cache texture as one upload. The order still holds -- the
         * jobs of a row were collected in order, so a later baked job's CPU compute (if its cache
         * missed) reads the earlier effects' CPU answers the same way a plan job's input render
         * would read their GPU ones. */
        if (!combined_effect_bake_upload(ma, job, tile_number)) {
          return false;
        }
        rendered++;
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
        /* The caller's rectangle widened this job's dirty band (it is part of the region printed
         * below, unioned into the cache's own). */
        const int provider_widened =
            (effects_region != nullptr && !BLI_rcti_is_empty(effects_region)) ? 1 : 0;
        perf_region.note("kind=%d region=%dx%d full=%d halo=%d provider=%d",
                         int(job.correction->filter),
                         BLI_rcti_size_x(&pass_rects[n - 1]),
                         BLI_rcti_size_y(&pass_rects[n - 1]),
                         job.region_exact ? 0 : 1,
                         job.halo,
                         provider_widened);
      }

      /* The stage the effect's input is laid over. Off by default (the whole canvas); the env
       * switch turns the regional path on for a plan the guard allows a band, and the stage is the
       * union of what its passes read (see #BKE_paint_layers_effect_input_region). Anything empty
       * or as large as the canvas falls back to the whole canvas, so a regional run is never
       * smaller than the plan needs and never larger than today. */
      int2 stage_origin = int2(0);
      int2 stage_size = canvas;
      bool regional = false;
      if (n > 0 && (combined_effect_input_region_enabled() || region_test_active) &&
          job.region_exact && !combined_effect_input_has_canvas_correction(job.input_entry))
      {
        const float *params = job.correction->filter_params;
        const EffectReach reach = BKE_paint_layers_effect_reach(
            eMaterialPaintLayerFilter(job.correction->filter), params);
        if (BKE_paint_layers_effect_input_region_supported(
                Span<const PaintLayerEffectPass>(job.passes.data(), job.passes.size()),
                reach,
                job.region_exact))
        {
          rcti stage;
          if (BKE_paint_layers_effect_input_region(
                  Span<const PaintLayerEffectPass>(job.passes.data(), job.passes.size()),
                  Span<const rcti>(pass_rects.data(), pass_rects.size()),
                  canvas.x,
                  canvas.y,
                  stage))
          {
            const int64_t stage_area = int64_t(stage.xmax - stage.xmin) *
                                       int64_t(stage.ymax - stage.ymin);
            const int64_t canvas_area = int64_t(canvas.x) * int64_t(canvas.y);
            if (stage_area > 0 && stage_area < canvas_area) {
              stage_origin = int2(stage.xmin, stage.ymin);
              stage_size = int2(stage.xmax - stage.xmin, stage.ymax - stage.ymin);
              regional = true;
            }
          }
        }
      }

      /* The content the effect reads, laid into a target of the stage's own resolution and texel
       * space. A regional stage is a sub-rectangle of the canvas: its passes run over
       * `[0, stage_size)` and sample the maps through `texel_to_uv`, whose scale stays the canvas
       * grid and whose translation is the stage's origin in canvas UV. The whole-canvas path is
       * the same code with `stage_origin` zero and `stage_size` the canvas. */
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
          /* Four textures are acquired through this lambda and it has exactly four slots; a fifth
           * would otherwise leak its texture until the context's pool is destroyed. */
          BLI_assert(texture == nullptr || target_scratch[3] != nullptr);
          return texture;
        };
        /* The stage's geometry: its size, its dispatch groups, and the texel-to-UV map whose scale
         * stays the canvas grid and whose translation is the stage origin in canvas UV. With the
         * origin at zero this is the old whole-canvas matrix. */
        const auto layout_target = [&](const int2 size, const int2 origin) {
          effect_target.size = size;
          effect_target.groups = math::divide_ceil(size, int2(16));
          effect_target.texel_to_uv = math::from_scale<float3x3, 2>(1.0f / float2(canvas));
          effect_target.texel_to_uv[2] = float3(
              float(origin.x) / float(canvas.x), float(origin.y) / float(canvas.y), 1.0f);
          effect_target.texel_to_uv_4x4 = float4x4::identity();
          effect_target.texel_to_uv_4x4[0] = float4(
              effect_target.texel_to_uv[0].x, effect_target.texel_to_uv[0].y, 0.0f, 0.0f);
          effect_target.texel_to_uv_4x4[1] = float4(
              effect_target.texel_to_uv[1].x, effect_target.texel_to_uv[1].y, 0.0f, 0.0f);
          effect_target.texel_to_uv_4x4[3] = float4(
              effect_target.texel_to_uv[2].x, effect_target.texel_to_uv[2].y, 0.0f, 1.0f);
        };
        const auto acquire_input = [&](const int2 size) -> bool {
          effect_target.row_color = target_acquire(size);
          effect_target.row_factor = target_acquire(size);
          effect_target.mask_accum = target_acquire(size);
          /* The capture returns before anything blends onto the destination, but the layer pass
           * binds it as an image all the same: a placeholder of its own keeps it from being the
           * texture the same dispatch samples. */
          effect_target.dest = target_acquire(int2(1));
          job.input = job_acquire(size);
          return effect_target.row_color != nullptr && effect_target.row_factor != nullptr &&
                 effect_target.mask_accum != nullptr && effect_target.dest != nullptr &&
                 job.input != nullptr;
        };
        /* Hand a failed stage's textures back before the full-canvas retry: a partial set would
         * otherwise sit in the job's scratch until its scope ends. */
        const auto release_input = [&]() {
          for (gpu::Texture *&texture : target_scratch) {
            if (texture != nullptr) {
              passes.pool.release_texture(texture);
              texture = nullptr;
            }
          }
          if (job.input != nullptr) {
            gpu::Texture *input = job.input;
            job.input = nullptr;
            /* The input may sit anywhere in the job's scratch (a later resource could have been
             * acquired before a fallback): drop it by value so the scope defer never releases it
             * twice. The target textures live in their own array, not in this vector. */
            if (job_scratch.contains(input)) {
              job_scratch.remove_first_occurrence_and_reorder(input);
            }
            passes.pool.release_texture(input);
          }
        };
        bool alloc_fallback = false;
        layout_target(stage_size, stage_origin);
        if (!acquire_input(stage_size)) {
          if (!regional) {
            return gpu_refuse("an effect input texture could not be allocated");
          }
          /* One retry at the full canvas: the regional stage is an optimisation and must never
           * leave the job half laid. */
          release_input();
          regional = false;
          stage_origin = int2(0);
          stage_size = canvas;
          alloc_fallback = true;
          layout_target(stage_size, stage_origin);
          if (!acquire_input(stage_size)) {
            return gpu_refuse("an effect input texture could not be allocated");
          }
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
        perf_input.note("kind=%d size=%dx%d origin=%d,%d canvas=%dx%d regional=%d full=%d%s%s",
                        int(job.correction->filter),
                        stage_size.x,
                        stage_size.y,
                        stage_origin.x,
                        stage_origin.y,
                        canvas.x,
                        canvas.y,
                        regional ? 1 : 0,
                        regional ? 0 : 1,
                        alloc_fallback ? " fallback=alloc" : "",
                        region_test_active ? " test=1" : "");
      }

      /* The luminance buffer a grayscale High Pass keeps, and the two ring slots the window
       * passes ping-pong through. Each is acquired only when the plan uses it: a box blur needs no
       * luminance buffer, a pointwise Noise needs neither, and the job's scratch defer above hands
       * every one of them back to the pool when the job's scope ends. A regional plan's window
       * passes read the stage grid, so these follow `stage_size`, not the canvas. */
      gpu::Texture *scratch[3] = {nullptr, nullptr, nullptr};
      if (!job.passes.is_empty() && job.passes[0].kind == ePaintLayerEffectPass::Luminance) {
        scratch[0] = job_acquire(stage_size);
        if (scratch[0] == nullptr) {
          return gpu_refuse("an effect scratch texture could not be allocated");
        }
      }
      if (n > 1) {
        scratch[1] = job_acquire(stage_size);
        scratch[2] = job_acquire(stage_size);
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
             buffers.rect_for(pass, job.region_exact, job.region, pass_rects),
             /* The stage the pass's textures live on: its reads and intermediate stores run at
              * `texel - texture_origin`, and only the plan's last pass writes the full-canvas
              * result at the canvas texel. The whole-canvas path keeps the origin zero and every
              * pass writing at its texel, exactly as before the stage existed. */
             stage_origin,
             regional ? paint_layer_effect_pass_is_final(pass) : true},
            luma_coefficients);
        buffers.commit(pass, field_dst != nullptr ? field_dst : dst);
      }

      /* The versions are stamped after the render, so an edit that landed while it ran is newer than
       * this entry and costs one more update rather than a result that never clears. The hashes are
       * the ones the entry was keyed on above -- a chain member's through the keyed form, its
       * composite marker and instance-unique hash. */
      if (job.correction->effect_member != nullptr) {
        BKE_paint_layers_effect_gpu_cache_publish_keyed(ma,
                                                        job.correction->effect_marker,
                                                        job.correction->effect_hash,
                                                        job.channel,
                                                        job.input_size.x,
                                                        job.input_size.y,
                                                        job.images,
                                                        PaintLayerEffectKeyExtras{job.chain_hash,
                                                                                     job.input_scalar_hash},
                                                        tile_number);
      }
      else {
        BKE_paint_layers_effect_gpu_cache_publish(ma,
                                                  *job.effect,
                                                  job.channel,
                                                  job.input_size.x,
                                                  job.input_size.y,
                                                  job.images,
                                                  PaintLayerEffectKeyExtras{job.chain_hash,
                                                                               job.input_scalar_hash},
                                                  tile_number);
      }
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
