/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The surface-decal core shared by every Transform Selection that carries its content over the mesh
 * instead of the UV affine: the 3D Viewport tool (#ImageSelectTransform3DState) and the Image
 * Editor's Transform / Move sessions (see `paint_image_select_preview_view3d.cc`).
 *
 * A decal is a stamp per canvas target (the lifted fragments re-sampled into a frame on the
 * surface, the soft selection coverage as alpha, Normal vectors rewritten into the frame basis) and
 * a surface patch: the mesh triangles around the frame center unfolded into the plane of the
 * center's triangle (#surface_unfold_build). The frame is a center UV, a roll relative to the
 * sampled tangent and scale factors on the frame axes, so the content crosses seams, island borders
 * and cube edges unbroken. Nothing of it lives in the material: the preview is a draw overlay, and
 * the commit rasterizes every patch triangle's UV footprint into the canvas targets.
 *
 * This file only holds what does not depend on who authors the frame; the sessions own their
 * gestures and call in here with plain data.
 */

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "MEM_guardedalloc.h"

#include "BLI_array.hh"
#include "BLI_index_range.hh"
#include "BLI_listbase_wrapper.hh"
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

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_view3d_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_mesh.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_target.hh"
#include "BKE_paint_material_composite.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "ED_image.hh"
#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_undo.hh"
#include "ED_view3d.hh"

#include "GPU_immediate.hh"
#include "GPU_shader_builtin.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "GEO_reverse_uv_sampler.hh"
#include "paint_image_select_fragment.hh"
#include "paint_image_select_intern.hh"
#include "paint_image_select_surface_decal.hh"
#include "paint_image_select_surface_unfold.hh"
#include "paint_image_select_view3d.hh"

namespace blender::ed::sculpt_paint {

/* -------------------------------------------------------------------- */
/** \name Helpers
 * \{ */

/** The patch's triangle ceiling: a Move rebuilds the patch on every drag step, so the growth is
 * bounded whatever the mesh density. */
constexpr int DECAL_PATCH_MAX_TRIS = 20000;

/** The patch is grown to this multiple of the stamp's half diagonal, so a rotation and a modest
 * scale reuse it without a rebuild. */
constexpr float DECAL_PATCH_RADIUS_MARGIN = 1.5f;

/**
 * Renormalize an encoded tangent-space normal after the bilinear resample: interpolation and the
 * lift rotation both keep the vector direction honest but the length drifts, and a stamp read
 * later must decode unit vectors. Alpha is untouched.
 */
static void surface_decal_stamp_normal_renormalize(float4 &color)
{
  float3 n(color[0] * 2.0f - 1.0f, color[1] * 2.0f - 1.0f, color[2] * 2.0f - 1.0f);
  if (math::length_squared(n) > 1e-12f) {
    n = math::normalize(n);
    color[0] = n.x * 0.5f + 0.5f;
    color[1] = n.y * 0.5f + 0.5f;
    color[2] = n.z * 0.5f + 0.5f;
  }
}

/** The stamp's extents on the surface in object units, signed (a negative scale mirrors the stamp)
 * and never zero, so the plane -> stamp map stays invertible. */
static float2 surface_decal_stamp_size(const SurfaceDecalSession &decal)
{
  float2 size(decal.base_size.x * decal.scale.x, decal.base_size.y * decal.scale.y);
  for (const int i : IndexRange(2)) {
    if (std::abs(size[i]) < 1e-6f) {
      size[i] = size[i] < 0.0f ? -1e-6f : 1e-6f;
    }
  }
  return size;
}

/** Plane (unfold coordinates, origin at the frame center) to stamp UV: the frame's roll and
 * extents, the stamp centered on the frame center. */
static float2 surface_decal_stamp_uv(const SurfaceDecalSession &decal,
                                         const float2 &size,
                                         const float2 &plane)
{
  const float2 axis_x(std::cos(decal.roll), std::sin(decal.roll));
  const float2 axis_y(-axis_x.y, axis_x.x);
  const float2 centered = plane - decal.plane_offset;
  return float2(0.5f + math::dot(centered, axis_x) / size.x,
                0.5f + math::dot(centered, axis_y) / size.y);
}

/**
 * Bilinear sample of a stamp at \a uv (0..1 over the whole stamp). The color is weighted by alpha
 * so a transparent texel's rgb never bleeds into its opaque neighbors; a tap beyond the stamp's
 * border counts as transparent, which fades the rim exactly as the UV write-back's footprint does.
 * The result is straight alpha.
 */
static float4 surface_decal_stamp_sample(const ImBuf &stamp, const float2 &uv)
{
  const float *data = stamp.float_data();
  const int width = stamp.x;
  const int height = stamp.y;
  const float fx = uv.x * float(width) - 0.5f;
  const float fy = uv.y * float(height) - 0.5f;
  const int x0 = int(std::floor(fx));
  const int y0 = int(std::floor(fy));
  const float tx = fx - float(x0);
  const float ty = fy - float(y0);

  float3 rgb(0.0f);
  float alpha = 0.0f;
  for (const int j : IndexRange(2)) {
    for (const int i : IndexRange(2)) {
      const int x = x0 + i;
      const int y = y0 + j;
      if (x < 0 || y < 0 || x >= width || y >= height) {
        continue;
      }
      const float weight = (i == 0 ? 1.0f - tx : tx) * (j == 0 ? 1.0f - ty : ty);
      const float *px = data + (int64_t(y) * width + x) * 4;
      const float tap_alpha = px[3] * weight;
      rgb += float3(px[0], px[1], px[2]) * tap_alpha;
      alpha += tap_alpha;
    }
  }
  if (alpha <= 1e-8f) {
    return float4(0.0f);
  }
  rgb /= alpha;
  return float4(rgb.x, rgb.y, rgb.z, alpha);
}

void surface_decal_release(SurfaceDecalSession &decal)
{
  for (ImBuf *&stamp : decal.stamps) {
    if (stamp != nullptr) {
      IMB_freeImBuf(stamp);
      stamp = nullptr;
    }
  }
  decal.stamps.clear();
  decal.patch = SurfaceUnfoldPatch();
  decal.patch_seed_tri = -1;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Stamp build
 * \{ */

/** The stamp's side cap: the stamp never exceeds it, whatever the source canvas carries. */
constexpr int DECAL_STAMP_MAX_SIDE = 4096;

/**
 * The stamp of one target: the lifted fragments re-sampled from their UV space into the cage's own
 * frame (stamp texel (i, j) reads the source texel the cage coordinate maps to), the selection
 * coverage (feather or binary, exactly what the UV write-back weights with) times the texel's own
 * alpha as the stamp's straight alpha. The values stay in the source canvas's own encoding (a byte
 * canvas's bytes as 0..1), because the stamp is written back to a canvas of that same encoding;
 * the session owns the buffer (it is not an Image, so it never reaches the file or the Main).
 * A Normal target's vectors are rewritten from the source UV basis into the frame basis
 * (#image_select_normal_rotation over the source-UV-to-cage map) and renormalized.
 */
ImBuf *surface_decal_stamp_build(const SurfaceDecalCage &cage_def,
                                 const ImagePaintSelectionTarget &target,
                                 const Span<SelectionTileFragment> fragments)
{
  if (target.image == nullptr || fragments.is_empty()) {
    return nullptr;
  }
  bool jacobian_ok = false;
  const float2x2 jacobian_inv = math::invert(cage_def.plane_uv_jacobian, jacobian_ok);
  if (!jacobian_ok) {
    return nullptr;
  }

  /* The stamp covers the cage rect one source texel per stamp texel (up to the cap): the cage is
   * the content's bounds, so nothing is cut and no stamp pixel is wasted. */
  const float2 cage_size_px = cage_def.plane_max - cage_def.plane_min;
  const int stamp_w = std::clamp(int(cage_size_px.x + 0.5f), 1, DECAL_STAMP_MAX_SIDE);
  const int stamp_h = std::clamp(int(cage_size_px.y + 0.5f), 1, DECAL_STAMP_MAX_SIDE);

  const float2x2 cage_rotation = image_select_v3d::plane_rotation(cage_def.frame_angle);
  /* Source UV -> cage coordinates: the forward map whose rotation part rewrites the Normal
   * vectors, exactly as the UV write-back rotates them through #image_select_normal_rotation
   * (the scales cancel in its polar decomposition). */
  const float2x2 uv_to_cage = math::transpose(cage_rotation) * jacobian_inv;
  const float2x2 lift_rotation = image_select_normal_rotation(uv_to_cage);
  const bool rotate_normals = !image_select_normal_rotation_is_identity(lift_rotation);
  const bool is_normal = target.channel == PAINT_MATERIAL_CHANNEL_NORMAL;

  ImBuf *stamp = IMB_allocImBuf(uint(stamp_w), uint(stamp_h), ImBufFlags::FloatData);
  if (stamp == nullptr || stamp->float_data_for_write() == nullptr) {
    if (stamp != nullptr) {
      IMB_freeImBuf(stamp);
    }
    return nullptr;
  }
  stamp->channels = 4;
  float *out = stamp->float_data_for_write();

  threading::parallel_for(IndexRange(stamp_h), 1, [&](const IndexRange rows) {
    for (const int py : rows) {
      for (int px = 0; px < stamp_w; px++) {
        /* The cage coordinate of this texel's center, back to source UV through the cage. */
        const float2 cage = cage_def.plane_min +
                            float2((float(px) + 0.5f) / float(stamp_w) * cage_size_px.x,
                                   (float(py) + 0.5f) / float(stamp_h) * cage_size_px.y);
        const float2 uv = cage_def.anchor_uv + cage_def.plane_uv_jacobian * (cage_rotation * cage);
        float4 color(0.0f, 0.0f, 0.0f, 0.0f);
        /* The last fragment whose footprint holds this texel wins, the order the UV write-back
         * composites the fragments in. */
        for (const SelectionTileFragment &frag : fragments) {
          if (frag.pixels.fragment_ibuf == nullptr) {
            continue;
          }
          const float2 tile_uv = image_select_udim_tile_uv_origin(frag.geom.tile_number);
          if (uv.x < tile_uv.x || uv.x >= tile_uv.x + 1.0f || uv.y < tile_uv.y ||
              uv.y >= tile_uv.y + 1.0f)
          {
            continue;
          }
          const float2 frag_origin_uv = tile_uv + float2(frag.geom.origin_px) /
                                                     float2(frag.geom.tile_size_px);
          const float2 frag_size_uv = float2(frag.geom.size_px) / float2(frag.geom.tile_size_px);
          const float2 f = (uv - frag_origin_uv) / frag_size_uv * float2(frag.geom.size_px);
          if (f.x < 0.0f || f.y < 0.0f || f.x >= float(frag.geom.size_px.x) ||
              f.y >= float(frag.geom.size_px.y))
          {
            continue;
          }
          const float *fmask = frag.pixels.fragment_mask_ibuf ?
                                   frag.pixels.fragment_mask_ibuf->float_data() :
                                   nullptr;
          const ImBuf *blend_mask = frag.edge_policy.use_outward_feather ?
                                        frag.preview.fragment_blend_mask_ibuf :
                                        nullptr;
          float m = 1.0f;
          if (blend_mask != nullptr) {
            m = image_select_sample_mask_bilinear(blend_mask, f.x, f.y);
          }
          else if (fmask != nullptr) {
            if (fmask[int(f.y) * frag.geom.size_px.x + int(f.x)] <= SELECTION_MASK_THRESHOLD) {
              continue;
            }
          }
          if (m <= 0.001f) {
            continue;
          }
          float4 tap(0.0f, 0.0f, 0.0f, 1.0f);
          if (!image_select_fragment_sample_bilinear(frag, f, 4, tap)) {
            continue;
          }
          if (rotate_normals && is_normal) {
            image_select_normal_color_rotate(tap, lift_rotation);
            surface_decal_stamp_normal_renormalize(tap);
          }
          color = tap;
          /* The stamp's straight alpha is the write-back's coverage: the selection weight times
           * the texel's own alpha. */
          color[3] = tap[3] * m;
        }
        float *dst_px = out + (int64_t(py) * stamp_w + px) * 4;
        dst_px[0] = color[0];
        dst_px[1] = color[1];
        dst_px[2] = color[2];
        dst_px[3] = color[3];
      }
    }
  });
  return stamp;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Surface frame
 * \{ */

bool surface_decal_frame_at_uv(const Object &ob,
                               const Depsgraph &depsgraph,
                               const ImageSelectView3DSurfaceMap &surface_map,
                               const float2 &uv,
                               const float3 &prev_tangent,
                               SurfaceDecalFrame &r_frame)
{
  if (ob.type != OB_MESH || ob.data == nullptr || surface_map.sampler == nullptr) {
    return false;
  }
  const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
  const Span<int> corner_verts = mesh.corner_verts();
  /* The evaluated positions, the same source the patch and the cage sampling use. */
  const Span<float3> positions = bke::pbvh::vert_positions_eval(depsgraph, ob);
  float3 co, no;
  int tri_index = -1;
  if (!surface_map.sample(corner_verts, positions, uv, co, no, &tri_index)) {
    return false;
  }
  if (tri_index < 0 || tri_index >= surface_map.corner_tris.size()) {
    return false;
  }
  const shape::SurfaceAnchorFrame basis = shape::surface_anchor_frame(no, prev_tangent);

  SurfaceDecalFrame frame;
  frame.co = co;
  frame.normal = basis.normal;
  frame.tangent = basis.tangent;
  frame.bitangent = basis.bitangent;
  frame.tri = tri_index;

  /* The triangle's plane (tangent, bitangent; object units) -> UV Jacobian, from its corners. */
  const int3 tri = surface_map.corner_tris[tri_index];
  float2 plane[3];
  float2 tri_uv[3];
  for (const int i : IndexRange(3)) {
    const int corner = tri[i];
    if (corner < 0 || corner >= corner_verts.size() || corner >= surface_map.uv_map.size() ||
        corner_verts[corner] >= positions.size())
    {
      return false;
    }
    const float3 d = positions[corner_verts[corner]] - co;
    plane[i] = float2(math::dot(d, basis.tangent), math::dot(d, basis.bitangent));
    tri_uv[i] = surface_map.uv_map[corner];
  }
  bool ok = false;
  const float2x2 plane_edges(plane[1] - plane[0], plane[2] - plane[0]);
  const float2x2 plane_edges_inv = math::invert(plane_edges, ok);
  if (ok) {
    const float2x2 uv_edges(tri_uv[1] - tri_uv[0], tri_uv[2] - tri_uv[0]);
    frame.plane_uv_jacobian = uv_edges * plane_edges_inv;
    frame.jacobian_valid = true;
  }
  r_frame = frame;
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Surface patch
 * \{ */

/**
 * Make the unfolded patch fit the current frame: grown around the frame center's triangle to
 * #DECAL_PATCH_RADIUS_MARGIN times the stamp's half diagonal, rebuilt only when the center (seed
 * triangle, point or tangent) changed or the stamp outgrew the patch (or shrank far below it, so a
 * Move over a dense mesh does not keep paying for a patch the stamp left behind). A rotation or a
 * scale only re-maps the same patch onto the stamp.
 *
 * \return false when no patch can be built (no surface under the center); the previous patch, if
 * any, is kept.
 */
bool surface_decal_patch_ensure(const bContext *C,
                                SurfaceDecalSession &decal,
                                Object *ob,
                                const ImageSelectView3DSurfaceMap &surface_map,
                                const int seed_tri,
                                const float3 &seed_co,
                                const float3 &seed_tangent)
{
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  if (ob == nullptr || depsgraph == nullptr || ob->type != OB_MESH || ob->data == nullptr ||
      surface_map.sampler == nullptr)
  {
    return false;
  }
  const Span<int3> corner_tris = surface_map.corner_tris.as_span();
  if (seed_tri < 0 || seed_tri >= corner_tris.size()) {
    return false;
  }

  const float2 size = surface_decal_stamp_size(decal);
  /* The stamp center may sit off the seed (Image Editor, center in a UV gap): the patch must reach
   * the whole stamp around that center. */
  const float needed = 0.5f * math::length(float2(std::abs(size.x), std::abs(size.y))) +
                       math::length(decal.plane_offset);
  const bool reusable = !decal.patch.tris.is_empty() && decal.patch_seed_tri == seed_tri &&
                        math::distance_squared(decal.patch_seed_co, seed_co) < 1e-14f &&
                        math::distance_squared(decal.patch_tangent, seed_tangent) < 1e-10f &&
                        needed <= decal.patch.radius &&
                        decal.patch.radius <= needed * DECAL_PATCH_RADIUS_MARGIN * 4.0f;
  if (reusable) {
    return true;
  }

  const Mesh &mesh = *id_cast<const Mesh *>(ob->data);
  const Span<int> corner_verts = mesh.corner_verts();
  /* The evaluated positions, the same source the anchor and the cage sampling use. */
  const Span<float3> positions = bke::pbvh::vert_positions_eval(*depsgraph, *ob);
  if (decal.adjacency.neighbors.size() != corner_tris.size()) {
    decal.adjacency = surface_unfold_adjacency_build(corner_tris, corner_verts);
  }
  SurfaceUnfoldPatch patch;
  if (!surface_unfold_build(corner_tris,
                            corner_verts,
                            positions,
                            decal.adjacency,
                            seed_tri,
                            seed_co,
                            seed_tangent,
                            std::max(needed * DECAL_PATCH_RADIUS_MARGIN, 1e-5f),
                            DECAL_PATCH_MAX_TRIS,
                            patch))
  {
    return false;
  }
  decal.patch = std::move(patch);
  decal.patch_seed_tri = seed_tri;
  decal.patch_seed_co = seed_co;
  decal.patch_tangent = seed_tangent;
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Preview (POST_VIEW overlay)
 * \{ */

void surface_decal_draw_preview(const ARegion *region,
                                const SurfaceDecalSession &decal,
                                const Object &ob,
                                const ImageSelectView3DSurfaceMap &surface_map,
                                const Span<ImagePaintSelectionTarget> targets,
                                Vector<Vector<gpu::Texture *>> &preview_textures,
                                gpu::Texture *pbr_default_textures[PBR_PREVIEW_CHANNEL_NUM],
                                const int preview_target)
{
  const Mesh *mesh = id_cast<const Mesh *>(ob.data);
  if (mesh == nullptr || decal.patch.tris.is_empty() || decal.stamps.is_empty()) {
    return;
  }
  const Span<int> corner_verts = mesh->corner_verts();
  const Span<float3> vert_normals = mesh->vert_normals();
  const Span<int3> corner_tris = surface_map.corner_tris.as_span();
  const float4x4 object_to_world(ob.object_to_world());
  const float2 size = surface_decal_stamp_size(decal);

  /* The patch triangles at their real surface positions (the flattening only serves the stamp
   * coordinates), so the preview lies on the surface exactly where the commit will write it. */
  Vector<float3> positions;
  Vector<float3> normals;
  Vector<float2> stamp_uvs;
  const int64_t vertex_num = int64_t(decal.patch.tris.size()) * 3;
  positions.reserve(vertex_num);
  normals.reserve(vertex_num);
  stamp_uvs.reserve(vertex_num);
  for (const SurfaceUnfoldTri &tri : decal.patch.tris) {
    if (tri.tri_index < 0 || tri.tri_index >= corner_tris.size()) {
      continue;
    }
    for (const int c : IndexRange(3)) {
      const int corner = corner_tris[tri.tri_index][c];
      const int vert = (corner >= 0 && corner < corner_verts.size()) ? corner_verts[corner] : -1;
      positions.append(math::transform_point(object_to_world, tri.co[c]));
      normals.append(vert >= 0 && vert < vert_normals.size() ? vert_normals[vert] :
                                                               float3(0.0f, 0.0f, 1.0f));
      stamp_uvs.append(surface_decal_stamp_uv(decal, size, tri.plane[c]));
    }
  }
  if (positions.is_empty()) {
    return;
  }
  /* The normals are gathered in object space: one #math::transform_normals pass applies the
   * inverse-transpose that keeps them pointing right off a non-uniformly scaled object. */
  math::transform_normals(float3x3(object_to_world), normals);

  if (preview_textures.size() != targets.size()) {
    preview_textures.resize(targets.size());
  }
  /* The stamps replace the fragments' display buffers in the session's texture table: one GPU copy
   * per target in slot 0 of its row, freed with the table. */
  const auto stamp_texture = [&](const int target_i) -> gpu::Texture * {
    if (target_i < 0 || target_i >= decal.stamps.size() || target_i >= preview_textures.size() ||
        decal.stamps[target_i] == nullptr)
    {
      return nullptr;
    }
    Vector<gpu::Texture *> &row = preview_textures[target_i];
    if (row.is_empty()) {
      row.append(nullptr);
    }
    if (row[0] == nullptr) {
      row[0] = IMB_create_gpu_texture("Transform3DStamp", decal.stamps[target_i], true, false, false);
      if (row[0] != nullptr) {
        /* Linear, not nearest: the stamp is usually minified heavily on the surface. */
        GPU_texture_filter_mode(row[0], true);
      }
    }
    return row[0];
  };

  /* The preview triangles lie exactly on the mesh surface: without a depth offset the shared
   * pixels z-fight and the texture shows through only in patches. */
  const RegionView3D *rv3d = (region != nullptr) ?
                                 static_cast<const RegionView3D *>(region->regiondata) :
                                 nullptr;
  if (rv3d != nullptr) {
    ED_view3d_polygon_offset(rv3d, 1.0f);
  }

  const int target_i = preview_target;
  GPU_blend(GPU_BLEND_ALPHA);
  GPU_depth_test(GPU_DEPTH_LESS_EQUAL);
  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  if (target_i < 0) {
    /* The PBR composite: every slot the session's targets carry shades together, a neutral
     * stand-in for the rest. The vertices carry the stamp UV, so the per-channel affines are the
     * identity (every stamp covers the same frame). */
    const uint src_uv = GPU_vertformat_attr_add(format, "src_uv", gpu::VertAttrType::SFLOAT_32_32);
    const uint nor = GPU_vertformat_attr_add(format, "nor", gpu::VertAttrType::SFLOAT_32_32_32);
    gpu::Texture *channel_textures[PBR_PREVIEW_CHANNEL_NUM] = {nullptr};
    float2 channel_scale[PBR_PREVIEW_CHANNEL_NUM];
    float2 channel_offset[PBR_PREVIEW_CHANNEL_NUM];
    for (const int slot : IndexRange(PBR_PREVIEW_CHANNEL_NUM)) {
      for (const int i : targets.index_range()) {
        if (image_select_pbr_channel_slot(targets[i].channel) == slot) {
          channel_textures[slot] = stamp_texture(i);
          break;
        }
      }
      if (channel_textures[slot] == nullptr) {
        if (pbr_default_textures[slot] == nullptr) {
          pbr_default_textures[slot] = image_select_view3d_pbr_default_texture_create(
              "SelectionPBRDefault", slot);
        }
        channel_textures[slot] = pbr_default_textures[slot];
      }
      channel_scale[slot] = float2(1.0f);
      channel_offset[slot] = float2(0.0f);
    }
    float3 view_direction;
    float3 light_direction;
    image_select_view3d_pbr_light_get(region, view_direction, light_direction);
    image_select_view3d_pbr_program_bind(
        channel_textures, channel_scale, channel_offset, light_direction, view_direction);
    immBegin(GPU_PRIM_TRIS, uint(positions.size()));
    for (const int64_t i : positions.index_range()) {
      immAttr3f(nor, normals[i].x, normals[i].y, normals[i].z);
      immAttr2f(src_uv, stamp_uvs[i].x, stamp_uvs[i].y);
      immVertex3f(pos, positions[i].x, positions[i].y, positions[i].z);
    }
    immEnd();
    immUnbindProgram();
  }
  else if (gpu::Texture *texture = stamp_texture(target_i)) {
    const uint texco = GPU_vertformat_attr_add(format, "texCoord", gpu::VertAttrType::SFLOAT_32_32);
    immBindBuiltinProgram(GPU_SHADER_3D_IMAGE_COLOR);
    immUniformColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    /* Clamp to the (transparent) border: the patch reaches past the stamp's rect, and those
     * triangles must stay invisible there. */
    immBindTextureSampler("image",
                          texture,
                          {GPU_SAMPLER_FILTERING_LINEAR,
                           GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER,
                           GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER});
    immBegin(GPU_PRIM_TRIS, uint(positions.size()));
    for (const int64_t i : positions.index_range()) {
      immAttr2f(texco, stamp_uvs[i].x, stamp_uvs[i].y);
      immVertex3f(pos, positions[i].x, positions[i].y, positions[i].z);
    }
    immEnd();
    immUnbindProgram();
  }
  GPU_blend(GPU_BLEND_NONE);
  if (rv3d != nullptr) {
    ED_view3d_polygon_offset(rv3d, 0.0f);
  }
}

void surface_decal_patch_uv_vertices(const SurfaceDecalSession &decal,
                                     const ImageSelectView3DSurfaceMap &surface_map,
                                     Vector<float2> &r_uv,
                                     Vector<float2> &r_stamp_uv)
{
  r_uv.clear();
  r_stamp_uv.clear();
  const Span<int3> corner_tris = surface_map.corner_tris.as_span();
  const float2 size = surface_decal_stamp_size(decal);
  for (const SurfaceUnfoldTri &tri : decal.patch.tris) {
    if (tri.tri_index < 0 || tri.tri_index >= corner_tris.size()) {
      continue;
    }
    /* The same corner UVs and stamp coordinates the bake rasterizes. */
    for (const int c : IndexRange(3)) {
      const int corner = corner_tris[tri.tri_index][c];
      r_uv.append((corner >= 0 && corner < surface_map.uv_map.size()) ? surface_map.uv_map[corner] :
                                                                         float2(0.0f));
      r_stamp_uv.append(surface_decal_stamp_uv(decal, size, tri.plane[c]));
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Commit (bake)
 * \{ */

/**
 * Bake the stamp of one target into its canvas, tile by tile: every patch triangle's UV footprint
 * is rasterized, and the canvas pixel's barycentric position inside it gives the stamp plane
 * position, hence the stamp texel, which composites straight-alpha over the canvas. A pixel is
 * written once per pass (the triangles share edges, and a double write would apply the coverage
 * twice). Tangent-space normals are turned per triangle by the polar rotation of the stamp-to-UV
 * map of that triangle, the rule the UV write-back applies, so the relief keeps its direction
 * across an edge and a mirrored island does not invert it.
 * Runs inside the shared image undo step: extra tiles register their "before" state there.
 */
static void surface_decal_bake_target(bContext *C,
                                      const SurfaceDecalSession &decal,
                                      const ImageSelectView3DSurfaceMap &surface_map,
                                      const ImagePaintSelectionTarget &target,
                                      Image *ima,
                                      const ImageUser &base_iuser,
                                      const int stamp_target_i,
                                      const Set<int> &fragment_tiles)
{
  const ImBuf *stamp = decal.stamps[stamp_target_i];
  if (stamp == nullptr || decal.patch.tris.is_empty() || ima == nullptr) {
    return;
  }
  const Span<int3> corner_tris = surface_map.corner_tris.as_span();
  const bool is_normal = target.channel == PAINT_MATERIAL_CHANNEL_NORMAL;
  const float2 size = surface_decal_stamp_size(decal);
  const float2x2 roll_rotation = image_select_v3d::plane_rotation(decal.roll);
  /* Stamp axes (texture u, v) -> plane: the frame's roll and its extents. */
  const float2x2 stamp_to_plane = roll_rotation * float2x2({size.x, 0.0f}, {0.0f, size.y});

  Set<int> snapshotted = fragment_tiles;
  ImageUndoStep *us_open = image_select_undo_session_step_get();

  for (ImageTile *tile : ListBaseWrapper<ImageTile>(ima->tiles)) {
    ImageUser tile_iuser = base_iuser;
    tile_iuser.tile = tile->tile_number;
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(ima, &tile_iuser, &lock);
    if (ibuf == nullptr || (!ibuf->float_buffer.data && !ibuf->byte_buffer.data)) {
      if (ibuf != nullptr) {
        BKE_image_release_ibuf(ima, ibuf, lock);
      }
      continue;
    }
    if (us_open != nullptr && !snapshotted.contains(tile->tile_number)) {
      ED_image_undo_push(ima, ibuf, &tile_iuser, us_open);
      ED_image_undo_capture_selection_mask(ima, tile->tile_number);
      snapshotted.add(tile->tile_number);
    }

    const int width = ibuf->x;
    const int height = ibuf->y;
    const bool is_float = ibuf->float_data() != nullptr;
    const int channels = is_float ? (ibuf->channels ? ibuf->channels : 4) : 4;
    const float2 tile_uv = image_select_udim_tile_uv_origin(tile->tile_number);
    const float2 tile_size_px = float2(float(width), float(height));
    float *dst_float = is_float ? ibuf->float_data_for_write() : nullptr;
    uint8_t *dst_byte = is_float ? nullptr : ibuf->byte_data_for_write();

    /* Per tile: the pixels already written in this pass. */
    Array<uint8_t> written;
    int dirty_x0 = width;
    int dirty_y0 = height;
    int dirty_x1 = 0;
    int dirty_y1 = 0;

    /* Two passes: the exact coverage first, then a gutter around every triangle. The preview draws
     * the patch in 3D and never shows a seam, but a UV rasterizer only hits the pixels whose
     * centers lie inside a triangle, so the row of pixels between two islands (a UV seam) stays
     * empty unless the border triangles are dilated like a bake padding. The gutter runs after
     * every triangle's exact pass, so it only fills pixels nothing covered, and the stamp UV
     * extrapolates affinely past the edge, which continues the content across the seam. */
    for (const int pass : IndexRange(2)) {
    const bool gutter = pass == 1;
    for (const SurfaceUnfoldTri &tri : decal.patch.tris) {
      if (tri.tri_index < 0 || tri.tri_index >= corner_tris.size()) {
        continue;
      }
      float2 uv[3];
      float2 corner_px[3];
      float2 stamp_uv[3];
      for (const int c : IndexRange(3)) {
        const int corner = corner_tris[tri.tri_index][c];
        if (corner < 0 || corner >= surface_map.uv_map.size()) {
          uv[c] = float2(0.0f);
        }
        else {
          uv[c] = surface_map.uv_map[corner];
        }
        corner_px[c] = (uv[c] - tile_uv) * tile_size_px;
        stamp_uv[c] = surface_decal_stamp_uv(decal, size, tri.plane[c]);
      }
      const float2 px_min = math::min(corner_px[0], math::min(corner_px[1], corner_px[2]));
      const float2 px_max = math::max(corner_px[0], math::max(corner_px[1], corner_px[2]));
      /* The gutter's reach in pixels: enough to close a seam row, not enough to eat a packed
       * neighbor island's margin. */
      constexpr float gutter_px = 1.5f;
      const int reach = gutter ? int(std::ceil(gutter_px)) : 0;
      const int x0 = std::max(0, int(std::floor(px_min.x)) - reach);
      const int y0 = std::max(0, int(std::floor(px_min.y)) - reach);
      const int x1 = std::min(width - 1, int(std::ceil(px_max.x)) + reach);
      const int y1 = std::min(height - 1, int(std::ceil(px_max.y)) + reach);
      if (x0 > x1 || y0 > y1) {
        continue;
      }
      /* The stamp's footprint of this triangle: skip it when it lies wholly off the stamp. */
      const float2 su_min = math::min(stamp_uv[0], math::min(stamp_uv[1], stamp_uv[2]));
      const float2 su_max = math::max(stamp_uv[0], math::max(stamp_uv[1], stamp_uv[2]));
      if (su_max.x < 0.0f || su_min.x > 1.0f || su_max.y < 0.0f || su_min.y > 1.0f) {
        continue;
      }
      const float2 e1 = corner_px[1] - corner_px[0];
      const float2 e2 = corner_px[2] - corner_px[0];
      const float denom = e1.x * e2.y - e1.y * e2.x;
      if (std::abs(denom) < 1e-12f) {
        continue;
      }
      /* Each barycentric weight times the height of its vertex over the opposite edge is the pixel
       * distance to that edge (negative outside): the gutter test reads it in pixels. */
      const float edge_len[3] = {math::length(corner_px[2] - corner_px[1]),
                                 math::length(corner_px[0] - corner_px[2]),
                                 math::length(corner_px[1] - corner_px[0])};
      const float height_px[3] = {std::abs(denom) / std::max(edge_len[0], 1e-6f),
                                  std::abs(denom) / std::max(edge_len[1], 1e-6f),
                                  std::abs(denom) / std::max(edge_len[2], 1e-6f)};

      /* The triangle's normal-vector rotation: the polar part of the map from the stamp's axes to
       * the canvas UV axes, d(UV)/d(stamp uv) = d(UV)/d(plane) * d(plane)/d(stamp uv). */
      float2x2 normal_rotation = float2x2::identity();
      bool rotate_normal = false;
      if (is_normal) {
        bool q_ok = false;
        const float2x2 q_inv = math::invert(
            float2x2(tri.plane[1] - tri.plane[0], tri.plane[2] - tri.plane[0]), q_ok);
        if (q_ok) {
          const float2x2 uv_edges(uv[1] - uv[0], uv[2] - uv[0]);
          normal_rotation = image_select_normal_rotation(uv_edges * q_inv * stamp_to_plane);
          rotate_normal = !image_select_normal_rotation_is_identity(normal_rotation);
        }
      }

      if (written.is_empty()) {
        written = Array<uint8_t>(int64_t(width) * int64_t(height), uint8_t(0));
      }
      dirty_x0 = std::min(dirty_x0, x0);
      dirty_y0 = std::min(dirty_y0, y0);
      dirty_x1 = std::max(dirty_x1, x1 + 1);
      dirty_y1 = std::max(dirty_y1, y1 + 1);

      const auto raster_rows = [&](const IndexRange rows) {
        for (const int py : rows) {
          for (int px = x0; px <= x1; px++) {
            const float2 p(float(px) + 0.5f, float(py) + 0.5f);
            const float2 d = p - corner_px[0];
            const float w1 = (d.x * e2.y - d.y * e2.x) / denom;
            const float w2 = (e1.x * d.y - e1.y * d.x) / denom;
            const float w0 = 1.0f - w1 - w2;
            /* A hair of slack so the pixel centers on a shared edge land in at least one of its
             * two triangles; the written mask drops the second hit. */
            constexpr float slack = -1e-4f;
            if (gutter) {
              /* Outside by no more than the gutter on every edge; the pixels inside were written by
               * the exact pass already and the written mask skips them below. */
              if (std::min({w0 * height_px[0], w1 * height_px[1], w2 * height_px[2]}) < -gutter_px) {
                continue;
              }
            }
            else if (w0 < slack || w1 < slack || w2 < slack) {
              continue;
            }
            const float2 s = stamp_uv[0] * w0 + stamp_uv[1] * w1 + stamp_uv[2] * w2;
            if (s.x < 0.0f || s.x > 1.0f || s.y < 0.0f || s.y > 1.0f) {
              continue;
            }
            const int64_t pixel = int64_t(py) * width + px;
            if (written[pixel] != 0) {
              continue;
            }
            written[pixel] = 1;
            float4 color = surface_decal_stamp_sample(*stamp, s);
            if (color[3] <= 1e-4f) {
              continue;
            }
            if (rotate_normal) {
              image_select_normal_color_rotate(color, normal_rotation);
            }
            if (is_float) {
              float *d_px = dst_float + pixel * channels;
              if (channels >= 4) {
                /* The map carries its own alpha, so the stamp composites "over" it (the same
                 * compositing the UV write-back uses, see #image_select_composite_over). */
                const float4 out = image_select_composite_over(
                    float4(d_px[0], d_px[1], d_px[2], d_px[3]), color, 1.0f);
                for (int c = 0; c < 4; c++) {
                  d_px[c] = out[c];
                }
              }
              else {
                const float weight = std::clamp(color[3], 0.0f, 1.0f);
                for (int c = 0; c < channels; c++) {
                  d_px[c] = (1.0f - weight) * d_px[c] + weight * color[c];
                }
              }
            }
            else {
              uint8_t *d_px = dst_byte + pixel * 4;
              const float4 dst_color(float(d_px[0]) / 255.0f,
                                     float(d_px[1]) / 255.0f,
                                     float(d_px[2]) / 255.0f,
                                     float(d_px[3]) / 255.0f);
              const float4 out = image_select_composite_over(dst_color, color, 1.0f);
              for (int c = 0; c < 4; c++) {
                /* Round rather than truncate, the UV write-back's byte rule (truncation biases a
                 * feathered rim down and compounds across repeated edits). */
                d_px[c] = uint8_t(std::clamp(out[c] * 255.0f + 0.5f, 0.0f, 255.0f));
              }
            }
          }
        }
      };
      const IndexRange rows(y0, y1 - y0 + 1);
      /* Rows of one triangle never share a pixel, so a large footprint splits across threads; a
       * small one is cheaper to run inline. */
      if (int64_t(x1 - x0 + 1) * rows.size() > 8192) {
        threading::parallel_for(rows, 8, raster_rows);
      }
      else {
        raster_rows(rows);
      }
    }
    }

    if (dirty_x1 > dirty_x0 && dirty_y1 > dirty_y0) {
      ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;
      BKE_image_mark_dirty(ima, ibuf);
      rcti dirty;
      BLI_rcti_init(&dirty, dirty_x0, dirty_x1, dirty_y0, dirty_y1);
      BKE_image_partial_update_mark_region(ima, tile, ibuf, &dirty);
    }
    BKE_image_release_ibuf(ima, ibuf, lock);
  }

  /* The canvas is displayed through cached GPU textures (see
   * #image_select_view3d_fragments_write_mapped for the same refresh). */
  BKE_image_free_gputextures(ima);
  DEG_id_tag_update(&ima->id, 0);
  WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, ima);
}

void surface_decal_commit(bContext *C,
                          const char *undo_label,
                          const SurfaceDecalSession &decal,
                          const ImageSelectView3DSurfaceMap &surface_map,
                          const Span<ImagePaintSelectionTarget> targets,
                          const Span<Vector<SelectionTileFragment>> fragments,
                          const FunctionRef<bool(const Image *)> image_is_alive)
{
  /* The committable targets, in resolve order. */
  Vector<ImagePaintSelectionTarget> commit_targets;
  Vector<Span<SelectionTileFragment>> commit_fragments;
  Vector<int> commit_target_index;
  for (const int target_i : targets.index_range()) {
    Image *ima = targets[target_i].image;
    if (ima == nullptr || fragments[target_i].is_empty() || !image_is_alive(ima)) {
      continue;
    }
    commit_targets.append(targets[target_i]);
    commit_fragments.append(Span<SelectionTileFragment>(fragments[target_i]));
    commit_target_index.append(target_i);
  }

  /* The stamps rasterize into the real canvas targets in ONE image undo step. */
  image_select_view3d_commit_targets_with_undo(
      C,
      undo_label,
      commit_targets,
      commit_fragments,
      [&](Image &ima, const ImageUser &iuser, Span<SelectionTileFragment> frags) {
        /* The cut, exactly as the UV commit re-applies it inside the write-back. */
        image_select_fragment_lift_source(C, &ima, iuser, frags);
        int slot = -1;
        for (const int i : commit_targets.index_range()) {
          if (commit_targets[i].image == &ima) {
            slot = i;
            break;
          }
        }
        if (slot < 0) {
          return;
        }
        Set<int> fragment_tiles;
        for (const SelectionTileFragment &frag : frags) {
          fragment_tiles.add(frag.geom.tile_number);
        }
        surface_decal_bake_target(C,
                                  decal,
                                  surface_map,
                                  commit_targets[slot],
                                  &ima,
                                  iuser,
                                  commit_target_index[slot],
                                  fragment_tiles);
      });
}

/** \} */

}  // namespace blender::ed::sculpt_paint
