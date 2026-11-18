/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * What the map-driven mask filter operators share: the editors a picker runs in (the Image Editor
 * region and UV under an event, the 3D Viewport's), the display swap that shows a picker's image
 * flat while it runs, and the redraw and liveness checks its session leans on. The operators live
 * one per file (`paint_image_mask_filters.cc`, `paint_image_normal_to_mask.cc`), so a helper a
 * second picker needs is here rather than copied there.
 */

#pragma once

#include <string>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

#include "DNA_object_enums.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_view3d_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_context.hh"
#include "BKE_main.hh"
#include "BKE_screen.hh"

#include "ED_image.hh"
#include "ED_screen.hh"

#include "UI_view2d.hh"

namespace blender {

struct Image;
struct Mesh;
struct Object;
struct Scene;

/** The Image Editor region under \a event, or null; \a r_sima is its space. */
inline ARegion *image_region_at(bContext *C, const wmEvent *event, SpaceImage **r_sima)
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
inline float2 region_event_uv(const ARegion &region, const wmEvent *event)
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
inline ARegion *view3d_region_at(bContext *C, const wmEvent *event)
{
  bScreen *screen = CTX_wm_screen(C);
  ScrArea *area = (screen != nullptr) ? BKE_screen_find_area_xy(screen, SPACE_VIEW3D, event->xy) :
                                        nullptr;
  return (area != nullptr) ? BKE_area_find_region_xy(area, RGN_TYPE_WINDOW, event->xy) : nullptr;
}

/**
 * The UV map the paint canvas uses, as the island detection reads it; the mesh's own default
 * otherwise. Defined beside the pickers that read it (`paint_image_mask_filters.cc`): it walks the
 * mesh and its BVH, which no header wants to carry.
 */
std::string canvas_uv_name_get(const Scene &scene, Object &ob, const Mesh &mesh);

/**
 * The UV under \a event's cursor, cast onto the mesh in the 3D Viewport: the ray through the
 * pixel hit-tested against the mesh, its hit's UV read from the canvas map. False off the mesh.
 * Defined beside `canvas_uv_name_get` for the same reason.
 */
bool view3d_event_uv(bContext *C, const wmEvent *event, float2 &r_uv);

inline void mask_filter_redraw_all(bContext *C)
{
  if (bScreen *screen = CTX_wm_screen(C)) {
    for (ScrArea &area : screen->areabase) {
      if (ELEM(area.spacetype, SPACE_IMAGE, SPACE_VIEW3D, SPACE_PROPERTIES)) {
        ED_area_tag_redraw(&area);
      }
    }
  }
}

/* -------------------------------------------------------------------- */
/** \name Map display swap
 *
 * While a picker runs the Image Editor shows the picker's image and every 3D Viewport is in flat
 * solid shading with textures (Workbench then draws the same image on the mesh, see
 * #ED_paint_mask_filter_preview_image_get). Everything is put back when the picker ends.
 * \{ */

/** Declared here for #display_swap_restore, defined below with the picker state it guards. */
inline bool image_is_alive(const Main &bmain, const Image *image);

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

inline void display_swap_apply(bContext *C, DisplaySwap &swap, Image *show, const bool flat_3d)
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
inline void display_swap_restore(bContext *C, DisplaySwap &swap)
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
inline Image *g_picker_image = nullptr;

/**
 * Whether \a image still is a data-block of \a bmain. A picker's preview is created in main, so an
 * undo or a file load can free it under the session; the pointer is never trusted past this test.
 * It only scans while a picker runs, as a null \a image answers at once.
 */
inline bool image_is_alive(const Main &bmain, const Image *image)
{
  return image != nullptr && BLI_findindex(&bmain.images, image) != -1;
}

/** \} */

}  // namespace blender
