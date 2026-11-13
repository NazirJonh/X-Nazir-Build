/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Selection masks in the 3D Viewport: anchor/plane mapping, the per-texel rasterizer that
 * projects mesh texels onto the anchor plane and writes the UV-space masks, the shared gesture
 * apply, the all/none/invert and gesture operators, the floating-session slot, and the 3D
 * overlay (marching-ants outline projected onto the surface).
 *
 * The mask storage, the edge policy, the undo capture and the Image-Editor borrow guard are the
 * Image Editor's own (BKE_image_paint_selection.hh + paint_image_select_intern.hh); this file
 * only adds the 3D projection half. See paint_image_select_view3d.hh for the flow.
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

/** Extra rejection depth floor, object units: keeps flat selections usable on tiny scales. */
constexpr float SELECT_VIEW3D_MIN_DEPTH = 1e-4f;
/** Rejection depth along the anchor normal, as a fraction of the gesture's larger half-axis. */
constexpr float SELECT_VIEW3D_MAX_DEPTH_FACTOR = 0.5f;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Small helpers
 * \{ */

namespace image_select_v3d {

/** Object-space corner \a corner (0..7) of an AABB (the local twin of the shapes' helper). */
inline float3 bounds_corner(const Bounds<float3> &bounds, const int corner)
{
  return float3((corner & 1) ? bounds.max.x : bounds.min.x,
                (corner & 2) ? bounds.max.y : bounds.min.y,
                (corner & 4) ? bounds.max.z : bounds.min.z);
}

/** Raw (acceptance-rule-free) projection of a point onto the anchor plane, for bounds culling.
 * False in View mode for a point behind the view, which the caller must not cull. */
inline bool project_raw(const ImageSelectView3DAnchor &anchor,
                        const float3 &co_object,
                        float2 &r_px)
{
  if (anchor.screen) {
    return anchor.project_screen(co_object, r_px);
  }
  const float3 d = co_object - anchor.anchor.co;
  r_px = float2(math::dot(d, anchor.frame.tangent), math::dot(d, anchor.frame.bitangent)) *
         anchor.anchor.px_per_unit;
  return true;
}

struct RasterTLS {
  Vector<int> touched_faces;
};

/** The tile's image buffer for \a iuser pinned to \a tile_number. */
ImBuf *tile_ibuf_get(Image *image, const ImageUser &base_iuser, const int tile_number)
{
  ImageUser iuser = base_iuser;
  iuser.tile = tile_number;
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, &lock);
  BKE_image_release_ibuf(image, ibuf, lock);
  return ibuf;
}

}  // namespace image_select_v3d

/** \} */

/* -------------------------------------------------------------------- */
/** \name Polls
 * \{ */

static bool view3d_object_valid(bContext *C, const Object *ob)
{
  if (ob == nullptr || ob->type != OB_MESH || ob->data == nullptr) {
    return false;
  }
  const Scene *scene = CTX_data_scene(C);
  return scene != nullptr && scene->toolsettings != nullptr;
}

bool image_paint_selection_view3d_poll(bContext *C)
{
  if (CTX_data_mode_enum(C) != CTX_MODE_SCULPT) {
    return false;
  }
  const ARegion *region = CTX_wm_region(C);
  if (region == nullptr || region->regiontype != RGN_TYPE_WINDOW) {
    return false;
  }
  if (CTX_wm_region_view3d(C) == nullptr) {
    return false;
  }
  Object *ob = CTX_data_active_object(C);
  if (!view3d_object_valid(C, ob)) {
    return false;
  }
  /* Resolve once: in Material mode this walks the material nodes. */
  const Vector<ImagePaintSelectionTarget> targets = image_paint_selection_view3d_targets_get(C,
                                                                                            *ob);
  if (targets.is_empty()) {
    return false;
  }
  for (const ImagePaintSelectionTarget &target : targets) {
    const Image *image = target.image;
    if (image == nullptr || !ID_IS_EDITABLE(image) || ID_IS_OVERRIDE_LIBRARY(image)) {
      return false;
    }
  }
  if (image_select_view3d_session_active() != nullptr) {
    return false;
  }
  if (image_select_view3d_canvas_borrowed_elsewhere(C, targets)) {
    return false;
  }
  return true;
}

bool image_paint_selection_view3d_tool_poll(bContext *C)
{
  /* A live session of this Viewport is polled through on purpose: the floating tools' and the
   * gestures' invoke callbacks handle it (re-drag inside the fragment, commit outside). The
   * strict poll below rejects, among others, any live session, so the two cases never overlap. */
  const PaintSelectView3DFloatingSession *session = image_select_view3d_session_active();
  if (session != nullptr) {
    return session->owner_v3d == CTX_wm_view3d(C);
  }
  return image_paint_selection_view3d_poll(C);
}

bool image_paint_selection_view3d_floating_poll(bContext *C)
{
  if (CTX_data_mode_enum(C) != CTX_MODE_SCULPT) {
    return false;
  }
  const PaintSelectView3DFloatingSession *session = image_select_view3d_session_active();
  if (session == nullptr) {
    return false;
  }
  return session->owner_v3d == CTX_wm_view3d(C);
}

bool image_select_view3d_select_tool_active(const bContext *C)
{
  const bToolRef *tref = WM_toolsystem_ref_from_context(C);
  return tref != nullptr && STRPREFIX(tref->idname, "builtin.image_paint_select_");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Targets and undo
 * \{ */

Vector<ImagePaintSelectionTarget> image_paint_selection_view3d_targets_get(bContext *C,
                                                                          Object &ob)
{
  Vector<ImagePaintSelectionTarget> targets;
  Scene *scene = CTX_data_scene(C);
  if (scene == nullptr || scene->toolsettings == nullptr) {
    return targets;
  }
  PaintModeSettings &paint_mode = scene->toolsettings->paint_mode;

  Image *canvas_image = nullptr;
  ImageUser canvas_iuser = {};
  ImageUser *canvas_iuser_ptr = nullptr;
  BKE_paint_canvas_image_get(&paint_mode, &ob, &canvas_image, &canvas_iuser_ptr);
  if (canvas_iuser_ptr != nullptr) {
    canvas_iuser = *canvas_iuser_ptr;
  }

  if (paint_mode.canvas_source != PAINT_CANVAS_SOURCE_MATERIAL) {
    if (canvas_image != nullptr) {
      targets.append({canvas_image, canvas_iuser});
    }
    return targets;
  }

  /* Resolve the brush and the channel visibility through the ACTIVE paint mode. These tools run in
   * Sculpt Mode, whose brush and `visible_material_channels` are their own: reading Image Paint's
   * pair here made the selection target only the maps that happened to be enabled/visible there
   * (in the common case `imapaint`'s brush has no Material Paint settings at all, so the resolver
   * fell back to the single legacy canvas image -- Base Color). Every other channel map the
   * Sculpt brush and the shape tools write then never received a mask, so painting was only
   * clipped on Base Color. */
  Paint *paint = BKE_paint_get_active_from_context(C);
  const Brush *brush = paint != nullptr ? BKE_paint_brush_for_read(paint) : nullptr;
  const BrushMaterialPaint *brush_paint = brush ? brush->material_paint : nullptr;
  const int visible_material_channels = paint != nullptr ? paint->visible_material_channels : 0;
  targets = image_paint_selection_material_targets_get(
      ob, paint_mode, brush_paint, visible_material_channels, canvas_iuser);
  /* A temporarily unresolved Material canvas keeps the single-canvas behavior. */
  if (targets.is_empty() && canvas_image != nullptr) {
    targets.append({canvas_image, canvas_iuser});
    image_paint_selection_owner_bind_active(&ob, canvas_image);
  }
  return targets;
}

bool image_paint_selection_view3d_primary_target_get(bContext *C,
                                                     Object &ob,
                                                     ImagePaintSelectionTarget &r_target)
{
  /* The masks of all targets are identical by construction, so the first resolved target is the
   * one to display; it must come from the same resolver the tools write through. */
  const Vector<ImagePaintSelectionTarget> targets = image_paint_selection_view3d_targets_get(C,
                                                                                            ob);
  if (targets.is_empty()) {
    return false;
  }
  r_target = targets.first();
  return true;
}

void image_paint_selection_view3d_undo_begin(const char *name,
                                             const Span<ImagePaintSelectionTarget> targets)
{
  ED_image_undo_push_begin(name, PaintMode::Sculpt);
  for (const ImagePaintSelectionTarget &target : targets) {
    for (const ImageTile *tile : ListBaseWrapper<ImageTile>(target.image->tiles)) {
      ED_image_undo_capture_selection_mask(target.image, tile->tile_number);
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Anchor and plane mapping
 * \{ */

bool ImageSelectView3DAnchor::project_screen(const float3 &co_object, float2 &r_px) const
{
  const float4 clip = this->screen->projmat * float4(co_object.x, co_object.y, co_object.z, 1.0f);
  if (clip.w <= 1e-6f) {
    return false;
  }
  r_px = (float2(clip.x, clip.y) / clip.w * 0.5f + float2(0.5f)) * this->screen->region_size;
  return true;
}

bool ImageSelectView3DAnchor::project_object_point(const float3 &co_object,
                                                   const float3 &no_object,
                                                   float2 &r_px) const
{
  if (this->screen) {
    /* Front faces only, like the Sculpt Mode gestures' "Front Faces Only". */
    const float3 to_view = this->screen->is_persp ? this->screen->view_origin - co_object :
                                                    -this->screen->view_forward;
    if (math::dot(no_object, to_view) <= 0.0f) {
      return false;
    }
    return this->project_screen(co_object, r_px);
  }
  if (!shape::surface_anchor_hit_accepts(
          anchor.co, frame.normal, co_object, no_object, max_depth))
  {
    return false;
  }
  const float3 d = co_object - anchor.co;
  r_px = float2(math::dot(d, frame.tangent), math::dot(d, frame.bitangent)) * anchor.px_per_unit;
  return true;
}

float3 ImageSelectView3DAnchor::plane_px_to_object(const float2 &px) const
{
  const float2 units = px / std::max(anchor.px_per_unit, 1e-6f);
  return anchor.co + frame.tangent * units.x + frame.bitangent * units.y;
}

/** Solve the affine plane-px -> UV mapping of a triangle: `uv = uv0 + J * (p - p0)`. */
static bool plane_uv_jacobian_from_tri(const float2 &p0,
                                       const float2 &p1,
                                       const float2 &p2,
                                       const float2 &uv0,
                                       const float2 &uv1,
                                       const float2 &uv2,
                                       float2x2 &r_jacobian)
{
  float2x2 edges;
  edges[0] = p1 - p0;
  edges[1] = p2 - p0;
  float2x2 uv_edges;
  uv_edges[0] = uv1 - uv0;
  uv_edges[1] = uv2 - uv0;
  bool success = false;
  const float2x2 inverse = math::invert(edges, success);
  if (!success) {
    return false;
  }
  r_jacobian = uv_edges * inverse;
  return true;
}

bool image_select_view3d_anchor_from_mval(bContext *C,
                                          const int2 &mval,
                                          ImageSelectView3DAnchor &r_anchor,
                                          const bool use_screen_space)
{
  Object *ob = CTX_data_active_object(C);
  const ARegion *region = CTX_wm_region(C);
  const RegionView3D *rv3d = CTX_wm_region_view3d(C);
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  const Scene *scene = CTX_data_scene(C);
  const View3D *v3d = CTX_wm_view3d(C);
  if (ob == nullptr || region == nullptr || rv3d == nullptr || depsgraph == nullptr ||
      scene == nullptr || v3d == nullptr || scene->toolsettings == nullptr)
  {
    return false;
  }
  const Sculpt *sculpt = scene->toolsettings->sculpt;
  if (sculpt == nullptr) {
    return false;
  }

  r_anchor = ImageSelectView3DAnchor{};
  r_anchor.object = ob;

  if (use_screen_space) {
    /* View mode needs no surface under the cursor: the shape is screen-space, so the gesture may
     * start off the mesh. */
    const float4x4 world_to_object_view = math::invert(float4x4(ob->object_to_world()));
    ImageSelectView3DAnchor::ScreenSpace screen;
    screen.projmat = ED_view3d_ob_project_mat_get(rv3d, ob);
    screen.region_size = float2(float(region->winx), float(region->winy));
    screen.is_persp = rv3d->is_persp;
    screen.view_origin = math::transform_point(
        world_to_object_view, float3(rv3d->viewinv[3][0], rv3d->viewinv[3][1], rv3d->viewinv[3][2]));
    screen.view_forward = math::normalize(math::transform_direction(
        world_to_object_view,
        float3(-rv3d->viewinv[2][0], -rv3d->viewinv[2][1], -rv3d->viewinv[2][2])));
    /* Occlusion, like the paint shapes' View projection: the select ID buffer tells which faces
     * are actually seen, so a gesture does not reach through nearer geometry. Left empty (every
     * face passes) in X-Ray, which selects through the surface. */
    if (!XRAY_ENABLED(v3d)) {
      const Main *bmain = CTX_data_main(C);
      ViewLayer *view_layer = CTX_data_view_layer(C);
      if (bmain != nullptr && view_layer != nullptr) {
        BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
        if (Base *base = BKE_view_layer_base_find(view_layer, ob)) {
          Vector<Base *> bases = {base};
          DRW_select_buffer_context_create(depsgraph, bases, SCE_SELECT_FACE);
          rcti region_rect;
          BLI_rcti_init(&region_rect, 0, region->winx, 0, region->winy);
          uint bitmap_num = 0;
          BLI_bitmap *bitmap = DRW_select_buffer_bitmap_from_rect(
              depsgraph,
              const_cast<ARegion *>(region),
              const_cast<View3D *>(v3d),
              &region_rect,
              &bitmap_num);
          if (bitmap != nullptr) {
            const uint offset = DRW_select_buffer_context_offset_for_object_elem(
                depsgraph, ob, SCE_SELECT_FACE);
            const Mesh &mesh = *id_cast<const Mesh *>(ob->data);
            Array<bool> face_visible(mesh.faces_num, false);
            int visible_num = 0;
            for (const int face : IndexRange(mesh.faces_num)) {
              /* The select buffer stores `range_start + face_index`; the bitmap from
               * #DRW_select_buffer_bitmap_from_rect is indexed by `select_id - 1`. Edit Mode's
               * own box selection uses the same `offset - 1`, so subtract it here too or the
               * visibility test is shifted by one face (visible faces read as hidden and vice
               * versa, which is especially visible on multi-object assemblies). */
              const uint index = offset - 1 + uint(face);
              face_visible[face] = index < bitmap_num && BLI_BITMAP_TEST_BOOL(bitmap, index);
              visible_num += int(face_visible[face]);
            }
            MEM_SAFE_DELETE(bitmap);
            /* An ID buffer that saw no face of this object is not a usable occlusion answer (the
             * buffer was not drawn for it), so fail open rather than selecting nothing, ever. */
            if (visible_num > 0) {
              screen.face_visible = std::move(face_visible);
            }
          }
        }
      }
    }
    r_anchor.screen = screen;
    return true;
  }

  /* Same raycast as the 3D paint-shape invoke. resolve_hit_object = false: the selection anchors
   * to the operator's active object, and the cursor side effects must not switch it. */
  ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
  const std::optional<CursorGeometryInfo> hit = cursor_geometry_info_update(*depsgraph,
                                                                            sculpt->paint,
                                                                            sculpt,
                                                                            vc,
                                                                            CTX_data_active_base(C),
                                                                            float2(mval),
                                                                            false,
                                                                            false,
                                                                            nullptr);
  if (!hit) {
    return false;
  }

  const float4x4 object_to_world(ob->object_to_world());
  const float4x4 world_to_object = math::invert(object_to_world);
  const float3 anchor_object = hit->location;
  const float3 normal_object = math::normalize(hit->normal);

  /* Tangent = screen horizontal in object space; the shared frame orthonormalizes it, so the
   * authored plane-px use the same basis the space projection and the overlay use. */
  const float3 view_x_world = float3(
      rv3d->viewinv[0][0], rv3d->viewinv[0][1], rv3d->viewinv[0][2]);
  r_anchor.frame = shape::surface_anchor_frame(
      normal_object, math::transform_direction(world_to_object, view_x_world));
  r_anchor.anchor.co = anchor_object;
  r_anchor.anchor.normal = normal_object;
  r_anchor.anchor.tangent = r_anchor.frame.tangent;
  r_anchor.anchor.max_depth = 0.0f;

  const float3 anchor_world = math::transform_point(object_to_world, anchor_object);
  const float anchor_world_v[3] = {anchor_world.x, anchor_world.y, anchor_world.z};
  const float pixel_size_world = ED_view3d_pixel_size(rv3d, anchor_world_v);
  const float px_per_unit_world = 1.0f / std::max(pixel_size_world, 1e-6f);
  r_anchor.anchor.px_per_unit = shape::surface_anchor_px_per_unit(
      px_per_unit_world, object_to_world, r_anchor.frame.tangent, r_anchor.frame.bitangent);

  /* UV / triangle restoration cache plus the plane-px -> UV Jacobian of the hit triangle. */
  const SculptSession *ss = ob->runtime ? ob->runtime->sculpt_session : nullptr;
  const int hit_face = (ss != nullptr) ? ss->active_face_index.value_or(-1) : -1;
  if (!shape::surface_anchor_cache_fill(*ob, *depsgraph, hit_face, r_anchor.anchor)) {
    return true;
  }
  if (!r_anchor.anchor.has_surface_uv) {
    return true;
  }
  const Mesh &mesh = *id_cast<const Mesh *>(ob->data);
  const StringRefNull uv_name = r_anchor.anchor.surface_uv_map[0] != '\0' ?
                                    StringRefNull(r_anchor.anchor.surface_uv_map) :
                                    StringRefNull(mesh.active_or_default_uv_map_name());
  const bke::AttributeReader<float2> uv_attribute = mesh.attributes().lookup<float2>(
      uv_name, bke::AttrDomain::Corner);
  if (!uv_attribute) {
    return true;
  }
  const VArraySpan<float2> uv_map(*uv_attribute);
  const Span<float3> positions = bke::pbvh::vert_positions_eval(*depsgraph, *ob);
  const Span<int3> corner_tris = mesh.corner_tris();
  const Span<int> corner_verts = mesh.corner_verts();
  if (r_anchor.anchor.tri < 0 || r_anchor.anchor.tri >= corner_tris.size()) {
    return true;
  }
  const int3 tri = corner_tris[r_anchor.anchor.tri];
  float2 tri_px[3];
  for (const int i : IndexRange(3)) {
    const float3 corner_co = positions[corner_verts[tri[i]]];
    const float3 d = corner_co - anchor_object;
    tri_px[i] = float2(math::dot(d, r_anchor.frame.tangent),
                       math::dot(d, r_anchor.frame.bitangent)) *
                r_anchor.anchor.px_per_unit;
  }
  r_anchor.jacobian_valid = plane_uv_jacobian_from_tri(tri_px[0],
                                                       tri_px[1],
                                                       tri_px[2],
                                                       uv_map[tri[0]],
                                                       uv_map[tri[1]],
                                                       uv_map[tri[2]],
                                                       r_anchor.plane_uv_jacobian);
  r_anchor.anchor_uv = r_anchor.anchor.surface_uv;
  return true;
}

bool image_select_view3d_mval_to_plane_px(const ImageSelectView3DAnchor &anchor,
                                          const ARegion &region,
                                          const int2 &mval,
                                          float2 &r_px)
{
  if (anchor.screen) {
    r_px = float2(mval);
    return true;
  }
  const Object *ob = anchor.object;
  if (ob == nullptr) {
    return false;
  }
  const float4x4 object_to_world(ob->object_to_world());
  const float4x4 world_to_object = math::invert(object_to_world);
  /* World plane through the anchor, normal from the inverse-transpose (non-uniform scale safe). */
  const float3 normal_world = math::normalize(
      math::transform_direction(math::transpose(world_to_object), anchor.frame.normal));
  const float3 anchor_world = math::transform_point(object_to_world, anchor.anchor.co);
  const float plane[4] = {normal_world.x,
                          normal_world.y,
                          normal_world.z,
                          -math::dot(normal_world, anchor_world)};
  const float mval_f[2] = {float(mval.x), float(mval.y)};
  float world[3];
  if (!ED_view3d_win_to_3d_on_plane(&region, plane, mval_f, false, world)) {
    /* Ray (near-)parallel to the plane: the caller keeps its last valid point. */
    return false;
  }
  const float3 co_object = math::transform_point(world_to_object,
                                                 float3(world[0], world[1], world[2]));
  const float3 d = co_object - anchor.anchor.co;
  r_px = float2(math::dot(d, anchor.frame.tangent), math::dot(d, anchor.frame.bitangent)) *
         anchor.anchor.px_per_unit;
  return true;
}

float2 image_select_view3d_plane_px_to_uv_delta(const ImageSelectView3DAnchor &anchor,
                                                const float2 &plane_px_delta)
{
  if (!anchor.jacobian_valid) {
    return float2(0.0f);
  }
  return anchor.plane_uv_jacobian * plane_px_delta;
}

bool image_select_view3d_plane_px_to_surface_uv(bContext *C,
                                                const ImageSelectView3DAnchor &anchor,
                                                const float2 &plane_px,
                                                float2 &r_uv)
{
  if (anchor.object == nullptr) {
    return false;
  }
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  if (depsgraph == nullptr) {
    return false;
  }
  const float3 co_object = anchor.plane_px_to_object(plane_px);
  shape::SurfaceAnchor probe = anchor.anchor;
  probe.co = co_object;
  probe.tri = -1;
  probe.tri_corners = int3(-1);
  if (!shape::surface_anchor_cache_fill(*anchor.object, *depsgraph, -1, probe)) {
    return false;
  }
  if (!probe.has_surface_uv) {
    return false;
  }
  r_uv = probe.surface_uv;
  return true;
}

bool image_select_view3d_anchor_from_uv(const bContext *C,
                                        const ImageSelectView3DSurfaceMap &surface_map,
                                        const float2 &uv,
                                        const float3 &prev_tangent,
                                        ImageSelectView3DAnchor &r_anchor)
{
  Object *ob = CTX_data_active_object(C);
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  const RegionView3D *rv3d = CTX_wm_region_view3d(C);
  if (ob == nullptr || depsgraph == nullptr || rv3d == nullptr || ob->type != OB_MESH ||
      ob->data == nullptr)
  {
    return false;
  }
  const Mesh &mesh = *id_cast<const Mesh *>(ob->data);
  const Span<int> corner_verts = mesh.corner_verts();
  /* The evaluated positions, the same source the mask tools and the anchor raycast use. */
  const Span<float3> positions = bke::pbvh::vert_positions_eval(*depsgraph, *ob);
  float3 co, no;
  int tri_index = -1;
  if (!surface_map.sample(corner_verts, positions, uv, co, no, &tri_index)) {
    return false;
  }
  if (tri_index < 0 || tri_index >= surface_map.corner_tris.size()) {
    return false;
  }

  ImageSelectView3DAnchor anchor{};
  anchor.object = ob;
  anchor.frame = shape::surface_anchor_frame(no, prev_tangent);
  anchor.anchor.co = co;
  anchor.anchor.normal = anchor.frame.normal;
  anchor.anchor.tangent = anchor.frame.tangent;
  anchor.anchor.max_depth = 0.0f;

  const float4x4 object_to_world(ob->object_to_world());
  const float3 co_world = math::transform_point(object_to_world, co);
  const float co_world_v[3] = {co_world.x, co_world.y, co_world.z};
  const float pixel_size_world = ED_view3d_pixel_size(rv3d, co_world_v);
  const float px_per_unit_world = 1.0f / std::max(pixel_size_world, 1e-6f);
  anchor.anchor.px_per_unit = shape::surface_anchor_px_per_unit(
      px_per_unit_world, object_to_world, anchor.frame.tangent, anchor.frame.bitangent);

  const int3 tri = surface_map.corner_tris[tri_index];
  float2 tri_px[3];
  for (const int i : IndexRange(3)) {
    const float3 corner_co = positions[corner_verts[tri[i]]];
    const float3 d = corner_co - co;
    tri_px[i] = float2(math::dot(d, anchor.frame.tangent),
                       math::dot(d, anchor.frame.bitangent)) *
                anchor.anchor.px_per_unit;
  }
  anchor.jacobian_valid = plane_uv_jacobian_from_tri(tri_px[0],
                                                     tri_px[1],
                                                     tri_px[2],
                                                     surface_map.uv_map[tri[0]],
                                                     surface_map.uv_map[tri[1]],
                                                     surface_map.uv_map[tri[2]],
                                                     anchor.plane_uv_jacobian);
  anchor.anchor_uv = uv;
  anchor.anchor.surface_uv = uv;
  anchor.anchor.tri = tri_index;
  anchor.anchor.tri_corners = tri;
  anchor.anchor.has_surface_uv = true;
  r_anchor = anchor;
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mask rasterization through the mesh
 * \{ */

bool image_select_view3d_rasterize_masks(bContext *C,
                                         Object &ob,
                                         const Depsgraph &depsgraph,
                                         const Span<ImagePaintSelectionTarget> targets,
                                         const ImageSelectView3DAnchor &anchor,
                                         const rctf &domain_px,
                                         const FunctionRef<bool(const float2 &plane_px)> inside_fn,
                                         const float fill_value,
                                         Vector<int> *r_touched_faces)
{
  bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob);
  if (pbvh == nullptr || pbvh->type() != bke::pbvh::Type::Mesh) {
    return false;
  }
  if (targets.is_empty() || domain_px.xmin > domain_px.xmax || domain_px.ymin > domain_px.ymax) {
    return false;
  }

  const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
  const Span<float3> vert_positions = bke::pbvh::vert_positions_eval(depsgraph, ob);
  const MeshAttributeData attribute_data(mesh);
  FaceSelectionMask face_selection_mask;
  face_selection_mask_build(mesh, face_selection_mask);
  const bool use_face_selection = face_selection_mask.state == FaceSelectionState::Active;
  const Span<int> corner_tri_faces = mesh.corner_tri_faces();
  const Span<bool> hide_poly = attribute_data.hide_poly;

  Vector<ePaintSymmetryFlags> passes;
  const int symmetry_flags = int(mesh_symmetry_xyz_get(ob));
  for (int i = 0; i <= symmetry_flags; i++) {
    if (is_symmetry_iteration_valid(i, symmetry_flags)) {
      passes.append(ePaintSymmetryFlags(i));
    }
  }

  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh->nodes<bke::pbvh::MeshNode>();
  Vector<int> candidates;
  for (const int i : nodes.index_range()) {
    const Bounds<float3> &bounds = nodes[i].bounds();
    rctf node_rct;
    BLI_rctf_init(&node_rct, FLT_MAX, -FLT_MAX, FLT_MAX, -FLT_MAX);
    bool any = false;
    for (int corner = 0; corner < 8; corner++) {
      const float3 p = image_select_v3d::bounds_corner(bounds, corner);
      for (const ePaintSymmetryFlags pass : passes) {
        float2 co;
        if (!image_select_v3d::project_raw(anchor, symmetry_flip(p, pass), co)) {
          /* Behind the view: the node's extent is unbounded, never cull it. */
          BLI_rctf_init(&node_rct, -FLT_MAX, FLT_MAX, -FLT_MAX, FLT_MAX);
          any = true;
          break;
        }
        node_rct.xmin = std::min(node_rct.xmin, co.x);
        node_rct.xmax = std::max(node_rct.xmax, co.x);
        node_rct.ymin = std::min(node_rct.ymin, co.y);
        node_rct.ymax = std::max(node_rct.ymax, co.y);
        any = true;
      }
    }
    rctf isect;
    if (any && BLI_rctf_isect(&node_rct, &domain_px, &isect)) {
      candidates.append(i);
    }
  }
  if (candidates.is_empty()) {
    return false;
  }
  IndexMaskMemory memory;
  const IndexMask node_mask = IndexMask::from_indices(candidates.as_span(), memory);

  Scene *scene = CTX_data_scene(C);
  const StringRef uv_map_name = BKE_paint_canvas_uvmap_name_get(&scene->toolsettings->paint_mode, &ob)
                                    .value_or("");

  std::atomic<bool> any_written{false};

  /* A local mutable copy: #ImageData::from_image and #build_pixels need a mutable ImageUser,
   * and the caller passes a const span. */
  Vector<ImagePaintSelectionTarget> targets_mutable(targets.begin(), targets.end());
  threading::EnumerableThreadSpecific<image_select_v3d::RasterTLS> all_tls;
  /* The first target whose buffers resolve: its masks are rasterized geometrically, the others
   * get exact copies (see #image_paint_selection_masks_copy). */
  Image *source_image = nullptr;
  for (const int target_i : targets_mutable.index_range()) {
    ImagePaintSelectionTarget &target = targets_mutable[target_i];
    Image *image = target.image;
    if (source_image != nullptr) {
      /* Copying avoids re-running the geometric rasterization and #build_pixels (which rebuilds
       * the PBVH pixel data and invalidates the paint cache once per image) for every Material
       * target; the masks are identical by construction. */
      image_paint_selection_masks_copy(*image, *source_image, target.iuser);
      continue;
    }
    std::unique_ptr<paint::image::ImageData> image_data = paint::image::ImageData::from_image(
        image, &target.iuser);
    if (!image_data) {
      continue;
    }
    if (!bke::pbvh::build_pixels(depsgraph, ob, *image, target.iuser, uv_map_name)) {
      continue;
    }
    bke::pbvh::pixels::PixelData &pixel_data = bke::pbvh::pixels::data_get(*pbvh);
    MutableSpan<bke::pbvh::pixels::PixelNode> pixel_nodes = pixel_data.nodes;

    /* Sequential: #fetch_image_buffers inserts into non-thread-safe maps, and every writable
     * mask tile is created once here (which also advances the mask revision) so the parallel
     * pass below only writes pixels through the captured pointers. */
    Map<bke::image::TileNumber, ImBuf *> mask_ptrs;
    node_mask.foreach_index([&](const int i) {
      bke::pbvh::pixels::PixelNode &pixel_node = pixel_nodes[i];
      if (pixel_node.tiles.is_empty()) {
        return;
      }
      paint::image::fetch_image_buffers(*image_data, nodes[i], pixel_node);
      for (const bke::pbvh::pixels::UDIMTilePixels &tile : pixel_node.tiles) {
        const ImBuf *ibuf = image_data->buffers.lookup_default(tile.tile_number, nullptr);
        if (ibuf == nullptr) {
          continue;
        }
        ImBuf *mask = BKE_image_paint_selection_mask_get(image, tile.tile_number, ibuf->x, ibuf->y);
        if (mask != nullptr) {
          mask_ptrs.add(tile.tile_number, mask);
        }
      }
    });

    node_mask.foreach_index(
        [&](const int i) {
          bke::pbvh::pixels::PixelNode &pixel_node = pixel_nodes[i];
          if (pixel_node.tiles.is_empty()) {
            return;
          }
          for (bke::pbvh::pixels::UDIMTilePixels &tile : pixel_node.tiles) {
            const ImBuf *ibuf = image_data->buffers.lookup_default(tile.tile_number, nullptr);
            if (ibuf == nullptr) {
              continue;
            }
            /* Writable pointer captured in the sequential phase above (no revision bump per
             * node, which a fresh mutable lookup would cause inside this parallel pass). */
            ImBuf *mask = mask_ptrs.lookup_default(tile.tile_number, nullptr);
            if (mask == nullptr || mask->x != ibuf->x || mask->y != ibuf->y) {
              continue;
            }
            float *mask_px = mask->float_data_for_write();

            threading::parallel_for(
                tile.pixel_rows.index_range(), 64, [&](const IndexRange rows) {
                  /* Looked up here, not in the outer per-node lambda: this nested #parallel_for
                   * runs on other worker threads, and sharing the outer thread's TLS between them
                   * raced on `touched_faces` (heap corruption / double free). */
                  image_select_v3d::RasterTLS &tls = all_tls.local();
                  Vector<float3> positions;
                  for (const int r : rows) {
                    const bke::pbvh::pixels::PackedPixelRow &pixel_row = tile.pixel_rows[r];
                    const int tri_local = pixel_row.uv_primitive_index;
                    if (tri_local < 0 ||
                        tri_local >= pixel_node.uv_primitives.tri_indices.size())
                    {
                      continue;
                    }
                    const int tri = pixel_node.uv_primitives.tri_indices[tri_local];
                    if (tri < 0 || tri >= pixel_data.vert_tris.size() ||
                        tri >= corner_tri_faces.size())
                    {
                      continue;
                    }
                    const int face = corner_tri_faces[tri];
                    if (!hide_poly.is_empty() && hide_poly[face]) {
                      continue;
                    }
                    if (use_face_selection && !face_selection_mask.select_poly[face]) {
                      continue;
                    }
                    if (anchor.screen && !anchor.screen->face_visible.is_empty() &&
                        !anchor.screen->face_visible[face])
                    {
                      continue;
                    }
                    const int3 tri_verts = pixel_data.vert_tris[tri];
                    if (tri_verts.x >= vert_positions.size() ||
                        tri_verts.y >= vert_positions.size() ||
                        tri_verts.z >= vert_positions.size())
                    {
                      continue;
                    }
                    const float3 v0 = vert_positions[tri_verts.x];
                    const float3 v1 = vert_positions[tri_verts.y];
                    const float3 v2 = vert_positions[tri_verts.z];
                    const float3 tri_normal = math::normalize(math::cross(v1 - v0, v2 - v0));

                    const IndexRange range(0, pixel_row.num_pixels);
                    positions.resize(range.size());
                    paint::image::calc_pixel_row_positions(vert_positions,
                                                           pixel_data.vert_tris,
                                                           pixel_node.uv_primitives.tri_indices,
                                                           pixel_node.uv_primitives.delta_barycentric_coords,
                                                           pixel_row,
                                                           range,
                                                           positions);

                    bool changed = false;
                    for (const int px : range.index_range()) {
                      const float3 position = positions[px];
                      for (const ePaintSymmetryFlags pass : passes) {
                        const float3 mirrored = symmetry_flip(position, pass);
                        const float3 mirrored_normal = symmetry_flip(tri_normal, pass);
                        float2 co;
                        if (!anchor.project_object_point(mirrored, mirrored_normal, co)) {
                          continue;
                        }
                        if (!inside_fn(co)) {
                          continue;
                        }
                        const int x = int(pixel_row.start_image_coordinate.x) + px;
                        const int y = int(pixel_row.start_image_coordinate.y);
                        if (x < 0 || y < 0 || x >= mask->x || y >= mask->y) {
                          continue;
                        }
                        mask_px[int64_t(y) * mask->x + x] = fill_value;
                        changed = true;
                        break;
                      }
                    }
                    if (changed) {
                      if (r_touched_faces != nullptr) {
                        tls.touched_faces.append(face);
                      }
                      any_written.store(true, std::memory_order_relaxed);
                    }
                  }
                });
          }
        },
        exec_mode::grain_size(1));

    source_image = image;
  }

  if (r_touched_faces != nullptr) {
    /* Collect touched faces from all threads. Each face may be added multiple times
     * (once per pixel row), so deduplicate using a Set. */
    Set<int> touched_faces_set;
    for (image_select_v3d::RasterTLS &tls : all_tls) {
      for (const int face : tls.touched_faces) {
        touched_faces_set.add(face);
      }
    }
    for (const int face : touched_faces_set) {
      r_touched_faces->append(face);
    }
  }

  return any_written.load();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name FACE / ISLAND expansion
 * \{ */

namespace image_select_v3d {

/** Rasterize \a faces' full UV footprints (per corner triangle) into every tile mask of \a
 * targets the footprints reach. Pure UV-space work on the original mesh, like the 2D expansion. */
void expand_faces_to_masks(const bContext *C,
                           const Object &ob,
                           const Span<ImagePaintSelectionTarget> targets,
                           const Span<int> faces,
                           const float fill_value)
{
  if (faces.is_empty() || targets.is_empty()) {
    return;
  }
  const Scene *scene = CTX_data_scene(C);
  const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
  const StringRef uv_name = BKE_paint_canvas_uvmap_name_get(
                                &scene->toolsettings->paint_mode, const_cast<Object *>(&ob))
                                .value_or(mesh.active_or_default_uv_map_name());
  const bke::AttributeReader<float2> uv_attribute = mesh.attributes().lookup<float2>(
      uv_name, bke::AttrDomain::Corner);
  if (!uv_attribute) {
    return;
  }
  const VArraySpan<float2> uv_map(*uv_attribute);
  const Span<int3> corner_tris = mesh.corner_tris();
  const Span<int> corner_tri_faces = mesh.corner_tri_faces();
  const OffsetIndices<int> faces_offsets = mesh.faces();

  for (const int face : faces) {
    if (face < 0 || face >= mesh.faces_num) {
      continue;
    }
    /* UV bounds of the face across its corner triangles. */
    rctf face_uv;
    BLI_rctf_init(&face_uv, FLT_MAX, -FLT_MAX, FLT_MAX, -FLT_MAX);
    for (const int tri_i : bke::mesh::face_triangles_range(faces_offsets, face)) {
      const int3 tri = corner_tris[tri_i];
      for (const int i : IndexRange(3)) {
        const float2 uv = uv_map[tri[i]];
        const float xy[2] = {uv.x, uv.y};
        BLI_rctf_do_minmax_v(&face_uv, xy);
      }
    }
    BLI_assert(face_uv.xmin <= face_uv.xmax);

    for (const ImagePaintSelectionTarget &target : targets) {
      Image *image = target.image;
      for (const ImageTile *tile : ListBaseWrapper<ImageTile>(image->tiles)) {
        const float2 tile_origin = BKE_image_get_tile_uv_origin(tile->tile_number);
        rctf tile_uv;
        tile_uv.xmin = tile_origin.x;
        tile_uv.ymin = tile_origin.y;
        tile_uv.xmax = tile_origin.x + 1.0f;
        tile_uv.ymax = tile_origin.y + 1.0f;
        rctf isect;
        if (!BLI_rctf_isect(&face_uv, &tile_uv, &isect)) {
          continue;
        }
        ImBuf *ibuf = tile_ibuf_get(image, target.iuser, tile->tile_number);
        if (ibuf == nullptr) {
          continue;
        }
        ImBuf *mask = BKE_image_paint_selection_mask_get(
            image, tile->tile_number, ibuf->x, ibuf->y);
        if (mask == nullptr) {
          continue;
        }
        float *mask_px = mask->float_data_for_write();
        for (const int tri_i : bke::mesh::face_triangles_range(faces_offsets, face)) {
          const int3 tri = corner_tris[tri_i];
          float2 px_verts[3];
          for (const int i : IndexRange(3)) {
            px_verts[i] = (uv_map[tri[i]] - tile_origin) * float2(float(mask->x), float(mask->y));
          }
          foreach_uv_polygon_pixel(
              Span<float2>(px_verts, 3), mask->x, mask->y, [&](const int x, const int y, bool) {
                mask_px[int64_t(y) * mask->x + x] = fill_value;
                return true;
              });
        }
      }
    }
  }
}

/** UV-island expansion: flood-fill the islands containing \a seed_faces in the original mesh's
 * UV layout (a temporary BMesh, like the 2D expansion) and rasterize them. */
void expand_islands_to_masks(const bContext *C,
                             Object &ob,
                             const Span<ImagePaintSelectionTarget> targets,
                             const Span<int> seed_faces,
                             const float fill_value)
{
  if (seed_faces.is_empty()) {
    return;
  }
  Scene *scene = CTX_data_scene(C);
  const Mesh *mesh = id_cast<const Mesh *>(ob.data);
  if (mesh == nullptr) {
    return;
  }
  const BMAllocTemplate allocsize = BMALLOC_TEMPLATE_FROM_ME(mesh);
  BMeshCreateParams create_params{};
  BMeshFromMeshParams convert_params{};
  convert_params.calc_face_normal = true;
  convert_params.calc_vert_normal = true;
  BMesh *bm = BM_mesh_create(&allocsize, &create_params);
  BM_mesh_bm_from_me(bm, mesh, &convert_params);

  const BMUVOffsets offsets = image_paint_selection_uv_offsets_get(bm, &ob, scene);
  if (offsets.uv >= 0) {
    Array<bool> island_face_tag(bm->totface, false);
    ED_uvedit_uv_islands_tag_from_face_indices(
        scene, bm, offsets, seed_faces, 0, island_face_tag);
    Vector<int> island_faces;
    for (const int i : island_face_tag.index_range()) {
      if (island_face_tag[i]) {
        island_faces.append(i);
      }
    }
    expand_faces_to_masks(C, ob, targets, island_faces, fill_value);
  }
  BM_mesh_free(bm);
}

}  // namespace image_select_v3d

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shared gesture apply
 * \{ */

bool image_select_view3d_gesture_apply(bContext *C,
                                       Object &ob,
                                       const ImageSelectView3DAnchor &anchor_in,
                                       const rctf &domain_px,
                                       const FunctionRef<bool(const float2 &plane_px)> inside_fn,
                                       const PaintSelectionEdgePolicy edge_policy,
                                       const char *undo_name,
                                       const eSelectOp sel_op)
{
  image_paint_selection_mask_map_ensure(C);
  Vector<ImagePaintSelectionTarget> targets = image_paint_selection_view3d_targets_get(C, ob);
  if (targets.is_empty()) {
    return false;
  }
  if (domain_px.xmin > domain_px.xmax || domain_px.ymin > domain_px.ymax) {
    return false;
  }
  /* The expansion level is the shared tool setting (drawn in the tool header), not an operator
   * property. */
  const int expand = CTX_data_scene(C) != nullptr ?
                         CTX_data_scene(C)->toolsettings->imapaint.selection_expand :
                         IMAGE_PAINT_SELECT_EXPAND_PIXELS;

  /* Depth rejection: half the gesture's larger half-axis, in object units, with a floor -- the
   * same rule the 3D paint shapes use for their anchor. */
  ImageSelectView3DAnchor anchor = anchor_in;
  const float2 size_px(domain_px.xmax - domain_px.xmin, domain_px.ymax - domain_px.ymin);
  const float half_object = 0.5f * math::length(size_px) /
                            std::max(anchor.anchor.px_per_unit, 1e-6f);
  anchor.max_depth = std::max(SELECT_VIEW3D_MIN_DEPTH,
                              SELECT_VIEW3D_MAX_DEPTH_FACTOR * half_object);
  anchor.anchor.max_depth = anchor.max_depth;

  const float fill_value = (sel_op == SEL_OP_SUB) ? 0.0f : 1.0f;

  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  if (depsgraph == nullptr) {
    return false;
  }

  image_paint_selection_view3d_undo_begin(undo_name, targets);
  if (sel_op == SEL_OP_SET) {
    for (const ImagePaintSelectionTarget &target : targets) {
      BKE_image_paint_selection_mask_free(target.image);
    }
  }

  Vector<int> touched_faces;
  image_select_view3d_rasterize_masks(
      C, ob, *depsgraph, targets, anchor, domain_px, inside_fn, fill_value, &touched_faces);

  /* FACE expansion replaces the touched faces' footprints with full faces; ISLAND expansion
   * floods the touched faces' UV islands (2D parity: ISLAND does not grow a NEW selection). */
  if (expand == IMAGE_PAINT_SELECT_EXPAND_ISLAND && sel_op != SEL_OP_SET) {
    image_select_v3d::expand_islands_to_masks(C, ob, targets, touched_faces, fill_value);
  }
  else if (expand == IMAGE_PAINT_SELECT_EXPAND_FACE) {
    image_select_v3d::expand_faces_to_masks(C, ob, targets, touched_faces, fill_value);
  }

  for (const ImagePaintSelectionTarget &target : targets) {
    BKE_image_paint_selection_edge_policy_set(target.image, edge_policy);
  }
  image_paint_selection_targets_update(C, targets);
  ED_image_undo_push_end();
  return true;
}

/** Simple click on a selection gesture: deselect everything (2D parity). */
static void view3d_apply_deselect(bContext *C, Object &ob)
{
  image_paint_selection_mask_map_ensure(C);
  Vector<ImagePaintSelectionTarget> targets = image_paint_selection_view3d_targets_get(C, ob);
  bool any = false;
  for (const ImagePaintSelectionTarget &target : targets) {
    any = any || BKE_image_paint_selection_mask_has_any(target.image);
  }
  if (!any) {
    return;
  }
  image_paint_selection_view3d_undo_begin("Deselect", targets);
  for (const ImagePaintSelectionTarget &target : targets) {
    BKE_image_paint_selection_mask_free(target.image);
  }
  image_paint_selection_targets_update(C, targets);
  ED_image_undo_push_end();
}

void image_select_view3d_deselect(bContext *C, Object &ob)
{
  view3d_apply_deselect(C, ob);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name All / None / Invert operators
 * \{ */

static wmOperatorStatus view3d_select_all_exec(bContext *C, wmOperator * /*op*/)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr || !image_paint_selection_view3d_poll(C)) {
    return OPERATOR_CANCELLED;
  }
  image_paint_selection_mask_map_ensure(C);
  Vector<ImagePaintSelectionTarget> targets = image_paint_selection_view3d_targets_get(C, *ob);
  if (targets.is_empty()) {
    return OPERATOR_CANCELLED;
  }
  image_paint_selection_view3d_undo_begin("Select All", targets);
  for (const ImagePaintSelectionTarget &target : targets) {
    for (const ImageTile *tile : ListBaseWrapper<ImageTile>(target.image->tiles)) {
      ImBuf *ibuf = image_select_v3d::tile_ibuf_get(
          target.image, target.iuser, tile->tile_number);
      if (ibuf == nullptr) {
        continue;
      }
      BKE_image_paint_selection_mask_fill(target.image, tile->tile_number, 1.0f);
    }
  }
  const PaintSelectionEdgePolicy hard = BKE_image_paint_selection_edge_policy_hard();
  for (const ImagePaintSelectionTarget &target : targets) {
    BKE_image_paint_selection_edge_policy_set(target.image, hard);
  }
  image_paint_selection_targets_update(C, targets);
  ED_image_undo_push_end();
  return OPERATOR_FINISHED;
}

static wmOperatorStatus view3d_select_none_exec(bContext *C, wmOperator * /*op*/)
{
  Object *ob = CTX_data_active_object(C);
  /* Bound to a plain click of the selection tools: a floating fragment is committed first, as the
   * gestures do, so the click never leaves a stale session behind. */
  if (image_select_view3d_session_active() != nullptr) {
    image_select_view3d_session_end(C, true);
  }
  if (ob == nullptr || !image_paint_selection_view3d_poll(C)) {
    return OPERATOR_CANCELLED;
  }
  image_paint_selection_mask_map_ensure(C);
  Vector<ImagePaintSelectionTarget> targets = image_paint_selection_view3d_targets_get(C, *ob);
  bool any = false;
  for (const ImagePaintSelectionTarget &target : targets) {
    any = any || BKE_image_paint_selection_mask_has_any(target.image);
  }
  if (!any) {
    return OPERATOR_CANCELLED;
  }
  image_paint_selection_view3d_undo_begin("Deselect", targets);
  for (const ImagePaintSelectionTarget &target : targets) {
    BKE_image_paint_selection_mask_free(target.image);
  }
  image_paint_selection_targets_update(C, targets);
  ED_image_undo_push_end();
  return OPERATOR_FINISHED;
}

static wmOperatorStatus view3d_select_invert_exec(bContext *C, wmOperator * /*op*/)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr || !image_paint_selection_view3d_poll(C)) {
    return OPERATOR_CANCELLED;
  }
  image_paint_selection_mask_map_ensure(C);
  Vector<ImagePaintSelectionTarget> targets = image_paint_selection_view3d_targets_get(C, *ob);
  if (targets.is_empty()) {
    return OPERATOR_CANCELLED;
  }
  image_paint_selection_view3d_undo_begin("Invert", targets);
  for (const ImagePaintSelectionTarget &target : targets) {
    for (const ImageTile *tile : ListBaseWrapper<ImageTile>(target.image->tiles)) {
      if (BKE_image_paint_selection_mask_lookup(target.image, tile->tile_number) != nullptr) {
        BKE_image_paint_selection_mask_invert(target.image, tile->tile_number);
      }
    }
  }
  image_paint_selection_targets_update(C, targets);
  ED_image_undo_push_end();
  return OPERATOR_FINISHED;
}

static void view3d_select_basic_props(wmOperatorType *ot)
{
  ot->flag = OPTYPE_REGISTER;
  ot->poll = image_paint_selection_view3d_poll;
}

void PAINT_OT_image_select_view3d_all(wmOperatorType *ot)
{
  ot->name = "All";
  ot->idname = "PAINT_OT_image_select_view3d_all";
  ot->description = "Select everything on the paint canvas";
  ot->exec = view3d_select_all_exec;
  view3d_select_basic_props(ot);
}

void PAINT_OT_image_select_view3d_none(wmOperatorType *ot)
{
  ot->name = "None";
  ot->idname = "PAINT_OT_image_select_view3d_none";
  ot->description = "Deselect everything on the paint canvas";
  ot->exec = view3d_select_none_exec;
  view3d_select_basic_props(ot);
  ot->poll = image_paint_selection_view3d_tool_poll;
}

void PAINT_OT_image_select_view3d_invert(wmOperatorType *ot)
{
  ot->name = "Invert";
  ot->idname = "PAINT_OT_image_select_view3d_invert";
  ot->description = "Invert the selection on the paint canvas";
  ot->exec = view3d_select_invert_exec;
  view3d_select_basic_props(ot);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Floating session slot and canvas borrow
 * \{ */

namespace image_select_v3d {

/** One floating selection tool at a time, across all Viewports (the borrow guard makes a second
 * session on the same canvas impossible anyway). */
PaintSelectView3DFloatingSession *g_view3d_session = nullptr;

void borrow_images_set(const PaintSelectView3DFloatingSession &session,
                       const Span<Image *> images,
                       const void *borrower)
{
  for (Image *image : images) {
    if (image == nullptr || image->runtime == nullptr) {
      continue;
    }
    /* A file load / memfile undo may have freed and re-created this image while the session
     * floated: touching its runtime then would be a dangling write (and the re-created image's
     * fresh runtime carries no borrow anyway, so skipping is exactly right). */
    if (!image_select_view3d_image_is_alive(session, image)) {
      continue;
    }
    image->runtime->paint_selection_borrowed_by = borrower;
  }
}

}  // namespace image_select_v3d

PaintSelectView3DFloatingSession *image_select_view3d_session_active()
{
  return image_select_v3d::g_view3d_session;
}

bool image_select_view3d_image_is_alive(const PaintSelectView3DFloatingSession &session,
                                        const Image *image)
{
  if (image == nullptr || session.bmain == nullptr) {
    return false;
  }
  /* Compare pointers only: a freed image must not be dereferenced to read its session UID. */
  for (const Image &candidate : session.bmain->images) {
    if (&candidate == image) {
      return true;
    }
  }
  return false;
}

void image_select_view3d_session_set(bContext *C,
                                     View3D *owner_v3d,
                                     PaintSelectView3DFloatingSession *session)
{
  BLI_assert(image_select_v3d::g_view3d_session == nullptr);
  BLI_assert(session != nullptr && owner_v3d != nullptr);
  session->owner_v3d = owner_v3d;
  session->owner_region = CTX_wm_region(C);
  session->owner_region_type =
      CTX_wm_region(C) != nullptr ? CTX_wm_region(C)->runtime->type : nullptr;
  session->bmain = CTX_data_main(C);
  image_select_v3d::g_view3d_session = session;
  image_select_v3d::borrow_images_set(*session, session->borrowed_images, owner_v3d);
}

void image_select_view3d_floating_draw_handle_clear(PaintSelectView3DFloatingSession &session)
{
  if (session.draw_handle != nullptr && session.owner_region_type != nullptr) {
    ED_region_draw_cb_exit(session.owner_region_type, session.draw_handle);
    session.draw_handle = nullptr;
  }
}

bool image_select_view3d_session_end(bContext *C, const bool commit)
{
  PaintSelectView3DFloatingSession *session = image_select_v3d::g_view3d_session;
  if (session == nullptr) {
    return false;
  }
  image_select_v3d::g_view3d_session = nullptr;
  /* Un-borrow before the tool writes its final result, so the commit is an ordinary selection
   * edit again (the borrow only blocks foreign edits, not the owner's own commit). */
  image_select_v3d::borrow_images_set(*session, session->borrowed_images, nullptr);
  if (commit) {
    session->commit(C);
  }
  else {
    session->cancel(C);
  }
  MEM_delete(session);
  if (C != nullptr) {
    WM_event_add_notifier(C, NC_WINDOW, nullptr);
  }
  return true;
}

void image_select_view3d_space_free(View3D *v3d)
{
  PaintSelectView3DFloatingSession *session = image_select_v3d::g_view3d_session;
  if (session == nullptr || session->owner_v3d != v3d) {
    return;
  }
  image_select_v3d::g_view3d_session = nullptr;
  image_select_v3d::borrow_images_set(*session, session->borrowed_images, nullptr);
  session->cancel(nullptr);
  MEM_delete(session);
}

bool image_select_view3d_canvas_borrowed_elsewhere(bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr) {
    return false;
  }
  return image_select_view3d_canvas_borrowed_elsewhere(
      C, image_paint_selection_view3d_targets_get(C, *ob));
}

bool image_select_view3d_canvas_borrowed_elsewhere(bContext *C,
                                                   const Span<ImagePaintSelectionTarget> targets)
{
  const void *borrower = CTX_wm_view3d(C);
  for (const ImagePaintSelectionTarget &target : targets) {
    if (target.image == nullptr || target.image->runtime == nullptr) {
      continue;
    }
    const void *borrowed_by = target.image->runtime->paint_selection_borrowed_by;
    if (borrowed_by != nullptr && borrowed_by != borrower) {
      return true;
    }
  }
  return false;
}

/* -------------------------------------------------------------------- */
/** \name Shared floating-session operators
 *
 * One confirm / cancel / undo-step operator per session instead of per tool: they dispatch
 * through the live session's virtual #PaintSelectView3DFloatingSession::commit / #cancel /
 * #undo_step, whichever tool is running.
 * \{ */

static bool image_select_view3d_floating_session_poll(bContext *C)
{
  const PaintSelectView3DFloatingSession *session = image_select_view3d_session_active();
  return session != nullptr && session->owner_v3d == CTX_wm_view3d(C);
}

static wmOperatorStatus image_select_view3d_floating_confirm_exec(bContext *C,
                                                                  wmOperator * /*op*/)
{
  if (!image_select_view3d_floating_session_poll(C)) {
    return OPERATOR_CANCELLED;
  }
  image_select_view3d_session_end(C, true);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus image_select_view3d_floating_cancel_exec(bContext *C,
                                                                 wmOperator * /*op*/)
{
  if (!image_select_view3d_floating_session_poll(C)) {
    return OPERATOR_CANCELLED;
  }
  image_select_view3d_session_end(C, false);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus image_select_view3d_floating_undo_step_exec(bContext *C,
                                                                    wmOperator * /*op*/)
{
  if (!image_select_view3d_floating_session_poll(C)) {
    return OPERATOR_CANCELLED;
  }
  PaintSelectView3DFloatingSession *session = image_select_view3d_session_active();
  if (!session->undo_step()) {
    /* Nothing left to undo inside the session: Ctrl+Z ends it with a cancel. */
    image_select_view3d_session_end(C, false);
  }
  return OPERATOR_FINISHED;
}

void PAINT_OT_image_select_view3d_floating_confirm(wmOperatorType *ot)
{
  ot->name = "Confirm Floating Selection";
  ot->idname = "PAINT_OT_image_select_view3d_floating_confirm";
  ot->description = "Apply the floating selection edit and end the session";

  ot->exec = image_select_view3d_floating_confirm_exec;
  ot->poll = image_select_view3d_floating_session_poll;
  ot->flag = 0;
}

void PAINT_OT_image_select_view3d_floating_cancel(wmOperatorType *ot)
{
  ot->name = "Cancel Floating Selection";
  ot->idname = "PAINT_OT_image_select_view3d_floating_cancel";
  ot->description = "Restore the pre-session state and end the floating selection session";

  ot->exec = image_select_view3d_floating_cancel_exec;
  ot->poll = image_select_view3d_floating_session_poll;
  ot->flag = 0;
}

void PAINT_OT_image_select_view3d_floating_undo_step(wmOperatorType *ot)
{
  ot->name = "Undo Floating Selection Step";
  ot->idname = "PAINT_OT_image_select_view3d_floating_undo_step";
  ot->description =
      "Revert the last drag of the floating selection; with nothing left, end the session";

  ot->exec = image_select_view3d_floating_undo_step_exec;
  ot->poll = image_select_view3d_floating_session_poll;
  ot->flag = 0;
}

/** Cycle the on-surface preview between the canvas images of the live session. */
static wmOperatorStatus image_select_view3d_preview_channel_exec(bContext *C,
                                                                 wmOperator * /*op*/)
{
  if (!image_select_view3d_floating_session_poll(C)) {
    return OPERATOR_CANCELLED;
  }
  PaintSelectView3DFloatingSession *session = image_select_view3d_session_active();
  session->preview_channel_cycle();
  if (session->owner_region != nullptr) {
    ED_region_tag_redraw(session->owner_region);
  }
  return OPERATOR_FINISHED;
}

void PAINT_OT_image_select_view3d_preview_channel(wmOperatorType *ot)
{
  ot->name = "Cycle Selection Preview Channel";
  ot->idname = "PAINT_OT_image_select_view3d_preview_channel";
  ot->description =
      "Cycle which of the material's paint canvases (base color, roughness, ...) the floating "
      "fragment preview shows on the surface";

  ot->exec = image_select_view3d_preview_channel_exec;
  ot->poll = image_select_view3d_floating_session_poll;
  ot->flag = 0;
}

/** \} */

/** \} */

}  // namespace blender::ed::sculpt_paint

/* Public ED_ API (called from space_view3d) */
namespace blender {

void ED_paint_image_select_view3d_space_free(View3D *v3d)
{
  ed::sculpt_paint::image_select_view3d_space_free(v3d);
}

void ED_paint_image_select_view3d_draw_cb_register(ARegionType *art)
{
  ED_region_draw_cb_activate(
      art, ed::sculpt_paint::image_select_view3d_region_draw, nullptr, REGION_DRAW_POST_VIEW);
}

void ED_paint_image_select_view3d_overlay_free()
{
  ed::sculpt_paint::image_select_view3d_overlay_free();
}

}  // namespace blender

