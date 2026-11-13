/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * See `paint_image_select_editor_decal.hh`.
 */

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "MEM_guardedalloc.h"

#include "BLI_listbase_wrapper.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.h"
#include "BLI_span.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_image_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "BKE_context.hh"
#include "BKE_mesh.hh"
#include "BKE_paint_bvh.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "GPU_immediate.hh"
#include "GPU_shader_builtin.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "UI_view2d.hh"

#include "../../space_image/image_runtime.hh"

#include "GEO_reverse_uv_sampler.hh"
#include "paint_image_select_decal_log.hh"
#include "paint_image_select_editor_decal.hh"
#include "paint_image_select_preview_view3d_intern.hh"

namespace blender::ed::sculpt_paint {

/* -------------------------------------------------------------------- */
/** \name Image Editor decal frame
 *
 * The Image Editor authors UV gestures (translation, rotation, scale around a pivot); the decal
 * frame is derived from them: the center UV is the source cage center through the mapping, the
 * roll is the rotation, the scale the gesture's scale. The frame is sampled on the surface with
 * #surface_decal_frame_at_uv, so no Viewport (no raycast, no px-per-unit) is involved: the plane
 * of the frame is measured in object units, with the stamp resolved to the source texel density
 * through #ImageSelectEditorDecal::px_per_unit.
 * \{ */

/**
 * The center's offset from the patch seed in the plane (object units), and whether the stamp still
 * overlaps the seed neighborhood: the offset stays within half the stamp extents along each stamp
 * axis, which means the stamp still covers part of the patch.
 */
static bool editor_decal_offset_get(const ImageSelectEditorDecal &decal,
                                    const ImageSelectEditorMapping &mapping,
                                    const float2 &center_uv,
                                    float2 &r_offset)
{
  if (!decal.seed_valid) {
    return false;
  }
  r_offset = decal.seed_jacobian_inv * (center_uv - decal.seed_uv);
  const float roll = decal.orientation * mapping.rotation;
  const float2 axis_x = float2(std::cos(roll), std::sin(roll));
  const float2 axis_y = float2(-axis_x.y, axis_x.x);
  const float2 size = float2(decal.session.base_size.x * mapping.scale.x,
                             decal.session.base_size.y * mapping.scale.y);
  return std::abs(math::dot(r_offset, axis_x)) <= 0.5f * std::abs(size.x) &&
         std::abs(math::dot(r_offset, axis_y)) <= 0.5f * std::abs(size.y);
}

static bool editor_decal_sample(const Object &ob,
                                const Depsgraph &depsgraph,
                                const ImageSelectView3DSurfaceMap &surface_map,
                                const float2 &uv,
                                float3 &r_co)
{
  const Mesh *mesh = id_cast<const Mesh *>(ob.data);
  if (mesh == nullptr) {
    return false;
  }
  float3 no;
  return surface_map.sample(mesh->corner_verts(),
                            bke::pbvh::vert_positions_eval(depsgraph, ob),
                            uv,
                            r_co,
                            no);
}

/* The cage rect is the UV bounds through the plane -> UV Jacobian, so a UV that is not a similarity
 * stays inside the stamp. */
bool image_select_editor_decal_begin(const bContext *C,
                                     Object &ob,
                                     const ImageSelectView3DSurfaceMap &surface_map,
                                     const Span<ImagePaintSelectionTarget> targets,
                                     const Span<Vector<SelectionTileFragment>> fragments,
                                     ImageSelectEditorDecal &r_decal)
{
  const Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  if (depsgraph == nullptr || ob.type != OB_MESH || surface_map.sampler == nullptr) {
    return false;
  }

  /* The source cage: the union of the fragments' tight selection rects, in global UV. */
  float2 uv_min = float2(FLT_MAX, FLT_MAX);
  float2 uv_max = float2(-FLT_MAX, -FLT_MAX);
  float canvas_w = 0.0f;
  for (const Vector<SelectionTileFragment> &per_target : fragments) {
    for (const SelectionTileFragment &frag : per_target) {
      if (frag.pixels.fragment_ibuf == nullptr) {
        continue;
      }
      const float2 tile_uv = image_select_udim_tile_uv_origin(frag.geom.tile_number);
      const float2 tile_size = float2(frag.geom.tile_size_px);
      const float2 lo = tile_uv + float2(image_select_fragment_ui_origin(frag)) / tile_size;
      const float2 hi = lo + float2(image_select_fragment_ui_size(frag)) / tile_size;
      uv_min = math::min(uv_min, lo);
      uv_max = math::max(uv_max, hi);
      canvas_w = std::max(canvas_w, tile_size.x);
    }
  }
  if (!(uv_min.x < uv_max.x && uv_min.y < uv_max.y) || canvas_w <= 0.0f) {
    return false;
  }
  const float2 center_uv = (uv_min + uv_max) * 0.5f;
  const float2 half_uv = (uv_max - uv_min) * 0.5f;

  /* The surface direction of +U at the center orients the frame, so an unmoved fragment lands on
   * itself with its own orientation. */
  const float eps_u = std::max(half_uv.x * 0.02f, 1e-4f);
  float3 co_center;
  if (!editor_decal_sample(ob, *depsgraph, surface_map, center_uv, co_center)) {
    return false;
  }
  float3 co_plus;
  float3 co_minus;
  float3 u_direction = float3(1.0f, 0.0f, 0.0f);
  float units_per_uv = 0.0f;
  if (editor_decal_sample(ob, *depsgraph, surface_map, center_uv + float2(eps_u, 0.0f), co_plus) &&
      editor_decal_sample(ob, *depsgraph, surface_map, center_uv - float2(eps_u, 0.0f), co_minus))
  {
    u_direction = co_plus - co_minus;
    units_per_uv = math::length(u_direction) / (2.0f * eps_u);
  }
  if (units_per_uv <= 1e-9f) {
    return false;
  }

  SurfaceDecalFrame frame;
  if (!surface_decal_frame_at_uv(
          ob, *depsgraph, surface_map, center_uv, u_direction, frame) ||
      !frame.jacobian_valid)
  {
    return false;
  }
  bool jacobian_ok = false;
  const float2x2 jacobian_inv = math::invert(frame.plane_uv_jacobian, jacobian_ok);
  if (!jacobian_ok) {
    return false;
  }

  /* Plane px = object units * texel density along U, so the stamp keeps the source resolution. */
  const float px_per_unit = canvas_w / units_per_uv;
  float2 plane_half = float2(0.0f);
  for (const int i : IndexRange(4)) {
    const float2 corner = float2((i & 1) ? half_uv.x : -half_uv.x, (i & 2) ? half_uv.y : -half_uv.y);
    const float2 plane = jacobian_inv * corner;
    plane_half = math::max(plane_half, math::abs(plane));
  }
  plane_half *= px_per_unit;

  SurfaceDecalCage cage;
  cage.anchor_uv = center_uv;
  cage.plane_uv_jacobian = float2x2(frame.plane_uv_jacobian[0] / px_per_unit,
                                    frame.plane_uv_jacobian[1] / px_per_unit);
  cage.plane_min = -plane_half;
  cage.plane_max = plane_half;
  cage.frame_angle = 0.0f;

  ImageSelectEditorDecal decal;
  decal.object = &ob;
  decal.source_center_uv = center_uv;
  decal.tangent = frame.tangent;
  decal.orientation = (frame.plane_uv_jacobian[0].x * frame.plane_uv_jacobian[1].y -
                       frame.plane_uv_jacobian[0].y * frame.plane_uv_jacobian[1].x) >= 0.0f ?
                          1.0f :
                          -1.0f;
  decal.seed_uv = center_uv;
  decal.seed_jacobian_inv = jacobian_inv;
  decal.seed_valid = true;
  decal.session.center_uv = center_uv;
  decal.session.roll = 0.0f;
  decal.session.scale = float2(1.0f);
  decal.session.base_size = (cage.plane_max - cage.plane_min) / px_per_unit;
  bool any_stamp = false;
  for (const int target_i : targets.index_range()) {
    ImBuf *stamp = surface_decal_stamp_build(cage, targets[target_i], fragments[target_i]);
    decal.session.stamps.append(stamp);
    any_stamp = any_stamp || stamp != nullptr;
  }
  if (!any_stamp ||
      !surface_decal_patch_ensure(C,
                                  decal.session,
                                  &ob,
                                  surface_map,
                                  frame.tri,
                                  frame.co,
                                  frame.tangent) ||
      decal.session.patch.tris.is_empty())
  {
    surface_decal_release(decal.session);
    return false;
  }
  r_decal = std::move(decal);
  return true;
}

bool image_select_editor_decal_update(const bContext *C,
                                      Object &ob,
                                      const ImageSelectView3DSurfaceMap &surface_map,
                                      ImageSelectEditorDecal &decal,
                                      const ImageSelectEditorMapping &mapping)
{
  decal.session.roll = decal.orientation * mapping.rotation;
  decal.session.scale = mapping.scale;

  const Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  if (depsgraph == nullptr) {
    return false;
  }
  const float2 center_uv = mapping.pivot +
                           mapping.linear * (decal.source_center_uv - mapping.pivot) +
                           mapping.translation;
  /* The frame is oriented by the surface direction of +U at the center, not carried over from the
   * previous step: the cage is authored in UV space and never turns, so a decal that kept its 3D
   * orientation would spin against it as soon as the center lands on a face whose UV axes run
   * another way (another island). The previous tangent stays the fallback where +U cannot be
   * sampled (an island border). */
  float3 u_direction = decal.tangent;
  {
    constexpr float eps_u = 1e-3f;
    float3 co_plus;
    float3 co_minus;
    if (editor_decal_sample(ob, *depsgraph, surface_map, center_uv + float2(eps_u, 0.0f), co_plus) &&
        editor_decal_sample(ob, *depsgraph, surface_map, center_uv - float2(eps_u, 0.0f), co_minus))
    {
      const float3 direction = co_plus - co_minus;
      if (math::length_squared(direction) > 1e-18f) {
        u_direction = direction;
      }
    }
  }
  SurfaceDecalFrame frame;
  if (surface_decal_frame_at_uv(ob, *depsgraph, surface_map, center_uv, u_direction, frame)) {
    if (frame.jacobian_valid) {
      /* A mirrored island turns a rotation the other way on the surface. */
      decal.orientation = (frame.plane_uv_jacobian[0].x * frame.plane_uv_jacobian[1].y -
                           frame.plane_uv_jacobian[0].y * frame.plane_uv_jacobian[1].x) >= 0.0f ?
                              1.0f :
                              -1.0f;
      decal.session.roll = decal.orientation * mapping.rotation;
    }
    if (decal.off_surface) {
      IMAGE_SELECT_DECAL_LOG(IMAGE_SELECT_DECAL_TAG_EDITOR
                             "center back on the surface at uv (%.4f, %.4f), offset reset "
                             "(was %.5f, %.5f)\n",
                             center_uv.x,
                             center_uv.y,
                             decal.session.plane_offset.x,
                             decal.session.plane_offset.y);
      decal.off_surface = false;
    }
    bool jacobian_ok = false;
    const float2x2 jacobian_inv = math::invert(frame.plane_uv_jacobian, jacobian_ok);
    if (frame.jacobian_valid && jacobian_ok) {
      decal.seed_uv = center_uv;
      decal.seed_jacobian_inv = jacobian_inv;
      decal.seed_valid = true;
    }
    decal.tangent = frame.tangent;
    decal.session.plane_offset = float2(0.0f);
    decal.session.center_uv = center_uv;
    return surface_decal_patch_ensure(
        C, decal.session, &ob, surface_map, frame.tri, frame.co, frame.tangent);
  }

  /* No surface under the center (a UV gap): keep the patch seeded at the last valid point and
   * place the stamp by the center's offset from it, while the stamp still overlaps the patch. */
  float2 offset = float2(0.0f);
  if (decal.session.patch_seed_tri < 0 || !editor_decal_offset_get(decal, mapping, center_uv, offset)) {
    return false;
  }
  if (!decal.off_surface) {
    IMAGE_SELECT_DECAL_LOG(IMAGE_SELECT_DECAL_TAG_EDITOR
                           "center left the surface at uv (%.4f, %.4f), plane offset "
                           "(%.5f, %.5f) units\n",
                           center_uv.x,
                           center_uv.y,
                           offset.x,
                           offset.y);
    decal.off_surface = true;
  }
  decal.session.plane_offset = offset;
  decal.session.center_uv = center_uv;
  return surface_decal_patch_ensure(C,
                                    decal.session,
                                    &ob,
                                    surface_map,
                                    decal.session.patch_seed_tri,
                                    decal.session.patch_seed_co,
                                    decal.session.patch_tangent);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Commit, handoff and 2D drawing
 * \{ */

/** Count the patch triangles whose UV footprint touches a tile of one of the targets' images. */
static int editor_decal_patch_tris_in_tiles(const ImageSelectEditorDecal &decal,
                                            const ImageSelectView3DSurfaceMap &surface_map,
                                            const Span<ImagePaintSelectionTarget> targets)
{
  const Span<int3> corner_tris = surface_map.corner_tris.as_span();
  int count = 0;
  for (const SurfaceUnfoldTri &tri : decal.session.patch.tris) {
    if (tri.tri_index < 0 || tri.tri_index >= corner_tris.size()) {
      continue;
    }
    const int corner = corner_tris[tri.tri_index][0];
    if (corner < 0 || corner >= surface_map.uv_map.size()) {
      continue;
    }
    const float2 uv = surface_map.uv_map[corner];
    bool in_tile = false;
    for (const ImagePaintSelectionTarget &target : targets) {
      if (target.image == nullptr) {
        continue;
      }
      for (const ImageTile *tile : ListBaseWrapper<ImageTile>(target.image->tiles)) {
        const float2 origin = image_select_udim_tile_uv_origin(tile->tile_number);
        if (uv.x >= origin.x && uv.x <= origin.x + 1.0f && uv.y >= origin.y &&
            uv.y <= origin.y + 1.0f)
        {
          in_tile = true;
        }
      }
    }
    count += in_tile ? 1 : 0;
  }
  return count;
}

void image_select_editor_decal_handoff_release(SpaceImage *sima)
{
  if (sima == nullptr || sima->runtime == nullptr) {
    return;
  }
  ImageSelectEditorDecalHandoff *&handoff = sima->runtime->paint_select.editor_decal_handoff;
  if (handoff == nullptr) {
    return;
  }
  /* The stamps are ImBufs: #surface_decal_release frees them, the holder's destructor does not. */
  surface_decal_release(handoff->decal.session);
  MEM_delete(handoff);
  handoff = nullptr;
}

void image_select_editor_decal_handoff_store(const PaintSelectFloatingSession &session,
                                             const ImageSelectEditorMapping &mapping)
{
  SpaceImage *sima = session.owner_sima;
  image_select_editor_decal_handoff_release(sima);
  ImageSelect3DPreview *preview = session.preview_3d;
  if (sima == nullptr || sima->runtime == nullptr || preview == nullptr || !preview->decal_mode ||
      preview->decal.session.patch.tris.is_empty())
  {
    return;
  }
  auto *handoff = MEM_new<ImageSelectEditorDecalHandoff>(__func__);
  handoff->decal = std::move(preview->decal);
  handoff->mapping = mapping;
  sima->runtime->paint_select.editor_decal_handoff = handoff;
  IMAGE_SELECT_DECAL_LOG(IMAGE_SELECT_DECAL_TAG_EDITOR
                         "handoff stored: patch tris=%d center uv (%.4f, %.4f) plane offset "
                         "(%.5f, %.5f)\n",
                         int(handoff->decal.session.patch.tris.size()),
                         handoff->decal.session.center_uv.x,
                         handoff->decal.session.center_uv.y,
                         handoff->decal.session.plane_offset.x,
                         handoff->decal.session.plane_offset.y);
}

/** Whether the commit's \a mapping is the one the handoff was shown at. Only the gesture values
 * are compared: the linear part follows the region's zoom and the pivot is re-derived by the exec
 * from the operator's anchor, so neither identifies the gesture. */
static bool editor_decal_mapping_matches(const ImageSelectEditorMapping &a,
                                         const ImageSelectEditorMapping &b)
{
  constexpr float tolerance = 1e-4f;
  return math::distance(a.translation, b.translation) < tolerance &&
         math::distance(a.scale, b.scale) < tolerance &&
         std::abs(a.rotation - b.rotation) < tolerance;
}

bool image_select_editor_decal_commit(bContext *C,
                                      const char *undo_label,
                                      const PaintSelectFloatingSession *session,
                                      const Span<ImagePaintSelectionTarget> targets,
                                      const Span<Vector<SelectionTileFragment>> fragments,
                                      const ImageSelectEditorMapping &mapping)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr || ob->type != OB_MESH || ob->data == nullptr ||
      targets.size() != fragments.size())
  {
    IMAGE_SELECT_DECAL_LOG(IMAGE_SELECT_DECAL_TAG_EDITOR
                           "commit refused: no active mesh object (ob=%d) or targets/fragments "
                           "mismatch (%d vs %d)\n",
                           int(ob != nullptr),
                           int(targets.size()),
                           int(fragments.size()));
    return false;
  }
  if (CTX_data_depsgraph_pointer(C) == nullptr) {
    IMAGE_SELECT_DECAL_LOG(IMAGE_SELECT_DECAL_TAG_EDITOR
                           "commit refused: no depsgraph in the context\n");
    return false;
  }

  /* The decal comes from one of two places, never from a rebuild: the live preview owns the decal
   * the user watched (patch, stamps, the tangent carried along the drag), and the handoff keeps the
   * decal of a Transform session that ended at its confirm. A rebuild from the source would depend
   * on the path taken, so it would land elsewhere than the drag did. */
  SpaceImage *sima = (session != nullptr) ? session->owner_sima : CTX_wm_space_image(C);
  ImageSelect3DPreview *preview = (session != nullptr) ? session->preview_3d : nullptr;
  const bool use_preview = preview != nullptr && preview->decal_mode && preview->decal.object == ob &&
                           preview->decal.session.stamps.size() == targets.size();
  ImageSelectEditorDecalHandoff *handoff = (sima != nullptr && sima->runtime != nullptr) ?
                                               sima->runtime->paint_select.editor_decal_handoff :
                                               nullptr;
  const bool use_handoff = !use_preview && handoff != nullptr && handoff->decal.object == ob &&
                           handoff->decal.session.stamps.size() == targets.size() &&
                           editor_decal_mapping_matches(handoff->mapping, mapping);
  if (!use_preview && !use_handoff) {
    if (handoff != nullptr) {
      IMAGE_SELECT_DECAL_LOG(IMAGE_SELECT_DECAL_TAG_EDITOR
                             "handoff dropped: it does not match this commit (object, targets or "
                             "mapping differ)\n");
      image_select_editor_decal_handoff_release(sima);
    }
    IMAGE_SELECT_DECAL_LOG(IMAGE_SELECT_DECAL_TAG_EDITOR
                           "commit refused: neither a live Decal preview nor a matching handoff; "
                           "the caller falls back to the UV affine\n");
    return false;
  }

  const Mesh &mesh = *id_cast<const Mesh *>(ob->data);
  ImageSelectView3DSurfaceMap local_surface_map;
  ImageSelectEditorDecal local_decal;
  ImageSelectView3DSurfaceMap &surface_map = use_preview ? preview->surface_map : local_surface_map;
  ImageSelectEditorDecal &decal = use_preview ? preview->decal : local_decal;
  if (!surface_map.ensure(*ob, mesh.active_or_default_uv_map_name())) {
    IMAGE_SELECT_DECAL_LOG(IMAGE_SELECT_DECAL_TAG_EDITOR
                           "commit refused: surface map unavailable\n");
    return false;
  }

  if (use_preview) {
    /* The preview skips a destination off the surface and keeps its last valid patch; the commit
     * follows the same rule instead of falling back to the UV affine, which tears a fragment that
     * straddles a seam. */
    const bool on_surface = image_select_editor_decal_update(C, *ob, surface_map, decal, mapping);
    IMAGE_SELECT_DECAL_LOG(IMAGE_SELECT_DECAL_TAG_EDITOR
                           "commit via live preview: center on surface=%d\n",
                           int(on_surface));
  }
  else {
    /* The decal of the ended session, exactly as the drag left it. The runtime slot is emptied:
     * the commit owns the decal from here (and frees it below). */
    local_decal = std::move(handoff->decal);
    image_select_editor_decal_handoff_release(sima);
    IMAGE_SELECT_DECAL_LOG(IMAGE_SELECT_DECAL_TAG_EDITOR
                           "commit via handoff: patch tris=%d center uv (%.4f, %.4f)\n",
                           int(local_decal.session.patch.tris.size()),
                           local_decal.session.center_uv.x,
                           local_decal.session.center_uv.y);
  }
  if (decal.session.patch.tris.is_empty()) {
    IMAGE_SELECT_DECAL_LOG(IMAGE_SELECT_DECAL_TAG_EDITOR "commit refused: empty patch\n");
    if (!use_preview) {
      surface_decal_release(decal.session);
    }
    return false;
  }
  IMAGE_SELECT_DECAL_LOG(IMAGE_SELECT_DECAL_TAG_EDITOR
                         "commit: patch tris=%d radius=%.5f center uv (%.4f, %.4f) "
                         "tris in canvas tiles=%d\n",
                         int(decal.session.patch.tris.size()),
                         decal.session.patch.radius,
                         decal.session.center_uv.x,
                         decal.session.center_uv.y,
                         editor_decal_patch_tris_in_tiles(decal, surface_map, targets));

  surface_decal_commit(C,
                       undo_label,
                       decal.session,
                       surface_map,
                       targets,
                       fragments,
                       [](const Image *ima) { return ima != nullptr; });
  IMAGE_SELECT_DECAL_LOG(IMAGE_SELECT_DECAL_TAG_EDITOR
                         "commit bake: %d target(s) baked through %d patch tris\n",
                         int(targets.size()),
                         int(decal.session.patch.tris.size()));
  if (!use_preview) {
    surface_decal_release(decal.session);
  }
  return true;
}

bool image_select_editor_decal_draw_2d(const bContext *C,
                                       const PaintSelectFloatingSession &session,
                                       const ARegion *region,
                                       const Image *image)
{
  ImageSelectEditorDecalView view;
  if (region == nullptr || !image_select_editor_decal_patch_get(C, session, view)) {
    return false;
  }
  ImageSelect3DPreview *preview = session.preview_3d;
  /* The stamp of the session's own image; any other target only when the image is unknown. */
  int target_i = -1;
  for (const int i : view.targets.index_range()) {
    if (view.targets[i].image == image) {
      target_i = i;
      break;
    }
  }
  if (target_i < 0 || target_i >= view.decal->stamps.size() ||
      view.decal->stamps[target_i] == nullptr)
  {
    return false;
  }
  if (preview->stamp_textures_2d.size() != view.targets.size()) {
    preview->stamp_textures_2d.resize(view.targets.size(), nullptr);
  }
  gpu::Texture *&texture = preview->stamp_textures_2d[target_i];
  if (texture == nullptr) {
    texture = IMB_create_gpu_texture(
        "DecalStamp2D", view.decal->stamps[target_i], true, false, false);
    if (texture != nullptr) {
      GPU_texture_filter_mode(texture, true);
    }
  }
  if (texture == nullptr) {
    return false;
  }

  Vector<float2> uvs;
  Vector<float2> stamp_uvs;
  surface_decal_patch_uv_vertices(*view.decal, *view.surface_map, uvs, stamp_uvs);
  if (uvs.is_empty()) {
    return false;
  }

  GPU_blend(GPU_BLEND_ALPHA);
  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);
  const uint texco = GPU_vertformat_attr_add(format, "texCoord", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_IMAGE_COLOR);
  immUniformColor4f(1.0f, 1.0f, 1.0f, 1.0f);
  /* Outside the stamp the patch stays transparent, like the bake and the 3D preview. */
  immBindTextureSampler("image",
                        texture,
                        {GPU_SAMPLER_FILTERING_LINEAR,
                         GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER,
                         GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER});
  immBegin(GPU_PRIM_TRIS, uint(uvs.size()));
  for (const int64_t i : uvs.index_range()) {
    float sx, sy;
    ui::view2d_view_to_region_fl(&region->v2d, uvs[i].x, uvs[i].y, &sx, &sy);
    immAttr2f(texco, stamp_uvs[i].x, stamp_uvs[i].y);
    immVertex2f(pos, sx, sy);
  }
  immEnd();
  immUnbindProgram();
  GPU_blend(GPU_BLEND_NONE);
  return true;
}

bool image_select_editor_decal_mapping_valid(const bContext *C,
                                             const PaintSelectFloatingSession &session,
                                             const ImageSelectEditorMapping &mapping)
{
  const ImageSelect3DPreview *preview = session.preview_3d;
  if (preview == nullptr || !preview->decal_mode || preview->decal.object == nullptr) {
    return true;
  }
  const Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  if (depsgraph == nullptr) {
    return true;
  }
  /* The same center #editor_decal_update seeds the patch at, so "valid" here is exactly "the patch
   * follows this mapping". */
  const float2 center_uv = mapping.pivot +
                           mapping.linear * (preview->decal.source_center_uv - mapping.pivot) +
                           mapping.translation;
  SurfaceDecalFrame frame;
  if (surface_decal_frame_at_uv(*preview->decal.object,
                                *depsgraph,
                                preview->surface_map,
                                center_uv,
                                preview->decal.tangent,
                                frame))
  {
    return true;
  }
  /* A center in a UV gap is still valid while the stamp overlaps the patch seed. */
  float2 offset;
  return editor_decal_offset_get(preview->decal, mapping, center_uv, offset);
}

bool image_select_editor_decal_patch_get(const bContext *C,
                                         const PaintSelectFloatingSession &session,
                                         ImageSelectEditorDecalView &r_view)
{
  ImageSelect3DPreview *preview = session.preview_3d;
  if (C == nullptr || preview == nullptr || !preview->decal_mode ||
      preview->surface_map.sampler == nullptr || preview->decal.object == nullptr ||
      preview->frame_get == nullptr)
  {
    return false;
  }
  /* The 3D draw refreshes the patch only while a Viewport shows the object; bring it to the
   * current mapping here too (a no-op when it already matches, the patch is reused). A destination
   * off the surface keeps the last valid patch, as in the Viewport. */
  if (preview->frame_get(*preview->session, preview->targets, preview->frame) &&
      preview->frame.fragments.size() == preview->targets.size())
  {
    image_select_editor_decal_update(
        C, *preview->decal.object, preview->surface_map, preview->decal, preview->frame.mapping);
  }
  if (preview->decal.session.patch.tris.is_empty()) {
    return false;
  }
  r_view.decal = &preview->decal.session;
  r_view.surface_map = &preview->surface_map;
  r_view.targets = preview->targets.as_span();
  return true;
}

/** \} */

}  // namespace blender::ed::sculpt_paint
