/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The space of a transform effect's step: a Mirror or Transform Mapping may act in a Decal frame of
 * its own instead of in UV, on a row that reads its map at the surface UV (a Paint row, a Fill in
 * UV, a Material row). The row's map stays what strokes write; only its read moves:
 *
 *   destination texel `u_t` -> `q = D[u_t]` (forward Decal map) -> the step over `c = 0.5 + 0.5 q`
 *   -> `q'` -> `UV' = P[q']` (inverse Decal map) -> the row's map read at `UV'`.
 *
 * So the content follows the surface across UV seams and cube edges, and a fold or a move is clipped
 * by the patch's border rather than by the UV islands.
 */

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include "BKE_paint_material_composite.hh"
#include "BKE_paint_material_decal_map.hh"
#include "BKE_paint_perf_debug.hh"

#include "BLI_array.hh"
#include "BLI_hash.hh"
#include "BLI_index_range.hh"
#include "BLI_math_vector.hh"
#include "BLI_task.hh"
#include "BLI_time.h"
#include "BLI_vector.hh"

#include "DNA_material_types.h"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "paint_lru_cache.hh"
#include "paint_material_composite_projection_internal.hh"

namespace blender {

/** The inverse map's resolution cap: an int per cell, 16 MiB at the cap. */
static constexpr int effect_space_inverse_resolution_max = 2048;
/** The patch reaches the frame's corners, not only its sides; see #BKE_paint_material_decal_map_get. */
static constexpr float effect_space_radius_scale = 1.41421356f;

/* -------------------------------------------------------------------- */
/** \name Resolve
 * \{ */

static void effect_space_record(CompositeEffectSpaceTrace *trace,
                                const CompositeEffectSpaceEvent::Type type,
                                const int step,
                                const int tri,
                                const float side_u = 1.0f,
                                const float side_v = 1.0f)
{
  if (trace == nullptr || trace->events_num == CompositeEffectSpaceChain::MAX_EVENTS) {
    return;
  }
  CompositeEffectSpaceEvent &event = trace->events[trace->events_num++];
  event.type = type;
  event.step = int8_t(step);
  event.tri = tri;
  event.side[0] = side_u;
  event.side[1] = side_v;
}

CompositeEffectSpaceResult composite_effect_space_resolve(const CompositeEffectSpaceChain &chain,
                                                          const float2 &uv,
                                                          const int64_t texel_index,
                                                          float2 &r_uv,
                                                          CompositeEffectSpaceTrace *r_trace)
{
  using Type = CompositeEffectSpaceEvent::Type;
  if (r_trace != nullptr) {
    r_trace->events_num = 0;
  }
  float2 coord = uv;
  float2 q(0.0f);
  bool in_decal = false;
  const auto exit_decal = [&]() -> bool {
    int tri = -1;
    if (chain.inverse_map == nullptr ||
        !BKE_paint_material_decal_inverse_lookup(*chain.decal_map, *chain.inverse_map, q, coord, tri))
    {
      return false;
    }
    effect_space_record(r_trace, Type::ExitDecal, -1, tri);
    in_decal = false;
    return true;
  };

  for (int i = 0; i < chain.steps_num; i++) {
    const CompositeEffectSpaceStep &step = chain.steps[i];
    if (step.decal) {
      if (!in_decal) {
        if (chain.decal_map == nullptr) {
          return CompositeEffectSpaceResult::OutsidePatch;
        }
        int tri = -1;
        if (i == 0 && texel_index >= 0 && texel_index < chain.decal_map->patch_tri.size()) {
          /* The destination texel itself: the forward map already holds its `q`. */
          tri = chain.decal_map->patch_tri[texel_index];
          if (tri < 0) {
            return CompositeEffectSpaceResult::OutsidePatch;
          }
          q = chain.decal_map->q[texel_index];
        }
        else if (!BKE_paint_material_decal_forward_lookup(*chain.decal_map, coord, q, tri)) {
          return CompositeEffectSpaceResult::OutsidePatch;
        }
        effect_space_record(r_trace, Type::EnterDecal, -1, tri);
        in_decal = true;
      }
      /* The step reads the frame like a UV square: its footprint `[-1, 1]` is `[0, 1]`, so the
       * Mirror center (0.5, 0.5) is the frame's center and a clipping wrap clips to the frame. */
      float cu = 0.5f + 0.5f * q.x;
      float cv = 0.5f + 0.5f * q.y;
      effect_space_record(
          r_trace, Type::Step, i, -1, step.transform.side_u(cu), step.transform.side_v(cv));
      if (!step.transform.apply_fold_affine(cu, cv, cu, cv)) {
        return CompositeEffectSpaceResult::Transparent;
      }
      q = float2((cu - 0.5f) * 2.0f, (cv - 0.5f) * 2.0f);
      continue;
    }
    if (in_decal && !exit_decal()) {
      return CompositeEffectSpaceResult::OutsidePatch;
    }
    effect_space_record(r_trace,
                        Type::Step,
                        i,
                        -1,
                        step.transform.side_u(coord.x),
                        step.transform.side_v(coord.y));
    if (!step.transform.apply_fold_affine(coord.x, coord.y, coord.x, coord.y)) {
      return CompositeEffectSpaceResult::Transparent;
    }
  }
  if (in_decal && !exit_decal()) {
    return CompositeEffectSpaceResult::OutsidePatch;
  }
  r_uv = coord;
  return CompositeEffectSpaceResult::Mapped;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Resolve Field Cache
 * \{ */

/** The cache's memory cap; a 4K field is ~150 MiB, so a bake's and a preview's fit together. */
static constexpr size_t effect_space_field_cache_bytes_max = size_t(320) << 20;
static constexpr int effect_space_field_cache_entries_max = 2;

static void effect_space_field_key_values(const CompositeEffectSpaceChain &chain,
                                          const int width,
                                          const int height,
                                          Vector<float> &r_values)
{
  r_values.append(float(width));
  r_values.append(float(height));
  r_values.append(chain.clip_outside_patch ? 1.0f : 0.0f);
  r_values.append(float(chain.steps_num));
  /* The inverse map is keyed by the forward one and its resolution; its bounds stand in for it. */
  if (chain.inverse_map != nullptr) {
    r_values.append(float(chain.inverse_map->width));
    r_values.append(float(chain.inverse_map->height));
    r_values.append(chain.inverse_map->q_min.x);
    r_values.append(chain.inverse_map->q_min.y);
    r_values.append(chain.inverse_map->q_max.x);
    r_values.append(chain.inverse_map->q_max.y);
  }
  if (chain.decal_map != nullptr) {
    r_values.append(float(chain.decal_map->patch_tris_num));
  }
  for (int i = 0; i < chain.steps_num; i++) {
    const CompositeEffectSpaceStep &s = chain.steps[i];
    const CompositeMappingTransform &t = s.transform;
    const float values[] = {
        s.decal ? 1.0f : 0.0f,
        t.scale_x,
        t.scale_y,
        t.cos_rot,
        t.sin_rot,
        t.offset_u,
        t.offset_v,
        t.mirror_u ? 1.0f : 0.0f,
        t.mirror_v ? 1.0f : 0.0f,
        t.mirror_negative_u ? 1.0f : 0.0f,
        t.mirror_negative_v ? 1.0f : 0.0f,
        t.mirror_center_u,
        t.mirror_center_v,
        t.repeat_u ? 1.0f : 0.0f,
        t.repeat_v ? 1.0f : 0.0f,
    };
    for (const float value : values) {
      r_values.append(value);
    }
  }
}

struct CompositeEffectSpaceFieldKey {
  PaintMaterialDecalMapKey map_key;
  int width = 0;
  int height = 0;
  Vector<float> values;

  bool operator==(const CompositeEffectSpaceFieldKey &other) const
  {
    return width == other.width && height == other.height && map_key == other.map_key &&
           values == other.values;
  }
};

static bke::LruByteCache<CompositeEffectSpaceFieldKey, CompositeEffectSpaceField>
    g_effect_space_field_cache(effect_space_field_cache_bytes_max,
                               effect_space_field_cache_entries_max);

std::shared_ptr<const CompositeEffectSpaceField> composite_effect_space_field_get(
    const CompositeEffectSpaceChain &chain,
    const int width,
    const int height,
    bool &r_hit,
    double &r_build_seconds)
{
  r_hit = false;
  r_build_seconds = 0.0;
  if (width <= 0 || height <= 0 || chain.decal_map == nullptr) {
    return nullptr;
  }
  Vector<float> key_values;
  effect_space_field_key_values(chain, width, height, key_values);
  CompositeEffectSpaceFieldKey key;
  key.map_key = chain.decal_map->key;
  key.width = width;
  key.height = height;
  key.values = std::move(key_values);

  const size_t new_bytes = size_t(width) * size_t(height) * (sizeof(float2) + 1);
  bool built = false;
  const double time_start = BLI_time_now_seconds();
  const std::shared_ptr<const CompositeEffectSpaceField> field =
      g_effect_space_field_cache.get_or_build(
          key,
          [&]() -> std::shared_ptr<const CompositeEffectSpaceField> {
            auto built_field = std::make_shared<CompositeEffectSpaceField>();
            built_field->width = width;
            built_field->height = height;
            built_field->map_key = key.map_key;
            built_field->key_values = key.values;
            built_field->src_uv = Array<float2>(int64_t(width) * height, float2(0.0f));
            built_field->status = Array<uint8_t>(int64_t(width) * height,
                                                 uint8_t(CompositeEffectSpaceResult::Mapped));
            float2 *src_uv_data = built_field->src_uv.data();
            uint8_t *status_data = built_field->status.data();
            threading::parallel_for(IndexRange(height), 8, [&](const IndexRange rows) {
              for (const int y : rows) {
                const float v = (float(y) + 0.5f) / float(height);
                for (int x = 0; x < width; x++) {
                  const float u = (float(x) + 0.5f) / float(width);
                  const int64_t index = int64_t(y) * width + x;
                  float2 src_uv(0.0f);
                  const CompositeEffectSpaceResult result = composite_effect_space_resolve(
                      chain, float2(u, v), index, src_uv, nullptr);
                  status_data[index] = uint8_t(result);
                  src_uv_data[index] = src_uv;
                }
              }
            });
            return built_field;
          },
          &built,
          new_bytes);
  if (built) {
    r_build_seconds = BLI_time_now_seconds() - time_start;
  }
  r_hit = !built;
  return field;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tangent Normal
 * \{ */

void composite_effect_space_remap_normal(const CompositeEffectSpaceChain &chain,
                                         const CompositeEffectSpaceTrace &trace,
                                         float3 &r_normal)
{
  using Type = CompositeEffectSpaceEvent::Type;
  float3 n = r_normal;
  for (int i = trace.events_num - 1; i >= 0; i--) {
    const CompositeEffectSpaceEvent &event = trace.events[i];
    switch (event.type) {
      case Type::Step: {
        /* One step's covariant inverse is the UV chain's own, on whichever 2D basis the vector is
         * in at this point: the tangent basis in UV, the unfolded plane's in the frame. */
        CompositeMappingChain single;
        single.steps[0] = chain.steps[event.step].transform;
        single.steps_num = 1;
        const float sides[1][2] = {{event.side[0], event.side[1]}};
        float xy[2] = {n.x, n.y};
        single.remap_tangent_normal(sides, xy);
        n.x = xy[0];
        n.y = xy[1];
        break;
      }
      case Type::ExitDecal: {
        /* Backwards from the source: the vector is in the source triangle's tangent basis; turn it
         * into the unfolded plane's basis on that triangle. */
        const PaintMaterialDecalTri &tri = chain.decal_map->tris[event.tri];
        if (math::is_zero(tri.tangent)) {
          /* A degenerate UV has no basis the map could have been stored in; leave the vector. */
          break;
        }
        const float3 n_object = tri.tangent * n.x + tri.bitangent * n.y + tri.normal * n.z;
        n = float3(math::dot(n_object, tri.plane_x),
                   math::dot(n_object, tri.plane_y),
                   math::dot(n_object, tri.normal));
        break;
      }
      case Type::EnterDecal: {
        /* The unfold is an isometry per triangle, so plane components carry over unchanged; the
         * destination triangle lays them back on the surface in its own tangent basis. */
        const PaintMaterialDecalTri &tri = chain.decal_map->tris[event.tri];
        if (math::is_zero(tri.tangent)) {
          break;
        }
        const float3 n_object = tri.plane_x * n.x + tri.plane_y * n.y + tri.normal * n.z;
        n = float3(math::dot(n_object, tri.tangent),
                   math::dot(n_object, tri.bitangent),
                   math::dot(n_object, tri.normal));
        break;
      }
    }
  }
  /* The UV tangent basis is only approximately orthonormal; a degenerate result passes through. */
  const float length = math::length(n);
  if (length > 1e-12f && std::isfinite(length)) {
    r_normal = n / length;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Resample
 * \{ */

ImBuf *BKE_paint_material_composite_buffer_remap(const ImBuf *src,
                                                 const int width,
                                                 const int height,
                                                 const Span<PaintMaterialCompositeMapping> steps)
{
  /* The same chain and the same resample a map read goes through, so a grey moved here lands where
   * the row's own mapping would put a map. No row mapping joins: the steps are all there is. */
  const PaintMaterialCompositeMapping no_row_mapping;
  const CompositeMappingChain chain = composite_mapping_chain_build(no_row_mapping, steps);
  if (chain.is_identity()) {
    return nullptr;
  }
  return composite_resample_mapped(src, width, height, chain);
}

ImBuf *composite_resample_effect_space(const ImBuf *src,
                                       const int ref_width,
                                       const int ref_height,
                                       const PaintMaterialCompositeMapping &mapping,
                                       const Span<PaintMaterialCompositeMapping> input_steps,
                                       const char * /*role*/)
{
  if (src == nullptr || ref_width <= 0 || ref_height <= 0 || src->x <= 0 || src->y <= 0) {
    return nullptr;
  }
  /* One Decal frame per row: the description copies the first Decal effect's frame onto every
   * Decal step, so the first one is the frame of all. */
  const PaintMaterialCompositeMapping *frame_step = nullptr;
  for (const PaintMaterialCompositeMapping &step : input_steps) {
    if (composite_mapping_step_is_decal(step)) {
      frame_step = &step;
      break;
    }
  }
  if (frame_step == nullptr) {
    return nullptr;
  }
  bke::paint_perf::ScopedTimer perf_timer("composite_resample_effect_space");
  /* A frame derived from a UV point carries its own patch radius, which the scale does not grow. */
  const std::shared_ptr<const PaintMaterialDecalMap> decal_map = BKE_paint_material_decal_map_get(
      frame_step->projection, ref_width, ref_height, effect_space_radius_scale);
  if (decal_map == nullptr) {
    return nullptr;
  }
  const int resolution = std::min(std::max(ref_width, ref_height),
                                  effect_space_inverse_resolution_max);
  const std::shared_ptr<const PaintMaterialDecalInverseMap> inverse_map =
      BKE_paint_material_decal_inverse_map_get(*decal_map, resolution);
  if (inverse_map == nullptr) {
    return nullptr;
  }

  CompositeEffectSpaceChain chain;
  /* Outside the patch the Decal steps cannot be read: the texel reads the UV steps alone, the
   * row's content where the effect does not reach, unless the frame clips. */
  CompositeMappingChain uv_chain;
  bool tangent_normal = mapping.tangent_normal;
  for (const PaintMaterialCompositeMapping &step : input_steps) {
    if (chain.steps_num == CompositeEffectSpaceChain::MAX_STEPS) {
      break;
    }
    CompositeEffectSpaceStep &entry = chain.steps[chain.steps_num++];
    entry.transform = CompositeMappingTransform(step);
    entry.decal = composite_mapping_step_is_decal(step);
    if (!entry.decal) {
      uv_chain.steps[uv_chain.steps_num++] = entry.transform;
    }
    /* The effects' steps carry the Normal flag of their channel, where the row's own mapping only
     * carries it while the row remaps. */
    tangent_normal |= step.tangent_normal;
  }
  if (mapping.enabled && chain.steps_num < CompositeEffectSpaceChain::MAX_STEPS) {
    CompositeEffectSpaceStep &entry = chain.steps[chain.steps_num++];
    entry.transform = CompositeMappingTransform(mapping);
    entry.decal = false;
    uv_chain.steps[uv_chain.steps_num++] = entry.transform;
  }
  chain.decal_map = decal_map.get();
  chain.inverse_map = inverse_map.get();
  chain.clip_outside_patch = frame_step->projection.clip;
  chain.tangent_normal = tangent_normal;
  uv_chain.tangent_normal = tangent_normal;
  perf_timer.note("src=%dx%d -> %dx%d steps=%d inverse=%dx%d clip=%d",
                  src->x,
                  src->y,
                  ref_width,
                  ref_height,
                  chain.steps_num,
                  inverse_map->width,
                  inverse_map->height,
                  int(chain.clip_outside_patch));

  const CompositeProjectionSampler sampler(src, true);
  if (!sampler.valid()) {
    return nullptr;
  }
  /* The resolve does not depend on the row's map, so the layer's channels share one field. A Normal
   * channel needs the per-texel trace, which the field does not hold, and resolves on its own. */
  bool field_hit = false;
  double field_build_seconds = 0.0;
  std::shared_ptr<const CompositeEffectSpaceField> field;
  if (!tangent_normal) {
    field = composite_effect_space_field_get(
        chain, ref_width, ref_height, field_hit, field_build_seconds);
  }
  const float2 *field_uv = field ? field->src_uv.data() : nullptr;
  const uint8_t *field_status = field ? field->status.data() : nullptr;
  ImBuf *dst = composite_alloc_transparent(ref_width, ref_height);
  if (dst == nullptr) {
    return nullptr;
  }
  float *out = dst->float_data_for_write();

  const auto encode_normal = [](float p[4], const float3 &n) {
    p[0] = (0.5f * n.x + 0.5f) * p[3];
    p[1] = (0.5f * n.y + 0.5f) * p[3];
    p[2] = (0.5f * n.z + 0.5f) * p[3];
  };
  const auto decode_normal = [](const float p[4]) {
    const float inv_alpha = 1.0f / p[3];
    return float3(2.0f * p[0] * inv_alpha - 1.0f,
                  2.0f * p[1] * inv_alpha - 1.0f,
                  2.0f * p[2] * inv_alpha - 1.0f);
  };

  /* Every row writes its own texels and the maps are only read, so rows run unsynchronized. */
  threading::parallel_for(IndexRange(ref_height), 8, [&](const IndexRange rows) {
    for (const int y : rows) {
      const float v = (float(y) + 0.5f) / float(ref_height);
      for (int x = 0; x < ref_width; x++) {
        const float u = (float(x) + 0.5f) / float(ref_width);
        const int64_t index = int64_t(y) * ref_width + x;
        float *p = out + index * 4;
        CompositeEffectSpaceTrace trace;
        float2 src_uv(0.0f);
        CompositeEffectSpaceResult result;
        if (field_status != nullptr) {
          result = CompositeEffectSpaceResult(field_status[index]);
          src_uv = field_uv[index];
        }
        else {
          result = composite_effect_space_resolve(
              chain, float2(u, v), index, src_uv, tangent_normal ? &trace : nullptr);
        }
        if (result == CompositeEffectSpaceResult::Transparent) {
          continue;
        }
        if (result == CompositeEffectSpaceResult::OutsidePatch) {
          if (chain.clip_outside_patch) {
            continue;
          }
          float uu;
          float vv;
          float step_sides[CompositeMappingChain::MAX_STEPS][2];
          if (!uv_chain.apply(u, v, uu, vv, step_sides)) {
            continue;
          }
          sampler.sample(uu, vv, true, true, p);
          if (tangent_normal && p[3] > 0.0f) {
            float3 n = decode_normal(p);
            float xy[2] = {n.x, n.y};
            uv_chain.remap_tangent_normal(step_sides, xy);
            encode_normal(p, float3(xy[0], xy[1], n.z));
          }
          continue;
        }
        /* The composed lookup repeats at its end, as the UV chain's does. */
        sampler.sample(src_uv.x, src_uv.y, true, true, p);
        if (tangent_normal && p[3] > 0.0f) {
          float3 n = decode_normal(p);
          composite_effect_space_remap_normal(chain, trace, n);
          encode_normal(p, n);
        }
      }
    }
  });

  return dst;
}

/** \} */

}  // namespace blender
