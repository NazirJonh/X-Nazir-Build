/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 * \brief Clone Stamp dab kernel: see #paint_clone.hh.
 */

#include <algorithm>

#include "paint_clone.hh"

#include "DNA_brush_types.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

/* Range-for over #Main.images and #Image.tiles needs the complete #ListBaseTIterator. */
#include "BLI_listbase.h"
#include "BLI_math_base.hh"
#include "BLI_math_color.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_rect.h"
#include "BLI_utildefines.h"
#include "BLI_uuid.h"

#include "BKE_attribute.hh"
#include "BKE_brush.hh"
#include "BKE_image.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_material_composite.hh"

#include "ED_paint.hh"
#include "ED_view3d.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"
#include "IMB_interp.hh"

#include "paint_clone_stroke.hh"

#include "mesh/paint_material_source.hh"

namespace blender::ed::sculpt_paint::clone {

/* -------------------------------------------------------------------- */
/** \name Stroke targets.
 * \{ */

void CloneChannelTarget::release()
{
  if (source_ibuf != nullptr) {
    IMB_freeImBuf(source_ibuf);
    source_ibuf = nullptr;
  }
  if (ibuf != nullptr && image != nullptr) {
    BKE_image_release_ibuf(image, ibuf, lock);
  }
  ibuf = nullptr;
  lock = nullptr;
  image = nullptr;
}

CloneChannelTarget::~CloneChannelTarget()
{
  this->release();
}

CloneChannelTarget::CloneChannelTarget(CloneChannelTarget &&other) noexcept
    : channel(other.channel),
      image(other.image),
      iuser(other.iuser),
      ibuf(other.ibuf),
      lock(other.lock),
      source_ibuf(other.source_ibuf),
      is_data(other.is_data)
{
  other.image = nullptr;
  other.ibuf = nullptr;
  other.lock = nullptr;
  other.source_ibuf = nullptr;
}

CloneChannelTarget &CloneChannelTarget::operator=(CloneChannelTarget &&other) noexcept
{
  if (this != &other) {
    this->release();
    channel = other.channel;
    image = other.image;
    iuser = other.iuser;
    ibuf = other.ibuf;
    lock = other.lock;
    source_ibuf = other.source_ibuf;
    is_data = other.is_data;
    other.image = nullptr;
    other.ibuf = nullptr;
    other.lock = nullptr;
    other.source_ibuf = nullptr;
  }
  return *this;
}

static bool clone_layer_id_exists(const Vector<bUUID> &ids, const bUUID &id)
{
  for (const bUUID &known : ids) {
    if (BLI_uuid_equal(known, id)) {
      return true;
    }
  }
  return false;
}

CloneStrokeTargets clone_stroke_targets_build(const Main &bmain,
                                              const Material &material,
                                              const int visible_material_channels)
{
  CloneStrokeTargets targets;

  /* Step 1: discover layer IDs via wired composite stacks of visible channels.
   * A tag-only layer with no wired channel is picked up by the fallback scan below. */
  Vector<bUUID> layer_ids;
  for (int ch = 0; ch < PAINT_MATERIAL_CHANNEL_NUM; ch++) {
    if ((visible_material_channels & (1 << ch)) == 0) {
      continue;
    }
    Vector<PaintMaterialCompositeImageLayer> stack_layers;
    if (!BKE_paint_material_composite_stack_from_material(
            bmain, material, ch, stack_layers, true))
    {
      continue;
    }
    for (const PaintMaterialCompositeImageLayer &stack_layer : stack_layers) {
      const Image *img = stack_layer.color_image;
      if (img == nullptr || BLI_uuid_is_nil(img->paint_layer_id)) {
        continue;
      }
      if (!clone_layer_id_exists(layer_ids, img->paint_layer_id)) {
        layer_ids.append(img->paint_layer_id);
      }
    }
  }

  /* Fallback: if no wired stack resolved (e.g. tag-only AO/mask layers), scan all images
   * for non-nil layer IDs whose channel role is visible. These are file-global IDs, so
   * restrict to images that actually carry a visible channel role. */
  if (layer_ids.is_empty()) {
    for (Image &image : const_cast<Main &>(bmain).images) {
      if (BLI_uuid_is_nil(image.paint_layer_id)) {
        continue;
      }
      const int role = image.paint_layer_channel;
      if (role < 0 || role >= PAINT_MATERIAL_CHANNEL_NUM) {
        continue;
      }
      if ((visible_material_channels & (1 << role)) == 0) {
        continue;
      }
      if (!clone_layer_id_exists(layer_ids, image.paint_layer_id)) {
        layer_ids.append(image.paint_layer_id);
      }
    }
  }

  /* Step 2: resolve (layer, channel) -> Image via canonical BKE helper, acquire once. */
  for (const bUUID &layer_id : layer_ids) {
    /* Roles are channels plus PAINT_LAYER_MAP_MASK; allocate +1 and ignore the mask role here.
     * std::array (not blender::Array, whose second template parameter is the inline capacity
     * and whose default constructor gives an EMPTY array) — same pattern as paint_ops.cc. */
    std::array<Image *, PAINT_MATERIAL_CHANNEL_NUM + 1> maps;
    maps.fill(nullptr);
    BKE_paint_material_layer_maps_get(bmain, material, layer_id, maps);

    CloneLayerTarget layer_target;
    layer_target.layer_id = layer_id;
    bool has_any = false;
    for (int ch = 0; ch < PAINT_MATERIAL_CHANNEL_NUM; ch++) {
      if ((visible_material_channels & (1 << ch)) == 0) {
        continue;
      }
      Image *image = maps[ch];
      if (image == nullptr) {
        continue; /* Missing channel map: skip without aborting whole stroke. */
      }
      if (image->source == IMA_SRC_TILED) {
        /* Multi-tile UDIM is unsupported. Skip tiled targets cleanly. */
        continue;
      }
      ImageUser iuser{};
      BKE_imageuser_default(&iuser);
      iuser.framenr = image->lastframe;
      void *lock = nullptr;
      ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, &lock);
      if (ibuf == nullptr || (ibuf->byte_data() == nullptr && ibuf->float_data() == nullptr)) {
        if (ibuf != nullptr) {
          BKE_image_release_ibuf(image, ibuf, lock);
        }
        continue;
      }
      if (!BKE_image_buffer_format_writable(ibuf)) {
        BKE_image_release_ibuf(image, ibuf, lock);
        continue;
      }
      /* Source and destination are the same image; the clone must read pixels as they were when
       * the stroke began, never its own output (see #CloneChannelTarget.source_ibuf). */
      ImBuf *source_ibuf = IMB_dupImBuf(ibuf);
      if (source_ibuf == nullptr) {
        BKE_image_release_ibuf(image, ibuf, lock);
        continue;
      }
      CloneChannelTarget target;
      target.channel = eMaterialPaintChannel(ch);
      target.image = image;
      target.iuser = iuser;
      target.ibuf = ibuf;
      target.lock = lock;
      target.source_ibuf = source_ibuf;
      target.is_data = (ch != PAINT_MATERIAL_CHANNEL_BASE_COLOR) &&
                       (ch != PAINT_MATERIAL_CHANNEL_EMISSION);
      layer_target.channels[ch] = std::move(target);
      has_any = true;
    }
    if (has_any) {
      targets.layers.append(std::move(layer_target));
    }
  }

  return targets;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Surface frames.
 * \{ */

VArraySpan<float2> clone_uv_map_get(const Mesh &mesh_eval,
                                    const ePaintCanvasSource canvas_mode,
                                    Object *ob_for_material,
                                    const int tri_index)
{
  const bke::AttributeAccessor attributes = mesh_eval.attributes();
  VArraySpan<float2> uv_map;
  if (canvas_mode == PAINT_CANVAS_SOURCE_MATERIAL) {
    const VArray<int> material_indices = *attributes.lookup_or_default<int>(
        "material_index", bke::AttrDomain::Face, 0);
    const int face_i = mesh_eval.corner_tri_faces()[tri_index];
    const Material *ma = BKE_object_material_get(ob_for_material,
                                                 short(material_indices[face_i] + 1));
    if (ma != nullptr && ma->texpaintslot != nullptr) {
      const TexPaintSlot *slot = &ma->texpaintslot[ma->paint_active_slot];
      if (slot != nullptr && slot->uvname != nullptr) {
        uv_map = *attributes.lookup<float2>(slot->uvname, bke::AttrDomain::Corner);
      }
    }
  }
  if (uv_map.is_empty()) {
    uv_map = *attributes.lookup<float2>(mesh_eval.active_uv_map_name(), bke::AttrDomain::Corner);
  }
  return uv_map;
}

bool clone_tri_uv_jacobian(const Mesh &mesh_eval,
                           const Span<float2> uv_map,
                           const int tri_index,
                           float3 &r_dp_du,
                           float3 &r_dp_dv,
                           float *r_denom)
{
  r_dp_du = float3(0.0f);
  r_dp_dv = float3(0.0f);
  if (r_denom != nullptr) {
    *r_denom = 0.0f;
  }
  if (uv_map.is_empty()) {
    return false;
  }
  const int3 &tri = mesh_eval.corner_tris()[tri_index];
  const Span<int> corner_verts = mesh_eval.corner_verts();
  const Span<float3> positions = mesh_eval.vert_positions();
  const float3 edge1 = positions[corner_verts[tri[1]]] - positions[corner_verts[tri[0]]];
  const float3 edge2 = positions[corner_verts[tri[2]]] - positions[corner_verts[tri[0]]];
  const float2 duv1 = uv_map[tri[1]] - uv_map[tri[0]];
  const float2 duv2 = uv_map[tri[2]] - uv_map[tri[0]];
  const float denom = duv1.x * duv2.y - duv2.x * duv1.y;
  if (math::abs(denom) < 1e-12f) {
    return false;
  }
  const float inv_denom = 1.0f / denom;
  r_dp_du = (edge1 * duv2.y - edge2 * duv1.y) * inv_denom;
  r_dp_dv = (edge2 * duv1.x - edge1 * duv2.x) * inv_denom;
  if (math::length(r_dp_du) < 1e-12f) {
    return false;
  }
  if (r_denom != nullptr) {
    *r_denom = denom;
  }
  return true;
}

bool clone_surface_frame_build(const Mesh &mesh_eval,
                               const Span<float2> uv_map,
                               const int tri_index,
                               const ARegion *region,
                               const float4x4 &projection_mat,
                               const float3 &view_right,
                               CloneSurfaceFrame &r_frame)
{
  r_frame = CloneSurfaceFrame();
  float denom = 0.0f;
  float3 dp_du, dp_dv;
  if (!clone_tri_uv_jacobian(mesh_eval, uv_map, tri_index, dp_du, dp_dv, &denom)) {
    return false;
  }
  const int3 &tri = mesh_eval.corner_tris()[tri_index];
  const Span<int> corner_verts = mesh_eval.corner_verts();
  const Span<float3> positions = mesh_eval.vert_positions();
  const std::array<float3, 3> tri_positions = {positions[corner_verts[tri[0]]],
                                               positions[corner_verts[tri[1]]],
                                               positions[corner_verts[tri[2]]]};

  /* Same tangent and handedness convention as #calc_uv_primitive_tangent, so a Normal sample
   * decodes here exactly as the material paint decodes it. */
  const float bitangent_sign = denom < 0.0f ? -1.0f : 1.0f;
  material::build_normal_write_basis(math::normalize(dp_du),
                                     bitangent_sign,
                                     Span<float3>(tri_positions.data(), 3),
                                     view_right,
                                     region,
                                     projection_mat,
                                     r_frame.t_screen,
                                     r_frame.b_screen,
                                     r_frame.n_m,
                                     r_frame.t_m,
                                     r_frame.b_m);

  /* Columns, because #MatBase is column-major: column i is where UV axis i lands. */
  r_frame.uv_to_screen[0] = float2(math::dot(dp_du, r_frame.t_screen),
                                   math::dot(dp_du, r_frame.b_screen));
  r_frame.uv_to_screen[1] = float2(math::dot(dp_dv, r_frame.t_screen),
                                   math::dot(dp_dv, r_frame.b_screen));
  r_frame.valid = true;
  return true;
}

float3 clone_view_right_get(const ViewContext &vc, const Object &ob)
{
  if (vc.rv3d == nullptr) {
    return float3(0.0f);
  }
  const float4x4 view_to_object = ob.world_to_object() * float4x4(vc.rv3d->viewinv);
  return math::normalize(math::transform_direction(view_to_object, float3(1.0f, 0.0f, 0.0f)));
}

/** Inverse of a 2x2, or nothing when it is singular (a patch seen exactly edge-on). */
static std::optional<float2x2> clone_mat2_invert(const float2x2 &m)
{
  const float det = m[0][0] * m[1][1] - m[1][0] * m[0][1];
  if (math::abs(det) < 1e-12f) {
    return std::nullopt;
  }
  const float inv_det = 1.0f / det;
  float2x2 out;
  out[0] = float2(m[1][1] * inv_det, -m[0][1] * inv_det);
  out[1] = float2(-m[1][0] * inv_det, m[0][0] * inv_det);
  return out;
}

/**
 * Whether \a uv is inside the canvas.
 *
 * The sampler wraps out-of-range coordinates, so a center outside the canvas would silently read
 * the opposite side of the image instead of reading nothing. Written as a negated `!(...)` so a
 * NaN -- which every comparison answers false to -- is rejected rather than accepted.
 */
static bool clone_uv_is_on_canvas(const float2 &uv)
{
  return !(!(uv[0] >= 0.0f) || !(uv[0] <= 1.0f) || !(uv[1] >= 0.0f) || !(uv[1] <= 1.0f));
}

bool clone_symmetry_transform_apply(CloneDabTransform &transform, const float2x2 &jacobian)
{
  const std::optional<float2x2> inv = clone_mat2_invert(jacobian);
  if (!inv.has_value()) {
    return false;
  }
  transform.dest_uv_to_source_uv = transform.dest_uv_to_source_uv * (*inv);
  transform.dest_uv_to_footprint_dir = transform.dest_uv_to_footprint_dir * (*inv);
  return true;
}

CloneDabTransform clone_dab_transform_build(const Mesh &mesh_eval,
                                            const ePaintCanvasSource canvas_mode,
                                            Object *ob_for_material,
                                            const CloneSurfaceFrame &source_frame,
                                            const int dest_tri_index,
                                            const ARegion *region,
                                            const float4x4 &projection_mat,
                                            const float3 &view_right)
{
  CloneDabTransform transform;
  if (!source_frame.valid) {
    return transform;
  }
  transform.source_frame = source_frame;
  const VArraySpan<float2> uv_map = clone_uv_map_get(
      mesh_eval, canvas_mode, ob_for_material, dest_tri_index);
  if (!clone_surface_frame_build(mesh_eval,
                                 uv_map,
                                 dest_tri_index,
                                 region,
                                 projection_mat,
                                 view_right,
                                 transform.dest_frame))
  {
    return transform;
  }
  const std::optional<float2x2> screen_to_uv_source = clone_mat2_invert(
      transform.source_frame.uv_to_screen);
  if (!screen_to_uv_source.has_value()) {
    return transform;
  }

  /* The footprint direction is the destination map with its scale divided out, so the brush keeps
   * the size the caller asked for in UV units and only its orientation comes from the view. The
   * determinant is the area factor, hence its square root as the isotropic scale. */
  const float2x2 &dest_map = transform.dest_frame.uv_to_screen;
  const float dest_det = dest_map[0][0] * dest_map[1][1] - dest_map[1][0] * dest_map[0][1];
  if (math::abs(dest_det) < 1e-12f) {
    return transform;
  }
  transform.dest_uv_to_footprint_dir = dest_map * (1.0f / math::sqrt(math::abs(dest_det)));

  transform.dest_uv_to_source_uv = *screen_to_uv_source * dest_map;
  transform.has_frames = true;
  return transform;
}

/**
 * Re-plant a tangent-space normal sampled at the source onto the destination surface.
 *
 * The sample is stored in the SOURCE triangle's tangent frame; written unchanged at the
 * destination it would decode against a different frame, and the cloned detail would light
 * wrongly -- most visibly when the two patches face different ways. So it is lifted to object
 * space through the source's tangent basis, read off in the source's screen-aligned basis (the
 * frame the detail is seen in, which is exactly what the stamp preserves), and planted back
 * through the destination's screen and tangent bases. Mirrors
 * #material::remap_decal_normal_to_packed_tangent, with the extra first step that a brush decal
 * does not need because it has no tangent frame of its own.
 *
 * \return false for a degenerate sample (flat grey or black), where the caller must leave the
 * texel alone rather than normalize a zero vector.
 */
static bool clone_remap_normal_to_dest_tangent(const float4 &packed_source,
                                               const bool is_float,
                                               const CloneSurfaceFrame &source_frame,
                                               const CloneSurfaceFrame &dest_frame,
                                               float3 &r_normal)
{
  float3 n_source;
  if (is_float) {
    /* A float buffer stores the normal directly; only its length has to be trusted. */
    n_source = float3(packed_source);
    const float source_len = math::length(n_source);
    if (source_len < 1e-6f) {
      return false;
    }
    n_source /= source_len;
  }
  else if (!BKE_paint_material_normal_from_sample(float3(packed_source), n_source)) {
    return false;
  }

  const float3 n_object = n_source.x * source_frame.t_m + n_source.y * source_frame.b_m +
                          n_source.z * source_frame.n_m;
  /* The detail as it is seen: components along screen right and up, and away from the surface. */
  const float3 n_screen = float3(math::dot(n_object, source_frame.t_screen),
                                 math::dot(n_object, source_frame.b_screen),
                                 math::dot(n_object, source_frame.n_m));

  const float3 n_dest_object = n_screen.x * dest_frame.t_screen +
                               n_screen.y * dest_frame.b_screen + n_screen.z * dest_frame.n_m;
  const float3 n_dest = float3(math::dot(n_dest_object, dest_frame.t_m),
                               math::dot(n_dest_object, dest_frame.b_m),
                               math::dot(n_dest_object, dest_frame.n_m));
  const float dest_len = math::length(n_dest);
  if (dest_len < 1e-6f) {
    return false;
  }
  r_normal = n_dest / dest_len;
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name CPU sampler.
 * \{ */

bool sample_channel_at_uv_cpu(const ImBuf &ibuf, const float2 &uv, float4 &r_color)
{
  if (ibuf.x <= 0 || ibuf.y <= 0) {
    return false;
  }
  /* Half-texel shift: UV 1.0 must land on the last texel center, not one past it. */
  const float u = uv[0] * float(ibuf.x) - 0.5f;
  const float v = uv[1] * float(ibuf.y) - 0.5f;

  if (ibuf.float_data() != nullptr) {
    r_color = imbuf::interpolate_bilinear_wrap_fl(&ibuf, u, v);
    return true;
  }
  if (ibuf.byte_data() != nullptr) {
    const uchar4 px = imbuf::interpolate_bilinear_wrap_byte(&ibuf, u, v);
    float rgba[4];
    rgba_uchar_to_float(rgba, px);
    r_color = float4(rgba[0], rgba[1], rgba[2], rgba[3]);
    return true;
  }
  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Blend policy + active stroke runtime.
 * \{ */

void clone_blend_channel_values(const float4 &src,
                                const float4 &dst,
                                const float strength,
                                const eMaterialPaintChannel channel,
                                const bool is_data,
                                float4 &r_result)
{
  const float s = std::clamp(strength, 0.0f, 1.0f);
  if (channel == PAINT_MATERIAL_CHANNEL_NORMAL && s >= 0.99f) {
    /* Full strength normal: preserve encoded RGB values exactly. */
    r_result = src;
    return;
  }
  if (is_data || channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
    /* Stored numbers ARE the values: mix them as they are. A Normal map below full strength is
     * handled by the packed-normal path in the caller, so reaching here means a degenerate frame,
     * where an encoded mix is still the least wrong answer. */
    r_result = dst + (src - dst) * s;
    return;
  }
  /* Color: decode, mix, re-encode. Alpha is linear coverage and never encoded. */
  float src_linear[4];
  float dst_linear[4];
  srgb_to_linearrgb_v3_v3(src_linear, src);
  srgb_to_linearrgb_v3_v3(dst_linear, dst);
  float mixed[4];
  interp_v3_v3v3(mixed, dst_linear, src_linear, s);
  linearrgb_to_srgb_v3_v3(r_result, mixed);
  r_result[3] = dst[3] + (src[3] - dst[3]) * s;
}

static void clone_write_pixel_to_ibuf(ImBuf &ibuf, const int x, const int y, const float4 &color)
{
  BLI_assert(x >= 0 && x < ibuf.x && y >= 0 && y < ibuf.y);
  const int64_t offset = (int64_t(y) * int64_t(ibuf.x) + int64_t(x)) * 4;
  if (ibuf.float_data() != nullptr) {
    float *px = ibuf.float_data_for_write() + offset;
    px[0] = color[0];
    px[1] = color[1];
    px[2] = color[2];
    px[3] = color[3];
  }
  else if (ibuf.byte_data() != nullptr) {
    uchar *px = ibuf.byte_data_for_write() + offset;
    const float rgba[4] = {color[0], color[1], color[2], color[3]};
    rgba_float_to_uchar(px, rgba);
  }
}

static bool clone_read_pixel_from_ibuf(const ImBuf &ibuf, const int x, const int y, float4 &r_color)
{
  BLI_assert(x >= 0 && x < ibuf.x && y >= 0 && y < ibuf.y);
  const int64_t offset = (int64_t(y) * int64_t(ibuf.x) + int64_t(x)) * 4;
  if (ibuf.float_data() != nullptr) {
    const float *px = ibuf.float_data() + offset;
    r_color = float4(px[0], px[1], px[2], px[3]);
    return true;
  }
  if (ibuf.byte_data() != nullptr) {
    const uchar *px = ibuf.byte_data() + offset;
    float rgba[4];
    rgba_uchar_to_float(rgba, px);
    r_color = float4(rgba[0], rgba[1], rgba[2], rgba[3]);
    return true;
  }
  return false;
}

/**
 * Capture the pristine pixels of one undo tile into the stroke's open image undo step.
 *
 * No-op when no image undo step is open: the pixel edit would then be un-undoable, matching every
 * other writer's contract that tiles go into the step the stroke opened. `find_prev = true` makes
 * a repeated push of a tile already captured this stroke a cheap map lookup that keeps the
 * originally captured pixels.
 */
static void clone_capture_undo_tile(CloneChannelTarget &target, const int tx, const int ty)
{
  if (!ED_image_undo_is_step_active()) {
    return;
  }
  PaintTileMap *undo_tiles = ED_image_paint_tile_map_get();
  ED_image_paint_tile_push(undo_tiles,
                           target.image,
                           target.ibuf,
                           &target.iuser,
                           tx,
                           ty,
                           nullptr,
                           nullptr,
                           false,
                           true);
}

void clone_stroke_apply_dab(CloneStrokeRuntime *runtime,
                            const float2 &dest_uv,
                            const float2 &dab_center_uv,
                            const CloneDabTransform &transform,
                            const float uv_radius,
                            const int channel_mask,
                            const float strength)
{
  if (runtime == nullptr || !runtime->targets.is_valid()) {
    return;
  }
  if (strength <= 0.0f || uv_radius <= 0.0f || channel_mask == 0) {
    return;
  }
  if (!clone_uv_is_on_canvas(dest_uv)) {
    /* The destination itself wraps just as the source does; a dab centered off-canvas would
     * smear along whichever edge its bounds collapse onto. */
    return;
  }

  /* True-clone offset: every pixel inside the footprint samples the source with the offset it
   * has from the dab center, so image detail is copied, not a flat color.
   *
   * `dest_uv - dab_center_uv` is what displaces the source: zero in Absolute, and in Relative the
   * distance the brush has travelled since the offset was fixed. It goes through the same mapping
   * as the per-pixel offsets below, so the travel is measured in the surface's screen-aligned
   * frame rather than raw UV -- otherwise a Relative source would drift wrongly the moment the
   * destination's parametrization is rotated or scaled against the source's. */
  const float2 source_base = runtime->source.uv +
                             transform.dest_uv_to_source_uv * (dest_uv - dab_center_uv);
  if (!clone_uv_is_on_canvas(source_base)) {
    return;
  }

  /* Footprint shape, read from the brush on every dab so switching Texture Clip mid-stroke takes
   * effect at once -- the same live switch the source cursor shows. */
  const bool footprint_is_rect = runtime->brush != nullptr &&
                                 runtime->brush->texture_clip_shape ==
                                     BRUSH_TEXTURE_CLIP_RECTANGLE;

  /* UV offset -> footprint coordinates, where the boundary is at 1: a unit disc for the round
   * brush, a unit square for the rectangular one. Going through the destination's screen-aligned
   * directions is what gives the rectangle edges that follow the view instead of the UV axes. */
  const float2x2 uv_to_footprint = transform.dest_uv_to_footprint_dir * (1.0f / uv_radius);
  const std::optional<float2x2> footprint_to_uv = clone_mat2_invert(uv_to_footprint);
  if (!footprint_to_uv.has_value()) {
    return;
  }
  /* Conservative UV half-extent of the footprint: the corner of the unit square is the farthest
   * point of either shape, so its image bounds both. */
  const float2x2 &to_uv = *footprint_to_uv;
  const float du_max = math::abs(to_uv[0][0]) + math::abs(to_uv[1][0]);
  const float dv_max = math::abs(to_uv[0][1]) + math::abs(to_uv[1][1]);

  for (CloneLayerTarget &layer : runtime->targets.layers) {
    /* Channels of THIS layer that the caller asked for and that have a writable target. The
     * footprint geometry below is shared by all of them, which is the whole point of resolving
     * them up front instead of re-entering per channel. */
    Vector<CloneChannelTarget *, PAINT_MATERIAL_CHANNEL_NUM> active;
    ImBuf *bounds_ibuf = nullptr;
    for (int ch = 0; ch < PAINT_MATERIAL_CHANNEL_NUM; ch++) {
      if ((channel_mask & (1 << ch)) == 0) {
        continue;
      }
      std::optional<CloneChannelTarget> &opt = layer.channels[ch];
      if (!opt.has_value() || opt->ibuf == nullptr || opt->source_ibuf == nullptr) {
        continue;
      }
      if (opt->ibuf->x <= 0 || opt->ibuf->y <= 0) {
        continue;
      }
      if (bounds_ibuf == nullptr) {
        bounds_ibuf = opt->ibuf;
      }
      else if (opt->ibuf->x != bounds_ibuf->x || opt->ibuf->y != bounds_ibuf->y) {
        /* The pixel loop below walks one grid for the whole layer. A channel map of a different
         * resolution would be indexed against the wrong grid, so it is left to its own pass
         * rather than silently painted at the wrong scale. */
        continue;
      }
      active.append(&opt.value());
    }
    if (active.is_empty() || bounds_ibuf == nullptr) {
      continue;
    }
    const ImBuf &grid = *bounds_ibuf;

    /* Half-open pixel bounds of the footprint, intersected with the canvas. An empty intersection
     * means the footprint missed this image entirely -- clamping instead would collapse it onto a
     * single edge row and paint a stray line there. */
    const int xmin = std::max(0, int(math::floor((dest_uv[0] - du_max) * float(grid.x))));
    const int xmax = std::min(int(grid.x), int(math::ceil((dest_uv[0] + du_max) * float(grid.x))));
    const int ymin = std::max(0, int(math::floor((dest_uv[1] - dv_max) * float(grid.y))));
    const int ymax = std::min(int(grid.y), int(math::ceil((dest_uv[1] + dv_max) * float(grid.y))));
    if (xmin >= xmax || ymin >= ymax) {
      continue;
    }

    bool wrote_any = false;
    rcti written_region;
    BLI_rcti_init_minmax(&written_region);
    /* Per-target memo of the undo tile captured last, so a run of texels inside one tile costs a
     * comparison instead of a map lookup. Indexed alongside #active. */
    Vector<int2, PAINT_MATERIAL_CHANNEL_NUM> last_tile(active.size(), int2(-1, -1));

    for (int py = ymin; py < ymax; py++) {
      const float v = (float(py) + 0.5f) / float(grid.y);
      const float dv = v - dest_uv[1];
      for (int px = xmin; px < xmax; px++) {
        const float u = (float(px) + 0.5f) / float(grid.x);
        const float du = u - dest_uv[0];
        /* Distance to the footprint boundary, normalized so that 1 is the edge of whichever
         * shape is active. The falloff curve is evaluated over that same [0, 1]. Resolved once
         * per texel and reused by every channel, which is why the channel loop is inside. */
        const float2 footprint_co = uv_to_footprint * float2(du, dv);
        const float dist = footprint_is_rect ?
                               math::max(math::abs(footprint_co.x), math::abs(footprint_co.y)) :
                               math::length(footprint_co);
        if (dist > 1.0f) {
          continue;
        }
        float factor = strength;
        if (runtime->brush != nullptr) {
          factor *= BKE_brush_curve_strength_clamped(runtime->brush, dist, 1.0f);
        }
        else {
          factor *= 1.0f - dist;
        }
        if (factor <= 0.0f) {
          continue;
        }
        /* The footprint offset travels through the screen-aligned frame of both patches, so the
         * stamp keeps the orientation it is seen at instead of the one the UV layout happens to
         * have. */
        const float2 source_uv = source_base + transform.dest_uv_to_source_uv * float2(du, dv);

        const int tx = px >> ED_IMAGE_UNDO_TILE_BITS;
        const int ty = py >> ED_IMAGE_UNDO_TILE_BITS;
        bool wrote_here = false;

        for (const int target_i : active.index_range()) {
          CloneChannelTarget &target = *active[target_i];
          ImBuf &ibuf = *target.ibuf;

          float4 src_color;
          if (!sample_channel_at_uv_cpu(*target.source_ibuf, source_uv, src_color)) {
            continue;
          }
          float4 dst_color;
          if (!clone_read_pixel_from_ibuf(ibuf, px, py, dst_color)) {
            continue;
          }
          /* A Normal map is only meaningful in the tangent frame it was authored in, so it can be
           * cloned across surfaces only when both frames are known. Without them the sample is
           * copied raw and the detail would light against the wrong basis. */
          const bool remap_normal = target.channel == PAINT_MATERIAL_CHANNEL_NORMAL &&
                                    transform.has_frames;
          const bool is_float_buffer = ibuf.float_data() != nullptr;
          float4 blended;
          if (remap_normal) {
            float3 target_normal;
            if (!clone_remap_normal_to_dest_tangent(src_color,
                                                    is_float_buffer,
                                                    transform.source_frame,
                                                    transform.dest_frame,
                                                    target_normal))
            {
              continue;
            }
            /* Blending in packed space would shorten the vector; the shared helper interpolates
             * the unpacked normals and renormalizes, which is what every other Normal writer
             * does. */
            float packed[3];
            BKE_pbr_normal_blend_mix(dst_color, target_normal, factor, is_float_buffer, packed);
            blended = float4(packed[0], packed[1], packed[2], 1.0f);
          }
          else {
            /* A float buffer is linear already; only byte pixels carry an sRGB encoding to undo. */
            const bool blend_as_data = target.is_data || is_float_buffer;
            clone_blend_channel_values(
                src_color, dst_color, factor, target.channel, blend_as_data, blended);
            /* Normal channel: keep alpha at 1 for encoded maps. */
            if (target.channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
              blended[3] = 1.0f;
            }
          }

          /* Capture the pristine tile before its first write of this stroke. */
          if (last_tile[target_i] != int2(tx, ty)) {
            clone_capture_undo_tile(target, tx, ty);
            last_tile[target_i] = int2(tx, ty);
          }
          clone_write_pixel_to_ibuf(ibuf, px, py, blended);
          wrote_here = true;
        }

        if (wrote_here) {
          const int written_px[2] = {px, py};
          BLI_rcti_do_minmax_v(&written_region, written_px);
          wrote_any = true;
        }
      }
    }

    if (wrote_any) {
      /* One mark per dab per target (not per pixel): the composite cache recomposites on
       * partial-update-log changes, and a whole-image tag per texel would rebuild the canvas
       * for every texel of every dab. The dirty flag alone only says the image needs saving --
       * the GPU texture and the material composite both refresh off the partial-update log, so
       * the touched rectangle has to be published there or the stroke stays invisible. */
      written_region.xmax += 1;
      written_region.ymax += 1;
      for (CloneChannelTarget *target : active) {
        BKE_image_mark_dirty(target->image, target->ibuf);
        for (ImageTile &tile : target->image->tiles) {
          BKE_image_partial_update_mark_region(
              target->image, &tile, target->ibuf, &written_region);
        }
      }
    }
  }
}

/** \} */

}  // namespace blender::ed::sculpt_paint::clone
