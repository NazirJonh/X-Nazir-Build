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
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>

#include "BKE_attribute.hh"
#include "BKE_customdata.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_gradient.hh"
#include "BKE_paint_layers_gradient_surface.hh"
#include "BKE_paint_material_composite.hh"
#include "BKE_paint_material_decal_map.hh"

#include "BLI_array.hh"
#include "BLI_hash.hh"
#include "BLI_index_range.hh"
#include "BLI_math_base.h"
#include "BLI_math_color.h"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_span.hh"
#include "BLI_task.hh"
#include "BLI_vector.hh"
#include "BLI_virtual_array.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_object_types.h"

#include "DEG_depsgraph_query.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "paint_material_composite_projection_internal.hh"

namespace blender {

CompositeProjectionSampler::CompositeProjectionSampler(const ImBuf *source,
                                                       const bool grey_single_channel)
    : src(source), grey_one_channel(grey_single_channel)
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

bool CompositeProjectionSampler::valid() const
{
  return src != nullptr && (src_is_float || src->byte_buffer.data != nullptr);
}

static const std::array<float, 256> &composite_sampler_srgb_table()
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

void CompositeProjectionSampler::tap_straight(const int tx, const int ty, float r_tap[4]) const
{
  if (!src_is_float) {
    const uchar *pixel = src->byte_data() + (int64_t(ty) * src->x + tx) * 4;
    if (decode_by_table) {
      const std::array<float, 256> &table = composite_sampler_srgb_table();
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
    if (grey_one_channel && src_channels == 1) {
      r_tap[1] = pixel[0];
      r_tap[2] = pixel[0];
    }
    else {
      r_tap[1] = src_channels > 1 ? pixel[1] : 0.0f;
      r_tap[2] = src_channels > 2 ? pixel[2] : 0.0f;
    }
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

bool CompositeProjectionSampler::sample(
    float u, float v, const bool repeat_u, const bool repeat_v, float r_rgba[4]) const
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

void composite_straight_from_premul(const float premul[4], float r_rgb[3], float &r_alpha)
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
  /* The allocation is already zeroed (#IMB_initImBuf uses `MEM_new_zeroed` unless
   * `UninitializedPixels` is set); a second pass over a 4K buffer is 256 MB of writes, and the
   * untouched texels of a projected bake never need their pages touched at all. */
  dst->channels = 4;
  IMB_colormanagement_assign_float_colorspace(
      dst, IMB_colormanagement_role_colorspace_name_get(COLOR_ROLE_SCENE_LINEAR));
  return dst;
}

/** `q = R^-1 (p - center) / (size * 0.5)`, the shader's frame-space position over the half
 * extent: size is the frame's full extent, the one the gizmo's cage and the panel show. */
static void composite_projection_position(const PaintMaterialCompositeProjection &projection,
                                          const float inv_rot[3][3],
                                          const float p[3],
                                          float r_q[3])
{
  /* Why the CPU repeats the graph here: the Vector Rotate node is Euler XYZ and inverted, and
   * `eul_to_mat3` builds the same XYZ matrix whose transpose is that inverse. The size is
   * normalized once for both sides (#BKE_paint_layers_projection_size_normalize); the graph
   * scales the Size input by one half before the same divide. */
  float offset[3];
  sub_v3_v3v3(offset, p, projection.center);
  mul_v3_m3v3(r_q, inv_rot, offset);
  for (int i = 0; i < 3; i++) {
    const float half_size = BKE_paint_layers_projection_size_normalize(projection.size[i]) * 0.5f;
    r_q[i] /= half_size;
  }
}

/** The row's 2D UV adjust, applied to a projected UV. The Cube atlas is read cell by cell, so it
 * never wraps: the is_cube flag keeps the point inside the atlas. */
static void composite_projection_uv_wrap(const PaintMaterialCompositeMapping &mapping,
                                         const bool is_cube,
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
  if (!is_cube && mapping.wrap != MA_PAINT_LAYER_MAPPING_WRAP_CLIP) {
    u -= floorf(u);
    v -= floorf(v);
  }
}

/** The face UV of Box and Triplanar, as the graph's `projection_face_uv`: face 0 is X, 1 is Y, 2
 * is Z. */
static void composite_projection_face_uv(const int face, const float q[3], float r_uv[2])
{
  if (face == 0) {
    /* +X: u from -Y, v from +Z. */
    r_uv[0] = 0.5f - q[1] * 0.5f;
    r_uv[1] = 0.5f + q[2] * 0.5f;
  }
  else if (face == 1) {
    r_uv[0] = 0.5f + q[0] * 0.5f;
    r_uv[1] = 0.5f + q[2] * 0.5f;
  }
  else {
    r_uv[0] = 0.5f + q[0] * 0.5f;
    r_uv[1] = 0.5f + q[1] * 0.5f;
  }
}

const Mesh *composite_projection_mesh(const Object *object)
{
  if (object == nullptr || object->type != OB_MESH) {
    return nullptr;
  }
  if (DEG_is_evaluated(object)) {
    /* The evaluated object's own data is not the final mesh when modifiers ran. The checked getter
     * answers null while the object's geometry is not evaluated (right after a mode switch or an
     * edit), when the unchecked one would hand back a mesh that is being replaced. The evaluated
     * object's #Object.data is an evaluated copy as well, so it is no fallback: the original has
     * to be reached through the ID. */
    if (const Mesh *mesh_eval = BKE_object_get_evaluated_mesh(object)) {
      return mesh_eval;
    }
    const Object *object_orig = DEG_get_original(object);
    return (object_orig != nullptr && object_orig->data != nullptr) ?
               id_cast<const Mesh *>(object_orig->data) :
               nullptr;
  }
  return (object->data != nullptr) ? id_cast<const Mesh *>(object->data) : nullptr;
}

/**
 * Mark the slots of \a object that hold \a material (every slot when it is null) and return how
 * many. Compared by original ID: an evaluated object's slots point at evaluated copies of its
 * materials, while the row's material is the original.
 */
static int composite_projection_slots_match(const Object &object,
                                            const Material *material,
                                            MutableSpan<bool> r_matches)
{
  const Material *material_orig = (material != nullptr) ? DEG_get_original(material) : nullptr;
  int matched = 0;
  for (int slot = 0; slot < object.totcol; slot++) {
    const Material *slot_material = BKE_object_material_get(const_cast<Object *>(&object),
                                                            short(slot + 1));
    if (material_orig == nullptr ||
        (slot_material != nullptr && DEG_get_original(slot_material) == material_orig))
    {
      r_matches[slot] = true;
      matched++;
    }
  }
  return matched;
}

/** The bands a projected bake is rasterized in, one task each. Each band owns whole canvas rows, so
 * two tasks never write the same texel and no atomic is needed; the count is bounded because every
 * band walks the triangle list once to find the ones that reach its rows. */
static constexpr int composite_projection_band_max = 32;

/**
 * Grow the colours of a projected bake past the UV islands, so a texture read with bilinear
 * filtering or mipmaps finds the island's own edge beside a seam instead of an empty texel. The
 * growth rule is the surface's (#BKE_paint_composite_pad_islands): only texels no triangle covers
 * borrow from their neighbour, so a texel the frame clipped away inside an island stays empty.
 *
 * \param island: one byte per texel, set where a triangle covered the texel center.
 * \param band_boxes: per band `(min_x, min_y, max_x, max_y)` of its covered texels, max below min
 *                    when it covered none.
 */
static void composite_projection_pad_colors(float *out,
                                            MutableSpan<uint8_t> island,
                                            const int width,
                                            const int height,
                                            const Span<int4> band_boxes,
                                            float *out_extra = nullptr)
{
  int2 box_min(width, height);
  int2 box_max(-1, -1);
  for (const int4 &box : band_boxes) {
    if (box.z < box.x) {
      continue;
    }
    box_min = math::min(box_min, int2(box.x, box.y));
    box_max = math::max(box_max, int2(box.z, box.w));
  }
  if (box_max.x < box_min.x) {
    return;
  }
  BKE_paint_composite_pad_islands(
      island,
      width,
      height,
      BKE_paint_composite_island_padding(std::max(width, height)),
      box_min,
      box_max,
      [&](const int64_t dst, const int64_t src) {
        copy_v4_v4(out + dst * 4, out + src * 4);
        if (out_extra != nullptr) {
          copy_v4_v4(out_extra + dst * 4, out_extra + src * 4);
        }
      });
}

/* The first UDIM tile number and the tiles per row of the UDIM numbering. */
static constexpr int composite_projection_udim_first = 1001;
static constexpr int composite_projection_udim_row_len = 10;
/* The slack of the closed tile-border test, and of the inside test of a barycentric weight. */
static constexpr float composite_projection_border_epsilon = 1e-5f;

/** The flags and constants of a projection's space, built once for the whole bake. */
struct CompositeProjectionKind {
  bool volume = false;
  bool box = false;
  bool cube = false;
  bool cylinder = false;
  bool sphere = false;
  float exponent = 1.0f;
  /* The Cube atlas table row, the one the generated graph reads. */
  const ProjectionCubeLayoutInfo *atlas = nullptr;
  /* The projection axis and the two frame axes a Planar frame reads its UV from. */
  int axis = 0;
  int u_index = 0;
  int v_index = 0;
};

static CompositeProjectionKind composite_projection_kind_make(
    const PaintMaterialCompositeProjection &projection)
{
  CompositeProjectionKind kind;
  const ProjectionModeInfo &mode = BKE_paint_layers_projection_mode_info(projection.space);
  kind.volume = mode.is_volume;
  kind.box = mode.samples_num > 1;
  kind.cube = projection.space == MA_PAINT_LAYER_MAPPING_SPACE_CUBE;
  kind.cylinder = projection.space == MA_PAINT_LAYER_MAPPING_SPACE_CYLINDER;
  kind.sphere = projection.space == MA_PAINT_LAYER_MAPPING_SPACE_SPHERE;
  kind.exponent = mode.uses_blend ?
                      BKE_paint_layers_projection_triplanar_exponent(projection.blend) :
                      PAINT_LAYER_PROJECTION_BOX_EXPONENT;
  kind.atlas = &BKE_paint_layers_projection_cube_layout_info(projection.cube_layout);
  kind.axis = clamp_i(
      projection.axis, MA_PAINT_LAYER_PROJECTION_AXIS_X, MA_PAINT_LAYER_PROJECTION_AXIS_Z);
  kind.u_index = (kind.axis == MA_PAINT_LAYER_PROJECTION_AXIS_X) ? 1 : 0;
  kind.v_index = (kind.axis == MA_PAINT_LAYER_PROJECTION_AXIS_Z) ? 1 : 2;
  return kind;
}

/** The mesh, UV, material and frame state both projected rasterizers share. */
struct CompositeProjectionBakeContext {
  const PaintMaterialCompositeProjection &projection;
  const Object *object = nullptr;
  CompositeProjectionKind kind;
  VArraySpan<float2> uvs;
  Span<float3> positions;
  Span<float3> normals;
  Span<int> corner_verts;
  Span<int3> tris;
  Array<bool> slot_matches;
  VArray<int> material_indices;
  Array<int> tri_faces;
  bool has_controller = false;
  float4x4 object_to_world = float4x4::identity();
  float4x4 world_to_controller = float4x4::identity();
  float3x3 normal_object_to_world = float3x3::identity();
  float3x3 normal_world_to_controller = float3x3::identity();
  int tile_x = 0;
  int tile_y = 0;
  int width = 0;
  int height = 0;
  float inv_frame_rot[3][3];

  explicit CompositeProjectionBakeContext(const PaintMaterialCompositeProjection &projection_)
      : projection(projection_)
  {
  }
};

/**
 * Fill \a ctx from the projection's object. False when the row names no mesh, render UV map,
 * triangle or matching material slot. \a need_normals reads the corner normals, which only the
 * rasterizer that weights by the surface normal needs.
 */
static bool composite_projection_context_init(CompositeProjectionBakeContext &ctx,
                                              const bool need_normals,
                                              const int width,
                                              const int height,
                                              const int tile_number)
{
  const PaintMaterialCompositeProjection &projection = ctx.projection;
  const Object *object = projection.object;
  const Mesh *mesh_ptr = composite_projection_mesh(object);
  if (mesh_ptr == nullptr) {
    return false;
  }
  const Mesh &mesh = *mesh_ptr;
  const StringRefNull uv_name_ref = BKE_paint_layers_gradient_uv_map_name(mesh);
  const char *uv_name = uv_name_ref.is_empty() ? nullptr : uv_name_ref.c_str();
  if (uv_name == nullptr) {
    return false;
  }
  const auto &attributes = mesh.attributes();
  const auto uv_attr = attributes.lookup<float2>(uv_name, bke::AttrDomain::Corner);
  if (!uv_attr) {
    return false;
  }
  ctx.object = object;
  ctx.uvs = VArraySpan<float2>(*uv_attr);
  ctx.positions = mesh.vert_positions();
  if (need_normals) {
    ctx.normals = mesh.corner_normals();
  }
  ctx.corner_verts = mesh.corner_verts();
  ctx.tris = mesh.corner_tris();
  if (ctx.tris.is_empty() || object->totcol <= 0) {
    return false;
  }
  ctx.slot_matches = Array<bool>(object->totcol, false);
  if (composite_projection_slots_match(*object, projection.material, ctx.slot_matches) == 0) {
    return false;
  }
  ctx.material_indices = *attributes.lookup_or_default<int>(
      "material_index", bke::AttrDomain::Face, 0);
  ctx.tri_faces = Array<int>(int(ctx.tris.size()));
  bke::mesh::corner_tris_calc_face_indices(mesh.faces(), ctx.tri_faces);

  /* The painted mesh is in its own object space; a controller Empty moves the frame into its own
   * space first, exactly as a Texture Coordinate node naming that object does in the shader. */
  ctx.has_controller = projection.controller != nullptr && projection.controller != object;
  if (ctx.has_controller) {
    ctx.object_to_world = object->object_to_world();
    ctx.world_to_controller = projection.controller->world_to_object();
    if (need_normals) {
      ctx.normal_object_to_world = math::transpose(math::invert(float3x3(ctx.object_to_world)));
      ctx.normal_world_to_controller = math::transpose(
          math::invert(float3x3(ctx.world_to_controller)));
    }
  }

  const int bake_tile = (tile_number >= composite_projection_udim_first) ?
                            tile_number :
                            composite_projection_udim_first;
  ctx.tile_x = (bake_tile - composite_projection_udim_first) % composite_projection_udim_row_len;
  ctx.tile_y = (bake_tile - composite_projection_udim_first) / composite_projection_udim_row_len;
  ctx.width = width;
  ctx.height = height;

  /* The frame rotation is constant across the bake; the normal reads it too, for the Box and
   * Triplanar face weights and the Planar facing's component along the axis. */
  float frame_rot[3][3];
  eul_to_mat3(frame_rot, projection.rotation);
  transpose_m3_m3(ctx.inv_frame_rot, frame_rot);
  ctx.kind = composite_projection_kind_make(projection);
  return true;
}

/** The corners of the triangle a texel is rasterized from, in its UV tile and in object space. */
struct CompositeProjectionTri {
  float tri_uv[3][2];
  float3 p0, p1, p2;
  float3 n0, n1, n2;
  /** The triangle's index in the mesh's corner triangles. */
  int index = 0;
};

/**
 * Rasterize the matching triangles into the UV tile of \a ctx and call \a texel_fn on each texel
 * a triangle covers: `(x, y, tri, p, n)`, the position and normal already in the
 * controller's space (the normal is zero when the context read none). Then pad the colors of
 * \a out past the islands. Bands own whole rows, so the callback may write its own texel.
 * \a out_extra, when given, is a second RGBA buffer the callback fills; it is padded identically,
 * so two maps of one surface never disagree about which island a padded texel borrows from.
 */
template<typename TexelFn>
static void composite_projection_raster(const CompositeProjectionBakeContext &ctx,
                                        float *out,
                                        TexelFn &&texel_fn,
                                        float *out_extra = nullptr)
{
  const int ref_width = ctx.width;
  const int ref_height = ctx.height;
  const bool has_normals = !ctx.normals.is_empty();
  /* The texels a triangle covered, for the padding below; per-band boxes of them keep that pass off
   * the empty parts of the canvas. */
  Array<uint8_t> island(int64_t(ref_width) * ref_height, uint8_t(0));
  const int band_num = std::min(composite_projection_band_max, ref_height);
  const int band_rows = (ref_height + band_num - 1) / band_num;
  Array<int4> band_boxes(band_num, int4(ref_width, ref_height, -1, -1));

  const float tile_origin_u = float(ctx.tile_x);
  const float tile_origin_v = float(ctx.tile_y);
  /* One bake is one UDIM tile. A triangle crossing a tile border is left out; UV islands are
   * expected to stay inside their tile. The far border belongs to the tile (an island touching
   * `u = 1` or `v = 1`, as a cube cross does, ends exactly there), so the test is a closed
   * range, not the vertex's floor. */
  const auto in_tile = [&](const float2 &uv) {
    return uv.x >= float(ctx.tile_x) - composite_projection_border_epsilon &&
           uv.x <= float(ctx.tile_x + 1) + composite_projection_border_epsilon &&
           uv.y >= float(ctx.tile_y) - composite_projection_border_epsilon &&
           uv.y <= float(ctx.tile_y + 1) + composite_projection_border_epsilon;
  };

  threading::parallel_for(IndexRange(band_num), 1, [&](const IndexRange bands) {
    for (const int band : bands) {
      const int band_y0 = band * band_rows;
      const int band_y1 = std::min(band_y0 + band_rows, ref_height) - 1;
      int4 &band_box = band_boxes[band];
      for (const int tri_index : IndexRange(ctx.tris.size())) {
        const int face = ctx.tri_faces[tri_index];
        const int material_index = ctx.material_indices[face];
        if (material_index < 0 || material_index >= ctx.object->totcol ||
            !ctx.slot_matches[material_index])
        {
          continue;
        }
        const int3 &tri_corners = ctx.tris[tri_index];
        const int c0 = tri_corners[0];
        const int c1 = tri_corners[1];
        const int c2 = tri_corners[2];
        const float2 uv0 = ctx.uvs[c0];
        const float2 uv1 = ctx.uvs[c1];
        const float2 uv2 = ctx.uvs[c2];
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
        const int y0 = std::max(clamp_i(int(floorf(min_v * float(ref_height))), 0, ref_height - 1),
                                band_y0);
        const int y1 = std::min(clamp_i(int(ceilf(max_v * float(ref_height))), 0, ref_height - 1),
                                band_y1);
        if (y0 > y1) {
          continue;
        }
        CompositeProjectionTri tri;
        tri.index = tri_index;
        tri.tri_uv[0][0] = local_uv0.x;
        tri.tri_uv[0][1] = local_uv0.y;
        tri.tri_uv[1][0] = local_uv1.x;
        tri.tri_uv[1][1] = local_uv1.y;
        tri.tri_uv[2][0] = local_uv2.x;
        tri.tri_uv[2][1] = local_uv2.y;
        tri.p0 = ctx.positions[ctx.corner_verts[c0]];
        tri.p1 = ctx.positions[ctx.corner_verts[c1]];
        tri.p2 = ctx.positions[ctx.corner_verts[c2]];
        if (has_normals) {
          tri.n0 = ctx.normals[c0];
          tri.n1 = ctx.normals[c1];
          tri.n2 = ctx.normals[c2];
        }

        for (int y = y0; y <= y1; y++) {
          for (int x = x0; x <= x1; x++) {
            const float u = (float(x) + 0.5f) / float(ref_width);
            const float v = (float(y) + 0.5f) / float(ref_height);
            const float co[2] = {u, v};
            float w01[3];
            barycentric_weights_v2(tri.tri_uv[0], tri.tri_uv[1], tri.tri_uv[2], co, w01);
            const float w0 = w01[0];
            const float w1 = w01[1];
            const float w2 = 1.0f - w0 - w1;
            if (!std::isfinite(w0) || !std::isfinite(w1) || !std::isfinite(w2)) {
              continue;
            }
            if (w0 < -composite_projection_border_epsilon ||
                w1 < -composite_projection_border_epsilon ||
                w2 < -composite_projection_border_epsilon)
            {
              continue;
            }
            island[int64_t(y) * ref_width + x] = 1;
            band_box = int4(std::min(band_box.x, x),
                            std::min(band_box.y, y),
                            std::max(band_box.z, x),
                            std::max(band_box.w, y));
            float p[3];
            p[0] = float(tri.p0.x) * w0 + float(tri.p1.x) * w1 + float(tri.p2.x) * w2;
            p[1] = float(tri.p0.y) * w0 + float(tri.p1.y) * w1 + float(tri.p2.y) * w2;
            p[2] = float(tri.p0.z) * w0 + float(tri.p1.z) * w1 + float(tri.p2.z) * w2;
            float n_interp[3] = {0.0f, 0.0f, 0.0f};
            if (has_normals) {
              n_interp[0] = float(tri.n0.x) * w0 + float(tri.n1.x) * w1 + float(tri.n2.x) * w2;
              n_interp[1] = float(tri.n0.y) * w0 + float(tri.n1.y) * w1 + float(tri.n2.y) * w2;
              n_interp[2] = float(tri.n0.z) * w0 + float(tri.n1.z) * w1 + float(tri.n2.z) * w2;
            }

            if (ctx.has_controller) {
              const float3 p_controller = math::transform_point(
                  ctx.world_to_controller,
                  math::transform_point(ctx.object_to_world, float3(p[0], p[1], p[2])));
              p[0] = p_controller.x;
              p[1] = p_controller.y;
              p[2] = p_controller.z;
              if (has_normals) {
                const float3 n_controller = ctx.normal_world_to_controller *
                                            (ctx.normal_object_to_world *
                                             float3(n_interp[0], n_interp[1], n_interp[2]));
                n_interp[0] = n_controller.x;
                n_interp[1] = n_controller.y;
                n_interp[2] = n_controller.z;
              }
            }
            texel_fn(x, y, tri, p, n_interp);
          }
        }
      }
    }
  });
  composite_projection_pad_colors(out, island, ref_width, ref_height, band_boxes, out_extra);
}

/** The cube-cross atlas UV of the dominant frame axis: a cell is a quarter by a third of it,
 * placed by the cross origins the graph's CUBE case uses. */
static void composite_projection_cube_uv(const CompositeProjectionKind &kind,
                                         const float q[3],
                                         float r_uv[2],
                                         int &r_face)
{
  const float ax = fabsf(q[0]);
  const float ay = fabsf(q[1]);
  const float az = fabsf(q[2]);
  int atlas_face = 0;
  if (ax + PAINT_LAYER_PROJECTION_EPSILON > ay && ax + PAINT_LAYER_PROJECTION_EPSILON > az) {
    atlas_face = (q[0] > 0.0f) ? 0 : 1;
  }
  else if (ay + PAINT_LAYER_PROJECTION_EPSILON > az) {
    atlas_face = (q[1] > 0.0f) ? 2 : 3;
  }
  else {
    atlas_face = (q[2] > 0.0f) ? 4 : 5;
  }
  float local_uv[2];
  composite_projection_face_uv(atlas_face / 2, q, local_uv);
  const ProjectionCubeLayoutInfo &atlas = *kind.atlas;
  r_uv[0] = atlas.origin[atlas_face][0] + local_uv[0] * atlas.cell_size[0];
  r_uv[1] = atlas.origin[atlas_face][1] + local_uv[1] * atlas.cell_size[1];
  r_face = atlas_face;
}

/** Cylinder: U winds around the axis, V reads the height along it. */
static void composite_projection_cylinder_uv(const CompositeProjectionKind &kind,
                                             const float q[3],
                                             float r_uv[2])
{
  const int a = (kind.axis + 1) % 3;
  const int b = (kind.axis + 2) % 3;
  const float angle = atan2f(q[b], q[a]);
  r_uv[0] = BKE_paint_layers_projection_angle_u(angle);
  r_uv[1] = 0.5f + q[kind.axis] * 0.5f;
}

/** Sphere: U winds around the axis, V is the polar angle from it. */
static void composite_projection_sphere_uv(const CompositeProjectionKind &kind,
                                           const float q[3],
                                           float r_uv[2])
{
  const int a = (kind.axis + 1) % 3;
  const int b = (kind.axis + 2) % 3;
  const float length = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
  const float ratio = clamp_f(
      q[kind.axis] / std::max(length, PAINT_LAYER_PROJECTION_EPSILON), -1.0f, 1.0f);
  const float angle = atan2f(q[b], q[a]);
  r_uv[0] = BKE_paint_layers_projection_angle_u(angle);
  r_uv[1] = BKE_paint_layers_projection_polar_v(acosf(ratio));
}

/**
 * The frame factor the generated graph builds: the two footprint axes with their smooth edges, the
 * depth slab when its toggle asks, all under the frame's clip mix; the facing fade joins after it
 * and applies even while the map repeats. The facing is a Planar input: it fades the faces that
 * look away from the one projection direction, which the volume kinds do not have -- the map
 * reaches a surface from every side -- so they never read it. A single-axis facing would blank the
 * faces the axis runs across, exactly the sides the map arrives from.
 */
static float composite_projection_frame_weight(const PaintMaterialCompositeProjection &projection,
                                               const CompositeProjectionKind &kind,
                                               const float q[3],
                                               const float n_frame[3])
{
  const int axis = kind.axis;
  float weight = 1.0f;
  if (projection.clip) {
    if (kind.box || kind.cube) {
      /* Box, Triplanar and Cube share the box inside: the three axes with their smooth
       * edges. The depth toggle is a Planar input: the box inside already bounds all
       * three axes, and the graph builds no depth slab for the volume kinds either. */
      weight = BKE_paint_layers_projection_axis_factor(q[0]) *
               BKE_paint_layers_projection_axis_factor(q[1]) *
               BKE_paint_layers_projection_axis_factor(q[2]);
    }
    else if (kind.cylinder) {
      /* Cylinder: inside the unit circle around the axis, and within the height slab. */
      const int a = (axis + 1) % 3;
      const int b = (axis + 2) % 3;
      const float radial = q[a] * q[a] + q[b] * q[b];
      weight = (radial < 1.0f) ? 1.0f : 0.0f;
      weight *= BKE_paint_layers_projection_axis_factor(q[axis]);
    }
    else if (kind.sphere) {
      /* Sphere: inside the unit ball. */
      const float length_sq = q[0] * q[0] + q[1] * q[1] + q[2] * q[2];
      weight = (length_sq < 1.0f) ? 1.0f : 0.0f;
    }
    else {
      weight = BKE_paint_layers_projection_axis_factor(q[kind.u_index]) *
               BKE_paint_layers_projection_axis_factor(q[kind.v_index]);
      if (projection.depth) {
        weight *= BKE_paint_layers_projection_axis_factor(q[axis]);
      }
    }
  }
  if (weight > 0.0f && projection.facing && !kind.volume) {
    const float facing_component = n_frame[axis];
    weight *= BKE_paint_layers_projection_facing_weight(
        projection.facing_scale, projection.facing_offset, facing_component);
  }
  return weight;
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
                                    const PaintMaterialCompositeMapping &mapping,
                                    const Span<PaintMaterialCompositeMapping> input_steps)
{
  const PaintMaterialCompositeProjection &mapping_projection = mapping.projection;
  /* A Decal lives in the painted object's space and its unfolded plane has no depth or facing, so
   * the controller, the depth slab and the facing fade are left out of its frame. */
  PaintMaterialCompositeProjection decal_projection;
  const bool is_decal = mapping_projection.space == MA_PAINT_LAYER_MAPPING_SPACE_DECAL;
  if (is_decal) {
    decal_projection = mapping_projection;
    decal_projection.controller = nullptr;
    decal_projection.depth = false;
    decal_projection.facing = false;
  }
  const PaintMaterialCompositeProjection &projection = is_decal ? decal_projection :
                                                                  mapping_projection;
  CompositeProjectionBakeContext ctx(projection);
  if (!composite_projection_context_init(
          ctx, true, ref_width, ref_height, projection.tile_number))
  {
    return nullptr;
  }
  std::shared_ptr<const PaintMaterialDecalMap> decal_map;
  if (is_decal) {
    decal_map = BKE_paint_material_decal_map_get(projection, ref_width, ref_height);
    if (decal_map == nullptr) {
      return nullptr;
    }
  }
  /* The transform effects' steps only; the row's own mapping stays the UV adjust of
   * #composite_projection_uv_wrap, which runs after them. */
  CompositeMappingChain steps_chain;
  for (const PaintMaterialCompositeMapping &step : input_steps) {
    if (steps_chain.steps_num == CompositeMappingChain::MAX_STEPS) {
      break;
    }
    steps_chain.steps[steps_chain.steps_num++] = CompositeMappingTransform(step);
  }
  const bool use_steps = !steps_chain.is_identity();
  CompositeProjectionSampler sampler(src);
  if (!sampler.valid()) {
    return nullptr;
  }
  ImBuf *dst = composite_alloc_transparent(ref_width, ref_height);
  if (dst == nullptr) {
    return nullptr;
  }
  float *out = dst->float_data_for_write();
  const CompositeProjectionKind &kind = ctx.kind;

  const auto sample_uv = [&](float u, float v, float r_rgba[4]) -> bool {
    composite_projection_uv_wrap(mapping, kind.cube, u, v);
    /* A tiled source is sampled from one tile buffer; its pattern wraps inside that tile. The Cube
     * atlas never wraps: its cells live at fixed sub-rectangles of [0, 1]. */
    const bool repeat = projection.source_tiled ||
                        (!kind.cube && mapping.wrap != MA_PAINT_LAYER_MAPPING_WRAP_CLIP);
    return sampler.sample(u, v, repeat, repeat, r_rgba);
  };

  const auto texel = [&](const int x,
                         const int y,
                         const CompositeProjectionTri &tri,
                         const float p[3],
                         const float n_interp[3]) {
    float q[3];
    const PaintMaterialDecalFrame *decal_frame = nullptr;
    if (decal_map != nullptr) {
      /* The map coordinate comes from the unfolded patch, not from `p - center`; the rest is
       * Planar's. The depth axis is zero: the texel is on the surface. */
      const int64_t decal_index = int64_t(y) * ref_width + x;
      const int decal_tri = decal_map->patch_tri[decal_index];
      if (decal_tri < 0) {
        return;
      }
      decal_frame = &decal_map->frames[decal_tri];
      q[kind.axis] = 0.0f;
      q[kind.u_index] = decal_map->q[decal_index].x;
      q[kind.v_index] = decal_map->q[decal_index].y;
    }
    else {
      composite_projection_position(projection, ctx.inv_frame_rot, p, q);
    }
    /* The image-aspect fit scales the frame-space coordinates before anything reads them,
     * exactly as the generated graph's Aspect input does; the depth axis keeps one. */
    for (int i = 0; i < 3; i++) {
      q[i] *= projection.aspect[i];
    }
    float n_frame[3];
    mul_v3_m3v3(n_frame, ctx.inv_frame_rot, n_interp);
    float weight = 1.0f;
    weight = composite_projection_frame_weight(projection, kind, q, n_frame);
    if (weight <= 0.0f) {
      return;
    }

    float straight[3] = {0.0f, 0.0f, 0.0f};
    float alpha = 0.0f;
    bool covered = false;

    const auto accumulate_sample =
        [&](float uu, float vv, const float weight, const int cube_face = -1) {
      /* The steps move the map coordinate, before the row's UV adjust. A step that clips leaves the
       * texel transparent outside the unit square of that coordinate (not of the canvas or the
       * island). A Cube tap is an atlas coordinate: the steps run in the local [0, 1] space of its
       * own cell, so a fold, a wrap or a clip never reaches over a neighbouring cell, and the result
       * returns to the atlas before the row's UV adjust. */
      float step_sides[CompositeMappingChain::MAX_STEPS][2];
      if (use_steps) {
        if (cube_face >= 0) {
          const ProjectionCubeLayoutInfo &atlas = *kind.atlas;
          float local_u = (uu - atlas.origin[cube_face][0]) / atlas.cell_size[0];
          float local_v = (vv - atlas.origin[cube_face][1]) / atlas.cell_size[1];
          if (!steps_chain.apply_unwrapped(local_u, local_v, local_u, local_v, step_sides)) {
            return;
          }
          uu = atlas.origin[cube_face][0] + local_u * atlas.cell_size[0];
          vv = atlas.origin[cube_face][1] + local_v * atlas.cell_size[1];
        }
        else if (!steps_chain.apply_unwrapped(uu, vv, uu, vv, step_sides)) {
          return;
        }
      }
      float tap[4];
      if (!sample_uv(uu, vv, tap)) {
        return;
      }
      float sample_rgb[3];
      float sample_alpha;
      composite_straight_from_premul(tap, sample_rgb, sample_alpha);
      if (use_steps && mapping.tangent_normal && sample_alpha > 0.0f) {
        /* The covariant counterpart of the steps, per tap: Box blends taps whose fold sides
         * differ. It runs before the frame-to-tangent conversion below, the order the UV path
         * composes them in. */
        float normal_xy[2] = {sample_rgb[0] * 2.0f - 1.0f, sample_rgb[1] * 2.0f - 1.0f};
        steps_chain.remap_tangent_normal(step_sides, normal_xy);
        sample_rgb[0] = normal_xy[0] * 0.5f + 0.5f;
        sample_rgb[1] = normal_xy[1] * 0.5f + 0.5f;
      }
      straight[0] += sample_rgb[0] * weight;
      straight[1] += sample_rgb[1] * weight;
      straight[2] += sample_rgb[2] * weight;
      alpha += sample_alpha * weight;
      covered = true;
    };

    /* The tiling multiplies the UV only, never the frame factor read above, like the graph's. */
    if (kind.box) {
      /* Box and Triplanar: the three face reads blended by the frame normal's powers, the
       * graph's normalized weights; the color mixes by them and the alpha sums. */
      const float sum = std::max(powf(fabsf(n_frame[0]), kind.exponent) +
                                     powf(fabsf(n_frame[1]), kind.exponent) +
                                     powf(fabsf(n_frame[2]), kind.exponent),
                                 PAINT_LAYER_PROJECTION_EPSILON);
      for (int tri_face = 0; tri_face < 3; tri_face++) {
        float uv[2];
        composite_projection_face_uv(tri_face, q, uv);
        const float face_weight = powf(fabsf(n_frame[tri_face]), kind.exponent) / sum;
        accumulate_sample(0.5f + (uv[0] - 0.5f) * projection.tiling[0],
                          0.5f + (uv[1] - 0.5f) * projection.tiling[1],
                          weight * face_weight);
      }
    }
    else if (kind.cube || kind.cylinder || kind.sphere) {
      float uv[2];
      int cube_face = -1;
      if (kind.cube) {
        composite_projection_cube_uv(kind, q, uv, cube_face);
      }
      else if (kind.cylinder) {
        composite_projection_cylinder_uv(kind, q, uv);
      }
      else {
        composite_projection_sphere_uv(kind, q, uv);
      }
      accumulate_sample(uv[0], uv[1], weight, cube_face);
    }
    else {
      /* Planar: X reads Y/Z, Y reads X/Z and Z reads X/Y. */
      accumulate_sample(0.5f + q[kind.u_index] * projection.tiling[0] * 0.5f,
                        0.5f + q[kind.v_index] * projection.tiling[1] * 0.5f,
                        weight);
    }

    if (!covered) {
      return;
    }
    /* A tangent-space Normal map sampled through the frame: the stored vectors read in the
     * frame's own basis and must be re-expressed in the destination triangle's tangent basis, or
     * the relief would turn with the frame instead of staying glued to the surface (the same
     * conversion the generated graph builds, through the shared
     * #BKE_paint_layers_projection_normal_frame_to_tangent). */
    if (mapping.tangent_normal) {
      float frame_rot[3][3];
      if (decal_frame != nullptr) {
        /* The frame turns with the unfold: the axes this triangle was laid on. */
        memcpy(frame_rot, decal_frame->axes, sizeof(frame_rot));
      }
      else {
        transpose_m3_m3(frame_rot, ctx.inv_frame_rot);
      }
      /* The destination triangle's tangent basis from its UV gradient (object space; the true
       * bitangent, so a mirrored island keeps its relief's direction). */
      const float2 duv1 = float2(tri.tri_uv[1][0] - tri.tri_uv[0][0],
                                 tri.tri_uv[1][1] - tri.tri_uv[0][1]);
      const float2 duv2 = float2(tri.tri_uv[2][0] - tri.tri_uv[0][0],
                                 tri.tri_uv[2][1] - tri.tri_uv[0][1]);
      const float det = duv1.x * duv2.y - duv1.y * duv2.x;
      if (std::abs(det) > 1e-12f) {
        const float r = 1.0f / det;
        const float3 e1 = tri.p1 - tri.p0;
        const float3 e2 = tri.p2 - tri.p0;
        float3 tangent = (e1 * duv2.y - e2 * duv1.y) * r;
        float3 bitangent = (e2 * duv1.x - e1 * duv2.x) * r;
        if (ctx.has_controller) {
          /* The frame axes live in the controller's space (that is what the rotation carries);
           * the triangle basis is read there too, so the two never mix spaces. */
          const float3x3 object_to_controller = ctx.normal_world_to_controller *
                                                ctx.normal_object_to_world;
          tangent = object_to_controller * tangent;
          bitangent = object_to_controller * bitangent;
        }
        /* The UV gradient carries the island's scale: unit directions, or the tangent components
         * would outweigh the unit surface normal in the encoding below. */
        tangent = math::normalize(tangent);
        bitangent = math::normalize(bitangent);
        const float3 surface_normal = math::normalize(
            float3(n_interp[0], n_interp[1], n_interp[2]));
        /* The accumulated straight colour is the premultiplied mean of the encoded taps: un-
         * multiply to the decoded vector, convert, and put it back premultiplied. */
        const float inv_alpha = 1.0f / alpha;
        const float n_frame[3] = {straight[0] * inv_alpha * 2.0f - 1.0f,
                                  straight[1] * inv_alpha * 2.0f - 1.0f,
                                  straight[2] * inv_alpha * 2.0f - 1.0f};
        float n_tangent[3];
        BKE_paint_layers_projection_normal_frame_to_tangent(
            frame_rot, tangent, bitangent, surface_normal, n_frame, n_tangent);
        straight[0] = (n_tangent[0] * 0.5f + 0.5f) * alpha;
        straight[1] = (n_tangent[1] * 0.5f + 0.5f) * alpha;
        straight[2] = (n_tangent[2] * 0.5f + 0.5f) * alpha;
      }
    }
    float *pixel = out + (int64_t(y) * ref_width + x) * 4;
    pixel[0] = straight[0] * alpha;
    pixel[1] = straight[1] * alpha;
    pixel[2] = straight[2] * alpha;
    pixel[3] = alpha;
  };

  composite_projection_raster(ctx, out, texel);
  return dst;
}

/**
 * Render a projected UV gradient into the painted object's UV space, as a float RGBA premultiplied
 * scene-linear buffer. The coordinate is the single projected UV the generated graph feeds the
 * gradient's line (#ProjectionNodes::uv[0]: the +X face for Box and Triplanar, the atlas cell for
 * Cube), read raw: no frame factor and no tiling ride on it,
 * exactly as in the graph, where the coverage stays the ramp's own alpha. Null when there is no
 * object, mesh or render UV map; the caller keeps the plain UV read then.
 */
ImBuf *BKE_paint_material_gradient_projected_render(
    const PaintMaterialCompositeGradient &gradient,
    const int ref_width,
    const int ref_height,
    const int tile_number)
{
  BLI_assert(gradient.projected);
  const PaintMaterialCompositeProjection &projection = gradient.projection;
  CompositeProjectionBakeContext ctx(projection);
  if (!composite_projection_context_init(ctx, false, ref_width, ref_height, tile_number)) {
    return nullptr;
  }
  ImBuf *dst = composite_alloc_transparent(ref_width, ref_height);
  if (dst == nullptr) {
    return nullptr;
  }
  float *out = dst->float_data_for_write();
  const CompositeProjectionKind &kind = ctx.kind;

  const auto texel = [&](const int x,
                         const int y,
                         const CompositeProjectionTri & /*tri*/,
                         const float p[3],
                         const float * /*n*/) {
    float q[3];
    composite_projection_position(projection, ctx.inv_frame_rot, p, q);
    /* The image-aspect fit scales the frame-space coordinates before the mode reads them,
     * exactly as the generated graph's Aspect input does. */
    for (int i = 0; i < 3; i++) {
      q[i] *= projection.aspect[i];
    }

    /* The single projected UV the graph feeds the line (no tiling on it). */
    float uv[2];
    if (kind.box) {
      composite_projection_face_uv(0, q, uv);
    }
    else if (kind.cube) {
      int cube_face = 0;
      composite_projection_cube_uv(kind, q, uv, cube_face);
    }
    else if (kind.cylinder) {
      composite_projection_cylinder_uv(kind, q, uv);
    }
    else if (kind.sphere) {
      composite_projection_sphere_uv(kind, q, uv);
    }
    else {
      /* Planar: X reads Y/Z, Y reads X/Z and Z reads
       * X/Y. */
      uv[0] = 0.5f + q[kind.u_index] * 0.5f;
      uv[1] = 0.5f + q[kind.v_index] * 0.5f;
    }

    float rgba[4];
    BKE_paint_material_generator_sample_coordinate(gradient, float3(uv[0], uv[1], 0.0f), rgba);
    float *pixel = out + (int64_t(y) * ref_width + x) * 4;
    pixel[0] = rgba[0] * rgba[3];
    pixel[1] = rgba[1] * rgba[3];
    pixel[2] = rgba[2] * rgba[3];
    pixel[3] = rgba[3];
  };

  composite_projection_raster(ctx, out, texel);
  return dst;
}

/* -------------------------------------------------------------------- */
/** \name Surface map (object position and normal per texel)
 * \{ */

/**
 * A threaded hash of a byte range. Chunks hash independently and combine in index order, so the
 * result does not depend on the thread schedule.
 */
static uint64_t composite_surface_hash_bytes(const void *data, const int64_t size)
{
  constexpr int64_t chunk_size = 1 << 16;
  const int64_t chunks_num = (size + chunk_size - 1) / chunk_size;
  Array<uint64_t> chunk_hashes(chunks_num, uint64_t(0));
  const uint8_t *bytes = static_cast<const uint8_t *>(data);
  threading::parallel_for(IndexRange(chunks_num), 16, [&](const IndexRange chunks) {
    for (const int64_t chunk : chunks) {
      const int64_t begin = chunk * chunk_size;
      const int64_t end = std::min(begin + chunk_size, size);
      uint64_t hash = 0xcbf29ce484222325ull ^ uint64_t(chunk);
      int64_t i = begin;
      for (; i + 8 <= end; i += 8) {
        uint64_t word;
        memcpy(&word, bytes + i, 8);
        hash = (hash ^ word) * 0x100000001b3ull;
        hash ^= hash >> 29;
      }
      for (; i < end; i++) {
        hash = (hash ^ uint64_t(bytes[i])) * 0x100000001b3ull;
      }
      chunk_hashes[chunk] = hash;
    }
  });
  uint64_t hash = uint64_t(size);
  for (const uint64_t chunk_hash : chunk_hashes) {
    hash = (hash * 0x9e3779b97f4a7c15ull) ^ chunk_hash;
    hash ^= hash >> 31;
  }
  return hash;
}

template<typename T> static uint64_t composite_surface_hash_span(const Span<T> span)
{
  return composite_surface_hash_bytes(span.data(), int64_t(span.size_in_bytes()));
}

/**
 * A cheap fingerprint of a mesh's geometry: array pointers, counts and an evenly strided sample of
 * the positions and UVs. It is only the validity test of the cached content hash.
 */
static uint64_t composite_mesh_fingerprint(const Mesh &mesh, const Span<float2> uvs)
{
  uint64_t hash = get_default_hash(mesh.verts_num, mesh.faces_num, mesh.corners_num);
  hash = get_default_hash(hash,
                          uintptr_t(&mesh),
                          uintptr_t(mesh.vert_positions().data()),
                          uintptr_t(mesh.face_offsets().data()),
                          uintptr_t(mesh.corner_verts().data()));
  const auto sample = [&](const auto span) {
    constexpr int64_t samples_num = 2048;
    const int64_t step = std::max<int64_t>(1, span.size() / samples_num);
    for (int64_t i = 0; i < span.size(); i += step) {
      hash = (hash ^ composite_surface_hash_bytes(&span[i], sizeof(span[i]))) * 0x100000001b3ull;
    }
  };
  sample(mesh.vert_positions());
  sample(uvs);
  return hash;
}

static struct {
  std::mutex mutex;
  struct Entry {
    const Mesh *mesh = nullptr;
    uint64_t fingerprint = 0;
    uint64_t content_hash = 0;
    std::chrono::steady_clock::time_point time;
  };
  Vector<Entry> entries;
} g_mesh_content_cache;

uint64_t composite_mesh_content_hash(const Mesh &mesh)
{
  const StringRefNull uv_name = BKE_paint_layers_gradient_uv_map_name(mesh);
  const bke::AttributeAccessor attributes = mesh.attributes();
  VArraySpan<float2> uvs;
  if (!uv_name.is_empty()) {
    if (const auto uv_attr = attributes.lookup<float2>(uv_name.c_str(), bke::AttrDomain::Corner)) {
      uvs = VArraySpan<float2>(*uv_attr);
    }
  }
  const uint64_t fingerprint = composite_mesh_fingerprint(mesh, uvs);
  const auto now = std::chrono::steady_clock::now();
  /* The sample can miss an in-place edit of a few vertices, so a trusted hash is re-verified after
   * this long: a stale surface map lives at most that long, even when no pointer or count moved. */
  constexpr std::chrono::milliseconds max_age(1000);
  {
    std::lock_guard lock(g_mesh_content_cache.mutex);
    for (const auto &entry : g_mesh_content_cache.entries) {
      if (entry.mesh == &mesh && entry.fingerprint == fingerprint && now - entry.time < max_age) {
        return entry.content_hash;
      }
    }
  }
  const VArraySpan<int> material_indices = *attributes.lookup_or_default<int>(
      "material_index", bke::AttrDomain::Face, 0);
  uint64_t content = composite_surface_hash_span(mesh.vert_positions());
  content = get_default_hash(content, composite_surface_hash_span(mesh.face_offsets()));
  content = get_default_hash(content, composite_surface_hash_span(mesh.corner_verts()));
  content = get_default_hash(content, composite_surface_hash_span<float2>(uvs));
  content = get_default_hash(content, composite_surface_hash_span<int>(material_indices));
  /* The corner normals follow sharp flags and custom normals, which positions do not cover. */
  content = get_default_hash(content, composite_surface_hash_span(mesh.corner_normals()));

  std::lock_guard lock(g_mesh_content_cache.mutex);
  auto &entries = g_mesh_content_cache.entries;
  for (auto &entry : entries) {
    if (entry.mesh == &mesh) {
      entry = {&mesh, fingerprint, content, now};
      return content;
    }
  }
  if (entries.size() >= 8) {
    entries.remove(0);
  }
  entries.append({&mesh, fingerprint, content, now});
  return content;
}

/** What one entry holds: the two RGBA32F texel arrays and the triangle basis while they have not
 * been released, else nothing. */
static size_t surface_map_cache_bytes(const PaintMaterialSurfaceMap &map)
{
  return map.texels_released ? 0 :
                               size_t(map.width) * size_t(map.height) * sizeof(float[4]) * 2 +
                                   size_t(map.tri_basis.size()) * sizeof(float4);
}

/**
 * The tangent basis of every corner triangle for the GPU's frame-to-tangent conversion of a
 * projected Normal map: the very `tangent` and `bitangent` the texel callback of
 * #composite_resample_projected derives from the triangle's UV gradient (object space, the true
 * bitangent, so a mirrored island keeps its relief's direction), laid as two texels per triangle id
 * (see #PaintMaterialSurfaceMap::tri_basis). A degenerate UV mapping stays zero, which the
 * conversion reads as "no basis" exactly as the CPU skips it.
 */
static Array<float4> surface_map_tri_basis_build(const CompositeProjectionBakeContext &ctx)
{
  const int basis_tris_num = std::min(int(ctx.tris.size()), PAINT_MATERIAL_SURFACE_TRI_BASIS_MAX);
  /* Pair 0 is the "none" id, so the table holds one pair more than there are triangles. */
  const int64_t texels_num = (int64_t(basis_tris_num) + 1) * 2;
  const int64_t rows_num = (texels_num + PAINT_MATERIAL_SURFACE_TRI_BASIS_WIDTH - 1) /
                           PAINT_MATERIAL_SURFACE_TRI_BASIS_WIDTH;
  Array<float4> basis(rows_num * PAINT_MATERIAL_SURFACE_TRI_BASIS_WIDTH, float4(0.0f));
  const float2 tile_origin(float(ctx.tile_x), float(ctx.tile_y));
  threading::parallel_for(IndexRange(basis_tris_num), 4096, [&](const IndexRange range) {
    for (const int64_t tri_index : range) {
      const int3 &corners = ctx.tris[tri_index];
      /* The tile-local UVs, as the rasterizer hands the texel callback, so the sums round alike. */
      const float2 uv0 = ctx.uvs[corners[0]] - tile_origin;
      const float2 uv1 = ctx.uvs[corners[1]] - tile_origin;
      const float2 uv2 = ctx.uvs[corners[2]] - tile_origin;
      const float2 duv1 = float2(uv1.x - uv0.x, uv1.y - uv0.y);
      const float2 duv2 = float2(uv2.x - uv0.x, uv2.y - uv0.y);
      const float det = duv1.x * duv2.y - duv1.y * duv2.x;
      if (!(std::abs(det) > 1e-12f)) {
        continue;
      }
      const float r = 1.0f / det;
      const float3 p0 = ctx.positions[ctx.corner_verts[corners[0]]];
      const float3 e1 = ctx.positions[ctx.corner_verts[corners[1]]] - p0;
      const float3 e2 = ctx.positions[ctx.corner_verts[corners[2]]] - p0;
      /* Unit directions (the gradient carries the island's scale), as the CPU bake and the graph's
       * normalized Tangent read them. */
      const float3 tangent = math::normalize((e1 * duv2.y - e2 * duv1.y) * r);
      const float3 bitangent = math::normalize((e2 * duv1.x - e1 * duv2.x) * r);
      const int64_t texel = (tri_index + 1) * 2;
      basis[texel] = float4(tangent, 1.0f);
      basis[texel + 1] = float4(bitangent, 0.0f);
    }
  });
  return basis;
}

/** What the store holds in sum: two 4K maps with texels are 1 GiB, a smaller canvas fits more. */
constexpr size_t SURFACE_MAP_CACHE_BYTES = size_t(1024) * 1024 * 1024;

/** The target of \a key without its content: the identity a rebuilt map of the same object,
 * material, tile and canvas replaces, which the key's content hash would not match. */
static bool surface_map_same_target(const PaintMaterialSurfaceMap &map,
                                    const PaintMaterialSurfaceMapKey &key)
{
  return map.key.object_uid == key.object_uid && map.key.material_uid == key.material_uid &&
         map.key.tile_number == key.tile_number && map.key.width == key.width &&
         map.key.height == key.height;
}

static struct {
  std::mutex mutex;
  /* The entries, oldest first: the vector is the LRU order, as the effect caches' store keeps its
   * own. A hit moves its entry to the back, the budget evicts from the front. */
  Vector<std::shared_ptr<const PaintMaterialSurfaceMap>> entries;
  size_t total_bytes = 0;
} g_surface_map_cache;

std::shared_ptr<const PaintMaterialSurfaceMap> BKE_paint_material_surface_map_get(
    const Object *object,
    const Material *material,
    const int width,
    const int height,
    const int tile_number,
    const bool need_texels)
{
  if (width <= 0 || height <= 0) {
    return nullptr;
  }
  const Mesh *mesh_ptr = composite_projection_mesh(object);
  if (mesh_ptr == nullptr || object->totcol <= 0) {
    return nullptr;
  }
  const Mesh &mesh = *mesh_ptr;
  const StringRefNull uv_name = BKE_paint_layers_gradient_uv_map_name(mesh);
  if (uv_name.is_empty()) {
    return nullptr;
  }
  const bke::AttributeAccessor attributes = mesh.attributes();
  const auto uv_attr = attributes.lookup<float2>(uv_name.c_str(), bke::AttrDomain::Corner);
  if (!uv_attr) {
    return nullptr;
  }
  const VArraySpan<float2> uvs(*uv_attr);
  Array<bool> slot_matches(object->totcol, false);
  if (composite_projection_slots_match(*object, material, slot_matches) == 0) {
    return nullptr;
  }
  const VArraySpan<int> material_indices = *attributes.lookup_or_default<int>(
      "material_index", bke::AttrDomain::Face, 0);

  /* No pointers: the evaluated and the original copy of one object must find the same entry. The
   * vertex positions are hashed by content, not versioned: a sculpt stroke moves them without a
   * topology change, and the mesh has no cheap positions generation to read, while ~12 MB per
   * million vertices hashes in a few milliseconds on all cores, against a rebuild of 100+ ms. */
  PaintMaterialSurfaceMapKey key;
  key.object_uid = object->id.session_uid;
  key.material_uid = (material != nullptr) ? DEG_get_original(material)->id.session_uid : 0;
  key.tile_number = tile_number;
  key.width = width;
  key.height = height;
  key.verts_num = mesh.verts_num;
  key.faces_num = mesh.faces_num;
  key.corners_num = mesh.corners_num;
  uint64_t content = composite_mesh_content_hash(mesh);
  for (const bool match : slot_matches) {
    content = get_default_hash(content, match);
  }
  key.content_hash = content;

  {
    std::lock_guard lock(g_surface_map_cache.mutex);
    for (const int i : g_surface_map_cache.entries.index_range()) {
      const std::shared_ptr<const PaintMaterialSurfaceMap> &candidate =
          g_surface_map_cache.entries[i];
      if (candidate->key == key && (!need_texels || !candidate->texels_released)) {
        /* The hit moves to the back: the vector is the LRU order. */
        std::shared_ptr<const PaintMaterialSurfaceMap> hit = candidate;
        g_surface_map_cache.entries.remove(i);
        g_surface_map_cache.entries.append(hit);
        return hit;
      }
    }
  }

  /* The map is in the painted object's space: a controller Empty is applied by the consumer, so one
   * entry serves every frame of the object. */
  PaintMaterialCompositeProjection projection;
  projection.present = true;
  projection.object = object;
  projection.material = material;
  CompositeProjectionBakeContext ctx(projection);
  if (!composite_projection_context_init(ctx, true, width, height, tile_number)) {
    return nullptr;
  }
  auto map = std::make_shared<PaintMaterialSurfaceMap>();
  map->key = key;
  map->width = width;
  map->height = height;
  const int64_t texels_num = int64_t(width) * height;
  map->position = Array<float4>(texels_num, float4(0.0f));
  map->normal = Array<float4>(texels_num, float4(0.0f));
  float *position_out = &map->position[0].x;
  float *normal_out = &map->normal[0].x;
  const auto texel = [&](const int x,
                         const int y,
                         const CompositeProjectionTri &tri,
                         const float p[3],
                         const float n_interp[3]) {
    const int64_t index = int64_t(y) * width + x;
    /* W marks a texel a triangle (or its padding) owns; the normal stays the interpolated, not
     * normalized one the CPU hands its own texel. The normal's W names the owning triangle, for the
     * tangent basis the GPU reads per triangle (id 0, "none", past what a float names exactly). */
    const float tri_id = (tri.index < PAINT_MATERIAL_SURFACE_TRI_BASIS_MAX) ? float(tri.index + 1) :
                                                                              0.0f;
    map->position[index] = float4(p[0], p[1], p[2], 1.0f);
    map->normal[index] = float4(n_interp[0], n_interp[1], n_interp[2], tri_id);
  };
  composite_projection_raster(ctx, position_out, texel, normal_out);
  map->tri_basis = surface_map_tri_basis_build(ctx);

  std::shared_ptr<const PaintMaterialSurfaceMap> result = std::move(map);
  const size_t bytes = surface_map_cache_bytes(*result);
  {
    std::lock_guard lock(g_surface_map_cache.mutex);
    /* A rebuilt map of the same target replaces its stale instance here, not through the budget:
     * the content hash in the key moved with the mesh, so the stale entry would not match by key,
     * and an entry whose texels are released holds no bytes for the budget to evict. */
    for (int64_t i = g_surface_map_cache.entries.size() - 1; i >= 0; i--) {
      if (surface_map_same_target(*g_surface_map_cache.entries[i], result->key)) {
        g_surface_map_cache.total_bytes -= surface_map_cache_bytes(*g_surface_map_cache.entries[i]);
        g_surface_map_cache.entries.remove(i);
      }
    }
    /* Room is made from the front, the least recently used entry first. The budget counts bytes,
     * and an entry alone above it still goes in, as the one entry the store holds. */
    while (!g_surface_map_cache.entries.is_empty() &&
           g_surface_map_cache.total_bytes + bytes > SURFACE_MAP_CACHE_BYTES)
    {
      g_surface_map_cache.total_bytes -= surface_map_cache_bytes(*g_surface_map_cache.entries[0]);
      g_surface_map_cache.entries.remove(0);
    }
    g_surface_map_cache.total_bytes += bytes;
    g_surface_map_cache.entries.append(result);
  }
  return result;
}

void BKE_paint_material_surface_map_texels_release(
    const std::shared_ptr<const PaintMaterialSurfaceMap> &map)
{
  std::lock_guard lock(g_surface_map_cache.mutex);
  if (map == nullptr || map->texels_released) {
    return;
  }
  const size_t bytes = surface_map_cache_bytes(*map);
  for (const int i : g_surface_map_cache.entries.index_range()) {
    if (g_surface_map_cache.entries[i] == map) {
      /* The entry is shared as const so a reader cannot change it, but it is owned here and the
       * one consumer that asked for this has copied the texels already. */
      PaintMaterialSurfaceMap &released = const_cast<PaintMaterialSurfaceMap &>(*map);
      released.position = Array<float4>();
      released.normal = Array<float4>();
      released.tri_basis = Array<float4>();
      released.texels_released = true;
      g_surface_map_cache.total_bytes -= bytes;
      return;
    }
  }
}

void BKE_paint_material_surface_map_free()
{
  std::lock_guard lock(g_surface_map_cache.mutex);
  g_surface_map_cache.entries.clear_and_shrink();
  g_surface_map_cache.total_bytes = 0;
  /* The Decal maps are rebuilt from the same mesh data, so they leave with the surface map. */
  BKE_paint_material_decal_map_free();
}

/** \} */

}  // namespace blender
