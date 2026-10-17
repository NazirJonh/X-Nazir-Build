/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Operators of the map-driven mask filters, all acting on the active mask item of the active
 * material so they can be started from the Properties editor:
 * - Color ID pick: while it runs the Image Editor and the 3D Viewport show the ID Map flat, with the
 *   area the colours match lit and the rest dimmed; a click adds the colour under the cursor.
 */

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <optional>
#include <string>
#include <memory>
#include <utility>

#include "BLI_array.hh"
#include "BLI_function_ref.hh"
#include "BLI_listbase.h"
#include "BLI_listbase_iterator.hh"
#include "BLI_map.hh"
#include "BLI_math_base.h"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_vector.hh"
#include "BLI_virtual_array.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_layer_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_view3d_types.h"

#include "BKE_attribute.hh"
#include "BKE_bvhutils.hh"
#include "BKE_context.hh"
#include "BKE_customdata.hh"
#include "BKE_global.hh"
#include "BKE_image.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_sample.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_target.hh"
#include "BKE_paint_material_composite.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"

#include "bmesh.hh"

#include "DEG_depsgraph.hh"

#include "GPU_immediate.hh"
#include "GPU_state.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_image.hh"
#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_select_utils.hh"
#include "ED_space_api.hh"
#include "ED_view3d.hh"

#include "../paint_intern.hh"
#include "../selection/paint_face_selection_mask.hh"
#include "../selection/paint_image_select_intern.hh"
#include "paint_image_uv_geom.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Target
 * \{ */

/** The active mask item of the active material when it is a map-coverage item of \a filter. */
static bool coverage_target_get(bContext *C,
                                const eMaterialPaintLayerFilter filter,
                                Material **r_ma,
                                MaterialPaintLayer **r_item)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr) {
    return false;
  }
  Material *ma = BKE_paint_layers_active_material_get(ob);
  if (ma == nullptr) {
    return false;
  }
  MaterialPaintLayer *item = BKE_paint_layers_active_mask_item_get(*ma);
  if (item == nullptr || !BKE_paint_layers_is_map_coverage(*item) || item->kind_subtype != filter) {
    return false;
  }
  *r_ma = ma;
  *r_item = item;
  return true;
}

/** The Image the item samples: the one its structural Base Color record holds, or null. */
static Image *coverage_item_image(const MaterialPaintLayer &item)
{
  return BKE_paint_layers_map_coverage_image_get(item);
}

/** The Image Editor region under \a event, or null; \a r_sima is its space. */
static ARegion *image_region_at(bContext *C, const wmEvent *event, SpaceImage **r_sima)
{
  bScreen *screen = CTX_wm_screen(C);
  ScrArea *area = (screen != nullptr) ? BKE_screen_find_area_xy(screen, SPACE_IMAGE, event->xy) :
                                        nullptr;
  ARegion *region = (area != nullptr) ? BKE_area_find_region_xy(area, RGN_TYPE_WINDOW, event->xy) :
                                        nullptr;
  if (region == nullptr) {
    return nullptr;
  }
  *r_sima = static_cast<SpaceImage *>(area->spacedata.first);
  return region;
}

/** UV coordinates under the cursor of \a region. */
static float2 region_event_uv(const ARegion &region, const wmEvent *event)
{
  float uv[2];
  ui::view2d_region_to_view(&region.v2d,
                            float(event->xy[0] - region.winrct.xmin),
                            float(event->xy[1] - region.winrct.ymin),
                            &uv[0],
                            &uv[1]);
  return float2(uv[0], uv[1]);
}

/** The 3D Viewport window region under \a event, or null. */
static ARegion *view3d_region_at(bContext *C, const wmEvent *event)
{
  bScreen *screen = CTX_wm_screen(C);
  ScrArea *area = (screen != nullptr) ? BKE_screen_find_area_xy(screen, SPACE_VIEW3D, event->xy) :
                                        nullptr;
  return (area != nullptr) ? BKE_area_find_region_xy(area, RGN_TYPE_WINDOW, event->xy) : nullptr;
}

/**
 * The UV map the paint canvas uses, as the island detection reads it; the mesh's own default
 * otherwise.
 */
static std::string canvas_uv_name_get(const Scene &scene, Object &ob, const Mesh &mesh)
{
  std::string uv_name;
  if (const std::optional<StringRef> canvas_uv = BKE_paint_canvas_uvmap_name_get(
          &scene.toolsettings->paint_mode, &ob))
  {
    uv_name = std::string(*canvas_uv);
  }
  if (uv_name.empty() || !mesh.attributes().contains(uv_name)) {
    uv_name = std::string(mesh.active_or_default_uv_map_name());
  }
  return uv_name;
}

static void mask_filter_redraw_all(bContext *C)
{
  if (bScreen *screen = CTX_wm_screen(C)) {
    for (ScrArea &area : screen->areabase) {
      if (ELEM(area.spacetype, SPACE_IMAGE, SPACE_VIEW3D, SPACE_PROPERTIES)) {
        ED_area_tag_redraw(&area);
      }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Map display swap
 *
 * While a picker runs the Image Editor shows the picker's image and every 3D Viewport is in flat
 * solid shading with textures (Workbench then draws the same image on the mesh, see
 * #ED_paint_mask_filter_preview_image_get). Everything is put back when the picker ends.
 * \{ */

static bool image_is_alive(const Main &bmain, const Image *image);

struct DisplaySwap {
  struct ImageSlot {
    SpaceImage *sima;
    Image *image;
  };
  struct ShadingSlot {
    View3D *v3d;
    decltype(View3DShading::type) type;
    decltype(View3DShading::light) light;
    decltype(View3DShading::color_type) color_type;
  };
  Vector<ImageSlot> images;
  Vector<ShadingSlot> shadings;
};

static void display_swap_apply(bContext *C, DisplaySwap &swap, Image *show, const bool flat_3d)
{
  bScreen *screen = CTX_wm_screen(C);
  Main *bmain = CTX_data_main(C);
  if (screen == nullptr) {
    return;
  }
  for (ScrArea &area : screen->areabase) {
    if (area.spacetype == SPACE_IMAGE && show != nullptr) {
      SpaceImage *sima = static_cast<SpaceImage *>(area.spacedata.first);
      swap.images.append({sima, sima->image});
      ED_space_image_set(bmain, sima, show, false);
    }
    else if (area.spacetype == SPACE_VIEW3D && flat_3d) {
      View3D *v3d = static_cast<View3D *>(area.spacedata.first);
      swap.shadings.append({v3d, v3d->shading.type, v3d->shading.light, v3d->shading.color_type});
      v3d->shading.type = OB_SOLID;
      v3d->shading.light = V3D_LIGHTING_FLAT;
      v3d->shading.color_type = V3D_SHADING_TEXTURE_COLOR;
    }
  }
}

/** Put back what #display_swap_apply changed, skipping spaces that closed in the meantime. */
static void display_swap_restore(bContext *C, DisplaySwap &swap)
{
  bScreen *screen = CTX_wm_screen(C);
  Main *bmain = CTX_data_main(C);
  if (screen != nullptr) {
    for (ScrArea &area : screen->areabase) {
      if (area.spacetype == SPACE_IMAGE) {
        for (const DisplaySwap::ImageSlot &slot : swap.images) {
          if (slot.sima == area.spacedata.first) {
            /* An image freed in the meantime (undo) is not put back. */
            ED_space_image_set(
                bmain, slot.sima, image_is_alive(*bmain, slot.image) ? slot.image : nullptr, false);
          }
        }
      }
      else if (area.spacetype == SPACE_VIEW3D) {
        for (const DisplaySwap::ShadingSlot &slot : swap.shadings) {
          if (slot.v3d == area.spacedata.first) {
            slot.v3d->shading.type = slot.type;
            slot.v3d->shading.light = slot.light;
            slot.v3d->shading.color_type = slot.color_type;
          }
        }
      }
    }
  }
  swap.images.clear();
  swap.shadings.clear();
}

/**
 * The image the running picker shows flat in the 3D Viewport; null when none runs. It is a global
 * because the Workbench engine reads it with no operator in reach, and only one modal picker runs at
 * a time. Never read it directly: #ED_paint_mask_filter_preview_image_get checks that it still
 * exists.
 */
static Image *g_picker_image = nullptr;

/**
 * Whether \a image still is a data-block of \a bmain. A picker's preview is created in main, so an
 * undo or a file load can free it under the session; the pointer is never trusted past this test.
 * It only scans while a picker runs, as a null \a image answers at once.
 */
static bool image_is_alive(const Main &bmain, const Image *image)
{
  return image != nullptr && BLI_findindex(&bmain.images, image) != -1;
}

Image *ED_paint_mask_filter_preview_image_get(const Main &bmain)
{
  return image_is_alive(bmain, g_picker_image) ? g_picker_image : nullptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Color ID pick
 * \{ */

/** Largest side of the preview image: the match is recomputed per hover, so it stays small. */
constexpr int ID_PREVIEW_MAX_SIDE = 1024;

struct IdColorPickSession {
  /** The database the preview lives in, for the liveness test of #image_is_alive. */
  Main *bmain = nullptr;
  Image *source = nullptr;
  /** The ID Map decoded to straight scene-linear float RGBA, tile 1001; the pixels compared. */
  ImBuf *decoded = nullptr;
  /** The generated image the editors show: the map with what the colours match lit, the rest dim. */
  Image *preview = nullptr;
  int preview_width = 0;
  int preview_height = 0;
  DisplaySwap swap;
  float hover[3] = {0.0f, 0.0f, 0.0f};
  bool has_hover = false;
  int added = 0;
  /** The output polarity the preview was drawn for: redrawn when the user flips it. */
  bool inverted = false;
  /** The Gradient output scale the preview was drawn for: redrawn when the user moves it. */
  float output = 1.0f;
  /** Whether the output was drawn with a transparent background, for the same refresh test. */
  bool output_alpha = false;
  /** Whether the preview was drawn for Advanced mode, for the same refresh test. */
  bool advanced = false;
};

/**
 * Redraw the preview: every texel the item's enabled colours (and the colour under the cursor, when
 * there is one) match shows the map's own colour, the others a dimmed one.
 */
static void id_color_preview_refresh(IdColorPickSession &session, const MaterialPaintLayer &item)
{
  Vector<PaintMaterialCompositeIdColor> colors;
  const bool advanced = BKE_paint_layers_mask_item_is_color_id_advanced(item);
  for (const MaterialPaintLayerIdColor &color : item.id_colors) {
    PaintMaterialCompositeIdColor entry;
    copy_v3_v3(entry.color, color.color);
    entry.tolerance = color.tolerance;
    entry.own_tolerance = advanced;
    entry.enabled = (color.flag & MA_PAINT_LAYER_ID_COLOR_ENABLED) != 0;
    colors.append(entry);
  }
  if (session.has_hover) {
    PaintMaterialCompositeIdColor entry;
    copy_v3_v3(entry.color, session.hover);
    /* The colour under the cursor is only a preview candidate: in Advanced it uses the hidden
     * shared tolerance, so only stored colours carry their own. */
    entry.tolerance = BKE_paint_layers_coverage_tolerance_get(item);
    entry.own_tolerance = advanced;
    colors.append(entry);
  }
  const float tolerance = BKE_paint_layers_coverage_tolerance_get(item);
  const float softness = advanced ? 0.0f : BKE_paint_layers_coverage_softness_get(item);
  const bool inverted = BKE_paint_layers_mask_item_is_inverted(item);
  const float output = BKE_paint_layers_mask_item_output_value(item);
  const bool output_alpha = BKE_paint_layers_mask_item_is_alpha_background(item);
  session.inverted = inverted;
  session.output = output;
  session.output_alpha = output_alpha;
  session.advanced = advanced;
  if (!image_is_alive(*session.bmain, session.preview)) {
    return;
  }
  void *lock = nullptr;
  ImBuf *dst = BKE_image_acquire_ibuf(session.preview, nullptr, &lock);
  if (dst == nullptr) {
    return;
  }
  uchar *out = dst->byte_data_for_write();
  const float *src = session.decoded->float_data();
  const int sw = session.decoded->x;
  const int sh = session.decoded->y;
  for (int y = 0; y < session.preview_height; y++) {
    const int sy = std::min(int(int64_t(y) * sh / session.preview_height), sh - 1);
    for (int x = 0; x < session.preview_width; x++) {
      const int sx = std::min(int(int64_t(x) * sw / session.preview_width), sw - 1);
      const float *texel = src + (int64_t(sy) * sw + sx) * 4;
      const float match = colors.is_empty() ?
                              0.0f :
                              BKE_paint_material_coverage_match(MA_PAINT_LAYER_FILTER_COLOR_ID,
                                                                texel,
                                                                tolerance,
                                                                softness,
                                                                colors.as_span());
      /* Lit is what the mask keeps: the matched area, or with the output flipped, everything else.
       * Gradient scales that by the item's standard value. With no colour to light yet the map
       * shows as it is: dimming everything would read as black. */
      const float kept = (inverted ? 1.0f - match : match) * output;
      const float shade = colors.is_empty() ? 1.0f : 0.25f + 0.75f * kept;
      uchar *pixel = out + (int64_t(y) * session.preview_width + x) * 4;
      for (int c = 0; c < 3; c++) {
        pixel[c] = uchar(std::clamp(texel[c] * shade, 0.0f, 1.0f) * 255.0f + 0.5f);
      }
      /* Alpha Background: the kept coverage is the alpha, the rest transparent. */
      pixel[3] = output_alpha ? uchar(std::clamp(kept, 0.0f, 1.0f) * 255.0f + 0.5f) : 255;
    }
  }
  dst->userflags |= IB_DISPLAY_BUFFER_INVALID;
  BKE_image_mark_dirty(session.preview, dst);
  BKE_image_partial_update_mark_full_update(session.preview);
  BKE_image_release_ibuf(session.preview, dst, lock);
}

static void id_color_pick_end(bContext *C, wmOperator *op)
{
  IdColorPickSession *session = static_cast<IdColorPickSession *>(op->customdata);
  if (session != nullptr) {
    g_picker_image = nullptr;
    /* The editors show the user's own images again before the preview they displayed goes. */
    display_swap_restore(C, session->swap);
    if (session->decoded != nullptr) {
      IMB_freeImBuf(session->decoded);
    }
    if (image_is_alive(*session->bmain, session->preview)) {
      BKE_id_free(session->bmain, session->preview);
    }
    MEM_delete(session);
    op->customdata = nullptr;
  }
  WM_cursor_modal_restore(CTX_wm_window(C));
  ED_workspace_status_text(C, nullptr);
  mask_filter_redraw_all(C);
}

/**
 * The UV under the cursor of the 3D Viewport at \a event, on the first face of the active mesh the
 * cursor ray hits. False when the cursor is off the mesh, or the mesh has no such UV map.
 *
 * The original mesh is used, as the island picker does: the ID Map is laid out by the UVs the user
 * authored, and the mesh the viewport shows flat while picking is that same layout.
 */
static bool view3d_event_uv(bContext *C, const wmEvent *event, float2 &r_uv)
{
  bScreen *screen = CTX_wm_screen(C);
  ScrArea *area = (screen != nullptr) ? BKE_screen_find_area_xy(screen, SPACE_VIEW3D, event->xy) :
                                        nullptr;
  ARegion *region = (area != nullptr) ? BKE_area_find_region_xy(area, RGN_TYPE_WINDOW, event->xy) :
                                        nullptr;
  Object *ob = CTX_data_active_object(C);
  if (region == nullptr || ob == nullptr || ob->type != OB_MESH) {
    return false;
  }
  const Mesh &mesh = *id_cast<const Mesh *>(ob->data);
  if (mesh.faces_num == 0) {
    return false;
  }
  const float2 mval(float(event->xy[0] - region->winrct.xmin),
                    float(event->xy[1] - region->winrct.ymin));
  float3 start_world;
  float3 end_world;
  if (!ED_view3d_win_to_segment_clipped(CTX_data_depsgraph_pointer(C),
                                        region,
                                        static_cast<View3D *>(area->spacedata.first),
                                        mval,
                                        start_world,
                                        end_world,
                                        true))
  {
    return false;
  }
  const float4x4 &world_to_object = ob->world_to_object();
  const float3 start = math::transform_point(world_to_object, start_world);
  const float3 end = math::transform_point(world_to_object, end_world);

  bke::BVHTreeFromMesh bvh = mesh.bvh_corner_tris();
  BVHTreeRayHit hit;
  hit.dist = FLT_MAX;
  hit.index = -1;
  BLI_bvhtree_ray_cast(
      bvh.tree, start, math::normalize(end - start), 0.0f, &hit, bvh.raycast_callback, &bvh);
  if (hit.index == -1) {
    return false;
  }

  const std::string uv_name = canvas_uv_name_get(*CTX_data_scene(C), *ob, mesh);
  const VArray<float2> uvs = *mesh.attributes().lookup<float2>(uv_name, bke::AttrDomain::Corner);
  if (!uvs) {
    return false;
  }
  const int3 &tri = mesh.corner_tris()[hit.index];
  const float3 bary = bke::mesh_surface_sample::compute_bary_coord_in_triangle(
      mesh.vert_positions(), mesh.corner_verts(), tri, hit.co);
  r_uv = bke::mesh_surface_sample::sample_corner_attribute_with_bary_coords(bary, tri, uvs);
  return true;
}

/**
 * The colour under the cursor at \a event, in the Image Editor or on the mesh in the 3D Viewport,
 * averaged over a small window of the decoded map: the very texels the shader and the CPU mask
 * compare. False off the map.
 */
static bool id_color_sample(bContext *C,
                            wmOperator *op,
                            const wmEvent *event,
                            const IdColorPickSession &session,
                            float r_color[3])
{
  if (session.decoded == nullptr) {
    return false;
  }
  float2 view;
  SpaceImage *sima = nullptr;
  if (const ARegion *region = image_region_at(C, event, &sima)) {
    view = region_event_uv(*region, event);
  }
  else if (!view3d_event_uv(C, event, view)) {
    return false;
  }
  /* Tile 1001 only: the preview is built from that tile. */
  if (view.x < 0.0f || view.x >= 1.0f || view.y < 0.0f || view.y >= 1.0f) {
    return false;
  }
  const ImBuf &map = *session.decoded;
  const int px = std::clamp(int(view.x * float(map.x)), 0, map.x - 1);
  const int py = std::clamp(int(view.y * float(map.y)), 0, map.y - 1);
  const int half = std::max(RNA_int_get(op->ptr, "sample_size"), 1) / 2;
  const float *pixels = map.float_data();
  float sum[3] = {0.0f, 0.0f, 0.0f};
  int count = 0;
  for (int y = std::max(py - half, 0); y <= std::min(py + half, map.y - 1); y++) {
    for (int x = std::max(px - half, 0); x <= std::min(px + half, map.x - 1); x++) {
      const float *texel = pixels + (int64_t(y) * map.x + x) * 4;
      sum[0] += texel[0];
      sum[1] += texel[1];
      sum[2] += texel[2];
      count++;
    }
  }
  if (count == 0) {
    return false;
  }
  for (int i = 0; i < 3; i++) {
    r_color[i] = sum[i] / float(count);
  }
  return true;
}

static bool id_color_pick_poll(bContext *C)
{
  Material *ma = nullptr;
  MaterialPaintLayer *item = nullptr;
  return coverage_target_get(C, MA_PAINT_LAYER_FILTER_COLOR_ID, &ma, &item);
}

static wmOperatorStatus id_color_pick_invoke(bContext *C, wmOperator *op, const wmEvent * /*event*/)
{
  Material *ma = nullptr;
  MaterialPaintLayer *item = nullptr;
  if (!coverage_target_get(C, MA_PAINT_LAYER_FILTER_COLOR_ID, &ma, &item)) {
    return OPERATOR_CANCELLED;
  }
  Image *source = coverage_item_image(*item);
  if (source == nullptr) {
    BKE_report(op->reports, RPT_WARNING, "Assign an ID Map to this item first");
    return OPERATOR_CANCELLED;
  }
  ImBuf *decoded = BKE_paint_material_mask_map_acquire(source, 1001, nullptr, 0, 0);
  if (decoded == nullptr || decoded->float_data() == nullptr) {
    if (decoded != nullptr) {
      IMB_freeImBuf(decoded);
    }
    BKE_report(op->reports, RPT_WARNING, "The ID Map cannot be read");
    return OPERATOR_CANCELLED;
  }
  IdColorPickSession *session = MEM_new<IdColorPickSession>(__func__);
  session->bmain = CTX_data_main(C);
  session->source = source;
  session->decoded = decoded;
  const int side = std::max(decoded->x, decoded->y);
  const float scale = (side > ID_PREVIEW_MAX_SIDE) ? float(ID_PREVIEW_MAX_SIDE) / float(side) : 1.0f;
  session->preview_width = std::max(int(float(decoded->x) * scale), 1);
  session->preview_height = std::max(int(float(decoded->y) * scale), 1);
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  /* Data, like the map it mirrors, so the editors show its values untouched. */
  session->preview = BKE_image_add_generated(CTX_data_main(C),
                                             session->preview_width,
                                             session->preview_height,
                                             "ID Map Preview",
                                             32,
                                             false,
                                             IMA_GENTYPE_BLANK,
                                             black,
                                             false,
                                             true,
                                             false);
  if (session->preview == nullptr) {
    IMB_freeImBuf(decoded);
    MEM_delete(session);
    return OPERATOR_CANCELLED;
  }
  id_us_min(&session->preview->id);
  op->customdata = session;

  id_color_preview_refresh(*session, *item);
  display_swap_apply(C, session->swap, session->preview, true);
  g_picker_image = session->preview;

  WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_EYEDROPPER);
  ED_workspace_status_text(
      C,
      "Hover the ID Map in the Image Editor or the mesh in the 3D Viewport | LMB: add the color | "
      "Shift+LMB: keep picking | Esc/RMB/Enter: finish");
  WM_event_add_modal_handler(C, op);
  mask_filter_redraw_all(C);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus id_color_pick_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  IdColorPickSession *session = static_cast<IdColorPickSession *>(op->customdata);
  Material *ma = nullptr;
  MaterialPaintLayer *item = nullptr;
  if (!coverage_target_get(C, MA_PAINT_LAYER_FILTER_COLOR_ID, &ma, &item)) {
    id_color_pick_end(C, op);
    return OPERATOR_CANCELLED;
  }

  /* Only a click on the map is the picker's own; the rest goes on to the editors. */
  bool consumed = false;

  /* The output switch and gradient value live in the Properties editor: when they change, so does
   * the map shown. */
  if (BKE_paint_layers_mask_item_is_inverted(*item) != session->inverted ||
      fabsf(BKE_paint_layers_mask_item_output_value(*item) - session->output) > 1e-6f ||
      BKE_paint_layers_mask_item_is_alpha_background(*item) != session->output_alpha ||
      BKE_paint_layers_mask_item_is_color_id_advanced(*item) != session->advanced)
  {
    id_color_preview_refresh(*session, *item);
    mask_filter_redraw_all(C);
  }

  switch (event->type) {
    case EVT_ESCKEY:
    case RIGHTMOUSE:
    case EVT_RETKEY: {
      const bool changed = session->added > 0;
      id_color_pick_end(C, op);
      return changed ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
    }
    case MOUSEMOVE: {
      float color[3];
      const bool over_map = id_color_sample(C, op, event, *session, color);
      /* Refresh only when the colour under the cursor really changed: it is a full redraw. */
      const bool changed = (over_map != session->has_hover) ||
                           (over_map && (std::abs(color[0] - session->hover[0]) > 1e-3f ||
                                         std::abs(color[1] - session->hover[1]) > 1e-3f ||
                                         std::abs(color[2] - session->hover[2]) > 1e-3f));
      if (changed) {
        session->has_hover = over_map;
        if (over_map) {
          copy_v3_v3(session->hover, color);
        }
        id_color_preview_refresh(*session, *item);
        mask_filter_redraw_all(C);
      }
      break;
    }
    case LEFTMOUSE: {
      /* A click in the Image Editor or the 3D Viewport is the picker's, even one that misses the
       * map: left to the editor it would start a brush stroke or a paint tool there. Clicks in
       * other areas (the Properties editor's buttons) still go on. */
      SpaceImage *sima = nullptr;
      if (image_region_at(C, event, &sima) == nullptr && view3d_region_at(C, event) == nullptr) {
        break;
      }
      consumed = true;
      if (event->val != KM_PRESS) {
        break;
      }
      float color[3];
      if (!id_color_sample(C, op, event, *session, color)) {
        break;
      }
      if (BKE_paint_layers_id_color_add(*ma, item, color) != nullptr) {
        session->added++;
        /* The new colour is part of the list now, so the hover no longer adds it twice. */
        session->has_hover = false;
        id_color_preview_refresh(*session, *item);
        WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, ma);
        mask_filter_redraw_all(C);
        /* One pick is the whole gesture; Shift keeps the picker open for several samples. */
        if ((event->modifier & KM_SHIFT) == 0) {
          id_color_pick_end(C, op);
          return OPERATOR_FINISHED;
        }
      }
      break;
    }
    default:
      break;
  }
  /* A bare PASS_THROUGH lets the event go on to the editor's own handlers (navigation). Together with
   * RUNNING_MODAL the window treats the event as an unhandled break and the editors never see it. */
  return consumed ? OPERATOR_RUNNING_MODAL : OPERATOR_PASS_THROUGH;
}

static void id_color_pick_cancel(bContext *C, wmOperator *op)
{
  id_color_pick_end(C, op);
}

void PAINT_OT_id_color_pick(wmOperatorType *ot)
{
  ot->name = "Pick Color ID";
  ot->idname = "PAINT_OT_id_color_pick";
  ot->description =
      "Sample the ID Map: the Image Editor and the 3D Viewport show it flat, the area the colors "
      "match lit; click to add the color under the cursor to the active Color ID mask item";

  ot->invoke = id_color_pick_invoke;
  ot->modal = id_color_pick_modal;
  ot->cancel = id_color_pick_cancel;
  ot->poll = id_color_pick_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ot->prop = RNA_def_int(ot->srna,
                         "sample_size",
                         3,
                         1,
                         31,
                         "Sample Size",
                         "Side of the square of texels averaged around the cursor",
                         1,
                         15);
}

/** \} */

}  // namespace blender
