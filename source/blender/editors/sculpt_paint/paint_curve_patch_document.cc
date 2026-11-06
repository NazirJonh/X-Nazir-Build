/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "paint_curve_patch_document.hh"

#include <algorithm>
#include <cstdint>

#include "BKE_attribute.hh"
#include "BKE_attribute_enums.hh"

#include "BLI_index_range.hh"
#include "BLI_string_ref.hh"

namespace blender::ed::sculpt_paint {

namespace {

constexpr uint64_t revision_seed = 1469598103934665603ull;
constexpr uint64_t revision_prime = 1099511628211ull;

void revision_hash_u64(uint64_t &hash, const uint64_t value)
{
  for (const int i : IndexRange(8)) {
    hash ^= (value >> (i * 8)) & 0xff;
    hash *= revision_prime;
  }
}

void revision_hash_string(uint64_t &hash, const StringRef value)
{
  revision_hash_u64(hash, value.size());
  for (const char c : value) {
    hash ^= uint64_t(static_cast<uint8_t>(c));
    hash *= revision_prime;
  }
}

template<typename T>
void revision_hash_value(uint64_t &hash, const T &value)
{
  const auto *bytes = reinterpret_cast<const uint8_t *>(&value);
  for (const int i : IndexRange(sizeof(T))) {
    hash ^= bytes[i];
    hash *= revision_prime;
  }
}

template<typename T, typename HashValue>
void revision_hash_attribute(uint64_t &hash,
                             const bke::AttributeIter &iter,
                             const HashValue &hash_value)
{
  const bke::AttributeReader<T> attribute = iter.get<T>();
  if (!attribute) {
    return;
  }
  revision_hash_u64(hash, attribute.varray.size());
  for (const int i : IndexRange(attribute.varray.size())) {
    hash_value(hash, attribute.varray[i]);
  }
}

void revision_hash_params(uint64_t &hash, const CurvePatchParams &params)
{
  /* The binding below names EVERY field of #CurvePatchParams in declaration order: adding,
   * removing or reordering one breaks the compile, which is the signal to update this function in
   * the same change. A field missed here would not change the document revision, and secondary
   * targets would silently keep a stale mirror of the patch. */
  const auto &[radius,
               radius_per_size,
               swap_axis,
               length_mode,
               length_repeat,
               radius_interp,
               end_falloff_mode,
               end_falloff_percent,
               start_point_shape,
               end_point_shape,
               stamp_mode,
               stamp_layout,
               stamp_use_random_angle,
               stamp_size_random,
               stamp_strength_random,
               stamp_projection,
               stamp_seed,
               spacing_frac,
               jitter_amount,
               base_angle,
               random_angle,
               plane_normal,
               opacity,
               hidden,
               final_quality] = params;

  revision_hash_value(hash, radius);
  revision_hash_value(hash, radius_per_size);
  revision_hash_value(hash, swap_axis);
  revision_hash_value(hash, length_mode);
  revision_hash_value(hash, length_repeat);
  revision_hash_value(hash, radius_interp);
  revision_hash_value(hash, end_falloff_mode);
  revision_hash_value(hash, end_falloff_percent);
  revision_hash_value(hash, start_point_shape);
  revision_hash_value(hash, end_point_shape);
  revision_hash_value(hash, stamp_mode);
  revision_hash_value(hash, stamp_layout);
  revision_hash_value(hash, stamp_use_random_angle);
  revision_hash_value(hash, stamp_size_random);
  revision_hash_value(hash, stamp_strength_random);
  revision_hash_value(hash, stamp_projection);
  revision_hash_value(hash, stamp_seed);
  revision_hash_value(hash, spacing_frac);
  revision_hash_value(hash, jitter_amount);
  revision_hash_value(hash, base_angle);
  revision_hash_value(hash, random_angle);
  revision_hash_value(hash, plane_normal.x);
  revision_hash_value(hash, plane_normal.y);
  revision_hash_value(hash, plane_normal.z);
  revision_hash_value(hash, opacity);
  revision_hash_value(hash, hidden);
  revision_hash_value(hash, final_quality);
}

void revision_hash_curve(uint64_t &hash, const bke::CurvesGeometry &curve)
{
  revision_hash_u64(hash, uint64_t(curve.points_num()));
  revision_hash_u64(hash, uint64_t(curve.curves_num()));
  curve.attributes().foreach_attribute([&](const bke::AttributeIter &iter) {
    revision_hash_string(hash, iter.name);
    revision_hash_u64(hash, uint64_t(iter.domain));
    revision_hash_u64(hash, uint64_t(iter.data_type));
    switch (iter.data_type) {
      case bke::AttrType::Bool:
        revision_hash_attribute<bool>(
            hash, iter, [](uint64_t &h, const bool value) { revision_hash_value(h, value); });
        break;
      case bke::AttrType::Int8:
        revision_hash_attribute<int8_t>(
            hash, iter, [](uint64_t &h, const int8_t value) { revision_hash_value(h, value); });
        break;
      case bke::AttrType::Int32:
        revision_hash_attribute<int>(
            hash, iter, [](uint64_t &h, const int value) { revision_hash_value(h, value); });
        break;
      case bke::AttrType::Int32_2D:
        revision_hash_attribute<int2>(
            hash,
            iter,
            [](uint64_t &h, const int2 value) {
              revision_hash_value(h, value.x);
              revision_hash_value(h, value.y);
            });
        break;
      case bke::AttrType::Float:
        revision_hash_attribute<float>(
            hash, iter, [](uint64_t &h, const float value) { revision_hash_value(h, value); });
        break;
      case bke::AttrType::Float2:
        revision_hash_attribute<float2>(
            hash,
            iter,
            [](uint64_t &h, const float2 value) {
              revision_hash_value(h, value.x);
              revision_hash_value(h, value.y);
            });
        break;
      case bke::AttrType::Float3:
        revision_hash_attribute<float3>(
            hash,
            iter,
            [](uint64_t &h, const float3 value) {
              revision_hash_value(h, value.x);
              revision_hash_value(h, value.y);
              revision_hash_value(h, value.z);
            });
        break;
      case bke::AttrType::Float4:
        revision_hash_attribute<float4>(
            hash,
            iter,
            [](uint64_t &h, const float4 value) {
              revision_hash_value(h, value.x);
              revision_hash_value(h, value.y);
              revision_hash_value(h, value.z);
              revision_hash_value(h, value.w);
            });
        break;
      default:
        /* Curve Patch's authored attributes are all among the types above. An unknown attribute
         * still contributes its name and type, so adding or removing one invalidates the mirror. */
        break;
    }
  });
}

}  // namespace

uint64_t curve_patch_document_content_revision(const CurvePatchDocument &document)
{
  uint64_t hash = revision_seed;
  revision_hash_u64(hash, uint64_t(document.patches.size()));
  revision_hash_u64(hash, uint64_t(document.active_patch));
  for (const CurvePatchItem &item : document.patches) {
    revision_hash_u64(hash, uint64_t(int64_t(item.source_curve_index)));
    revision_hash_u64(hash, uint64_t(int64_t(item.curve_id)));
    revision_hash_string(hash, item.name);
    revision_hash_params(hash, item.params);
    revision_hash_curve(hash, item.control_curve);
  }
  return hash;
}

int curve_patch_index_for_source_curve(const CurvePatchDocument &document,
                                       const int source_curve_index)
{
  for (const int i : document.patches.index_range()) {
    if (document.patches[i].source_curve_index == source_curve_index) {
      return i;
    }
  }
  return -1;
}

void curve_patch_document_active_patch_set(CurvePatchDocument &document, const int index)
{
  if (document.patches.is_empty()) {
    document.active_patch = 0;
    document.active_point = -1;
    return;
  }
  document.active_patch = std::clamp(index, 0, int(document.patches.size()) - 1);
  /* The point belonged to the patch that was active until now. */
  document.active_point = -1;
  document.texture_pick_point = -1;
}

}  // namespace blender::ed::sculpt_paint
