/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Selection masks in the 3D Viewport: the UV -> surface map, the mask boundary walkers, the
 * floating-selection preview drawing (flat and PBR composite) and the 3D overlay (marching-ants
 * outline projected onto the surface). Split from paint_image_select_view3d.cc to keep that file
 * under the line limit.
 */

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>

#include "BLF_api.hh"
#include "BLT_translation.hh"

#include "MEM_guardedalloc.h"

#include "BLI_array.hh"
#include "BLI_bitmap.h"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_function_ref.hh"
#include "BLI_index_mask.hh"
#include "BLI_listbase_wrapper.hh"
#include "BLI_map.hh"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_task.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_brush_types.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_userdef_types.h"
#include "DNA_view3d_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_attribute.hh"
#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_image_paint_selection.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_library.hh"
#include "BKE_mesh.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_paint_bvh_pixels.hh"
#include "BKE_paint_layers_target.hh"
#include "BKE_paint_types.hh"
#include "BKE_screen.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "DRW_engine.hh"
#include "DRW_select_buffer.hh"

#include "ED_image.hh"
#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_select_utils.hh"
#include "ED_space_api.hh"
#include "ED_undo.hh"
#include "ED_uvedit.hh"
#include "ED_view3d.hh"

#include "GPU_batch.hh"
#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_shader_builtin.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"
#include "GPU_vertex_buffer.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_interface.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "../shapes/paint_shape_space.hh"
#include "../paint_intern.hh"
#include "../mesh/mesh_brush_common.hh"
#include "paint_face_selection_mask.hh"
#include "paint_image_select_intern.hh"
#include "paint_image_select_view3d.hh"
#include "../mesh/paint_image_uv_geom.hh"
#include "../mesh/sculpt_intern.hh"

#include "GEO_reverse_uv_sampler.hh"
namespace blender::ed::sculpt_paint {

/* -------------------------------------------------------------------- */
/** \name Constants
 * \{ */

/** Marching-ants dash width (matching the Image Editor overlay). */
constexpr float SELECT_VIEW3D_DASH_WIDTH = 8.0f;
/** Upper bound of the overlay's triangle count (a normal selection needs far fewer). */
constexpr int64_t SELECT_VIEW3D_OUTLINE_MAX_TRIS = 200000;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Surface map (UV -> surface sampling)
 * \{ */

bool ImageSelectView3DSurfaceMap::ensure(const Object &ob, const StringRef uv_name)
{
  const Mesh *mesh = id_cast<const Mesh *>(ob.data);
  if (mesh == nullptr) {
    return false;
  }
  /* Identity guards: rebuild when the object, the UV map or the topology changed. */
  const uint32_t ob_uid = uint32_t(ob.id.session_uid);
  const int64_t corner_tri_num = int64_t(mesh->corner_tris().size());
  if (this->sampler != nullptr && this->object_session_uid == ob_uid &&
      this->corner_tri_num == corner_tri_num && uv_name == this->uv_map_name)
  {
    return true;
  }
  const std::string name = uv_name.is_empty() ? std::string(mesh->active_or_default_uv_map_name()) :
                                                std::string(uv_name);
  if (name.empty()) {
    return false;
  }
  const bke::AttributeReader<float2> uv_attribute = mesh->attributes().lookup<float2>(
      name, bke::AttrDomain::Corner);
  if (!uv_attribute) {
    return false;
  }
  const VArraySpan<float2> uv_span(*uv_attribute);
  const Span<int3> corner_tris = mesh->corner_tris();

  this->uv_map = Array<float2>(uv_span);
  this->corner_tris = Array<int3>(corner_tris);
  this->sampler = std::make_unique<geometry::ReverseUVSampler>(this->uv_map.as_span(),
                                                               this->corner_tris.as_span());
  this->object_session_uid = ob_uid;
  this->corner_tri_num = corner_tri_num;
  this->uv_map_name = name;
  return true;
}

bool ImageSelectView3DSurfaceMap::sample(const Span<int> corner_verts,
                                         const Span<float3> vert_positions,
                                         const float2 &uv,
                                         float3 &r_co,
                                         float3 &r_no,
                                         int *r_tri_index) const
{
  if (this->sampler == nullptr) {
    return false;
  }
  const geometry::ReverseUVSampler::Result result = this->sampler->sample(uv);
  if (result.type == geometry::ReverseUVSampler::ResultType::None ||
      result.tri_index < 0 || result.tri_index >= this->corner_tris.size())
  {
    return false;
  }
  if (r_tri_index != nullptr) {
    *r_tri_index = result.tri_index;
  }
  const int3 tri = this->corner_tris[result.tri_index];
  if (tri.x >= corner_verts.size() || tri.y >= corner_verts.size() ||
      tri.z >= corner_verts.size())
  {
    return false;
  }
  const int3 verts = int3(corner_verts[tri.x], corner_verts[tri.y], corner_verts[tri.z]);
  if (verts.x >= vert_positions.size() || verts.y >= vert_positions.size() ||
      verts.z >= vert_positions.size())
  {
    return false;
  }
  const float3 bary = result.bary_weights;
  r_co = vert_positions[verts.x] * bary.x + vert_positions[verts.y] * bary.y +
         vert_positions[verts.z] * bary.z;
  const float3 e1 = vert_positions[verts.y] - vert_positions[verts.x];
  const float3 e2 = vert_positions[verts.z] - vert_positions[verts.x];
  r_no = math::normalize(math::cross(e1, e2));
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mask boundary in UV space
 * \{ */

void image_select_mask_boundary_segments(const float *data,
                                         const int2 size,
                                         const int2 scan_min,
                                         const int2 scan_max,
                                         const FunctionRef<float2(const int2 &)> uv_cell,
                                         const int stride,
                                         Vector<float2> &r_segments)
{
  const int stride_clamped = std::max(stride, 1);
  const int x0 = std::clamp(scan_min.x, 0, size.x - 1);
  const int x1 = std::clamp(scan_max.x, 0, size.x);
  const int y0 = std::clamp(scan_min.y, 0, size.y - 1);
  const int y1 = std::clamp(scan_max.y, 0, size.y);
  if (x0 >= x1 || y0 >= y1) {
    return;
  }
  const auto selected = [&](const int x, const int y) {
    return x >= 0 && y >= 0 && x < size.x && y < size.y &&
           data[int64_t(y) * size.x + x] > SELECTION_MASK_THRESHOLD;
  };

  /* Rows are independent: each worker accumulates its own segments, concatenated afterwards
   * (segment order does not matter, every line is surface-mapped independently). */
  const int row_count = ((y1 - y0 - 1) / stride_clamped) + 1;
  threading::EnumerableThreadSpecific<Vector<float2>> all_tls;
  threading::parallel_for(IndexRange(row_count), 64, [&](const IndexRange rows) {
    Vector<float2> &local = all_tls.local();
    for (const int r : rows) {
      const int y = y0 + r * stride_clamped;
      for (int x = x0; x < x1; x += stride_clamped) {
        if (!selected(x, y)) {
          continue;
        }
        if (!selected(x - 1, y)) {
          local.append(uv_cell(int2(x, y)));
          local.append(uv_cell(int2(x, y + 1)));
        }
        if (!selected(x + 1, y)) {
          local.append(uv_cell(int2(x + 1, y)));
          local.append(uv_cell(int2(x + 1, y + 1)));
        }
        if (!selected(x, y - 1)) {
          local.append(uv_cell(int2(x, y)));
          local.append(uv_cell(int2(x + 1, y)));
        }
        if (!selected(x, y + 1)) {
          local.append(uv_cell(int2(x, y + 1)));
          local.append(uv_cell(int2(x + 1, y + 1)));
        }
      }
    }
  });
  for (const Vector<float2> &local : all_tls) {
    r_segments.extend(local);
  }
}

Vector<float2> image_select_view3d_mask_boundary_uv(const Image &image,
                                                    const int tile_number,
                                                    const ImageUser & /*iuser*/,
                                                    const int step)
{
  Vector<float2> segments;
  const ImBuf *mask = BKE_image_paint_selection_mask_lookup(&image, tile_number);
  if (mask == nullptr || mask->float_data() == nullptr) {
    return segments;
  }
  const float *data = mask->float_data();
  const int w = mask->x;
  const int h = mask->y;
  const float2 uv_origin = BKE_image_get_tile_uv_origin(tile_number);
  const auto uv_cell = [&](const int2 &px) {
    return uv_origin + float2(float(px.x) / float(w), float(px.y) / float(h));
  };

  /* Scan only the selection's bounding box (memoized in BKE against the mask revision): the
   * boundary cannot lie outside it, and an 8K tile is mostly empty around a small selection. */
  int2 scan_min(0, 0);
  int2 scan_max(w, h);
  int bounds_min[2];
  int bounds_max[2];
  if (BKE_image_paint_selection_mask_bounds(&image, tile_number, bounds_min, bounds_max)) {
    scan_min = int2(bounds_min[0], bounds_min[1]);
    scan_max = int2(bounds_max[0], bounds_max[1]);
  }
  image_select_mask_boundary_segments(data, int2(w, h), scan_min, scan_max, uv_cell, step,
                                      segments);
  return segments;
}

void image_select_view3d_draw_uv_outlines(const Object &ob,
                                          const ImageSelectView3DSurfaceMap &surface_map,
                                          const Span<Vector<float2>> uv_polylines,
                                          const float color[4],
                                          const float dash_width)
{
  if (uv_polylines.is_empty()) {
    return;
  }
  const Mesh *mesh = id_cast<const Mesh *>(ob.data);
  if (mesh == nullptr) {
    return;
  }
  const Span<int> corner_verts = mesh->corner_verts();
  const Span<float3> vert_positions = mesh->vert_positions();

  float4x4 object_to_world(ob.object_to_world());

  int64_t total_points = 0;
  for (const Vector<float2> &poly : uv_polylines) {
    total_points += int64_t(poly.size());
  }

  GPU_blend(GPU_BLEND_ALPHA);
  GPU_line_width(1.0f);
  const uint pos_3d = GPU_vertformat_attr_add(immVertexFormat(), "pos",
                                              gpu::VertAttrType::SFLOAT_32_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_LINE_DASHED_UNIFORM_COLOR);
  float viewport_size[4];
  GPU_viewport_size_get_f(viewport_size);
  immUniform2f("viewport_size", viewport_size[2], viewport_size[3]);
  immUniform1i("colors_len", 2); /* Two-tone dash. */
  immUniform4f("color", color[0], color[1], color[2], color[3]);
  immUniform4f("color2", 1.0f, 1.0f, 1.0f, 1.0f);
  immUniform1f("dash_width", dash_width);
  immUniform1f("udash_factor", 0.5f);

  /* The inputs are segment pairs (#image_select_mask_boundary_segments), not connected
   * polylines: chaining consecutive points would join unrelated boundary segments with long
   * lines. Segments whose ends miss the surface are dropped, so the vertex count is only known
   * after sampling and #immBegin must receive exactly that count. */
  Vector<float3> line_verts;
  line_verts.reserve(total_points);
  for (const Vector<float2> &poly : uv_polylines) {
    for (int64_t i = 0; i + 1 < poly.size(); i += 2) {
      float3 co_a, no_a, co_b, no_b;
      if (!surface_map.sample(corner_verts, vert_positions, poly[i], co_a, no_a)) {
        continue;
      }
      if (!surface_map.sample(corner_verts, vert_positions, poly[i + 1], co_b, no_b)) {
        continue;
      }
      line_verts.append(math::transform_point(object_to_world, co_a));
      line_verts.append(math::transform_point(object_to_world, co_b));
    }
  }
  if (!line_verts.is_empty()) {
    immBegin(GPU_PRIM_LINES, uint(line_verts.size()));
    for (const float3 &v : line_verts) {
      immVertex3f(pos_3d, v.x, v.y, v.z);
    }
    immEnd();
  }
  immUnbindProgram();
  GPU_blend(GPU_BLEND_NONE);
}

void image_select_view3d_draw_uv_texture_ext(
    const Object &ob,
    const ImageSelectView3DSurfaceMap &surface_map,
    gpu::Texture *texture,
    const float2 &dest_rect_min,
    const float2 &dest_rect_max,
    const FunctionRef<float2(const float2 &dest_uv)> &dest_uv_to_texco)
{
  const Mesh *mesh = id_cast<const Mesh *>(ob.data);
  if (mesh == nullptr || texture == nullptr || surface_map.uv_map.is_empty() ||
      dest_rect_min.x >= dest_rect_max.x || dest_rect_min.y >= dest_rect_max.y)
  {
    return;
  }
  const Span<int> corner_verts = mesh->corner_verts();
  const Span<float3> vert_positions = mesh->vert_positions();
  const float4x4 object_to_world(ob.object_to_world());

  /* Every mesh triangle whose UV bounds touch the rect, with the texture coordinate derived from
   * its own UVs through \a dest_uv_to_texco: the fragment is drawn on the surface exactly where
   * the commit will write it. */
  Vector<float3> positions;
  Vector<float2> tex_coords;
  for (const int3 &tri : surface_map.corner_tris) {
    const float2 uv0 = surface_map.uv_map[tri.x];
    const float2 uv1 = surface_map.uv_map[tri.y];
    const float2 uv2 = surface_map.uv_map[tri.z];
    const float2 tri_min = math::min(uv0, math::min(uv1, uv2));
    const float2 tri_max = math::max(uv0, math::max(uv1, uv2));
    if (tri_max.x < dest_rect_min.x || tri_min.x > dest_rect_max.x ||
        tri_max.y < dest_rect_min.y || tri_min.y > dest_rect_max.y)
    {
      continue;
    }
    for (const int c : IndexRange(3)) {
      positions.append(math::transform_point(object_to_world,
                                             vert_positions[corner_verts[tri[c]]]));
      tex_coords.append(dest_uv_to_texco(surface_map.uv_map[tri[c]]));
    }
  }
  if (positions.is_empty()) {
    return;
  }

  GPU_blend(GPU_BLEND_ALPHA);
  GPU_depth_test(GPU_DEPTH_LESS_EQUAL);
  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  const uint texco = GPU_vertformat_attr_add(format, "texCoord", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_IMAGE_COLOR);
  immUniformColor4f(1.0f, 1.0f, 1.0f, 1.0f);
  /* Clamp to the (transparent) border: triangles that only partly overlap the rect sample outside
   * [0, 1] and must stay invisible there. */
  immBindTextureSampler("image",
                        texture,
                        {GPU_SAMPLER_FILTERING_LINEAR,
                         GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER,
                         GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER});
  immBegin(GPU_PRIM_TRIS, uint(positions.size()));
  for (const int64_t i : positions.index_range()) {
    immAttr2f(texco, tex_coords[i].x, tex_coords[i].y);
    immVertex3f(pos, positions[i].x, positions[i].y, positions[i].z);
  }
  immEnd();
  immUnbindProgram();
  GPU_blend(GPU_BLEND_NONE);
}

void image_select_view3d_draw_uv_texture(const Object &ob,
                                         const ImageSelectView3DSurfaceMap &surface_map,
                                         gpu::Texture *texture,
                                         const float2 &uv_min,
                                         const float2 &uv_size)
{
  if (uv_size.x <= 0.0f || uv_size.y <= 0.0f) {
    return;
  }
  image_select_view3d_draw_uv_texture_ext(
      ob, surface_map, texture, uv_min, uv_min + uv_size, [&](const float2 &dest_uv) {
        return (dest_uv - uv_min) / uv_size;
      });
}

void image_select_view3d_pbr_program_bind(
    gpu::Texture *channel_textures[PBR_PREVIEW_CHANNEL_NUM],
    const float2 channel_scale[PBR_PREVIEW_CHANNEL_NUM],
    const float2 channel_offset[PBR_PREVIEW_CHANNEL_NUM],
    const float3 &light_direction,
    const float3 &view_direction)
{
  immBindBuiltinProgram(GPU_SHADER_IMAGE_SELECT_PBR);

  static const char *texture_names[PBR_PREVIEW_CHANNEL_NUM] = {
      "base_color_tex",
      "roughness_tex",
      "metallic_tex",
      "normal_tex",
  };
  static const char *scale_names[PBR_PREVIEW_CHANNEL_NUM] = {
      "texco_scale_base_color",
      "texco_scale_roughness",
      "texco_scale_metallic",
      "texco_scale_normal",
  };
  static const char *offset_names[PBR_PREVIEW_CHANNEL_NUM] = {
      "texco_offset_base_color",
      "texco_offset_roughness",
      "texco_offset_metallic",
      "texco_offset_normal",
  };
  for (const int slot : IndexRange(PBR_PREVIEW_CHANNEL_NUM)) {
    /* Clamp to the (transparent) border, like the flat draw: triangles that only partly overlap
     * the rect sample outside [0, 1] and must stay invisible there. */
    immBindTextureSampler(texture_names[slot],
                          channel_textures[slot],
                          {GPU_SAMPLER_FILTERING_LINEAR,
                           GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER,
                           GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER});
    immUniform2f(scale_names[slot], channel_scale[slot].x, channel_scale[slot].y);
    immUniform2f(offset_names[slot], channel_offset[slot].x, channel_offset[slot].y);
  }
  immUniform3f("light_direction", light_direction.x, light_direction.y, light_direction.z);
  immUniform3f("view_direction", view_direction.x, view_direction.y, view_direction.z);
}

void image_select_view3d_pbr_light_get(const ARegion *region,
                                       float3 &r_view_direction,
                                       float3 &r_light_direction)
{
  r_view_direction = float3(0.0f, 0.0f, 1.0f);
  r_light_direction = float3(0.0f, 0.0f, 1.0f);
  if (region == nullptr || region->regiondata == nullptr) {
    return;
  }
  const RegionView3D *rv3d = static_cast<const RegionView3D *>(region->regiondata);
  const float3 view_z(rv3d->viewinv[2]);
  const float3 view_y(rv3d->viewinv[1]);
  const float3 view_x(rv3d->viewinv[0]);
  r_view_direction = math::normalize(view_z);
  r_light_direction = math::normalize(view_z + view_y * 0.8f + view_x * 0.6f);
}

void image_select_view3d_pbr_texco(const SelectionTileFragment &frag,
                                   const float2 &uv_offset,
                                   float2 &r_scale,
                                   float2 &r_offset)
{
  const float2 tile_size = float2(frag.geom.tile_size_px);
  const float2 tile_uv = image_select_udim_tile_uv_origin(frag.geom.tile_number);
  const float2 rect_min = tile_uv + float2(frag.geom.origin_px) / tile_size + uv_offset;
  const float2 rect_size_uv = math::max(float2(frag.geom.size_px) / tile_size, float2(1e-12f));
  r_scale = float2(1.0f) / rect_size_uv;
  r_offset = -rect_min * r_scale;
}

gpu::Texture *image_select_view3d_pbr_default_texture_create(const char *name, const int slot)
{
  /* Base color white, roughness mid, metallic 0, flat tangent-space normal. */
  static const float defaults[PBR_PREVIEW_CHANNEL_NUM][4] = {
      {1.0f, 1.0f, 1.0f, 1.0f},
      {0.5f, 0.5f, 0.5f, 1.0f},
      {0.0f, 0.0f, 0.0f, 1.0f},
      {0.5f, 0.5f, 1.0f, 1.0f},
  };
  BLI_assert(slot >= 0 && slot < PBR_PREVIEW_CHANNEL_NUM);
  return GPU_texture_create_2d(name,
                               1,
                               1,
                               1,
                               gpu::TextureFormat::SFLOAT_16_16_16_16,
                               GPU_TEXTURE_USAGE_SHADER_READ,
                               defaults[slot]);
}

void image_select_view3d_draw_uv_texture_pbr(
    const Object &ob,
    const ImageSelectView3DSurfaceMap &surface_map,
    const float2 &dest_rect_min,
    const float2 &dest_rect_max,
    gpu::Texture *channel_textures[PBR_PREVIEW_CHANNEL_NUM],
    const float2 channel_scale[PBR_PREVIEW_CHANNEL_NUM],
    const float2 channel_offset[PBR_PREVIEW_CHANNEL_NUM],
    const FunctionRef<float2(const float2 &)> dest_to_src,
    const float3 &light_direction,
    const float3 &view_direction)
{
  const Mesh *mesh = id_cast<const Mesh *>(ob.data);
  if (mesh == nullptr || surface_map.uv_map.is_empty() || dest_rect_min.x >= dest_rect_max.x ||
      dest_rect_min.y >= dest_rect_max.y)
  {
    return;
  }
  for (const int slot : IndexRange(PBR_PREVIEW_CHANNEL_NUM)) {
    if (channel_textures[slot] == nullptr) {
      return;
    }
  }

  /* The same triangle walk as #image_select_view3d_draw_uv_texture_ext, plus the vertex normals
   * the PBR shader shades with. The texture coordinate passed per vertex is the destination UV
   * itself; the per-channel scale/offset uniforms map it into each channel's own fragment rect
   * (the channels may live at different tile resolutions). */
  const Span<int> corner_verts = mesh->corner_verts();
  const Span<float3> vert_positions = mesh->vert_positions();
  const Span<float3> vert_normals = mesh->vert_normals();
  const float4x4 object_to_world(ob.object_to_world());

  Vector<float3> positions;
  Vector<float3> normals;
  Vector<float2> dest_uvs;
  for (const int3 &tri : surface_map.corner_tris) {
    const float2 uv0 = surface_map.uv_map[tri.x];
    const float2 uv1 = surface_map.uv_map[tri.y];
    const float2 uv2 = surface_map.uv_map[tri.z];
    const float2 tri_min = math::min(uv0, math::min(uv1, uv2));
    const float2 tri_max = math::max(uv0, math::max(uv1, uv2));
    if (tri_max.x < dest_rect_min.x || tri_min.x > dest_rect_max.x ||
        tri_max.y < dest_rect_min.y || tri_min.y > dest_rect_max.y)
    {
      continue;
    }
    for (const int c : IndexRange(3)) {
      const int corner = tri[c];
      positions.append(math::transform_point(object_to_world, vert_positions[corner_verts[corner]]));
      normals.append(vert_normals[corner_verts[corner]]);
      dest_uvs.append(dest_to_src(surface_map.uv_map[corner]));
    }
  }
  if (positions.is_empty()) {
    return;
  }
  /* The normals are gathered in object space: one #math::transform_normals pass applies the
   * inverse-transpose that keeps them pointing right off a non-uniformly scaled object. */
  math::transform_normals(float3x3(object_to_world), normals);

  GPU_blend(GPU_BLEND_ALPHA);
  GPU_depth_test(GPU_DEPTH_LESS_EQUAL);
  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  const uint src_uv = GPU_vertformat_attr_add(format, "src_uv", gpu::VertAttrType::SFLOAT_32_32);
  const uint nor = GPU_vertformat_attr_add(format, "nor", gpu::VertAttrType::SFLOAT_32_32_32);
  image_select_view3d_pbr_program_bind(
      channel_textures, channel_scale, channel_offset, light_direction, view_direction);

  immBegin(GPU_PRIM_TRIS, uint(positions.size()));
  for (const int64_t i : positions.index_range()) {
    immAttr3f(nor, normals[i].x, normals[i].y, normals[i].z);
    immAttr2f(src_uv, dest_uvs[i].x, dest_uvs[i].y);
    immVertex3f(pos, positions[i].x, positions[i].y, positions[i].z);
  }
  immEnd();
  immUnbindProgram();
  GPU_blend(GPU_BLEND_NONE);
}

bool image_select_pbr_composite_possible(const Span<ImagePaintSelectionTarget> targets)
{
  for (const ImagePaintSelectionTarget &target : targets) {
    if (image_select_pbr_channel_slot(target.channel) >= 0) {
      return true;
    }
  }
  return false;
}

int image_select_preview_target_resolve(const Span<ImagePaintSelectionTarget> targets,
                                        const int preview_target)
{
  if (preview_target >= 0 && preview_target < targets.size()) {
    return preview_target;
  }
  if (image_select_pbr_composite_possible(targets)) {
    return -1;
  }
  for (const int target_i : targets.index_range()) {
    if (targets[target_i].is_color_channel) {
      return target_i;
    }
  }
  return 0;
}

int image_select_preview_target_cycle(const Span<ImagePaintSelectionTarget> targets,
                                      const int preview_target)
{
  if (targets.is_empty()) {
    return preview_target;
  }
  /* A canvas of PBR maps alone keeps the composite; a single-target canvas keeps its one flat
   * view. */
  Vector<int> choices;
  if (image_select_pbr_composite_possible(targets)) {
    choices.append(-1);
  }
  for (const int target_i : targets.index_range()) {
    if (image_select_pbr_channel_slot(targets[target_i].channel) < 0) {
      choices.append(target_i);
    }
  }
  if (choices.is_empty()) {
    return -1;
  }
  int current = int(std::find(choices.begin(), choices.end(), preview_target) - choices.begin());
  if (current == choices.size()) {
    /* An index that is no longer a choice (or the initial -1 without a composite) starts from
     * the beginning. */
    current = -1;
  }
  return choices[(current + 1) % int(choices.size())];
}

gpu::Texture *image_select_view3d_fragment_texture(Vector<Vector<gpu::Texture *>> &textures,
                                                   const Span<Vector<SelectionTileFragment>> fragments,
                                                   const int target_i,
                                                   const int frag_i)
{
  if (textures.size() != fragments.size()) {
    textures.resize(fragments.size());
  }
  Vector<gpu::Texture *> &per_target = textures[target_i];
  if (per_target.size() != fragments[target_i].size()) {
    per_target.resize(fragments[target_i].size(), nullptr);
  }
  const SelectionTileFragment &frag = fragments[target_i][frag_i];
  if (per_target[frag_i] == nullptr && frag.preview.fragment_display_ibuf != nullptr) {
    per_target[frag_i] = IMB_create_gpu_texture(
        "SelectionFragment", frag.preview.fragment_display_ibuf, true, false, false);
    if (per_target[frag_i] != nullptr) {
      /* Linear, not nearest: the fragment is usually minified heavily on the surface, and
       * nearest sampling drops whole texel rows there (the preview showed through only in
       * patches). No mipmaps in this texture path, so the minified rim stays a bit sharp. */
      GPU_texture_filter_mode(per_target[frag_i], true);
    }
  }
  return per_target[frag_i];
}

void image_select_view3d_draw_pbr_composite(
    const Object &ob,
    const ImageSelectView3DSurfaceMap &surface_map,
    const ARegion *region,
    const Span<ImagePaintSelectionTarget> targets,
    const Span<Vector<SelectionTileFragment>> fragments,
    Vector<Vector<gpu::Texture *>> &textures,
    gpu::Texture *default_textures[PBR_PREVIEW_CHANNEL_NUM],
    const FunctionRef<float2(const float2 &)> forward,
    const FunctionRef<float2(const float2 &)> inverse)
{
  /* The target that feeds a slot: the lowest one wins (a clipboard cannot carry a channel twice,
   * this is the single place that guarantees it). */
  const auto slot_target_index = [&](const int slot) {
    for (const int target_i : targets.index_range()) {
      if (image_select_pbr_channel_slot(targets[target_i].channel) == slot) {
        return target_i;
      }
    }
    return -1;
  };
  int driver_slot = -1;
  int driver_target = -1;
  for (const int slot : IndexRange(PBR_PREVIEW_CHANNEL_NUM)) {
    driver_target = slot_target_index(slot);
    if (driver_target >= 0) {
      driver_slot = slot;
      break;
    }
  }
  if (driver_target < 0) {
    return;
  }

  float3 view_direction;
  float3 light_direction;
  image_select_view3d_pbr_light_get(region, view_direction, light_direction);

  const Span<SelectionTileFragment> driver_fragments = fragments[driver_target];
  for (const int frag_i : driver_fragments.index_range()) {
    const SelectionTileFragment &driver_frag = driver_fragments[frag_i];
    if (driver_frag.preview.fragment_display_ibuf == nullptr) {
      continue;
    }

    gpu::Texture *channel_textures[PBR_PREVIEW_CHANNEL_NUM] = {nullptr};
    float2 channel_scale[PBR_PREVIEW_CHANNEL_NUM];
    float2 channel_offset[PBR_PREVIEW_CHANNEL_NUM];
    for (const int slot : IndexRange(PBR_PREVIEW_CHANNEL_NUM)) {
      const SelectionTileFragment *slot_frag = nullptr;
      int slot_target = -1;
      int slot_frag_i = -1;
      if (slot == driver_slot) {
        slot_frag = &driver_frag;
        slot_target = driver_target;
        slot_frag_i = frag_i;
      }
      else if (const int target_i = slot_target_index(slot); target_i >= 0) {
        /* One fragment per tile by construction (#image_select_extract_per_tile). */
        for (const int other_i : fragments[target_i].index_range()) {
          if (fragments[target_i][other_i].geom.tile_number == driver_frag.geom.tile_number) {
            slot_frag = &fragments[target_i][other_i];
            slot_target = target_i;
            slot_frag_i = other_i;
            break;
          }
        }
      }
      if (slot_frag != nullptr) {
        channel_textures[slot] = image_select_view3d_fragment_texture(
            textures, fragments, slot_target, slot_frag_i);
      }
      if (channel_textures[slot] == nullptr) {
        if (default_textures[slot] == nullptr) {
          default_textures[slot] = image_select_view3d_pbr_default_texture_create(
              "SelectionPBRDefault", slot);
        }
        channel_textures[slot] = default_textures[slot];
      }
      /* A stand-in takes the driver's rect, so its border clamp keeps it from covering the whole
       * triangle outside the fragment. The vertices carry the source UV, so no offset here. */
      image_select_view3d_pbr_texco(slot_frag != nullptr ? *slot_frag : driver_frag,
                                    float2(0.0f),
                                    channel_scale[slot],
                                    channel_offset[slot]);
    }

    /* The destination rect: the four source corners through the forward transform. */
    const float2 tile_uv = image_select_udim_tile_uv_origin(driver_frag.geom.tile_number);
    const float2 tile_size = float2(driver_frag.geom.tile_size_px);
    const float2 src_origin = tile_uv + float2(driver_frag.geom.origin_px) / tile_size;
    const float2 src_size = float2(driver_frag.geom.size_px) / tile_size;
    float2 dest_min(FLT_MAX, FLT_MAX);
    float2 dest_max(-FLT_MAX, -FLT_MAX);
    {
      for (const int i : IndexRange(4)) {
        const float2 corner = forward(src_origin + float2((i & 1) ? src_size.x : 0.0f,
                                                           (i & 2) ? src_size.y : 0.0f));
        dest_min = math::min(dest_min, corner);
        dest_max = math::max(dest_max, corner);
      }
    }
    /* The vertices carry the source UV through the inverse transform. */
    const auto dest_to_src = [&](const float2 &dest_uv) -> float2 {
      return inverse(dest_uv);
    };
    image_select_view3d_draw_uv_texture_pbr(ob,
                                            surface_map,
                                            dest_min,
                                            dest_max,
                                            channel_textures,
                                            channel_scale,
                                            channel_offset,
                                            dest_to_src,
                                            light_direction,
                                            view_direction);
  }
}

void image_select_view3d_preview_textures_free(Vector<Vector<gpu::Texture *>> &textures)
{
  bool any = false;
  for (const Vector<gpu::Texture *> &per_target : textures) {
    for (const gpu::Texture *tex : per_target) {
      any = any || tex != nullptr;
    }
  }
  if (!any) {
    textures.clear();
    return;
  }
  const bool context_enabled = DRW_gpu_context_is_enabled();
  if (!context_enabled) {
    DRW_gpu_context_enable();
  }
  for (Vector<gpu::Texture *> &per_target : textures) {
    for (gpu::Texture *tex : per_target) {
      if (tex != nullptr) {
        GPU_texture_free(tex);
      }
    }
  }
  if (!context_enabled) {
    /* Restores the window's drawable: this runs between redraws (session end after a commit),
     * and leaving the GPU context unset crashes the next #wm_draw_update. */
    DRW_gpu_context_disable();
  }
  textures.clear();
}

void image_select_view3d_preview_texture_free(gpu::Texture *&texture)
{
  if (texture == nullptr) {
    return;
  }
  const bool context_enabled = DRW_gpu_context_is_enabled();
  if (!context_enabled) {
    DRW_gpu_context_enable();
  }
  GPU_texture_free(texture);
  texture = nullptr;
  if (!context_enabled) {
    /* Restores the window's drawable: see #image_select_view3d_preview_textures_free. */
    DRW_gpu_context_disable();
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name 3D overlay (marching-ants outline projected onto the surface)
 * \{ */

namespace image_select_v3d {

/**
 * The mask as a GPU 2D-array texture (one layer per mask-bearing tile) plus the UV triangles that
 * cover the selection's neighborhoods. The boundary shader samples the mask through the mesh UVs
 * and draws the 0.5-crossing as a screen-space-dashed line: no CPU boundary extraction, the
 * outline follows the surface exactly, and depth testing works because the overlay is drawn on
 * the surface itself.
 */
struct OverlayCache {
  const Image *image = nullptr;
  uint64_t revision = 0;
  uint32_t ob_uid = 0;
  int64_t corner_tri_num = 0;
  std::string uv_map_name;
  /** Mask texture (SFLOAT_16, #texture_size per layer) and the tile number of each layer. */
  gpu::Texture *mask_texture = nullptr;
  Vector<int> layer_tiles;
  int2 texture_size = int2(0);
  /** Region uploaded into each texture layer last time (invalid when nothing was uploaded): the
   * next upload covers its union with the new bounds, which is where the mask may have changed
   * (a change either adds a selected pixel — inside the new bounds — or clears one — inside the
   * previous ones). */
  Vector<rcti> layer_uploaded;
  /** Triangle batch: interleaved world position, global UV and tile layer per corner. */
  gpu::VertBuf *tris_verts = nullptr;
  gpu::Batch *tris_batch = nullptr;
  /** Corner indices of every emitted triangle (3 each) for the position refreshes. */
  Vector<int3> tri_corners;
  /** CPU mirrors of the static attributes (re-filled on every position upload). */
  Vector<float2> tri_uvs;
  Vector<float> tri_layers;
  /** UV bounds of every corner triangle (min.xy, max.zw in global UV) and the mesh identity they
   * were computed from: the per-rebuild triangle filter is then a plain scan over them instead of
   * an attribute walk with per-triangle min/max. */
  Array<float4> tri_uv_bounds;
  uint32_t tri_bounds_ob_uid = 0;
  const Mesh *tri_bounds_mesh = nullptr;
  int64_t tri_bounds_corner_tri_num = 0;
  std::string tri_bounds_uv_map_name;
  /** PBVH positions-changed counter and object transform at the last position upload. */
  int64_t positions_changed_count = -1;
  float4x4 object_to_world = float4x4::identity();
};

OverlayCache &overlay_cache_get()
{
  static OverlayCache cache;
  return cache;
}

/** The boundary shader; registered as a builtin so its lifetime is managed by the GPU module
 * (created on first use here, freed with the backend on exit). */
gpu::Shader *overlay_shader_get()
{
  return GPU_shader_get_builtin_shader(GPU_SHADER_IMAGE_SELECT_MASK);
}

/** Free every GPU resource of the cache. Must run with a current GPU context. */
void overlay_gpu_free(OverlayCache &cache)
{
  GPU_BATCH_DISCARD_SAFE(cache.tris_batch);
  GPU_VERTBUF_DISCARD_SAFE(cache.tris_verts);
  GPU_TEXTURE_FREE_SAFE(cache.mask_texture);
  cache.layer_tiles.clear();
  cache.layer_uploaded.clear();
  cache.tri_corners.clear();
  cache.tri_uvs.clear();
  cache.tri_layers.clear();
  cache.texture_size = int2(0);
  cache.positions_changed_count = -1;
}

/**
 * Sub-update every layer with the region that may have changed since the last upload: the union
 * of the previously uploaded bounds and the current selection bounds. That union covers every
 * possible change between two mask revisions — a change either adds a selected pixel, which
 * lands inside the new bounds, or clears one, which lands inside the previous ones — and the
 * mask values re-loaded there are authoritative, so no whole-texture clear is needed (a full 8K
 * UDIM clear per edit would dominate the rebuild cost).
 */
static void overlay_mask_texture_update(OverlayCache &cache, const Image &image)
{
  BLI_assert(cache.layer_uploaded.size() == cache.layer_tiles.size());
  for (const int layer : cache.layer_tiles.index_range()) {
    const int tile_number = cache.layer_tiles[layer];
    const ImBuf *mask = BKE_image_paint_selection_mask_lookup(&image, tile_number);
    if (mask == nullptr || mask->float_data() == nullptr) {
      /* The tile's mask was freed: its layer entry is stale until the layout scan of the current
       * rebuild removes it. */
      continue;
    }
    rcti new_bounds;
    int bounds_min[2];
    int bounds_max[2];
    if (BKE_image_paint_selection_mask_bounds(&image, tile_number, bounds_min, bounds_max)) {
      BLI_rcti_init(&new_bounds,
                    std::clamp(bounds_min[0], 0, mask->x - 1),
                    std::clamp(bounds_max[0], 0, mask->x),
                    std::clamp(bounds_min[1], 0, mask->y - 1),
                    std::clamp(bounds_max[1], 0, mask->y));
    }
    else {
      /* The selection left this tile entirely. */
      BLI_rcti_init(&new_bounds, 1, 0, 1, 0);
    }
    /* Re-upload the union of the last uploaded region (where selected texels may have been
     * cleared) and the new bounds. */
    rcti upload = new_bounds;
    if (BLI_rcti_is_valid(&cache.layer_uploaded[layer])) {
      if (BLI_rcti_is_valid(&upload)) {
        BLI_rcti_union(&upload, &cache.layer_uploaded[layer]);
      }
      else {
        /* The union with the empty sentinel would stretch the region to the tile origin. */
        upload = cache.layer_uploaded[layer];
      }
    }
    if (BLI_rcti_is_valid(&upload) && BLI_rcti_size_x(&upload) > 0 &&
        BLI_rcti_size_y(&upload) > 0)
    {
      const float *region = mask->float_data() + int64_t(upload.ymin) * mask->x + upload.xmin;
      GPU_texture_update_sub(cache.mask_texture,
                             GPU_DATA_FLOAT,
                             region,
                             upload.xmin,
                             upload.ymin,
                             layer,
                             BLI_rcti_size_x(&upload),
                             BLI_rcti_size_y(&upload),
                             1,
                             uint(mask->x));
    }
    /* Only the new bounds are stored: the union above already refreshed the previous region. */
    cache.layer_uploaded[layer] = new_bounds;
  }
}

/** Collect the UV triangles covering the selection's neighborhoods and record their static
 * (uv / layer) attributes. The position attribute is filled by #overlay_tri_positions_update. */
static void overlay_tri_topology_update(const Object &ob,
                                        const Image &image,
                                        OverlayCache &cache)
{
  const Mesh *mesh = id_cast<const Mesh *>(ob.data);
  if (mesh == nullptr || cache.mask_texture == nullptr) {
    return;
  }
  const bke::AttributeReader<float2> uv_attribute = mesh->attributes().lookup<float2>(
      cache.uv_map_name, bke::AttrDomain::Corner);
  if (!uv_attribute) {
    return;
  }
  const VArraySpan<float2> uv_map(*uv_attribute);
  const Span<int3> corner_tris = mesh->corner_tris();

  /* UV bounds of each texture layer's selection, dilated by a small margin so the boundary
   * shader always sees mask values on both sides of the edge. */
  struct TileBox {
    float2 min;
    float2 max;
    int layer;
  };
  Vector<TileBox> boxes;
  for (const int layer : cache.layer_tiles.index_range()) {
    const int tile_number = cache.layer_tiles[layer];
    int bounds_min[2];
    int bounds_max[2];
    if (!BKE_image_paint_selection_mask_bounds(&image, tile_number, bounds_min, bounds_max)) {
      continue;
    }
    const float2 uv_origin = BKE_image_get_tile_uv_origin(tile_number);
    const float2 size = float2(cache.texture_size);
    TileBox box;
    box.min = uv_origin + (float2(float(bounds_min[0]), float(bounds_min[1])) - 2.0f) / size;
    box.max = uv_origin + (float2(float(bounds_max[0]), float(bounds_max[1])) + 2.0f) / size;
    box.layer = layer;
    boxes.append(box);
  }

  /* The per-triangle UV bounds depend only on the mesh and its UV map, not on the selection:
   * rebuild them just when that identity changes, so the filter below stays a cheap scan across
   * rebuilds on heavy meshes. Limitation: an in-place UV edit that keeps the mesh pointer, the
   * triangle count and the UV map name would leave the cached bounds stale. The overlay only
   * draws in sculpt mode where UVs do not change, so the key above is sufficient there. */
  const uint32_t ob_uid = uint32_t(ob.id.session_uid);
  if (cache.tri_uv_bounds.size() != int64_t(corner_tris.size()) ||
      cache.tri_bounds_ob_uid != ob_uid || cache.tri_bounds_mesh != mesh ||
      cache.tri_bounds_corner_tri_num != corner_tris.size() ||
      cache.tri_bounds_uv_map_name != cache.uv_map_name)
  {
    cache.tri_uv_bounds.reinitialize(corner_tris.size());
    for (const int i : corner_tris.index_range()) {
      const float2 uv0 = uv_map[corner_tris[i].x];
      const float2 uv1 = uv_map[corner_tris[i].y];
      const float2 uv2 = uv_map[corner_tris[i].z];
      cache.tri_uv_bounds[i] = float4(math::min(uv0, math::min(uv1, uv2)),
                                      math::max(uv0, math::max(uv1, uv2)));
    }
    cache.tri_bounds_ob_uid = ob_uid;
    cache.tri_bounds_mesh = mesh;
    cache.tri_bounds_corner_tri_num = corner_tris.size();
    cache.tri_bounds_uv_map_name = cache.uv_map_name;
  }

  cache.tri_corners.clear();
  cache.tri_uvs.clear();
  cache.tri_layers.clear();
  for (const int i : corner_tris.index_range()) {
    if (cache.tri_corners.size() >= SELECT_VIEW3D_OUTLINE_MAX_TRIS) {
      break;
    }
    const int3 tri = corner_tris[i];
    const float4 &tri_bounds = cache.tri_uv_bounds[i];
    for (const TileBox &box : boxes) {
      if (tri_bounds.z < box.min.x || tri_bounds.x > box.max.x || tri_bounds.w < box.min.y ||
          tri_bounds.y > box.max.y)
      {
        continue;
      }
      /* Triangle UVs may straddle the tile border only for broken UVs; the shader clamps such
       * fragments to this triangle's own tile. */
      cache.tri_corners.append(tri);
      cache.tri_uvs.extend({uv_map[tri.x], uv_map[tri.y], uv_map[tri.z]});
      cache.tri_layers.extend({float(box.layer), float(box.layer), float(box.layer)});
      break;
    }
  }
}

/** (Re-)create the triangle batch and fill its static attributes. */
static void overlay_tri_batch_ensure(OverlayCache &cache)
{
  const int64_t vert_count = cache.tri_corners.size() * 3;
  if (cache.tris_verts != nullptr && int64_t(cache.tris_verts->vertex_len) == vert_count) {
    /* Same size: keep the batch and the vertex buffer, every attribute is re-filled by
     * #overlay_tri_positions_update. */
    return;
  }
  GPU_BATCH_DISCARD_SAFE(cache.tris_batch);
  GPU_VERTBUF_DISCARD_SAFE(cache.tris_verts);

  if (vert_count == 0) {
    return;
  }

  GPUVertFormat format;
  GPU_vertformat_clear(&format);
  GPU_vertformat_attr_add(&format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  GPU_vertformat_attr_add(&format, "uv", gpu::VertAttrType::SFLOAT_32_32);
  GPU_vertformat_attr_add(&format, "layer_in", gpu::VertAttrType::SFLOAT_32);
  cache.tris_verts = GPU_vertbuf_create_with_format_ex(format, GPU_USAGE_DYNAMIC);
  GPU_vertbuf_data_alloc(*cache.tris_verts, uint(vert_count));
  cache.tris_batch = GPU_batch_calloc();
  GPU_batch_init(cache.tris_batch, GPU_PRIM_TRIS, cache.tris_verts, nullptr);
}

/** Refresh the world-space triangle positions from the (deforming) evaluated mesh. */
static void overlay_tri_positions_update(const bContext *C,
                                         const Object &ob,
                                         OverlayCache &cache)
{
  if (cache.tris_verts == nullptr) {
    return;
  }
  const Mesh *mesh = id_cast<const Mesh *>(ob.data);
  const Span<int> corner_verts = mesh->corner_verts();
  const Span<float3> vert_positions = bke::pbvh::vert_positions_eval(
      *CTX_data_depsgraph_pointer(C), const_cast<Object &>(ob));
  const float4x4 object_to_world(ob.object_to_world());

  struct TriVert {
    float3 pos;
    float2 uv;
    float layer;
  };
  MutableSpan<TriVert> verts = cache.tris_verts->data<TriVert>();
  int64_t v = 0;
  for (const int64_t tri_i : cache.tri_corners.index_range()) {
    const int3 tri = cache.tri_corners[tri_i];
    for (const int c : IndexRange(3)) {
      verts[v].pos = math::transform_point(object_to_world,
                                           vert_positions[corner_verts[tri[c]]]);
      verts[v].uv = cache.tri_uvs[v];
      verts[v].layer = cache.tri_layers[v];
      v++;
    }
  }
  GPU_vertbuf_tag_dirty(cache.tris_verts);

  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob);
  cache.positions_changed_count = pbvh != nullptr ? pbvh->positions_changed_count() : -1;
  cache.object_to_world = object_to_world;
}

void overlay_rebuild(const bContext *C, const Object &ob, OverlayCache &cache)
{
  Scene *scene = CTX_data_scene(C);
  ImagePaintSelectionTarget primary = {};
  const bool has_target = image_paint_selection_view3d_primary_target_get(
      const_cast<bContext *>(C), const_cast<Object &>(ob), primary);
  Image *image = has_target ? primary.image : nullptr;

  const Mesh *mesh = id_cast<const Mesh *>(ob.data);
  const StringRef uv_name = BKE_paint_canvas_uvmap_name_get(
                                &scene->toolsettings->paint_mode, const_cast<Object *>(&ob))
                                .value_or(mesh != nullptr ? mesh->active_or_default_uv_map_name() :
                                                            "");
  cache.image = image;
  cache.revision = BKE_image_paint_selection_mask_revision_get(image);
  cache.ob_uid = uint32_t(ob.id.session_uid);
  cache.corner_tri_num = mesh ? mesh->corner_tris().size() : 0;
  cache.uv_map_name = std::string(uv_name);

  /* Mask texture: one layer per tile that carries a mask; tiles of a different resolution than
   * the first layer are skipped (UDIM tiles of one image share their resolution in practice). */
  int2 texture_size(0);
  Vector<int> layer_tiles;
  if (image != nullptr && uv_name.size() != 0) {
    for (const ImageTile *tile : ConstListBaseWrapper<ImageTile>(image->tiles)) {
      const ImBuf *mask = BKE_image_paint_selection_mask_lookup(image, tile->tile_number);
      if (mask == nullptr || mask->float_data() == nullptr) {
        continue;
      }
      if (texture_size.x == 0) {
        texture_size = int2(mask->x, mask->y);
      }
      if (mask->x != texture_size.x || mask->y != texture_size.y) {
        continue;
      }
      layer_tiles.append(tile->tile_number);
    }
  }

  /* Keep the texture across rebuilds whenever the tile layout and resolution are unchanged: a
   * recreated 8K UDIM array would turn every rebuild into an allocation spike. */
  const bool texture_reusable = cache.mask_texture != nullptr &&
                                cache.texture_size == texture_size &&
                                cache.layer_tiles == layer_tiles;
  if (!texture_reusable) {
    overlay_gpu_free(cache);
    if (layer_tiles.is_empty()) {
      return;
    }
    cache.layer_tiles = layer_tiles;
    cache.texture_size = texture_size;
    cache.mask_texture = GPU_texture_create_2d_array("paint_selection_mask",
                                                     texture_size.x,
                                                     texture_size.y,
                                                     cache.layer_tiles.size(),
                                                     1,
                                                     gpu::TextureFormat::SFLOAT_16,
                                                     GPU_TEXTURE_USAGE_SHADER_READ,
                                                     nullptr);
    if (cache.mask_texture == nullptr) {
      overlay_gpu_free(cache);
      return;
    }
    /* A fresh texture starts undefined: clear it once here. From then on the incremental
     * updates below keep it in sync, the layout only ever changes through this branch. */
    /* Zero every layer through plain uploads: #GPU_texture_clear needs an attachment-capable
     * texture and clears a single layer of an array on the GL backend, so a texture created with
     * shader-read usage only would keep undefined texels and the shader would draw boundary
     * lines wherever that garbage crosses the 0.5 threshold. */
    constexpr int ZERO_ROWS_PER_UPLOAD = 256;
    const Array<float> zero_rows(int64_t(texture_size.x) * ZERO_ROWS_PER_UPLOAD, 0.0f);
    for (const int layer : cache.layer_tiles.index_range()) {
      for (int y = 0; y < texture_size.y; y += ZERO_ROWS_PER_UPLOAD) {
        GPU_texture_update_sub(cache.mask_texture,
                               GPU_DATA_FLOAT,
                               zero_rows.data(),
                               0,
                               y,
                               layer,
                               texture_size.x,
                               std::min(ZERO_ROWS_PER_UPLOAD, texture_size.y - y),
                               1);
      }
    }
    GPU_texture_extend_mode(cache.mask_texture, GPU_SAMPLER_EXTEND_MODE_EXTEND);
    GPU_texture_filter_mode(cache.mask_texture, true);
    cache.layer_uploaded.reinitialize(cache.layer_tiles.size());
    for (rcti &uploaded : cache.layer_uploaded) {
      BLI_rcti_init(&uploaded, 1, 0, 1, 0);
    }
  }
  /* On the reused-texture path the previously uploaded regions stay as they are: the update
   * below covers exactly where the mask may have changed. */
  overlay_mask_texture_update(cache, *image);

  /* A selection drag changes the mask every step but rarely the set of triangles around it: when
   * the covered triangles (and their layers) are the same as before, the batch already holds the
   * right static attributes, and the world positions are refreshed by #overlay_draw only when the
   * mesh or object really moved. Skipping the full position upload per step is what keeps a drag
   * smooth on heavy meshes. */
  Vector<int3> previous_corners = std::move(cache.tri_corners);
  Vector<float> previous_layers = std::move(cache.tri_layers);
  cache.tri_corners = {};
  cache.tri_layers = {};
  overlay_tri_topology_update(ob, *image, cache);
  if (cache.tris_batch != nullptr && cache.tris_verts != nullptr &&
      previous_corners.size() == cache.tri_corners.size() &&
      previous_layers.size() == cache.tri_layers.size() &&
      std::equal(previous_corners.begin(), previous_corners.end(), cache.tri_corners.begin()) &&
      std::equal(previous_layers.begin(), previous_layers.end(), cache.tri_layers.begin()))
  {
    return;
  }
  overlay_tri_batch_ensure(cache);
  if (cache.tris_batch == nullptr) {
    /* Nothing covers the selection (e.g. an empty boundary): keep the texture, nothing to draw.
     */
    return;
  }
  overlay_tri_positions_update(C, ob, cache);
}

void overlay_draw(const bContext *C, const ARegion &region)
{
  const Object *ob = CTX_data_active_object(C);
  const Scene *scene = CTX_data_scene(C);
  if (ob == nullptr || scene == nullptr || scene->toolsettings == nullptr ||
      CTX_data_mode_enum(C) != CTX_MODE_SCULPT)
  {
    return;
  }
  OverlayCache &cache = overlay_cache_get();

  ImagePaintSelectionTarget primary = {};
  const bool has_target = image_paint_selection_view3d_primary_target_get(
      const_cast<bContext *>(C), *const_cast<Object *>(ob), primary);
  Image *image = has_target ? primary.image : nullptr;
  const bool has_selection = image != nullptr && BKE_image_paint_selection_mask_has_any(image);
  if (!has_selection) {
    if (cache.image != nullptr) {
      /* The last mask was removed: drop the cached geometry. */
      overlay_gpu_free(cache);
      cache = OverlayCache{};
    }
    return;
  }

  const uint64_t revision = BKE_image_paint_selection_mask_revision_get(image);
  const Mesh *mesh = id_cast<const Mesh *>(ob->data);
  const StringRef uv_name = BKE_paint_canvas_uvmap_name_get(
                                &scene->toolsettings->paint_mode, const_cast<Object *>(ob))
                                .value_or(mesh != nullptr ? mesh->active_or_default_uv_map_name() :
                                                            "");
  const bool stale = cache.image != image || cache.revision != revision ||
                     cache.ob_uid != uint32_t(ob->id.session_uid) ||
                     cache.corner_tri_num != (mesh ? int64_t(mesh->corner_tris().size()) : 0) ||
                     uv_name != cache.uv_map_name;
  if (stale) {
    /* Rebuild right away: the incremental path (bounds sub-updates into a reused texture, a
     * plain scan over cached triangle bounds) is cheap enough to run on every revision change,
     * so no redraw-gating is needed — region redraw tags would be ignored while drawing anyway. */
    overlay_rebuild(C, *ob, cache);
  }

  gpu::Shader *shader = overlay_shader_get();
  if (shader == nullptr || cache.tris_batch == nullptr || cache.mask_texture == nullptr) {
    return;
  }

  /* Follow the (deforming) evaluated mesh: re-upload the triangle positions when the PBVH
   * reports changed positions or the object transform moved; camera-only redraws (orbit, pan,
   * zoom) reuse the batch with no per-vertex CPU work. */
  const float4x4 object_to_world(ob->object_to_world());
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(*ob);
  const int64_t positions_count = pbvh != nullptr ? pbvh->positions_changed_count() : -1;
  if (positions_count != cache.positions_changed_count ||
      !(cache.object_to_world == object_to_world))
  {
    overlay_tri_positions_update(C, *ob, cache);
  }

  /* Save GPU state to restore after drawing. */
  const GPUBlend prev_blend = GPU_blend_get();
  const GPUDepthTest prev_depth_test = GPU_depth_test_get();

  GPU_blend(GPU_BLEND_ALPHA);
  /* The overlay is drawn on the surface itself: depth testing culls the parts hidden behind the
   * mesh with no per-vertex lift. */
  GPU_depth_test(GPU_DEPTH_LESS_EQUAL);
  /* The triangles lie exactly on the mesh surface, so without a depth offset they z-fight with it
   * and the dashed outline breaks up or vanishes; the offset pulls them just in front while the
   * depth test still hides the part behind the mesh. */
  const RegionView3D *rv3d = static_cast<const RegionView3D *>(region.regiondata);
  if (rv3d != nullptr) {
    ED_view3d_polygon_offset(rv3d, 1.0f);
  }
  /* POST_VIEW: the ambient matrix is the view-projection, so the batch's world-space vertices
   * map correctly without extra transforms. */
  GPU_batch_set_shader(cache.tris_batch, shader);
  GPU_texture_bind(cache.mask_texture, 0);
  GPU_batch_uniform_4f(cache.tris_batch, "line_color", 0.4f, 0.4f, 0.4f, 1.0f);
  GPU_batch_uniform_4f(cache.tris_batch, "line_color2", 1.0f, 1.0f, 1.0f, 1.0f);
  GPU_batch_uniform_1f(cache.tris_batch, "dash_width", SELECT_VIEW3D_DASH_WIDTH);
  GPU_batch_uniform_1f(cache.tris_batch, "fill_alpha", 0.0f);
  GPU_batch_draw(cache.tris_batch);
  GPU_texture_unbind(cache.mask_texture);

  /* Restore previous GPU state. */
  if (rv3d != nullptr) {
    ED_view3d_polygon_offset(rv3d, 0.0f);
  }
  GPU_depth_test(prev_depth_test);
  GPU_blend(prev_blend);
}

}  // namespace image_select_v3d

void image_select_view3d_region_draw(const bContext *C, ARegion *region, void * /*arg*/)
{
  if (C == nullptr || region == nullptr) {
    return;
  }
  image_select_v3d::overlay_draw(C, *region);
}

void image_select_view3d_overlay_free()
{
  image_select_v3d::OverlayCache &cache = image_select_v3d::overlay_cache_get();
  if (cache.mask_texture == nullptr && cache.tris_batch == nullptr) {
    /* Nothing allocated (no overlay drawn yet, e.g. in background mode): never touch the GPU
     * context machinery just to clear plain data. */
    cache = image_select_v3d::OverlayCache{};
    return;
  }
  /* Called from #view3d_free (file load, window close, exit): no GPU context is guaranteed to be
   * current here, and the discard calls below would assert or crash in the driver. Make the
   * shared viewport context current for the duration, unless the caller already holds it (a
   * nested enable would deadlock on its non-recursive mutex). */
  const bool context_enabled = DRW_gpu_context_is_enabled();
  if (!context_enabled) {
    DRW_gpu_context_enable();
  }
  image_select_v3d::overlay_gpu_free(cache);
  cache = image_select_v3d::OverlayCache{};
  if (!context_enabled) {
    /* Restores the window's drawable (the `_ex` variant without restore is only for closing
     * Blender): view3d_free also runs on file load and area changes, with windows still drawing. */
    DRW_gpu_context_disable();
  }
}

/** \} */

}  // namespace blender::ed::sculpt_paint
