/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Operators of the map-driven mask filters, all acting on the active mask item of the active
 * material so they can be started from the Properties editor:
 * - UV Island pick: hover an island in the Image Editor to preview it (there and in the 3D
 *   Viewport), click to add it to the map, Shift-click to remove it.
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
#include "BLI_string.h"
#include "BLI_task.hh"
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
/** \name UV Island map write
 * \{ */

/** Faces of one object to write into the island map. */
struct CaptureFaces {
  Object *ob = nullptr;
  Vector<int> faces;
};

/** One object's faces as a BMesh, grouped by the UDIM tiles their UVs touch. */
struct CaptureObject {
  BMesh *bm = nullptr;
  BMUVOffsets offsets = {};
  Map<int, Vector<int>> tile_faces;
};

static void capture_object_free(CaptureObject &item)
{
  if (item.bm != nullptr) {
    BM_mesh_free(item.bm);
    item.bm = nullptr;
  }
}

/** The faces bucketed by tile, the way the 2D geometry fill buckets its faces. */
static void capture_faces_bucket(CaptureObject &item, const Span<int> faces)
{
  BM_mesh_elem_table_ensure(item.bm, BM_FACE);
  for (const int face_index : faces) {
    BMFace *efa = BM_face_at_index(item.bm, face_index);
    if (efa == nullptr) {
      continue;
    }
    rctf bounds;
    BLI_rctf_init_minmax(&bounds);
    BMIter liter;
    BMLoop *l;
    BM_ITER_ELEM (l, &liter, efa, BM_LOOPS_OF_FACE) {
      BLI_rctf_do_minmax_v(&bounds, BM_ELEM_CD_GET_FLOAT_P(l, item.offsets.uv));
    }
    for (int ty = std::max(int(floorf(bounds.ymin)), 0); ty <= int(floorf(bounds.ymax)); ty++) {
      for (int tx = std::max(int(floorf(bounds.xmin)), 0);
           tx <= std::min(int(floorf(bounds.xmax)), 9);
           tx++)
      {
        const int tile_number = 1001 + ty * 10 + tx;
        if (tile_number > IMA_UDIM_MAX) {
          continue;
        }
        item.tile_faces.lookup_or_add_default(tile_number).append(face_index);
      }
    }
  }
}

/**
 * The mesh objects of the view layer that show \a ma, outside Edit Mode: their face selection lives
 * in `.select_poly`. The map belongs to the material, so every object sharing it contributes.
 */
static Vector<Object *> capture_objects_get(bContext *C, const Material &ma)
{
  Vector<Object *> objects;
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
  for (Base &base : view_layer->object_bases) {
    Object *ob = base.object;
    if (ob == nullptr || ob->type != OB_MESH || (ob->mode & OB_MODE_EDIT)) {
      continue;
    }
    Mesh *mesh = id_cast<Mesh *>(ob->data);
    if (mesh == nullptr || mesh->faces_num == 0) {
      continue;
    }
    for (int slot = 1; slot <= ob->totcol; slot++) {
      if (BKE_object_material_get(ob, short(slot)) == &ma) {
        objects.append(ob);
        break;
      }
    }
  }
  return objects;
}

/**
 * Write \a targets' faces into the map of the UV Island item \a item, creating the map when the
 * item has none. \a subtract writes the empty value over the faces instead of the kept one. A face
 * on a UDIM tile the map does not have yet gets the tile added first.
 *
 * The map pixels live outside the global undo, so the write is one image undo step named
 * \a undo_name; the operator calling this therefore carries no #OPTYPE_UNDO of its own.
 */
static bool capture_write(bContext *C,
                          ReportList *reports,
                          Material &ma,
                          MaterialPaintLayer &item,
                          const Span<CaptureFaces> targets,
                          const bool subtract,
                          const char *undo_name)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);

  Vector<CaptureObject> items;
  for (const CaptureFaces &target : targets) {
    if (target.faces.is_empty()) {
      continue;
    }
    Mesh *mesh = id_cast<Mesh *>(target.ob->data);
    /* The original mesh, not the evaluated one: the island is the UV layout the user authored. */
    CaptureObject item_data;
    const BMAllocTemplate allocsize = BMALLOC_TEMPLATE_FROM_ME(mesh);
    BMeshCreateParams create_params{};
    BMeshFromMeshParams convert_params{};
    convert_params.calc_face_normal = false;
    convert_params.calc_vert_normal = false;
    item_data.bm = BM_mesh_create(&allocsize, &create_params);
    BM_mesh_bm_from_me(item_data.bm, mesh, &convert_params);
    if (item_data.bm->totface != mesh->faces_num) {
      /* #BM_mesh_bm_from_me skips degenerate faces: the mesh indices would then name other BMesh
       * faces, so this object cannot be captured. */
      BKE_reportf(reports,
                  RPT_WARNING,
                  "Object \"%s\" has degenerate faces and was skipped",
                  target.ob->id.name + 2);
      capture_object_free(item_data);
      continue;
    }
    item_data.offsets = image_paint_selection_uv_offsets_get(item_data.bm, target.ob, scene);
    if (item_data.offsets.uv < 0) {
      capture_object_free(item_data);
      continue;
    }
    capture_faces_bucket(item_data, target.faces);
    items.append(std::move(item_data));
  }
  if (items.is_empty()) {
    return false;
  }

  /* The material's own size wins inside the call; the scene setting is the fallback for a material
   * that has none yet, as for every other paint-layer map. */
  Image *map = BKE_paint_layers_map_coverage_map_ensure(
      *bmain, ma, item, scene->toolsettings->paint_mode.new_channel_image_size);
  if (map == nullptr) {
    for (CaptureObject &item_data : items) {
      capture_object_free(item_data);
    }
    BKE_report(reports, RPT_ERROR, "Cannot create the island map");
    return false;
  }
  Vector<int> tiles;
  for (const CaptureObject &item_data : items) {
    for (const int tile_number : item_data.tile_faces.keys()) {
      if (!tiles.contains(tile_number)) {
        tiles.append(tile_number);
      }
    }
  }
  /* A tile the map lacks is added before the undo step opens, so the step snapshots it (all zeros)
   * and undoing the write leaves it empty. Removing islands never needs a new tile. */
  for (const int tile_number : tiles) {
    if (BKE_image_get_tile(map, tile_number) != nullptr || subtract) {
      continue;
    }
    if (!BKE_paint_layers_map_coverage_tile_ensure(*map, tile_number)) {
      BKE_reportf(reports,
                  RPT_WARNING,
                  "The island map cannot get UDIM tile %d, its islands were skipped",
                  tile_number);
    }
  }

  /* Opened before the first pixel changes: the step snapshots the tiles as they are now. */
  ImageUser undo_iuser;
  BKE_imageuser_default(&undo_iuser);
  ED_image_undo_push_begin_with_image_all_udims(undo_name, map, &undo_iuser);

  const float fill = subtract ? 0.0f : 1.0f;
  const float white[3] = {1.0f, 1.0f, 1.0f};
  for (const int tile_number : tiles) {
    if (BKE_image_get_tile(map, tile_number) == nullptr) {
      continue;
    }
    ImageUser iuser{};
    BKE_imageuser_default(&iuser);
    iuser.tile = tile_number;
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(map, &iuser, &lock);
    if (ibuf == nullptr) {
      continue;
    }
    const float2 origin = image_select_udim_tile_uv_origin(tile_number);
    for (CaptureObject &item_data : items) {
      const Vector<int> *faces = item_data.tile_faces.lookup_ptr(tile_number);
      if (faces == nullptr || faces->is_empty()) {
        continue;
      }
      image_paint_rasterize_faces_to_ibuf(item_data.bm,
                                          item_data.offsets,
                                          *faces,
                                          origin,
                                          map,
                                          tile_number,
                                          ibuf,
                                          white,
                                          1.0f,
                                          IMB_BLEND_MIX,
                                          fill);
    }
    /* Same refresh as every other code that rewrites a map in place. */
    ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;
    BKE_image_mark_dirty(map, ibuf);
    BKE_image_partial_update_mark_full_update(map);
    BKE_image_release_ibuf(map, ibuf, lock);
  }
  ED_image_undo_push_end();
  for (CaptureObject &item_data : items) {
    capture_object_free(item_data);
  }

  DEG_id_tag_update(&ma.id, ID_RECALC_SHADING);
  WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, &ma);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_PROPERTIES, nullptr);
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name UV Island pick
 * \{ */

/** One face of an island: its UVs for the Image Editor and its world positions for the Viewport. */
struct IslandPoly {
  Vector<float2> uv;
  Vector<float3> world;
};

/**
 * The faces drawn over the Image Editor and the 3D Viewport while the island picker runs: the island
 * under the cursor and the ones already added in this session. The callbacks are registered on the
 * region *types*, so they only draw in the regions of the screen the picker runs in.
 */
struct IslandOverlay {
  Vector<IslandPoly> hover;
  Vector<IslandPoly> picked;
  bool hover_subtracts = false;
  /** The box being dragged, in UV space so it stays on the image while the view is navigated. */
  bool box_active = false;
  float2 box_uv_a = float2(0.0f);
  float2 box_uv_b = float2(0.0f);
  /** The same for a box dragged in a 3D Viewport: that region and its two corners in pixels. */
  bool box3d_active = false;
  const ARegion *box3d_region = nullptr;
  float2 box3d_a = float2(0.0f);
  float2 box3d_b = float2(0.0f);

  const ARegion *image_region = nullptr;
  Vector<const ARegion *> view3d_regions;
  ARegionType *image_type = nullptr;
  ARegionType *view3d_type = nullptr;
  void *image_handle = nullptr;
  void *view3d_handle = nullptr;
};

static void island_poly_draw(const Span<float2> region_co, const float4 &fill, const float4 &line)
{
  const uint pos = GPU_vertformat_attr_add(
      immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
  immUniformColor4fv(fill);
  immBegin(GPU_PRIM_TRI_FAN, uint(region_co.size()));
  for (const float2 &co : region_co) {
    immVertex2f(pos, co.x, co.y);
  }
  immEnd();
  immUniformColor4fv(line);
  immBegin(GPU_PRIM_LINE_LOOP, uint(region_co.size()));
  for (const float2 &co : region_co) {
    immVertex2f(pos, co.x, co.y);
  }
  immEnd();
  immUnbindProgram();
}

static void island_overlay_draw(const bContext * /*C*/, ARegion *region, void *arg)
{
  const IslandOverlay &overlay = *static_cast<const IslandOverlay *>(arg);
  const bool in_image = (region == overlay.image_region);
  const bool in_view3d = overlay.view3d_regions.contains(region);
  if (!in_image && !in_view3d) {
    return;
  }

  const float4 picked_fill(0.2f, 1.0f, 0.4f, 0.30f);
  const float4 picked_line(0.2f, 1.0f, 0.4f, 0.95f);
  const float4 hover_fill = overlay.hover_subtracts ? float4(1.0f, 0.25f, 0.2f, 0.35f) :
                                                       float4(1.0f, 0.85f, 0.1f, 0.35f);
  const float4 hover_line = overlay.hover_subtracts ? float4(1.0f, 0.25f, 0.2f, 1.0f) :
                                                       float4(1.0f, 0.85f, 0.1f, 1.0f);

  GPU_blend(GPU_BLEND_ALPHA);
  const auto draw_set = [&](const Span<IslandPoly> polys, const float4 &fill, const float4 &line) {
    Vector<float2> region_co;
    for (const IslandPoly &poly : polys) {
      region_co.clear();
      if (in_image) {
        for (const float2 &uv : poly.uv) {
          float2 co;
          ui::view2d_view_to_region_fl(&region->v2d, uv.x, uv.y, &co.x, &co.y);
          region_co.append(co);
        }
      }
      else {
        bool clipped = false;
        for (const float3 &world : poly.world) {
          float2 co;
          if (ED_view3d_project_float_global(region, world, co, V3D_PROJ_TEST_NOP) !=
              V3D_PROJ_RET_OK)
          {
            clipped = true;
            break;
          }
          region_co.append(co);
        }
        if (clipped) {
          continue;
        }
      }
      if (region_co.size() >= 3) {
        island_poly_draw(region_co, fill, line);
      }
    }
  };
  draw_set(overlay.picked, picked_fill, picked_line);
  draw_set(overlay.hover, hover_fill, hover_line);

  /* The selection box, only in the Image Editor where it is dragged. */
  if (in_image && overlay.box_active) {
    float2 a, b;
    ui::view2d_view_to_region_fl(
        &region->v2d, overlay.box_uv_a.x, overlay.box_uv_a.y, &a.x, &a.y);
    ui::view2d_view_to_region_fl(
        &region->v2d, overlay.box_uv_b.x, overlay.box_uv_b.y, &b.x, &b.y);
    const Vector<float2> corners = {float2(a.x, a.y), float2(b.x, a.y), float2(b.x, b.y), float2(a.x, b.y)};
    island_poly_draw(corners.as_span(),
                     float4(1.0f, 1.0f, 1.0f, 0.08f),
                     overlay.hover_subtracts ? float4(1.0f, 0.25f, 0.2f, 1.0f) :
                                               float4(1.0f, 1.0f, 1.0f, 0.9f));
  }
  if (in_view3d && overlay.box3d_active && region == overlay.box3d_region) {
    const float2 a = overlay.box3d_a;
    const float2 b = overlay.box3d_b;
    const Vector<float2> corners = {
        float2(a.x, a.y), float2(b.x, a.y), float2(b.x, b.y), float2(a.x, b.y)};
    island_poly_draw(corners.as_span(),
                     float4(1.0f, 1.0f, 1.0f, 0.08f),
                     overlay.hover_subtracts ? float4(1.0f, 0.25f, 0.2f, 1.0f) :
                                               float4(1.0f, 1.0f, 1.0f, 0.9f));
  }
  GPU_blend(GPU_BLEND_NONE);
}

/** The faces of \a island on \a ob as drawable polygons; \a r_polys is appended to. */
static void island_polys_append(const Scene &scene,
                                Object &ob,
                                const Span<bool> island,
                                Vector<IslandPoly> &r_polys)
{
  const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
  const std::string uv_name = canvas_uv_name_get(scene, ob, mesh);
  const VArray<float2> uvs = *mesh.attributes().lookup<float2>(uv_name, bke::AttrDomain::Corner);
  if (!uvs) {
    return;
  }
  const Span<float3> positions = mesh.vert_positions();
  const Span<int> corner_verts = mesh.corner_verts();
  const OffsetIndices<int> faces = mesh.faces();
  const float4x4 &to_world = ob.object_to_world();
  for (const int face_i : faces.index_range()) {
    if (!island[face_i]) {
      continue;
    }
    IslandPoly poly;
    for (const int corner : faces[face_i]) {
      poly.uv.append(uvs[corner]);
      poly.world.append(math::transform_point(to_world, positions[corner_verts[corner]]));
    }
    r_polys.append(std::move(poly));
  }
}

struct IslandPickSession {
  /** The database the preview lives in, for the liveness test of #image_is_alive. */
  Main *bmain = nullptr;
  Material *ma = nullptr;
  MaterialPaintLayer *item = nullptr;
  Vector<Object *> objects;
  /** The island under the cursor, per object, ready to be written by a click. */
  Vector<CaptureFaces> hover_targets;
  IslandOverlay overlay;
  int2 last_mouse = int2(-100000);
  /** A click or a box in progress: where it started (screen pixels and UV) and what it will do. */
  bool dragging = false;
  int2 press_xy = int2(0);
  float2 press_uv = float2(0.0f);
  bool press_subtract = false;
  /** The 3D Viewport region a drag started in, null for a drag in the Image Editor. */
  ARegion *press_view3d = nullptr;
  int2 press_region_xy = int2(0);
  int added = 0;
  /** The mask the item outputs, as the editors show it while picking (see #island_map_preview_refresh). */
  Image *preview = nullptr;
  int preview_width = 0;
  int preview_height = 0;
  /** The UDIM tiles the preview has, ascending: those the objects' UVs touch. */
  Vector<int> preview_tiles;
  /** The item's output polarity the preview was drawn for: it is redrawn when the user flips it. */
  bool inverted = false;
  /** The Gradient output scale the preview was drawn for: it is redrawn when the user moves it. */
  float output = 1.0f;
  /** Whether the preview was drawn with a transparent background. */
  bool output_alpha = false;
  DisplaySwap swap;
};

/** Side of the picker's preview image, a fixed size: it only has to show which islands are in. */
constexpr int ISLAND_PREVIEW_SIDE = 1024;

/**
 * Redraw the preview from the item's island map, tile by tile: the mask the item outputs, white
 * where an island is kept, so a click is seen to have added or removed it. A tile the map does not
 * have yet, and an item with no map, read as empty.
 */
static void island_map_preview_refresh(IslandPickSession &session, const MaterialPaintLayer &item)
{
  if (!image_is_alive(*session.bmain, session.preview)) {
    return;
  }
  Image *map_image = coverage_item_image(item);
  /* The mask the item outputs: the kept value, flipped by the output switch and scaled by the
   * Gradient value, so the preview is the map the user gets. */
  const bool inverted = BKE_paint_layers_mask_item_is_inverted(item);
  const float output = BKE_paint_layers_mask_item_output_value(item);
  const bool output_alpha = BKE_paint_layers_mask_item_is_alpha_background(item);
  session.inverted = inverted;
  session.output = output;
  session.output_alpha = output_alpha;

  for (const int tile_number : session.preview_tiles) {
    ImBuf *map = (map_image != nullptr) ?
                     BKE_paint_material_mask_map_acquire(map_image, tile_number, nullptr, 0, 0) :
                     nullptr;
    const float *src = (map != nullptr) ? map->float_data() : nullptr;

    ImageUser iuser;
    BKE_imageuser_default(&iuser);
    iuser.tile = tile_number;
    void *lock = nullptr;
    ImBuf *dst = BKE_image_acquire_ibuf(session.preview, &iuser, &lock);
    if (dst == nullptr) {
      if (map != nullptr) {
        IMB_freeImBuf(map);
      }
      continue;
    }
    uchar *out = dst->byte_data_for_write();
    for (int y = 0; y < session.preview_height; y++) {
      for (int x = 0; x < session.preview_width; x++) {
        float added = 0.0f;
        if (src != nullptr) {
          const int sx = std::min(int(int64_t(x) * map->x / session.preview_width), map->x - 1);
          const int sy = std::min(int(int64_t(y) * map->y / session.preview_height), map->y - 1);
          added = std::clamp(src[(int64_t(sy) * map->x + sx) * 4], 0.0f, 1.0f);
        }
        const float value = std::clamp((inverted ? 1.0f - added : added) * output, 0.0f, 1.0f);
        uchar *pixel = out + (int64_t(y) * session.preview_width + x) * 4;
        for (int c = 0; c < 3; c++) {
          pixel[c] = uchar(value * 255.0f + 0.5f);
        }
        /* Alpha Background: the kept coverage is the alpha, the rest transparent. */
        pixel[3] = output_alpha ? uchar(value * 255.0f + 0.5f) : 255;
      }
    }
    if (map != nullptr) {
      IMB_freeImBuf(map);
    }
    dst->userflags |= IB_DISPLAY_BUFFER_INVALID;
    BKE_image_mark_dirty(session.preview, dst);
    BKE_image_release_ibuf(session.preview, dst, lock);
  }
  BKE_image_partial_update_mark_full_update(session.preview);
}

/**
 * The UDIM tiles the UV faces of \a objects touch, ascending; tile 1001 is always there so the
 * picker has something to show for an object laid out in 0-1 only.
 */
static Vector<int> objects_uv_tiles_get(const Scene &scene, const Span<Object *> objects)
{
  Vector<int> tiles = {1001};
  for (Object *ob : objects) {
    const Mesh &mesh = *id_cast<const Mesh *>(ob->data);
    const VArray<float2> uvs = *mesh.attributes().lookup<float2>(
        canvas_uv_name_get(scene, *ob, mesh), bke::AttrDomain::Corner);
    if (!uvs) {
      continue;
    }
    const OffsetIndices<int> faces = mesh.faces();
    for (const int face_i : faces.index_range()) {
      rctf bounds;
      BLI_rctf_init_minmax(&bounds);
      for (const int corner : faces[face_i]) {
        const float2 uv = uvs[corner];
        BLI_rctf_do_minmax_v(&bounds, &uv.x);
      }
      for (int ty = std::max(int(floorf(bounds.ymin)), 0); ty <= int(floorf(bounds.ymax)); ty++) {
        for (int tx = std::max(int(floorf(bounds.xmin)), 0);
             tx <= std::min(int(floorf(bounds.xmax)), 9);
             tx++)
        {
          const int tile_number = 1001 + ty * 10 + tx;
          if (tile_number <= IMA_UDIM_MAX && !tiles.contains(tile_number)) {
            tiles.append(tile_number);
          }
        }
      }
    }
  }
  std::sort(tiles.begin(), tiles.end());
  return tiles;
}
static void island_pick_end(bContext *C, wmOperator *op)
{
  IslandPickSession *session = static_cast<IslandPickSession *>(op->customdata);
  if (session != nullptr) {
    g_picker_image = nullptr;
    /* The editors show the user's own images again before the preview they displayed goes. */
    display_swap_restore(C, session->swap);
    if (image_is_alive(*session->bmain, session->preview)) {
      BKE_id_free(session->bmain, session->preview);
    }
    if (session->overlay.image_handle != nullptr) {
      ED_region_draw_cb_exit(session->overlay.image_type, session->overlay.image_handle);
    }
    if (session->overlay.view3d_handle != nullptr) {
      ED_region_draw_cb_exit(session->overlay.view3d_type, session->overlay.view3d_handle);
    }
    MEM_delete(session);
    op->customdata = nullptr;
  }
  WM_cursor_modal_restore(CTX_wm_window(C));
  ED_workspace_status_text(C, nullptr);
  mask_filter_redraw_all(C);
}

/**
 * Recompute what a click would write: the islands \a find reports per object, as faces and as the
 * polygons that preview them in the editors.
 */
static void island_pick_hover_collect(
    bContext *C,
    IslandPickSession &session,
    const FunctionRef<bool(const Scene &, Object &, Array<bool> &)> find)
{
  Scene *scene = CTX_data_scene(C);
  session.hover_targets.clear();
  session.overlay.hover.clear();
  for (Object *ob : session.objects) {
    Array<bool> island;
    if (!find(*scene, *ob, island)) {
      continue;
    }
    CaptureFaces target;
    target.ob = ob;
    for (const int face_i : island.index_range()) {
      if (island[face_i]) {
        target.faces.append(face_i);
      }
    }
    island_polys_append(*scene, *ob, island, session.overlay.hover);
    session.hover_targets.append(std::move(target));
  }
}

/** The island under \a uv. */
static void island_pick_hover_update(bContext *C, IslandPickSession &session, const float2 &uv)
{
  island_pick_hover_collect(
      C, session, [&](const Scene &scene, Object &ob, Array<bool> &r_island) {
        return paint_mask_island_find_object(scene, ob, uv, r_island);
      });
}

/** Every island the UV rectangle spanned by \a uv_a and \a uv_b touches. */
static void island_pick_hover_update_rect(bContext *C,
                                          IslandPickSession &session,
                                          const float2 &uv_a,
                                          const float2 &uv_b)
{
  rctf rect;
  rect.xmin = std::min(uv_a.x, uv_b.x);
  rect.xmax = std::max(uv_a.x, uv_b.x);
  rect.ymin = std::min(uv_a.y, uv_b.y);
  rect.ymax = std::max(uv_a.y, uv_b.y);
  island_pick_hover_collect(
      C, session, [&](const Scene &scene, Object &ob, Array<bool> &r_island) {
        return paint_mask_island_find_rect(scene, ob, rect, r_island);
      });
}


/**
 * The faces of \a ob that a 3D Viewport selection reaches: with \a rect, every front-facing face
 * whose projected centre is inside it; without, the front-most face under \a point. Occlusion by
 * other geometry is not tested, which suits the flat map display the picker puts the viewport in.
 */
static void view3d_seed_faces(const ARegion &region,
                              Object &ob,
                              const rcti *rect,
                              const int2 point,
                              Array<bool> &r_seeds)
{
  const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
  r_seeds.reinitialize(mesh.faces_num);
  r_seeds.fill(false);
  const RegionView3D *rv3d = static_cast<const RegionView3D *>(region.regiondata);
  if (rv3d == nullptr) {
    return;
  }
  const float3 eye = float3(rv3d->viewinv[3][0], rv3d->viewinv[3][1], rv3d->viewinv[3][2]);
  const Span<float3> positions = mesh.vert_positions();
  const Span<int> corner_verts = mesh.corner_verts();
  const OffsetIndices<int> faces = mesh.faces();
  const float4x4 &to_world = ob.object_to_world();

  float best_distance = FLT_MAX;
  int best_face = -1;
  Vector<float2> projected;
  for (const int face_i : faces.index_range()) {
    projected.clear();
    float3 world_center(0.0f);
    bool visible = true;
    for (const int corner : faces[face_i]) {
      const float3 world = math::transform_point(to_world, positions[corner_verts[corner]]);
      float2 co;
      if (ED_view3d_project_float_global(&region, world, co, V3D_PROJ_TEST_NOP) !=
          V3D_PROJ_RET_OK)
      {
        visible = false;
        break;
      }
      projected.append(co);
      world_center += world;
    }
    if (!visible || projected.size() < 3) {
      continue;
    }
    /* A face seen from behind winds clockwise on screen. */
    float twice_area = 0.0f;
    float2 centre(0.0f);
    for (const int i : projected.index_range()) {
      const float2 &a = projected[i];
      const float2 &b = projected[(i + 1) % projected.size()];
      twice_area += a.x * b.y - b.x * a.y;
      centre += a;
    }
    if (twice_area <= 0.0f) {
      continue;
    }
    centre /= float(projected.size());
    if (rect != nullptr) {
      if (BLI_rcti_isect_pt(rect, int(centre.x), int(centre.y))) {
        r_seeds[face_i] = true;
      }
      continue;
    }
    const float2 mouse(float(point.x), float(point.y));
    const float(*poly_co)[2] = reinterpret_cast<const float(*)[2]>(projected.data());
    if (isect_point_poly_v2(mouse, poly_co, uint(projected.size()))) {
      const float distance = math::distance(world_center / float(projected.size()), eye);
      if (distance < best_distance) {
        best_distance = distance;
        best_face = face_i;
      }
    }
  }
  if (rect == nullptr && best_face >= 0) {
    r_seeds[best_face] = true;
  }
}

/** The islands a 3D Viewport selection (a box, or a single point) reaches. */
static void island_pick_hover_update_view3d(bContext *C,
                                            IslandPickSession &session,
                                            const ARegion &region,
                                            const rcti *rect,
                                            const int2 point)
{
  island_pick_hover_collect(
      C, session, [&](const Scene &scene, Object &ob, Array<bool> &r_island) {
        Array<bool> seeds;
        view3d_seed_faces(region, ob, rect, point, seeds);
        return paint_mask_island_find_from_faces(scene, ob, seeds, r_island);
      });
}

static bool uv_island_pick_poll(bContext *C)
{
  Material *ma = nullptr;
  MaterialPaintLayer *item = nullptr;
  return coverage_target_get(C, MA_PAINT_LAYER_FILTER_UV_ISLAND, &ma, &item);
}

static wmOperatorStatus uv_island_pick_invoke(bContext *C, wmOperator *op, const wmEvent * /*event*/)
{
  Material *ma = nullptr;
  MaterialPaintLayer *item = nullptr;
  bScreen *screen = CTX_wm_screen(C);
  if (!coverage_target_get(C, MA_PAINT_LAYER_FILTER_UV_ISLAND, &ma, &item) || screen == nullptr) {
    return OPERATOR_CANCELLED;
  }

  IslandPickSession *session = MEM_new<IslandPickSession>(__func__);
  session->bmain = CTX_data_main(C);
  session->ma = ma;
  session->item = item;
  session->objects = capture_objects_get(C, *ma);
  if (session->objects.is_empty()) {
    MEM_delete(session);
    BKE_report(op->reports, RPT_WARNING, "No mesh object uses this material");
    return OPERATOR_CANCELLED;
  }

  /* One callback per region type; the regions of this screen are told apart when drawing. */
  IslandOverlay &overlay = session->overlay;
  for (ScrArea &area : screen->areabase) {
    for (ARegion &region : area.regionbase) {
      if (region.regiontype != RGN_TYPE_WINDOW) {
        continue;
      }
      if (area.spacetype == SPACE_IMAGE && overlay.image_region == nullptr) {
        overlay.image_region = &region;
        overlay.image_type = region.runtime->type;
      }
      else if (area.spacetype == SPACE_VIEW3D) {
        overlay.view3d_regions.append(&region);
        overlay.view3d_type = region.runtime->type;
      }
    }
  }
  if (overlay.image_region == nullptr) {
    MEM_delete(session);
    BKE_report(op->reports, RPT_WARNING, "Open an Image Editor to pick islands");
    return OPERATOR_CANCELLED;
  }
  overlay.image_handle = ED_region_draw_cb_activate(
      overlay.image_type, island_overlay_draw, &overlay, REGION_DRAW_POST_PIXEL);
  if (overlay.view3d_type != nullptr) {
    overlay.view3d_handle = ED_region_draw_cb_activate(
        overlay.view3d_type, island_overlay_draw, &overlay, REGION_DRAW_POST_PIXEL);
  }
  op->customdata = session;

  /* The map itself is shown while picking: islands already added read green, so each click is seen
   * to have added (or, with Shift, removed) exactly that island. */
  const float dark[4] = {0.10f, 0.10f, 0.12f, 1.0f};
  session->preview_width = ISLAND_PREVIEW_SIDE;
  session->preview_height = ISLAND_PREVIEW_SIDE;
  session->preview = BKE_image_add_generated(CTX_data_main(C),
                                             session->preview_width,
                                             session->preview_height,
                                             "UV Island Preview",
                                             32,
                                             false,
                                             IMA_GENTYPE_BLANK,
                                             dark,
                                             false,
                                             true,
                                             /*tiled=*/true);
  if (session->preview != nullptr) {
    id_us_min(&session->preview->id);
    /* One tile for every UDIM tile the objects' UVs touch, so islands there can be seen and picked
     * in the Image Editor. */
    session->preview_tiles = objects_uv_tiles_get(*CTX_data_scene(C), session->objects);
    for (const int tile_number : session->preview_tiles) {
      BKE_paint_layers_map_coverage_tile_ensure(*session->preview, tile_number);
    }
    island_map_preview_refresh(*session, *item);
    display_swap_apply(C, session->swap, session->preview, true);
    g_picker_image = session->preview;
  }

  WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_CROSS);
  ED_workspace_status_text(C,
                           "Drag: box select islands (Image Editor or 3D Viewport) | Click: one island | Shift: remove | "
                           "Esc/RMB/Enter: finish");
  WM_event_add_modal_handler(C, op);
  mask_filter_redraw_all(C);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus uv_island_pick_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  IslandPickSession *session = static_cast<IslandPickSession *>(op->customdata);
  Material *ma = nullptr;
  MaterialPaintLayer *item = nullptr;
  if (!coverage_target_get(C, MA_PAINT_LAYER_FILTER_UV_ISLAND, &ma, &item)) {
    /* The item went away (undo, deletion): nothing left to write into. */
    island_pick_end(C, op);
    return OPERATOR_CANCELLED;
  }
  session->ma = ma;
  session->item = item;
  /* Only a click on the map is the picker's own; everything else goes on to the editors, so zoom,
   * pan and orbit keep working while it runs. */
  bool consumed = false;

  /* The output switch and gradient value live in the Properties editor: when they change, so does
   * the map shown. */
  if (BKE_paint_layers_mask_item_is_inverted(*item) != session->inverted ||
      fabsf(BKE_paint_layers_mask_item_output_value(*item) - session->output) > 1e-6f ||
      BKE_paint_layers_mask_item_is_alpha_background(*item) != session->output_alpha)
  {
    island_map_preview_refresh(*session, *item);
    mask_filter_redraw_all(C);
  }

  switch (event->type) {
    case EVT_ESCKEY:
    case RIGHTMOUSE:
    case EVT_RETKEY: {
      if (session->dragging && event->type == EVT_ESCKEY) {
        /* Esc during a drag drops the gesture only; a second Esc leaves the picker. */
        session->press_view3d = nullptr;
        session->overlay.box3d_active = false;
        session->dragging = false;
        session->overlay.box_active = false;
        session->overlay.hover.clear();
        session->hover_targets.clear();
        mask_filter_redraw_all(C);
        consumed = true;
        break;
      }
      const bool changed = session->added > 0;
      island_pick_end(C, op);
      return changed ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
    }
    case EVT_LEFTSHIFTKEY:
    case EVT_RIGHTSHIFTKEY:
      /* Shift flips the preview to the remove colour before the click, not after the next move. */
      if (!session->dragging) {
        session->overlay.hover_subtracts = (event->val != KM_RELEASE);
      }
      mask_filter_redraw_all(C);
      break;
    case MOUSEMOVE: {
      /* The flood fill and the polygon build are not free: skip sub-pixel jitter. */
      if (math::distance(float2(float(event->xy[0]), float(event->xy[1])),
                         float2(float(session->last_mouse.x), float(session->last_mouse.y))) < 3.0f)
      {
        break;
      }
      SpaceImage *sima = nullptr;
      ARegion *image_region = image_region_at(C, event, &sima);
      if (session->dragging && session->press_view3d != nullptr) {
        /* A box dragged in a 3D Viewport; the mouse may leave the region while it is held. */
        ARegion *region = session->press_view3d;
        session->last_mouse = int2(event->xy[0], event->xy[1]);
        const int2 now(event->xy[0] - region->winrct.xmin, event->xy[1] - region->winrct.ymin);
        const float2 moved = float2(float(event->xy[0] - session->press_xy.x),
                                    float(event->xy[1] - session->press_xy.y));
        if (math::length(moved) >= 4.0f) {
          IslandOverlay &overlay = session->overlay;
          overlay.box3d_active = true;
          overlay.box3d_region = region;
          overlay.box3d_a = float2(float(session->press_region_xy.x), float(session->press_region_xy.y));
          overlay.box3d_b = float2(float(now.x), float(now.y));
          rcti rect;
          BLI_rcti_init(&rect,
                        std::min(session->press_region_xy.x, now.x),
                        std::max(session->press_region_xy.x, now.x),
                        std::min(session->press_region_xy.y, now.y),
                        std::max(session->press_region_xy.y, now.y));
          island_pick_hover_update_view3d(C, *session, *region, &rect, now);
          mask_filter_redraw_all(C);
        }
        break;
      }
      if (image_region != nullptr) {
        session->last_mouse = int2(event->xy[0], event->xy[1]);
        const float2 uv = region_event_uv(*image_region, event);
        if (session->dragging) {
          /* A drag past a few pixels is a box: every island it touches is previewed. */
          const float2 moved = float2(float(event->xy[0] - session->press_xy.x),
                                      float(event->xy[1] - session->press_xy.y));
          if (math::length(moved) >= 4.0f) {
            session->overlay.box_active = true;
            session->overlay.box_uv_a = session->press_uv;
            session->overlay.box_uv_b = uv;
            island_pick_hover_update_rect(C, *session, session->press_uv, uv);
          }
        }
        else {
          session->overlay.hover_subtracts = (event->modifier & KM_SHIFT) != 0;
          island_pick_hover_update(C, *session, uv);
        }
        mask_filter_redraw_all(C);
        break;
      }
      if (!session->dragging) {
        /* Hovering a 3D Viewport previews the island under the cursor there. */
        if (ARegion *region = view3d_region_at(C, event)) {
          session->last_mouse = int2(event->xy[0], event->xy[1]);
          session->overlay.hover_subtracts = (event->modifier & KM_SHIFT) != 0;
          island_pick_hover_update_view3d(
              C,
              *session,
              *region,
              nullptr,
              int2(event->xy[0] - region->winrct.xmin, event->xy[1] - region->winrct.ymin));
          mask_filter_redraw_all(C);
        }
      }
      break;
    }
    case LEFTMOUSE: {
      SpaceImage *sima = nullptr;
      ARegion *image_region = image_region_at(C, event, &sima);
      if (event->val == KM_PRESS) {
        ARegion *view3d_region = (image_region == nullptr) ? view3d_region_at(C, event) : nullptr;
        if (image_region == nullptr && view3d_region == nullptr) {
          break;
        }
        /* The press starts a click or a box; Shift held now makes it a removal. */
        consumed = true;
        session->dragging = true;
        session->press_xy = int2(event->xy[0], event->xy[1]);
        session->press_view3d = view3d_region;
        if (view3d_region != nullptr) {
          session->press_region_xy = int2(event->xy[0] - view3d_region->winrct.xmin,
                                          event->xy[1] - view3d_region->winrct.ymin);
        }
        else {
          session->press_uv = region_event_uv(*image_region, event);
        }
        session->press_subtract = (event->modifier & KM_SHIFT) != 0;
        session->overlay.hover_subtracts = session->press_subtract;
        session->overlay.box_active = false;
        session->overlay.box3d_active = false;
        break;
      }
      if (event->val != KM_RELEASE || !session->dragging) {
        break;
      }

      consumed = true;
      session->dragging = false;
      ARegion *press_view3d = session->press_view3d;
      session->press_view3d = nullptr;
      const bool was_box = session->overlay.box_active || session->overlay.box3d_active;
      session->overlay.box_active = false;
      session->overlay.box3d_active = false;

      if (press_view3d != nullptr) {
        const int2 now(event->xy[0] - press_view3d->winrct.xmin,
                       event->xy[1] - press_view3d->winrct.ymin);
        if (was_box) {
          rcti rect;
          BLI_rcti_init(&rect,
                        std::min(session->press_region_xy.x, now.x),
                        std::max(session->press_region_xy.x, now.x),
                        std::min(session->press_region_xy.y, now.y),
                        std::max(session->press_region_xy.y, now.y));
          island_pick_hover_update_view3d(C, *session, *press_view3d, &rect, now);
        }
        else {
          island_pick_hover_update_view3d(
              C, *session, *press_view3d, nullptr, session->press_region_xy);
        }
      }
      else if (image_region == nullptr) {
        /* Released outside the map: the gesture is dropped. */
        session->overlay.hover.clear();
        session->hover_targets.clear();
        mask_filter_redraw_all(C);
        break;
      }
      else if (was_box) {
        island_pick_hover_update_rect(
            C, *session, session->press_uv, region_event_uv(*image_region, event));
      }
      else {
        island_pick_hover_update(C, *session, session->press_uv);
      }
      if (session->hover_targets.is_empty()) {
        mask_filter_redraw_all(C);
        break;
      }
      const bool subtract = session->press_subtract;
      const bool written = capture_write(C,
                                         op->reports,
                                         *ma,
                                         *item,
                                         session->hover_targets,
                                         subtract,
                                         op->type->name);
      if (written) {
        session->added++;
        /* The map changed: show it, so what is in and what is not stays visible. */
        island_map_preview_refresh(*session, *item);
      }
      session->overlay.hover.clear();
      session->hover_targets.clear();
      mask_filter_redraw_all(C);
      break;
    }
    default:
      break;
  }
  /* A bare PASS_THROUGH lets the event go on to the editor's own handlers (navigation). Together with
   * RUNNING_MODAL the window treats the event as an unhandled break and the editors never see it. */
  return consumed ? OPERATOR_RUNNING_MODAL : OPERATOR_PASS_THROUGH;
}

static void uv_island_pick_cancel(bContext *C, wmOperator *op)
{
  island_pick_end(C, op);
}

void PAINT_OT_uv_island_pick(wmOperatorType *ot)
{
  ot->name = "Select UV Islands";
  ot->idname = "PAINT_OT_uv_island_pick";
  ot->description =
      "Drag a box in the Image Editor or the 3D Viewport to select every UV island it touches, or "
      "click one island; "
      "they are added to the map of the active UV Island mask item. Hold Shift to remove them";

  ot->invoke = uv_island_pick_invoke;
  ot->modal = uv_island_pick_modal;
  ot->cancel = uv_island_pick_cancel;
  ot->poll = uv_island_pick_poll;
  /* No #OPTYPE_UNDO: every click is its own image undo step, pushed by #capture_write. */
  ot->flag = OPTYPE_REGISTER;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Bake to image
 *
 * The result of a map-coverage item (its match, flipped by the output switch and scaled by the
 * Gradient value) written into a new image, so it can be used as a mask map of another object. The
 * arithmetic is #BKE_paint_material_coverage_match, the one the composite and the strokes use.
 * \{ */

static bool mask_item_bake_target_get(bContext *C, Material **r_ma, MaterialPaintLayer **r_item)
{
  Object *ob = CTX_data_active_object(C);
  Material *ma = (ob != nullptr) ? BKE_paint_layers_active_material_get(ob) : nullptr;
  if (ma == nullptr) {
    return false;
  }
  MaterialPaintLayer *item = BKE_paint_layers_active_mask_item_get(*ma);
  if (item == nullptr || !BKE_paint_layers_is_map_coverage(*item) ||
      BKE_paint_layers_map_coverage_image_get(*item) == nullptr)
  {
    return false;
  }
  *r_ma = ma;
  *r_item = item;
  return true;
}

static bool mask_item_bake_poll(bContext *C)
{
  Material *ma = nullptr;
  MaterialPaintLayer *item = nullptr;
  return mask_item_bake_target_get(C, &ma, &item);
}

static wmOperatorStatus mask_item_bake_exec(bContext *C, wmOperator *op)
{
  Material *ma = nullptr;
  MaterialPaintLayer *item = nullptr;
  if (!mask_item_bake_target_get(C, &ma, &item)) {
    return OPERATOR_CANCELLED;
  }
  Main *bmain = CTX_data_main(C);
  Image *map_image = BKE_paint_layers_map_coverage_image_get(*item);

  const int16_t kind = item->kind_subtype;
  const float tolerance = BKE_paint_layers_coverage_tolerance_get(*item);
  const float softness = BKE_paint_layers_coverage_softness_get(*item);
  Vector<PaintMaterialCompositeIdColor> colors;
  if (kind == MA_PAINT_LAYER_FILTER_COLOR_ID) {
    const bool advanced = BKE_paint_layers_mask_item_is_color_id_advanced(*item);
    for (const MaterialPaintLayerIdColor &color : item->id_colors) {
      PaintMaterialCompositeIdColor entry;
      copy_v3_v3(entry.color, color.color);
      entry.tolerance = color.tolerance;
      entry.own_tolerance = advanced;
      entry.enabled = (color.flag & MA_PAINT_LAYER_ID_COLOR_ENABLED) != 0;
      colors.append(entry);
    }
  }
  const bool inverted = BKE_paint_layers_mask_item_is_inverted(*item);
  const float output = BKE_paint_layers_mask_item_output_value(*item);
  const bool alpha_background = BKE_paint_layers_mask_item_is_alpha_background(*item);

  Vector<int> tiles;
  for (const ImageTile &tile : map_image->tiles) {
    tiles.append(tile.tile_number == 0 ? 1001 : tile.tile_number);
  }
  if (tiles.is_empty()) {
    tiles.append(1001);
  }

  /* The first tile gives the size; the others are made like it. */
  int width = 0;
  int height = 0;
  if (ImBuf *first = BKE_paint_material_mask_map_acquire(map_image, tiles[0], nullptr, 0, 0)) {
    width = first->x;
    height = first->y;
    IMB_freeImBuf(first);
  }
  if (width <= 0 || height <= 0) {
    BKE_report(op->reports, RPT_ERROR, "The map of the mask item cannot be read");
    return OPERATOR_CANCELLED;
  }

  const MaterialPaintLayer *owner = BKE_paint_layers_mask_item_owner(*ma, *item);
  char name[192];
  BLI_snprintf(name,
               sizeof(name),
               "%s %s Mask",
               (owner != nullptr && owner->name[0] != '\0') ? owner->name : "Layer",
               item->name[0] != '\0' ? item->name : "Item");
  const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  Image *baked = BKE_image_add_generated(bmain,
                                         uint(width),
                                         uint(height),
                                         name,
                                         32,
                                         false,
                                         IMA_GENTYPE_BLANK,
                                         clear,
                                         false,
                                         true,
                                         /*tiled=*/tiles.size() > 1);
  if (baked == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "Cannot create the image");
    return OPERATOR_CANCELLED;
  }
  /* The alpha is the mask when the item says so, so it has to reach a file straight. */
  baked->alpha_mode = IMA_ALPHA_STRAIGHT;

  for (const int tile_number : tiles) {
    if (!BKE_paint_layers_map_coverage_tile_ensure(*baked, tile_number) && tiles.size() > 1) {
      continue;
    }
    ImBuf *src = BKE_paint_material_mask_map_acquire(map_image, tile_number, nullptr, 0, 0);
    const float *src_pixels = (src != nullptr) ? src->float_data() : nullptr;

    ImageUser iuser;
    BKE_imageuser_default(&iuser);
    iuser.tile = tile_number;
    void *lock = nullptr;
    ImBuf *dst = BKE_image_acquire_ibuf(baked, &iuser, &lock);
    uchar *out = (dst != nullptr) ? dst->byte_data_for_write() : nullptr;
    if (out != nullptr) {
      threading::parallel_for(IndexRange(dst->y), 8, [&](const IndexRange rows) {
        for (const int y : rows) {
          for (const int x : IndexRange(dst->x)) {
            float rgb[3] = {0.0f, 0.0f, 0.0f};
            if (src_pixels != nullptr) {
              /* Nearest texel, in case the tile is not the size of the first one. */
              const int sx = std::min(int(int64_t(x) * src->x / dst->x), src->x - 1);
              const int sy = std::min(int(int64_t(y) * src->y / dst->y), src->y - 1);
              copy_v3_v3(rgb, src_pixels + (int64_t(sy) * src->x + sx) * 4);
            }
            float value = BKE_paint_material_coverage_match(
                kind, rgb, tolerance, softness, colors.as_span());
            if (inverted) {
              value = 1.0f - value;
            }
            value = std::clamp(value * output, 0.0f, 1.0f);
            const uchar byte = uchar(value * 255.0f + 0.5f);
            uchar *pixel = out + (int64_t(y) * dst->x + x) * 4;
            pixel[0] = byte;
            pixel[1] = byte;
            pixel[2] = byte;
            pixel[3] = alpha_background ? byte : 255;
          }
        }
      });
      dst->userflags |= IB_DISPLAY_BUFFER_INVALID;
      BKE_image_mark_dirty(baked, dst);
    }
    if (dst != nullptr) {
      BKE_image_release_ibuf(baked, dst, lock);
    }
    if (src != nullptr) {
      IMB_freeImBuf(src);
    }
  }
  BKE_image_partial_update_mark_full_update(baked);

  BKE_reportf(op->reports, RPT_INFO, "Created image \"%s\"", baked->id.name + 2);
  WM_event_add_notifier(C, NC_IMAGE | NA_ADDED, baked);
  return OPERATOR_FINISHED;
}

void PAINT_OT_mask_item_bake(wmOperatorType *ot)
{
  ot->name = "Bake Mask to Image";
  ot->idname = "PAINT_OT_mask_item_bake";
  ot->description =
      "Write the result of the active Color ID or UV Island mask item into a new image: its "
      "value in the color and, with Alpha Background, in the alpha";

  ot->exec = mask_item_bake_exec;
  ot->poll = mask_item_bake_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
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
