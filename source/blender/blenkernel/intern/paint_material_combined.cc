/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_paint_material_combined.hh"

#include <algorithm>
#include <cmath>

#include "BLI_hash.hh"
#include "BLI_index_range.hh"
#include "BLI_map.hh"
#include "BLI_math_base.h"
#include "BLI_math_color.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_rect.h"
#include "BLI_task.hh"
#include "BLI_time.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

namespace blender {

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
  uint64_t hash = get_default_hash(this->lights_num,
                                   this->ambient_color,
                                   this->exposure,
                                   this->ao_influence);
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
  if (dst_ibuf->x != inputs.width || dst_ibuf->y != inputs.height) {
    return false;
  }
  /* Strict: a channel whose buffer disagrees is a gatherer bug, and absorbing it here would hide
   * the size mismatch behind plausible pixels. */
  for (const CombinedChannelInput &channel : inputs.channels) {
    if (channel.ibuf != nullptr &&
        (channel.ibuf->x != inputs.width || channel.ibuf->y != inputs.height))
    {
      return false;
    }
  }

  rcti area;
  BLI_rcti_init(&area, 0, inputs.width, 0, inputs.height);
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

  const IndexRange rows(area.ymin, BLI_rcti_size_y(&area));
  /* Grained by texels rather than by rows, so a task is the same size whatever the canvas is: a
   * fixed 64 rows is 16k texels on a 256-wide dab region and 256k on a 4096-wide canvas, and the
   * latter is coarse enough to leave threads idle at the tail of every full rebuild. */
  const int64_t grain = std::max<int64_t>(1, 65536 / std::max(area_width, 1));
  threading::parallel_for(rows, grain, [&](const IndexRange range) {
    for (const int y : range) {
      for (const int x : IndexRange(area_xmin, area_width)) {
        const int64_t texel = int64_t(y) * width + x;

        const float4 base = base_reader.read(texel);
        const float metallic = std::clamp(metallic_reader.read(texel).x, 0.0f, 1.0f);
        const float roughness = std::clamp(roughness_reader.read(texel).x, 0.0f, 1.0f);
        const float specular = specular_reader.read(texel).x;
        const float ao_map = ao_reader.read(texel).x;
        const float alpha = alpha_reader.read(texel).x;
        const float4 emission = emission_reader.read(texel);

        float3 normal(0.0f, 0.0f, 1.0f);
        if (normal_is_encoded) {
          const float4 encoded = normal_reader.read(texel);
          normal = float3(encoded.x, encoded.y, encoded.z) * 2.0f - 1.0f;
        }
        else {
          const float4 raw = normal_reader.read(texel);
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

        float *out = dst + texel * 4;
        out[0] = lit.x;
        out[1] = lit.y;
        out[2] = lit.z;
        out[3] = alpha;
      }
    }
  });

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

struct CombinedCacheEntry {
  /** Float RGBA, scene linear. */
  ImBuf *ibuf = nullptr;
  int width = 0;
  int height = 0;
  uint64_t inputs_hash = 0;
  uint64_t lighting_hash = 0;
  /** The whole buffer has to be recomputed. Set on creation, and whenever a region is unknown. */
  bool dirty_full = true;
  /** Bounding rectangle of everything tagged since the last evaluation. */
  rcti dirty_region = {0, 0, 0, 0};
  /**
   * Images the inputs read directly. Layer-stack images are absent by design: their edits arrive
   * as a `changed_region` from the composite that owns them.
   */
  Vector<uint32_t> image_session_uids;
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

static void combined_entry_free(CombinedCacheEntry &entry)
{
  if (entry.ibuf != nullptr) {
    IMB_freeImBuf(entry.ibuf);
    entry.ibuf = nullptr;
  }
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
    CombinedCacheEntry &entry = g_cache.entries.lookup(key);
    total -= combined_entry_size_in_bytes(entry);
    combined_entry_free(entry);
    g_cache.entries.remove(key);
  }
}

/** Whether \a inputs would produce anything at all. */
static bool combined_inputs_are_usable(const CombinedInputs &inputs)
{
  return inputs.width > 0 && inputs.height > 0;
}

ImBuf *BKE_paint_material_combined_cache_ensure(const Material &ma,
                                                const CombinedInputs &inputs,
                                                const CombinedPreviewLighting &lighting,
                                                const uint64_t inputs_hash,
                                                const rcti &changed_region,
                                                const Span<uint32_t> dependency_image_uids,
                                                uint64_t *r_revision,
                                                CombinedEvalStats *r_stats)
{
  if (r_stats != nullptr) {
    *r_stats = CombinedEvalStats{};
  }
  if (!combined_inputs_are_usable(inputs)) {
    return nullptr;
  }

  const uint32_t key = ma.id.session_uid;
  CombinedCacheEntry &entry = g_cache.entries.lookup_or_add_default(key);
  entry.last_use = ++g_cache.use_counter;

  const bool size_changed = entry.ibuf == nullptr || entry.width != inputs.width ||
                            entry.height != inputs.height;
  if (size_changed) {
    combined_entry_free(entry);
    entry.ibuf = IMB_allocImBuf(uint(inputs.width), uint(inputs.height), ImBufFlags::FloatData);
    if (entry.ibuf == nullptr) {
      g_cache.entries.remove(key);
      return nullptr;
    }
    entry.ibuf->channels = 4;
    entry.width = inputs.width;
    entry.height = inputs.height;
    /* Scene linear, so the editor's view transform applies to it as it does to a render result.
     * This is the intended departure from the byte, display-referred channel passes. */
    IMB_colormanagement_assign_float_colorspace(
        entry.ibuf, IMB_colormanagement_role_colorspace_name_get(COLOR_ROLE_SCENE_LINEAR));
  }

  const uint64_t lighting_hash = lighting.hash();
  const bool rebuild_all = size_changed || entry.inputs_hash != inputs_hash ||
                           entry.lighting_hash != lighting_hash || entry.dirty_full;

  if (!BLI_rcti_is_empty(&changed_region)) {
    if (BLI_rcti_is_empty(&entry.dirty_region)) {
      entry.dirty_region = changed_region;
    }
    else {
      BLI_rcti_union(&entry.dirty_region, &changed_region);
    }
  }

  const bool rebuild_region = !rebuild_all && !BLI_rcti_is_empty(&entry.dirty_region);

  if (rebuild_all || rebuild_region) {
    const rcti *region = rebuild_all ? nullptr : &entry.dirty_region;
    if (!BKE_paint_material_combined_eval(inputs, lighting, entry.ibuf, region, r_stats)) {
      combined_entry_free(entry);
      g_cache.entries.remove(key);
      return nullptr;
    }
    entry.inputs_hash = inputs_hash;
    entry.lighting_hash = lighting_hash;
    entry.dirty_full = false;
    entry.revision = ++g_cache.revision_counter;
    BLI_rcti_init(&entry.dirty_region, 0, 0, 0, 0);
    entry.image_session_uids.clear();
    for (const uint32_t uid : dependency_image_uids) {
      entry.image_session_uids.append_non_duplicates(uid);
    }
  }

  if (r_revision != nullptr) {
    *r_revision = entry.revision;
  }
  combined_cache_enforce_budget(key);
  return entry.ibuf;
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

void BKE_paint_material_combined_cache_tag_image_region(const Image &image, const rcti &region)
{
  const uint32_t session_uid = image.id.session_uid;
  for (CombinedCacheEntry &entry : g_cache.entries.values()) {
    if (!entry.image_session_uids.contains(session_uid)) {
      continue;
    }
    if (BLI_rcti_is_empty(&entry.dirty_region)) {
      entry.dirty_region = region;
    }
    else {
      BLI_rcti_union(&entry.dirty_region, &region);
    }
  }
}

void BKE_paint_material_combined_cache_tag_image_changed(const Image &image)
{
  const uint32_t session_uid = image.id.session_uid;
  for (CombinedCacheEntry &entry : g_cache.entries.values()) {
    if (entry.image_session_uids.contains(session_uid)) {
      entry.dirty_full = true;
    }
  }
}

void BKE_paint_material_combined_cache_free_material(const Material &ma)
{
  if (g_cache.entries.is_empty()) {
    /* This runs from #ID free, so it is on the path of every material in every file ever loaded,
     * almost none of which was ever previewed. */
    return;
  }
  if (CombinedCacheEntry *entry = g_cache.entries.lookup_ptr(ma.id.session_uid)) {
    combined_entry_free(*entry);
    g_cache.entries.remove(ma.id.session_uid);
  }
}

void BKE_paint_material_combined_cache_free_all()
{
  for (CombinedCacheEntry &entry : g_cache.entries.values()) {
    combined_entry_free(entry);
  }
  g_cache.entries.clear();
}

bool BKE_paint_material_combined_cache_contains(const Material &ma)
{
  return g_cache.entries.contains(ma.id.session_uid);
}

/** \} */

}  // namespace blender
