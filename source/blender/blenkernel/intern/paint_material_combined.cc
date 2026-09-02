/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_paint_material_combined.hh"

#include <algorithm>
#include <cmath>
#include <memory>

#include "BLI_hash.hh"
#include "BLI_index_range.hh"
#include "BLI_map.hh"
#include "BLI_math_base.h"
#include "BLI_math_color.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_rect.h"
#include "BLI_set.hh"
#include "BLI_task.hh"
#include "BLI_time.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "BKE_image_partial_update.hh"
#include "BKE_paint_material_channel_perf_debug.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

namespace blender {

/* The image change log the Combined cache subscribes to. Brought in wholesale because the switch
 * over #ePartialUpdateCollectResult reads badly with the full qualification on every label. */
using namespace bke::image::partial_update;

/* -------------------------------------------------------------------- */
/** \name Defaults
 * \{ */

float4 BKE_paint_material_combined_default_value(const eMaterialPaintChannel channel)
{
  switch (channel) {
    case PAINT_MATERIAL_CHANNEL_BASE_COLOR:
      /* Principled's own default, not the paint neutral: a material with no base colour map should
       * preview as grey plastic rather than as black. */
      return float4(0.8f, 0.8f, 0.8f, 1.0f);
    case PAINT_MATERIAL_CHANNEL_METALLIC:
      return float4(0.0f);
    case PAINT_MATERIAL_CHANNEL_ROUGHNESS:
      return float4(0.5f);
    case PAINT_MATERIAL_CHANNEL_SPECULAR:
      return float4(0.5f);
    case PAINT_MATERIAL_CHANNEL_NORMAL:
      /* Flat tangent normal. The scalar paint-defaults table cannot express this. */
      return float4(0.0f, 0.0f, 1.0f, 0.0f);
    case PAINT_MATERIAL_CHANNEL_AO:
    case PAINT_MATERIAL_CHANNEL_ALPHA:
      return float4(1.0f);
    case PAINT_MATERIAL_CHANNEL_EMISSION:
      return float4(0.0f, 0.0f, 0.0f, 1.0f);
    case PAINT_MATERIAL_CHANNEL_CUSTOM:
    case PAINT_MATERIAL_CHANNEL_HEIGHT:
      /* Never gathered and never shaded in this preview; the value exists only so the table is
       * total. */
      return float4(0.0f);
  }
  BLI_assert_unreachable();
  return float4(0.0f);
}

uint64_t CombinedPreviewLighting::hash() const
{
  uint64_t hash = get_default_hash(
      this->lights_num, this->ambient_color, this->exposure, this->ao_influence);
  /* Only the lights that are actually used: the remaining slots keep whatever a previous rig left
   * in them, and folding those in would report a change the shading cannot see. */
  for (const int i : IndexRange(std::clamp(this->lights_num, 0, int(this->lights.size())))) {
    const CombinedPreviewLight &light = this->lights[i];
    hash = get_default_hash(
        hash, light.direction, light.diffuse_color, light.specular_color, light.wrap);
  }
  return hash;
}

CombinedPreviewLighting BKE_paint_material_combined_lighting_default()
{
  /* Four directional lights in the Workbench studio arrangement -- a key from the upper right, a
   * fill from the left, a rim from below and a soft top -- plus a small ambient term so an
   * unlit-facing texel is not pure black.
   *
   * The rig's *arrangement*; where it is standing comes from
   * #BKE_paint_material_combined_lighting_rotate_z. Nothing else in the feature may hard-code a
   * light: the rig is an argument everywhere, so a richer UI replaces one call site. */
  CombinedPreviewLighting lighting;
  lighting.lights_num = 4;

  lighting.lights[0].direction = math::normalize(float3(0.5f, 0.5f, 1.0f));
  lighting.lights[0].diffuse_color = float3(1.0f, 0.98f, 0.95f);
  lighting.lights[0].specular_color = float3(1.0f, 0.98f, 0.95f);
  lighting.lights[0].wrap = 0.2f;

  lighting.lights[1].direction = math::normalize(float3(-0.7f, 0.15f, 0.7f));
  lighting.lights[1].diffuse_color = float3(0.30f, 0.33f, 0.38f);
  lighting.lights[1].specular_color = float3(0.25f, 0.28f, 0.33f);
  lighting.lights[1].wrap = 0.4f;

  lighting.lights[2].direction = math::normalize(float3(0.0f, -0.8f, 0.6f));
  lighting.lights[2].diffuse_color = float3(0.18f, 0.18f, 0.20f);
  lighting.lights[2].specular_color = float3(0.18f, 0.18f, 0.20f);
  lighting.lights[2].wrap = 0.5f;

  lighting.lights[3].direction = math::normalize(float3(0.0f, 0.85f, 0.5f));
  lighting.lights[3].diffuse_color = float3(0.22f, 0.24f, 0.28f);
  lighting.lights[3].specular_color = float3(0.20f, 0.22f, 0.26f);
  lighting.lights[3].wrap = 0.3f;

  lighting.ambient_color = float3(0.05f);
  lighting.exposure = 1.0f;
  lighting.ao_influence = 1.0f;
  return lighting;
}

void BKE_paint_material_combined_lighting_rotate_z(CombinedPreviewLighting &lighting,
                                                   const float angle)
{
  if (angle == 0.0f) {
    return;
  }
  const float sin_angle = std::sin(angle);
  const float cos_angle = std::cos(angle);
  /* Only the lights actually in use: a rotation applied to an unused slot would move it out from
   * under #hash's own bound and read as a change the shading cannot see. */
  for (const int i : IndexRange(std::clamp(lighting.lights_num, 0, int(lighting.lights.size())))) {
    float3 &direction = lighting.lights[i].direction;
    direction = float3(direction.x * cos_angle - direction.y * sin_angle,
                       direction.x * sin_angle + direction.y * cos_angle,
                       direction.z);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Evaluator
 * \{ */

/**
 * Byte sRGB to scene linear, one entry per byte value.
 *
 * A table rather than #srgb_to_linearrgb per component: the inner loop runs once per pixel per
 * colour channel, and a `powf` there is the difference between a dab that lands within the frame
 * and one that does not.
 */
static const float *srgb_to_linear_lut()
{
  static std::array<float, 256> lut = []() {
    std::array<float, 256> table;
    for (const int i : IndexRange(256)) {
      table[i] = srgb_to_linearrgb(float(i) * (1.0f / 255.0f));
    }
    return table;
  }();
  return lut.data();
}

/**
 * One channel resolved for the whole evaluation, so the pixel loop never branches on source kind.
 *
 * Hoisting this out is one of the techniques the performance budget assumes: the common material
 * has constant metallic, roughness and specular, and paying a branch and a bounds check per pixel
 * for each of them costs more than the shading does.
 */
struct ChannelReader {
  const uchar *bytes = nullptr;
  const float *floats = nullptr;
  /** Non-null exactly when #bytes is sRGB-encoded. */
  const float *lut = nullptr;
  float4 constant = float4(0.0f);

  float4 read(const int64_t texel) const
  {
    if (this->bytes != nullptr) {
      const uchar *p = this->bytes + texel * 4;
      if (this->lut != nullptr) {
        return float4(this->lut[p[0]], this->lut[p[1]], this->lut[p[2]], float(p[3]) / 255.0f);
      }
      return float4(float(p[0]), float(p[1]), float(p[2]), float(p[3])) * (1.0f / 255.0f);
    }
    if (this->floats != nullptr) {
      return float4(this->floats + texel * 4);
    }
    return this->constant;
  }
};

/** GGX normal distribution, `alpha` being the squared roughness. */
static float combined_ggx_d(const float n_dot_h, const float alpha)
{
  const float alpha2 = alpha * alpha;
  const float denom = n_dot_h * n_dot_h * (alpha2 - 1.0f) + 1.0f;
  return alpha2 / std::max(float(M_PI) * denom * denom, 1e-8f);
}

/** Smith height-correlated visibility, Schlick form, `k` being half the squared roughness. */
static float combined_smith_g(const float n_dot_v, const float n_dot_l, const float k)
{
  const float gv = n_dot_v / std::max(n_dot_v * (1.0f - k) + k, 1e-6f);
  const float gl = n_dot_l / std::max(n_dot_l * (1.0f - k) + k, 1e-6f);
  return gv * gl;
}

bool BKE_paint_material_combined_eval(const CombinedInputs &inputs,
                                      const CombinedPreviewLighting &lighting,
                                      ImBuf *dst_ibuf,
                                      const rcti *region,
                                      CombinedEvalStats *r_stats)
{
  if (r_stats != nullptr) {
    *r_stats = CombinedEvalStats{};
  }
  if (dst_ibuf == nullptr || dst_ibuf->float_data() == nullptr) {
    return false;
  }
  if (inputs.width <= 0 || inputs.height <= 0) {
    return false;
  }
  const int out_width = inputs.out_width();
  const int out_height = inputs.out_height();
  if (out_width <= 0 || out_height <= 0) {
    return false;
  }
  if (dst_ibuf->x != out_width || dst_ibuf->y != out_height) {
    return false;
  }
  /* Strict: a channel whose buffer disagrees is a gatherer bug, and absorbing it here would hide
   * the size mismatch behind plausible pixels. Checked against the *input* resolution -- only the
   * destination is allowed to differ. */
  for (const CombinedChannelInput &channel : inputs.channels) {
    if (channel.ibuf != nullptr &&
        (channel.ibuf->x != inputs.width || channel.ibuf->y != inputs.height))
    {
      return false;
    }
  }

  /* In output coordinates: a dab arrives in canvas texels and is scaled by the gather, which is
   * the only caller that ever asks for a reduced output. */
  rcti area;
  BLI_rcti_init(&area, 0, out_width, 0, out_height);
  if (region != nullptr) {
    rcti clipped = *region;
    if (!BLI_rcti_isect(&area, &clipped, &area)) {
      /* Nothing to do, and not a failure: a dab entirely outside the canvas is legitimate. */
      return true;
    }
  }

  const double time_start = BLI_time_now_seconds();

  /* Resolve every channel once, before the loop. */
  std::array<ChannelReader, PAINT_MATERIAL_CHANNEL_NUM> readers;
  for (const int i : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    const CombinedChannelInput &input = inputs.channels[i];
    ChannelReader &reader = readers[i];
    reader.constant = input.constant;
    if (input.ibuf == nullptr) {
      continue;
    }
    if (input.ibuf->float_data() != nullptr) {
      /* A float source is already linear by the time the gatherer hands it over, so #is_srgb is
       * honoured for byte buffers only. */
      reader.floats = input.ibuf->float_data();
    }
    else if (input.ibuf->byte_data() != nullptr) {
      reader.bytes = input.ibuf->byte_data();
      if (input.is_srgb) {
        reader.lut = srgb_to_linear_lut();
      }
    }
  }

  const ChannelReader &base_reader = readers[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
  const ChannelReader &metallic_reader = readers[PAINT_MATERIAL_CHANNEL_METALLIC];
  const ChannelReader &roughness_reader = readers[PAINT_MATERIAL_CHANNEL_ROUGHNESS];
  const ChannelReader &specular_reader = readers[PAINT_MATERIAL_CHANNEL_SPECULAR];
  const ChannelReader &normal_reader = readers[PAINT_MATERIAL_CHANNEL_NORMAL];
  const ChannelReader &ao_reader = readers[PAINT_MATERIAL_CHANNEL_AO];
  const ChannelReader &alpha_reader = readers[PAINT_MATERIAL_CHANNEL_ALPHA];
  const ChannelReader &emission_reader = readers[PAINT_MATERIAL_CHANNEL_EMISSION];

  /* The normal channel's buffer stores an encoded normal when it has one, and a plain vector when
   * it is a constant: the constant table already holds (0, 0, 1), which decoding would turn into
   * (-1, -1, 1). Decided once here rather than per pixel. */
  const bool normal_is_encoded = normal_reader.bytes != nullptr || normal_reader.floats != nullptr;

  /* Only the lights that can change a pixel. A light whose diffuse and specular colours are both
   * black still costs a normalize, a GGX, a Smith and a Schlick per texel, and the studio rig's
   * dimmer fills go black as soon as a user turns them down. Filtered once here rather than
   * branched on 16 million times. */
  std::array<CombinedPreviewLight, 4> lights;
  int lights_num = 0;
  for (const int i : IndexRange(std::clamp(lighting.lights_num, 0, int(lighting.lights.size())))) {
    const CombinedPreviewLight &light = lighting.lights[i];
    if (math::is_zero(light.diffuse_color) && math::is_zero(light.specular_color)) {
      continue;
    }
    lights[lights_num++] = light;
  }
  const float3 view_dir(0.0f, 0.0f, 1.0f);

  const int width = inputs.width;
  const int area_xmin = area.xmin;
  const int area_width = BLI_rcti_size_x(&area);
  /* Resolved once, on this thread: the accessor may copy the buffer to make it mutable, which must
   * not happen from inside #threading::parallel_for. */
  float *dst = dst_ibuf->float_data_for_write();

  const bool is_scaled = out_width != inputs.width || out_height != inputs.height;
  /* Input texels per output pixel. Only ever a reduction in practice: the editor never asks for
   * more output than the canvas has. */
  const float scale_x = float(inputs.width) / float(out_width);
  const float scale_y = float(inputs.height) / float(out_height);
  /* A box filter, so that halving the resolution does not turn a normal map into sparkle when the
   * view pans. Capped at two samples per axis: four reads per channel is the most this is worth
   * paying for a preview that is itself being dragged. */
  const int samples_x = is_scaled ? std::clamp(int(scale_x + 0.5f), 1, 2) : 1;
  const int samples_y = is_scaled ? std::clamp(int(scale_y + 0.5f), 1, 2) : 1;
  const float sample_weight = 1.0f / float(samples_x * samples_y);

  const IndexRange rows(area.ymin, BLI_rcti_size_y(&area));
  /* Grained by texels rather than by rows, so a task is the same size whatever the canvas is: a
   * fixed 64 rows is 16k texels on a 256-wide dab region and 256k on a 4096-wide canvas, and the
   * latter is coarse enough to leave threads idle at the tail of every full rebuild. */
  const int64_t grain = std::max<int64_t>(1, 65536 / std::max(area_width, 1));

  /* The whole shading loop, parameterized by how one output pixel reads its inputs.
   *
   * \a make_sampler is called once per pixel and returns a callable that reads one channel there,
   * so the texels an output pixel covers are resolved once rather than once per channel.
   *
   * A generic lambda, so each sampler compiles into its own instantiation of the loop, rather than
   * a branch inside it: whether the output is reduced is a runtime value, and testing it eight
   * times per pixel -- once per channel -- is not something the compiler can be relied on to
   * hoist. The unscaled instantiation is the path every non-viewport caller takes and has to stay
   * exactly as cheap as it was before a reduced output existed at all. */
  const auto shade_area = [&](auto make_sampler) {
    threading::parallel_for(rows, grain, [&](const IndexRange range) {
      for (const int y : range) {
        for (const int x : IndexRange(area_xmin, area_width)) {
          const auto sample = make_sampler(x, y);

          const float4 base = sample(base_reader);
          const float metallic = std::clamp(sample(metallic_reader).x, 0.0f, 1.0f);
          const float roughness = std::clamp(sample(roughness_reader).x, 0.0f, 1.0f);
          const float specular = sample(specular_reader).x;
          const float ao_map = sample(ao_reader).x;
          const float alpha = sample(alpha_reader).x;
          const float4 emission = sample(emission_reader);

          float3 normal(0.0f, 0.0f, 1.0f);
          if (normal_is_encoded) {
            /* Averaging before decoding is the same as averaging after: the encoding is affine,
             * and the normalize below fixes the shortening that averaging a fan of normals causes.
             */
            const float4 encoded = sample(normal_reader);
            normal = float3(encoded.x, encoded.y, encoded.z) * 2.0f - 1.0f;
          }
          else {
            const float4 raw = sample(normal_reader);
            normal = float3(raw.x, raw.y, raw.z);
          }
          normal = math::normalize(normal);
          if (UNLIKELY(math::is_zero(normal))) {
            normal = float3(0.0f, 0.0f, 1.0f);
          }

          const float3 base_rgb(base.x, base.y, base.z);
          const float3 diffuse_color = base_rgb * (1.0f - metallic);
          const float3 f0 = math::interpolate(float3(0.08f * specular), base_rgb, metallic);
          const float ao = math::interpolate(1.0f, ao_map, lighting.ao_influence);

          const float alpha_ggx = std::max(roughness * roughness, 1e-4f);
          const float smith_k = alpha_ggx * 0.5f;
          const float n_dot_v = std::max(math::dot(normal, view_dir), 1e-4f);

          float3 lit(0.0f);
          for (const int light_index : IndexRange(lights_num)) {
            const CombinedPreviewLight &light = lights[light_index];
            const float3 light_dir = light.direction;
            const float n_dot_l_raw = math::dot(normal, light_dir);

            const float n_dot_l_wrapped = std::max(
                (n_dot_l_raw + light.wrap) / (1.0f + light.wrap), 0.0f);
            lit += diffuse_color * light.diffuse_color * n_dot_l_wrapped;

            const float n_dot_l = std::max(n_dot_l_raw, 0.0f);
            if (n_dot_l <= 0.0f) {
              continue;
            }
            const float3 half_vector = math::normalize(light_dir + view_dir);
            const float n_dot_h = std::max(math::dot(normal, half_vector), 0.0f);
            const float v_dot_h = std::max(math::dot(view_dir, half_vector), 0.0f);

            const float d = combined_ggx_d(n_dot_h, alpha_ggx);
            /* Schlick's fifth power by multiplication. A `powf` here is measurable in the inner
             * loop and this form is exact for the integer exponent. */
            const float t = 1.0f - v_dot_h;
            const float t2 = t * t;
            const float t5 = t2 * t2 * t;
            const float3 fresnel = f0 + (float3(1.0f) - f0) * t5;
            const float g = combined_smith_g(n_dot_v, n_dot_l, smith_k);

            lit += light.specular_color * fresnel * (d * g * n_dot_l);
          }

          /* Ambient occlusion darkens the ambient term only. Applying it to direct light is what
           * makes a preview look dirty and is not how EEVEE applies it. */
          lit += (diffuse_color + f0) * lighting.ambient_color * ao;
          /* Emission is unlit, and added after shading. Scaled by Principled's Emission Strength,
           * which is a separate socket from the colour this channel maps to. */
          lit += float3(emission.x, emission.y, emission.z) * inputs.emission_strength;
          lit *= lighting.exposure;

          /* Indexed by the *output* row: the destination is the only buffer here whose width may
           * differ from the inputs'. */
          float *out = dst + (int64_t(y) * out_width + x) * 4;
          out[0] = lit.x;
          out[1] = lit.y;
          out[2] = lit.z;
          out[3] = alpha;
        }
      }
    });
  };

  if (is_scaled) {
    shade_area([&](const int x, const int y) {
      /* The input texels this output pixel covers, resolved once for all eight channels. */
      std::array<int64_t, 4> texels;
      int texel_num = 0;
      for (const int sy : IndexRange(samples_y)) {
        const float fy = (float(y) + (float(sy) + 0.5f) / float(samples_y)) * scale_y;
        const int iy = std::clamp(int(fy), 0, inputs.height - 1);
        for (const int sx : IndexRange(samples_x)) {
          const float fx = (float(x) + (float(sx) + 0.5f) / float(samples_x)) * scale_x;
          const int ix = std::clamp(int(fx), 0, width - 1);
          texels[texel_num++] = int64_t(iy) * width + ix;
        }
      }
      return [texels, texel_num, weight = sample_weight](const ChannelReader &reader) {
        float4 total(0.0f);
        for (const int i : IndexRange(texel_num)) {
          total += reader.read(texels[i]);
        }
        return total * weight;
      };
    });
  }
  else {
    shade_area([&](const int x, const int y) {
      const int64_t texel = int64_t(y) * width + x;
      return [texel](const ChannelReader &reader) { return reader.read(texel); };
    });
  }

  if (r_stats != nullptr) {
    r_stats->elapsed_seconds = BLI_time_now_seconds() - time_start;
    r_stats->pixels_processed = int64_t(area_width) * BLI_rcti_size_y(&area);
  }
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Combined Cache
 *
 * Keyed by the material's #ID.session_uid, like the composite cache and for the same reason: a
 * freed material hands its address to the next one, and a pointer-keyed cache would then serve the
 * old preview for the new material.
 *
 * Main thread only. Everything that reaches it -- the Image Editor's buffer acquisition, an ID
 * free, an undo step -- runs there.
 * \{ */

/**
 * Grow \a r_rect to also cover \a add, or replace it with \a add when the union would be more than
 * a rectangle.
 *
 * The policy on top of #BLI_rcti_union_is_exact, and the reason that predicate is asked at all:
 * this rectangle records what *is* valid, so claiming a corner neither call covered would serve a
 * stale pixel. Replacing instead throws away real coverage and costs a re-shade later, which is
 * the cheaper of the two.
 */
static void combined_rect_absorb(rcti &r_rect, const rcti &add)
{
  if (BLI_rcti_is_empty(&add)) {
    return;
  }
  /* #BLI_rcti_union reads a zeroed rectangle as a real one at the origin, so "nothing yet" has to
   * be handled before it, not by it. */
  if (BLI_rcti_is_empty(&r_rect)) {
    r_rect = add;
    return;
  }
  if (BLI_rcti_union_is_exact(&r_rect, &add)) {
    BLI_rcti_union(&r_rect, &add);
  }
  else {
    r_rect = add;
  }
}

/**
 * Owning handle for the one resource a cache entry holds.
 *
 * By value rather than a raw pointer freed by hand, because the entry is destroyed from four
 * places -- eviction, a failed evaluation, a per-material drop and the teardown -- and a path
 * added later that forgets to free is a leak nothing reports.
 */
struct ImBufDeleter {
  void operator()(ImBuf *ibuf) const
  {
    IMB_freeImBuf(ibuf);
  }
};
using ImBufPtr = std::unique_ptr<ImBuf, ImBufDeleter>;

struct PartialUpdateUserDeleter {
  void operator()(PartialUpdateUser *user) const
  {
    BKE_image_partial_update_free(user);
  }
};
using PartialUpdateUserPtr = std::unique_ptr<PartialUpdateUser, PartialUpdateUserDeleter>;

struct CombinedCacheEntry {
  /** Float RGBA, scene linear. */
  ImBufPtr ibuf;
  int width = 0;
  int height = 0;
  /**
   * The canvas the preview stands for, which #width and #height are a power-of-two fraction of.
   *
   * Kept because #BKE_paint_material_combined_cache_size_get answers with it: every caller of
   * #ED_space_image_get_size wants the canvas -- #image_main_region_set_view2d builds the view
   * from it -- and none of them wants the resolution the preview happens to be shaded at.
   */
  int canvas_width = 0;
  int canvas_height = 0;
  uint64_t inputs_hash = 0;
  uint64_t lighting_hash = 0;
  /** The whole buffer has to be recomputed. Set on creation, and whenever a region is unknown. */
  bool dirty_full = true;
  /** Bounding rectangle of everything tagged since the last evaluation. */
  rcti dirty_region = {0, 0, 0, 0};
  /**
   * The rectangle of #ibuf that the current #inputs_hash and #lighting_hash have been applied to.
   * Empty means none of it has been.
   *
   * Read together with #dirty_region, never alone: a pixel is up to date only when it is inside
   * this rectangle AND outside that one. A single rectangle each rather than a set of them,
   * because the only two shapes that ever arrive are a dab and a viewport, and a region list would
   * cost more bookkeeping than the shading it saves.
   */
  rcti valid_region = {0, 0, 0, 0};
  /**
   * One partial-update subscription per image the inputs read directly, keyed by #ID.session_uid.
   * Layer-stack images are absent by design: their edits arrive as a `changed_region` from the
   * composite that owns them.
   *
   * Polled rather than reported, for the same reason the composite cache is: an image records what
   * changed in it, per tile, whoever caused the change. Being told instead meant being told twice
   * -- once precisely by the stroke, and once as "everything" by the depsgraph flush that follows
   * the same dab -- and the blanket answer won. That turned every dab into a full re-shade of the
   * visible canvas, which measured at ~25 ms a frame on a 2048 canvas against ~4 ms for the dab
   * alone.
   *
   * Never holds an #Image pointer: the images arrive with every `cache_ensure` call, so a poll
   * always has a fresh one, and a cache outliving an ID it pointed at would be a crash rather than
   * a stale pixel.
   */
  Map<uint32_t, PartialUpdateUserPtr> partial_update_users;
  uint64_t revision = 0;
  int64_t last_use = 0;
};

/**
 * The one Combined cache of the session.
 *
 * Main thread only, like #CompositeCache and for the same reason: everything that reaches it runs
 * on the main thread and there is no worker writing results back.
 */
struct CombinedCache {
  Map<uint32_t, CombinedCacheEntry> entries;
  /** Monotonic, and only ever compared: the source of #CombinedCacheEntry.last_use. */
  int64_t use_counter = 0;
  /** Never reset: a consumer compares revisions over time, across entries that come and go. */
  uint64_t revision_counter = 0;
};

static CombinedCache g_cache;

/** Float RGBA at 4096 square is 256 MiB on its own, so this cannot share the composite's budget.
 * It bounds a pathological case; in practice the cache holds one or two entries. */
static constexpr int64_t COMBINED_CACHE_BUDGET_BYTES = 512 * 1024 * 1024;

static int64_t combined_entry_size_in_bytes(const CombinedCacheEntry &entry)
{
  return int64_t(entry.width) * entry.height * 4 * int64_t(sizeof(float));
}

/** Evict least recently used entries until the cache fits the budget, never the one just made. */
static void combined_cache_enforce_budget(const uint32_t keep)
{
  int64_t total = 0;
  for (const CombinedCacheEntry &entry : g_cache.entries.values()) {
    total += combined_entry_size_in_bytes(entry);
  }
  while (total > COMBINED_CACHE_BUDGET_BYTES) {
    const uint32_t *oldest_key = nullptr;
    int64_t oldest_use = INT64_MAX;
    for (const auto item : g_cache.entries.items()) {
      if (item.key == keep) {
        continue;
      }
      if (item.value.last_use < oldest_use) {
        oldest_use = item.value.last_use;
        oldest_key = &item.key;
      }
    }
    if (oldest_key == nullptr) {
      break;
    }
    const uint32_t key = *oldest_key;
    total -= combined_entry_size_in_bytes(g_cache.entries.lookup(key));
    g_cache.entries.remove(key);
  }
}

/**
 * Subscribe \a entry to every image in \a images, drop the subscriptions it no longer needs, and
 * fold whatever they report into its dirty state.
 *
 * The whole reason this cache learns about pixel edits by asking rather than by being told. A
 * caller that edits an image has nothing to report here, and -- the point -- a caller that tags
 * the image ID for unrelated reasons can no longer turn a known rectangle into "the whole canvas".
 *
 * Requires #CombinedCacheEntry.canvas_width and #height to be current: a change arrives in the
 * image's texels, which are the canvas's, and the entry's rectangles are in output pixels.
 */
static void combined_entry_poll_dependencies(CombinedCacheEntry &entry, Span<Image *> images)
{
  BLI_assert(entry.canvas_width > 0 && entry.canvas_height > 0);
  const bool is_scaled = entry.width != entry.canvas_width || entry.height != entry.canvas_height;
  /* Rounded outwards, so a dab never loses its edge and leaves a stale fringe. */
  const auto to_output = [&](const rcti &region) {
    if (!is_scaled) {
      return region;
    }
    const float scale_x = float(entry.width) / float(entry.canvas_width);
    const float scale_y = float(entry.height) / float(entry.canvas_height);
    rcti scaled;
    BLI_rcti_init(&scaled,
                  int(floorf(float(region.xmin) * scale_x)),
                  int(ceilf(float(region.xmax) * scale_x)),
                  int(floorf(float(region.ymin) * scale_y)),
                  int(ceilf(float(region.ymax) * scale_y)));
    return scaled;
  };

  Set<uint32_t> live;
  for (Image *image : images) {
    if (image == nullptr) {
      continue;
    }
    const uint32_t session_uid = image->id.session_uid;
    if (!live.add(session_uid)) {
      /* The same image can feed several channels; one subscription answers for all of them, and
       * polling it twice would hand the second poll an empty changeset. */
      continue;
    }
    PartialUpdateUser *user = entry.partial_update_users
                                  .lookup_or_add_cb(session_uid,
                                                    [&]() {
                                                      return PartialUpdateUserPtr(
                                                          BKE_image_partial_update_create(image));
                                                    })
                                  .get();

    switch (BKE_image_partial_update_collect_changes(image, user)) {
      case ePartialUpdateCollectResult::FullUpdateNeeded:
        /* A brand new subscription lands here too, which is right: nothing of this image has been
         * shaded into the preview yet. */
        entry.dirty_full = true;
        break;
      case ePartialUpdateCollectResult::NoChangesDetected:
        break;
      case ePartialUpdateCollectResult::PartialChangesDetected: {
        PartialUpdateRegion change;
        while (BKE_image_partial_update_get_next_change(user, &change) ==
               ePartialUpdateIterResult::ChangeAvailable)
        {
          /* The preview is one canvas, so a change reported for any tile but the first would land
           * at the wrong place in it. Give up precision rather than shade the wrong pixels. */
          if (change.tile_number != 1001) {
            entry.dirty_full = true;
            break;
          }
          const rcti scaled = to_output(change.region);
          if (BLI_rcti_is_empty(&entry.dirty_region)) {
            entry.dirty_region = scaled;
          }
          else {
            BLI_rcti_union(&entry.dirty_region, &scaled);
          }
        }
        break;
      }
    }
  }

  /* A channel that stopped reading an image stops watching it, or the entry keeps an allocation
   * and a poll per frame for something it no longer reads. */
  Vector<uint32_t> stale;
  for (const uint32_t session_uid : entry.partial_update_users.keys()) {
    if (!live.contains(session_uid)) {
      stale.append(session_uid);
    }
  }
  for (const uint32_t session_uid : stale) {
    entry.partial_update_users.remove(session_uid);
  }
}

/** Whether \a inputs would produce anything at all. */
static bool combined_inputs_are_usable(const CombinedInputs &inputs)
{
  return inputs.width > 0 && inputs.height > 0 && inputs.out_width() > 0 &&
         inputs.out_height() > 0;
}

ImBuf *BKE_paint_material_combined_cache_ensure(const Material &ma,
                                                const CombinedInputs &inputs,
                                                const CombinedPreviewLighting &lighting,
                                                const CombinedCacheRequest &request,
                                                uint64_t *r_revision,
                                                rcti *r_changed_region,
                                                CombinedEvalStats *r_stats)
{
  if (r_stats != nullptr) {
    *r_stats = CombinedEvalStats{};
  }
  /* Cleared up front so that every exit below, including the ones that produce nothing, reports
   * "no pixels moved" rather than leaving the caller's rectangle as it found it. */
  if (r_changed_region != nullptr) {
    BLI_rcti_init(r_changed_region, 0, 0, 0, 0);
  }
  if (!combined_inputs_are_usable(inputs)) {
    return nullptr;
  }

  const uint32_t key = ma.id.session_uid;
  CombinedCacheEntry &entry = g_cache.entries.lookup_or_add_default(key);
  entry.last_use = ++g_cache.use_counter;

  /* The entry takes the *output* resolution: the buffer still covers the whole canvas, but at
   * whatever coarseness the caller asked for. Every rectangle below -- `changed_region`, `clip`,
   * `valid_region`, `dirty_region` -- is therefore in output coordinates. */
  const int out_width = inputs.out_width();
  const int out_height = inputs.out_height();
  const bool size_changed = entry.ibuf == nullptr || entry.width != out_width ||
                            entry.height != out_height;
  /* Recorded unconditionally rather than inside the branch below: a canvas resize the octave
   * absorbs -- 4096 to 2048 while the display cap keeps the output at 1024 -- leaves the buffer
   * the same size, and the entry would go on reporting the old canvas. */
  entry.canvas_width = inputs.width;
  entry.canvas_height = inputs.height;

  if (size_changed) {
    entry.ibuf.reset(IMB_allocImBuf(uint(out_width), uint(out_height), ImBufFlags::FloatData));
    if (entry.ibuf == nullptr) {
      g_cache.entries.remove(key);
      return nullptr;
    }
    entry.ibuf->channels = 4;
    entry.width = out_width;
    entry.height = out_height;
    /* Scene linear, so the editor's view transform applies to it as it does to a render result.
     * This is the intended departure from the byte, display-referred channel passes. */
    IMB_colormanagement_assign_float_colorspace(
        entry.ibuf.get(), IMB_colormanagement_role_colorspace_name_get(COLOR_ROLE_SCENE_LINEAR));
  }

  /* What the images the inputs read directly say changed since the last call.
   *
   * Placed after the reallocation above because the poll scales what it finds into the entry's own
   * output pixels, and those dimensions are only current once the buffer is. A resize keeps its
   * subscriptions -- nothing about the images changed -- and gets its full rebuild from
   * `size_changed` instead. */
  combined_entry_poll_dependencies(entry, request.dependency_images);

  const uint64_t lighting_hash = lighting.hash();
  const bool rebuild_all = size_changed || entry.inputs_hash != request.inputs_hash ||
                           entry.lighting_hash != lighting_hash || entry.dirty_full;

  if (size_changed) {
    BLI_rcti_init(&entry.valid_region, 0, 0, 0, 0);
  }

  if (!BLI_rcti_is_empty(&request.changed_region)) {
    if (BLI_rcti_is_empty(&entry.dirty_region)) {
      entry.dirty_region = request.changed_region;
    }
    else {
      BLI_rcti_union(&entry.dirty_region, &request.changed_region);
    }
  }

  /* What the caller is about to read. An empty clip means the whole buffer, which is what every
   * caller but the viewport passes -- the eyedropper and the scopes read pixels this function was
   * never told about, so they must never be served a partially shaded buffer. */
  rcti buffer_rect;
  BLI_rcti_init(&buffer_rect, 0, entry.width, 0, entry.height);
  rcti need = buffer_rect;
  if (!BLI_rcti_is_empty(&request.clip) && !BLI_rcti_isect(&request.clip, &buffer_rect, &need)) {
    /* The clip is entirely off the canvas: nothing to shade, and nothing became invalid. */
    if (r_revision != nullptr) {
      *r_revision = entry.revision;
    }
    combined_cache_enforce_budget(key);
    return entry.ibuf.get();
  }

  PAINT_CHANNEL_PERF_COMBINED_SET_REBUILD(
      rebuild_all, BLI_rcti_size_x(&entry.dirty_region), BLI_rcti_size_y(&entry.dirty_region));

  rcti to_shade;
  BLI_rcti_init(&to_shade, 0, 0, 0, 0);
  if (rebuild_all) {
    /* Nothing in the buffer survives the change, so validity restarts from what is shaded now. The
     * dirty region goes with it: every pixel outside `need` is now outside #valid_region too, and
     * the invariant already reports those as out of date. */
    BLI_rcti_init(&entry.valid_region, 0, 0, 0, 0);
    BLI_rcti_init(&entry.dirty_region, 0, 0, 0, 0);
    to_shade = need;
  }
  else {
    rcti dirty_visible;
    if (BLI_rcti_isect(&entry.dirty_region, &need, &dirty_visible)) {
      to_shade = dirty_visible;
    }
    rcti unshaded;
    BLI_rcti_difference_bounds(&need, &entry.valid_region, &unshaded);
    if (!BLI_rcti_is_empty(&unshaded)) {
      if (BLI_rcti_is_empty(&to_shade)) {
        to_shade = unshaded;
      }
      else {
        BLI_rcti_union(&to_shade, &unshaded);
      }
    }
  }

  if (!BLI_rcti_is_empty(&to_shade)) {
    if (!BKE_paint_material_combined_eval(inputs, lighting, entry.ibuf.get(), &to_shade, r_stats))
    {
      g_cache.entries.remove(key);
      return nullptr;
    }
    entry.inputs_hash = request.inputs_hash;
    entry.lighting_hash = lighting_hash;
    entry.dirty_full = false;
    entry.revision = ++g_cache.revision_counter;
    if (r_changed_region != nullptr) {
      *r_changed_region = to_shade;
    }

    /* Whatever was dirty and did not get shaded stays dirty -- when that remainder is a rectangle.
     *
     * When it is not, keeping the superset would be a real cost rather than a rounding: the next
     * call in the same frame would find the whole region dirty again and shade it a second and a
     * third time. The Image Editor resolves the preview three times per redraw, and a stroke's
     * first dab re-flattens the composite whole, so this is the common case, not a corner.
     *
     * The way out is to change which of the two rectangles carries the information. "Everything
     * except the part just shaded" has no single-rectangle form as a *dirty* region, but its
     * complement does: what was shaded is clean, and everything else is simply not valid. So the
     * dirty region is dropped and #valid_region becomes exactly what was shaded. Nothing is lost
     * -- a pixel outside it is reported out of date either way -- and the repetition disappears.
     */
    rcti dirty_left;
    if (BLI_rcti_difference_bounds(&entry.dirty_region, &to_shade, &dirty_left)) {
      entry.dirty_region = dirty_left;
      combined_rect_absorb(entry.valid_region, to_shade);
    }
    else {
      BLI_rcti_init(&entry.dirty_region, 0, 0, 0, 0);
      entry.valid_region = to_shade;
    }
  }

  if (r_revision != nullptr) {
    *r_revision = entry.revision;
  }
  combined_cache_enforce_budget(key);
  return entry.ibuf.get();
}

void BKE_paint_material_combined_cache_invalidate(const Material *ma)
{
  if (ma == nullptr) {
    for (CombinedCacheEntry &entry : g_cache.entries.values()) {
      entry.dirty_full = true;
    }
    return;
  }
  if (CombinedCacheEntry *entry = g_cache.entries.lookup_ptr(ma->id.session_uid)) {
    entry->dirty_full = true;
  }
}

void BKE_paint_material_combined_cache_free_material(const Material &ma)
{
  if (g_cache.entries.is_empty()) {
    /* This runs from #ID free, so it is on the path of every material in every file ever loaded,
     * almost none of which was ever previewed. */
    return;
  }
  /* Removing is the whole drop: the entry owns its buffer outright. */
  g_cache.entries.remove(ma.id.session_uid);
}

void BKE_paint_material_combined_cache_free_all()
{
  /* Clearing is the whole teardown: every entry owns its buffer outright. */
  g_cache.entries.clear();
}

bool BKE_paint_material_combined_cache_contains(const Material &ma)
{
  return g_cache.entries.contains(ma.id.session_uid);
}

bool BKE_paint_material_combined_cache_size_get(const Material &ma, int &r_width, int &r_height)
{
  const CombinedCacheEntry *entry = g_cache.entries.lookup_ptr(ma.id.session_uid);
  if (entry == nullptr || entry->ibuf == nullptr) {
    return false;
  }
  /* Deliberately does not touch #CombinedCacheEntry.last_use: a size query is not a use, and
   * letting one keep an entry alive would defeat the budget's eviction order. */
  /* The canvas, not the buffer: the preview may be shaded at a fraction of it, and no caller of
   * #ED_space_image_get_size wants that fraction. */
  r_width = entry->canvas_width;
  r_height = entry->canvas_height;
  return true;
}

bool BKE_paint_material_combined_cache_output_size_get(const Material &ma,
                                                       int &r_width,
                                                       int &r_height)
{
  const CombinedCacheEntry *entry = g_cache.entries.lookup_ptr(ma.id.session_uid);
  if (entry == nullptr || entry->ibuf == nullptr) {
    return false;
  }
  /* Like #BKE_paint_material_combined_cache_size_get, deliberately not a use. */
  r_width = entry->width;
  r_height = entry->height;
  return true;
}

/** \} */

}  // namespace blender
