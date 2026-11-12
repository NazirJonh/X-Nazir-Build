/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The CPU projection of a row map: the frame transform and per-mode UVs the generated graph
 * builds, and the bake of a map into the UV space of the painted object through them.
 */

#include <algorithm>
#include <array>
#include <cmath>

#include "BKE_attribute.hh"
#include "BKE_customdata.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_material_composite.hh"

#include "BLI_array.hh"
#include "BLI_index_range.hh"
#include "BLI_math_base.h"
#include "BLI_math_color.h"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_virtual_array.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_object_types.h"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "paint_material_composite_projection_internal.hh"

namespace blender {

/**
 * A sampler over an acquired source buffer, decoded exactly like #composite_resample_mapped's
 * taps: byte storage is straight and decoded to scene-linear, float storage is premultiplied and
 * straightened for the filter, and the result is premultiplied again.
 */
struct CompositeProjectionSampler {
  const ImBuf *src = nullptr;
  bool src_is_float = false;
  int src_channels = 4;
  const ColorSpace *colorspace = nullptr;
  bool decode = false;
  bool decode_by_table = false;

  explicit CompositeProjectionSampler(const ImBuf *source) : src(source)
  {
    if (src == nullptr) {
      return;
    }
    src_is_float = src->byte_buffer.data == nullptr && src->float_buffer.data != nullptr;
    src_channels = src->channels == 0 ? 4 : src->channels;
    colorspace = src_is_float ? src->float_buffer.colorspace : src->byte_buffer.colorspace;
    decode = BKE_paint_material_composite_colorspace_is_converted(colorspace);
    decode_by_table = decode && !src_is_float && colorspace != nullptr &&
                      IMB_colormanagement_space_name_is_srgb(
                          IMB_colormanagement_colorspace_get_name(colorspace));
  }

  bool valid() const
  {
    return src != nullptr && (src_is_float || src->byte_buffer.data != nullptr);
  }

  static const std::array<float, 256> &srgb_table()
  {
    static const std::array<float, 256> table = []() {
      std::array<float, 256> values;
      for (const int i : IndexRange(256)) {
        values[i] = srgb_to_linearrgb(float(i) * (1.0f / 255.0f));
      }
      return values;
    }();
    return table;
  }

  void tap_straight(const int tx, const int ty, float r_tap[4]) const
  {
    if (!src_is_float) {
      const uchar *pixel = src->byte_data() + (int64_t(ty) * src->x + tx) * 4;
      if (decode_by_table) {
        const std::array<float, 256> &table = srgb_table();
        r_tap[0] = table[pixel[0]];
        r_tap[1] = table[pixel[1]];
        r_tap[2] = table[pixel[2]];
      }
      else {
        r_tap[0] = float(pixel[0]) / 255.0f;
        r_tap[1] = float(pixel[1]) / 255.0f;
        r_tap[2] = float(pixel[2]) / 255.0f;
      }
      r_tap[3] = float(pixel[3]) / 255.0f;
    }
    else {
      const float *pixel = src->float_buffer.data + (int64_t(ty) * src->x + tx) * src_channels;
      r_tap[0] = pixel[0];
      r_tap[1] = src_channels > 1 ? pixel[1] : 0.0f;
      r_tap[2] = src_channels > 2 ? pixel[2] : 0.0f;
      r_tap[3] = src_channels == 4 ? pixel[3] : 1.0f;
      if (r_tap[3] > 0.0f) {
        const float inv = 1.0f / r_tap[3];
        r_tap[0] *= inv;
        r_tap[1] *= inv;
        r_tap[2] *= inv;
      }
    }
    if (decode && !decode_by_table) {
      IMB_colormanagement_colorspace_to_scene_linear_v4(r_tap, false, colorspace);
    }
    r_tap[0] *= r_tap[3];
    r_tap[1] *= r_tap[3];
    r_tap[2] *= r_tap[3];
  }

  /** Bilinear, premultiplied scene-linear tap. False when the coordinate is clipped away. */
  bool sample(float u,
              float v,
              const bool repeat_u,
              const bool repeat_v,
              float r_rgba[4]) const
  {
    r_rgba[0] = r_rgba[1] = r_rgba[2] = r_rgba[3] = 0.0f;
    if (!valid()) {
      return false;
    }
    if (!repeat_u && (u < 0.0f || u >= 1.0f)) {
      return false;
    }
    if (!repeat_v && (v < 0.0f || v >= 1.0f)) {
      return false;
    }
    if (repeat_u) {
      u -= floorf(u);
    }
    if (repeat_v) {
      v -= floorf(v);
    }
    const float su = u * float(src->x) - 0.5f;
    const float sv = v * float(src->y) - 0.5f;
    const int x0 = int(floorf(su));
    const int y0 = int(floorf(sv));
    const float a = su - float(x0);
    const float b = sv - float(y0);
    const auto wrap_texel = [](int i, const int n) {
      const int r = i % n;
      return r < 0 ? r + n : r;
    };
    float t00[4], t10[4], t01[4], t11[4];
    tap_straight(wrap_texel(x0, src->x), wrap_texel(y0, src->y), t00);
    tap_straight(wrap_texel(x0 + 1, src->x), wrap_texel(y0, src->y), t10);
    tap_straight(wrap_texel(x0, src->x), wrap_texel(y0 + 1, src->y), t01);
    tap_straight(wrap_texel(x0 + 1, src->x), wrap_texel(y0 + 1, src->y), t11);
    for (const int k : IndexRange(4)) {
      r_rgba[k] = (1.0f - a) * (1.0f - b) * t00[k] + a * (1.0f - b) * t10[k] +
                  (1.0f - a) * b * t01[k] + a * b * t11[k];
    }
    return true;
  }
};

/** The straight colour and alpha of a premultiplied tap. */
static void composite_straight_from_premul(const float premul[4], float r_rgb[3], float &r_alpha)
{
  r_alpha = premul[3];
  if (r_alpha > 0.0f) {
    const float inv = 1.0f / r_alpha;
    r_rgb[0] = premul[0] * inv;
    r_rgb[1] = premul[1] * inv;
    r_rgb[2] = premul[2] * inv;
  }
  else {
    r_rgb[0] = r_rgb[1] = r_rgb[2] = 0.0f;
  }
}

ImBuf *composite_alloc_transparent(const int width, const int height)
{
  if (width <= 0 || height <= 0) {
    return nullptr;
  }
  ImBuf *dst = IMB_allocImBuf(uint(width), uint(height), ImBufFlags::FloatData);
  if (dst == nullptr || dst->float_data_for_write() == nullptr) {
    if (dst != nullptr) {
      IMB_freeImBuf(dst);
    }
    return nullptr;
  }
  dst->channels = 4;
  std::fill_n(dst->float_data_for_write(), int64_t(width) * height * 4, 0.0f);
  IMB_colormanagement_assign_float_colorspace(
      dst, IMB_colormanagement_role_colorspace_name_get(COLOR_ROLE_SCENE_LINEAR));
  return dst;
}

/** `q = R^-1 (p - center) / size`, the shader's frame-space position. */
static void composite_projection_position(const PaintMaterialCompositeProjection &projection,
                                          const float inv_rot[3][3],
                                          const float p[3],
                                          float r_q[3])
{
  /* Why the CPU repeats the graph here: the Vector Rotate node is Euler XYZ and inverted, and
   * `eul_to_mat3` builds the same XYZ matrix whose transpose is that inverse. The size is
   * normalized once for both sides (#BKE_paint_layers_projection_size_normalize). */
  float offset[3];
  sub_v3_v3v3(offset, p, projection.center);
  mul_v3_m3v3(r_q, inv_rot, offset);
  for (int i = 0; i < 3; i++) {
    const float size = BKE_paint_layers_projection_size_normalize(projection.size[i]);
    r_q[i] /= size;
  }
}

/** The row's 2D UV adjust, applied to a projected UV. */
static void composite_projection_uv_wrap(const PaintMaterialCompositeMapping &mapping,
                                         float &u,
                                         float &v)
{
  if (mapping.enabled) {
    const float c = cosf(mapping.rotation);
    const float s = sinf(mapping.rotation);
    const float ru = u * c - v * s;
    const float rv = u * s + v * c;
    u = ru * BKE_paint_layers_mapping_scale_normalize(mapping.scale[0]) + mapping.offset[0];
    v = rv * BKE_paint_layers_mapping_scale_normalize(mapping.scale[1]) + mapping.offset[1];
  }
  if (mapping.wrap != MA_PAINT_LAYER_MAPPING_WRAP_CLIP) {
    u -= floorf(u);
    v -= floorf(v);
  }
}

/**
 * Bake one map into the painted object's UV space through a projected row, as a float RGBA
 * premultiplied scene-linear buffer. The projection math is the generated graph's: the frame
 * transform, the per-mode UVs, the normal weights of Box and Triplanar, the cube-cross atlas and
 * the frame clip. Null when there is no object, mesh or render UV map.
 */
ImBuf *composite_resample_projected(const ImBuf *src,
                                    const int ref_width,
                                    const int ref_height,
                                    const PaintMaterialCompositeMapping &mapping)
{
  const PaintMaterialCompositeProjection &projection = mapping.projection;
  const Object *object = projection.object;
  if (object == nullptr || object->type != OB_MESH || object->data == nullptr) {
    return nullptr;
  }
  const Mesh &mesh = *id_cast<const Mesh *>(object->data);
  const char *uv_name = CustomData_get_render_layer_name(&mesh.corner_data, CD_PROP_FLOAT2);
  if (uv_name == nullptr) {
    return nullptr;
  }
  const auto &attributes = mesh.attributes();
  const auto uv_attr = attributes.lookup<float2>(uv_name, bke::AttrDomain::Corner);
  if (!uv_attr) {
    return nullptr;
  }
  const VArraySpan<float2> uvs(*uv_attr);
  const Span<float3> positions = mesh.vert_positions();
  const Span<float3> normals = mesh.corner_normals();
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<int3> tris = mesh.corner_tris();
  if (tris.is_empty()) {
    return nullptr;
  }
  if (object->totcol <= 0) {
    return nullptr;
  }
  Array<bool> slot_matches(object->totcol, false);
  int matched_slots = 0;
  for (int slot = 0; slot < object->totcol; slot++) {
    const Material *slot_material = BKE_object_material_get(
        const_cast<Object *>(object), short(slot + 1));
    if (projection.material == nullptr || slot_material == projection.material) {
      slot_matches[slot] = true;
      matched_slots++;
    }
  }
  if (matched_slots == 0) {
    return nullptr;
  }
  const VArray<int> material_indices = *attributes.lookup_or_default<int>(
      "material_index", bke::AttrDomain::Face, 0);
  Array<int> tri_faces(int(tris.size()));
  bke::mesh::corner_tris_calc_face_indices(mesh.faces(), tri_faces);

  CompositeProjectionSampler sampler(src);
  if (!sampler.valid()) {
    return nullptr;
  }
  ImBuf *dst = composite_alloc_transparent(ref_width, ref_height);
  if (dst == nullptr) {
    return nullptr;
  }
  float *out = dst->float_data_for_write();

  /* The painted mesh is in its own object space; a controller Empty moves the frame into its own
   * space first, exactly as a Texture Coordinate node naming that object does in the shader. */
  const bool has_controller = projection.controller != nullptr &&
                              projection.controller != object;
  float4x4 object_to_world = float4x4::identity();
  float4x4 world_to_controller = float4x4::identity();
  float3x3 normal_object_to_world = float3x3::identity();
  float3x3 normal_world_to_controller = float3x3::identity();
  if (has_controller) {
    object_to_world = object->object_to_world();
    world_to_controller = projection.controller->world_to_object();
    normal_object_to_world = math::transpose(math::invert(float3x3(object_to_world)));
    normal_world_to_controller = math::transpose(math::invert(float3x3(world_to_controller)));
  }

  const int tile_number = (projection.tile_number >= 1001) ? projection.tile_number : 1001;
  const int tile_x = (tile_number - 1001) % 10;
  const int tile_y = (tile_number - 1001) / 10;
  const float tile_origin_u = float(tile_x);
  const float tile_origin_v = float(tile_y);
  /* The frame rotation is constant across the bake; the normal reads it too, for the facing
   * weight's component along the axis. */
  float frame_rot[3][3];
  float inv_frame_rot[3][3];
  eul_to_mat3(frame_rot, projection.rotation);
  transpose_m3_m3(inv_frame_rot, frame_rot);

  const auto sample_uv = [&](float u, float v, float r_rgba[4]) -> bool {
    composite_projection_uv_wrap(mapping, u, v);
    /* A tiled source is sampled from one tile buffer; its pattern wraps inside that tile. */
    const bool repeat = projection.source_tiled ||
                        mapping.wrap != MA_PAINT_LAYER_MAPPING_WRAP_CLIP;
    return sampler.sample(u, v, repeat, repeat, r_rgba);
  };

  for (const int tri_index : IndexRange(tris.size())) {
    const int face = tri_faces[tri_index];
    const int material_index = material_indices[face];
    if (material_index < 0 || material_index >= object->totcol ||
        !slot_matches[material_index])
    {
      continue;
    }
    const int3 &tri = tris[tri_index];
    const int c0 = tri[0];
    const int c1 = tri[1];
    const int c2 = tri[2];
    const float2 uv0 = uvs[c0];
    const float2 uv1 = uvs[c1];
    const float2 uv2 = uvs[c2];
    /* One bake is one UDIM tile. A triangle crossing a tile border is left out; UV islands are
     * expected to stay inside their tile. */
    const auto in_tile = [&](const float2 &uv) {
      return int(floorf(uv.x)) == tile_x && int(floorf(uv.y)) == tile_y;
    };
    if (!in_tile(uv0) || !in_tile(uv1) || !in_tile(uv2)) {
      continue;
    }
    const float2 local_uv0 = uv0 - float2(tile_origin_u, tile_origin_v);
    const float2 local_uv1 = uv1 - float2(tile_origin_u, tile_origin_v);
    const float2 local_uv2 = uv2 - float2(tile_origin_u, tile_origin_v);
    const float min_u = std::min({local_uv0.x, local_uv1.x, local_uv2.x});
    const float max_u = std::max({local_uv0.x, local_uv1.x, local_uv2.x});
    const float min_v = std::min({local_uv0.y, local_uv1.y, local_uv2.y});
    const float max_v = std::max({local_uv0.y, local_uv1.y, local_uv2.y});
    if (max_u < 0.0f || max_v < 0.0f || min_u >= 1.0f || min_v >= 1.0f) {
      continue;
    }
    const int x0 = clamp_i(int(floorf(min_u * float(ref_width))), 0, ref_width - 1);
    const int x1 = clamp_i(int(ceilf(max_u * float(ref_width))), 0, ref_width - 1);
    const int y0 = clamp_i(int(floorf(min_v * float(ref_height))), 0, ref_height - 1);
    const int y1 = clamp_i(int(ceilf(max_v * float(ref_height))), 0, ref_height - 1);
    const float tri_uv[3][2] = {
        {local_uv0.x, local_uv0.y},
        {local_uv1.x, local_uv1.y},
        {local_uv2.x, local_uv2.y},
    };
    const float3 &p0 = positions[corner_verts[c0]];
    const float3 &p1 = positions[corner_verts[c1]];
    const float3 &p2 = positions[corner_verts[c2]];
    const float3 &n0 = normals[c0];
    const float3 &n1 = normals[c1];
    const float3 &n2 = normals[c2];

    for (int y = y0; y <= y1; y++) {
      for (int x = x0; x <= x1; x++) {
        const float u = (float(x) + 0.5f) / float(ref_width);
        const float v = (float(y) + 0.5f) / float(ref_height);
        const float co[2] = {u, v};
        float w01[2];
        barycentric_weights_v2(tri_uv[0], tri_uv[1], tri_uv[2], co, w01);
        const float w0 = w01[0];
        const float w1 = w01[1];
        const float w2 = 1.0f - w0 - w1;
        if (!std::isfinite(w0) || !std::isfinite(w1) || !std::isfinite(w2)) {
          continue;
        }
        if (w0 < -1e-5f || w1 < -1e-5f || w2 < -1e-5f) {
          continue;
        }
        float p[3];
        p[0] = float(p0.x) * w0 + float(p1.x) * w1 + float(p2.x) * w2;
        p[1] = float(p0.y) * w0 + float(p1.y) * w1 + float(p2.y) * w2;
        p[2] = float(p0.z) * w0 + float(p1.z) * w1 + float(p2.z) * w2;
        float n_interp[3];
        n_interp[0] = float(n0.x) * w0 + float(n1.x) * w1 + float(n2.x) * w2;
        n_interp[1] = float(n0.y) * w0 + float(n1.y) * w1 + float(n2.y) * w2;
        n_interp[2] = float(n0.z) * w0 + float(n1.z) * w1 + float(n2.z) * w2;

        if (has_controller) {
          const float3 p_controller = math::transform_point(
              world_to_controller,
              math::transform_point(object_to_world, float3(p[0], p[1], p[2])));
          p[0] = p_controller.x;
          p[1] = p_controller.y;
          p[2] = p_controller.z;
          const float3 n_controller = normal_world_to_controller *
                                      (normal_object_to_world *
                                       float3(n_interp[0], n_interp[1], n_interp[2]));
          n_interp[0] = n_controller.x;
          n_interp[1] = n_controller.y;
          n_interp[2] = n_controller.z;
        }

        float q[3];
        composite_projection_position(projection, inv_frame_rot, p, q);
        /* The image-aspect fit scales the frame-space coordinates before anything reads them,
         * exactly as the generated graph's Aspect input does; the depth axis keeps one. */
        for (int i = 0; i < 3; i++) {
          q[i] *= projection.aspect[i];
        }

        /* The frame factor the generated graph builds: the two footprint axes with their smooth
         * edges, the depth slab when its toggle asks, all under the frame's clip mix; the facing
         * fade joins after it and applies even while the map repeats. */
        const int axis = clamp_i(projection.axis,
                                 MA_PAINT_LAYER_PROJECTION_AXIS_X,
                                 MA_PAINT_LAYER_PROJECTION_AXIS_Z);
        const int u_index = (axis == MA_PAINT_LAYER_PROJECTION_AXIS_X) ? 1 : 0;
        const int v_index = (axis == MA_PAINT_LAYER_PROJECTION_AXIS_Z) ? 1 : 2;
        float weight = 1.0f;
        if (projection.clip) {
          weight = BKE_paint_layers_projection_axis_factor(q[u_index]) *
                   BKE_paint_layers_projection_axis_factor(q[v_index]);
          if (projection.depth) {
            weight *= BKE_paint_layers_projection_axis_factor(q[axis]);
          }
        }
        if (weight > 0.0f && projection.facing) {
          float n_frame[3];
          mul_v3_m3v3(n_frame, inv_frame_rot, n_interp);
          weight *= BKE_paint_layers_projection_facing_weight(
              projection.facing_scale, projection.facing_offset, n_frame[axis]);
        }
        if (weight <= 0.0f) {
          continue;
        }

        float straight[3] = {0.0f, 0.0f, 0.0f};
        float alpha = 0.0f;
        bool covered = false;

        const auto accumulate_sample = [&](const float uu,
                                           const float vv,
                                           const float weight) {
          float tap[4];
          if (!sample_uv(uu, vv, tap)) {
            return;
          }
          float sample_rgb[3];
          float sample_alpha;
          composite_straight_from_premul(tap, sample_rgb, sample_alpha);
          straight[0] += sample_rgb[0] * weight;
          straight[1] += sample_rgb[1] * weight;
          straight[2] += sample_rgb[2] * weight;
          alpha += sample_alpha * weight;
          covered = true;
        };

        /* Planar: X reads Y/Z, Y reads X/Z and Z reads X/Y. */
        accumulate_sample(0.5f + q[u_index] * 0.5f, 0.5f + q[v_index] * 0.5f, weight);

        if (!covered) {
          continue;
        }
        float *pixel = out + (int64_t(y) * ref_width + x) * 4;
        pixel[0] = straight[0] * alpha;
        pixel[1] = straight[1] * alpha;
        pixel[2] = straight[2] * alpha;
        pixel[3] = alpha;
      }
    }
  }
  return dst;
}

}  // namespace blender

