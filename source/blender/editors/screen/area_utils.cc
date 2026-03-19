/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edscr
 *
 * Helper functions for area/region API.
 */

#include <algorithm>
#include <cmath>
#include <limits>

#include "BKE_screen.hh"

#include "DNA_space_types.h"
#include "DNA_userdef_types.h"
#include "DNA_view3d_types.h"

#include "BLI_rect.h"
#include "BLI_utildefines.h"

#include "WM_message.hh"

#include "ED_screen.hh"

#include "UI_interface.hh"

namespace blender {

eUserPref_CategoryTabsDisplayMode ED_category_tabs_display_mode_get(const ScrArea *area)
{
  if (!area || !area->spacedata.first) {
    return static_cast<eUserPref_CategoryTabsDisplayMode>(U.category_tabs_display_mode);
  }

  switch (area->spacetype) {
    case SPACE_VIEW3D: {
      const View3D *v3d = static_cast<const View3D *>(area->spacedata.first);
      return static_cast<eUserPref_CategoryTabsDisplayMode>(v3d->category_tabs_display_mode);
    }
    case SPACE_PROPERTIES: {
      const SpaceProperties *sbuts = static_cast<const SpaceProperties *>(area->spacedata.first);
      return static_cast<eUserPref_CategoryTabsDisplayMode>(sbuts->category_tabs_display_mode);
    }
    case SPACE_NODE: {
      const SpaceNode *snode = static_cast<const SpaceNode *>(area->spacedata.first);
      return static_cast<eUserPref_CategoryTabsDisplayMode>(snode->category_tabs_display_mode);
    }
    case SPACE_IMAGE: {
      const SpaceImage *sima = static_cast<const SpaceImage *>(area->spacedata.first);
      return static_cast<eUserPref_CategoryTabsDisplayMode>(sima->category_tabs_display_mode);
    }
    default:
      return static_cast<eUserPref_CategoryTabsDisplayMode>(U.category_tabs_display_mode);
  }
}

float ED_category_tabs_zoom_get(const ScrArea *area)
{
  float category_tabs_zoom = 1.0f;

  if (area && area->spacedata.first) {
    switch (area->spacetype) {
      case SPACE_VIEW3D: {
        const View3D *v3d = static_cast<const View3D *>(area->spacedata.first);
        switch (ED_category_tabs_display_mode_get(area)) {
          case USER_CATEGORY_TABS_GLYPHS_ONLY:
            category_tabs_zoom = v3d->category_tabs_zoom_icon;
            break;
          case USER_CATEGORY_TABS_GLYPHS_TEXT:
            category_tabs_zoom = v3d->category_tabs_zoom_mixed;
            break;
          case USER_CATEGORY_TABS_TEXT_ONLY:
          default:
            category_tabs_zoom = v3d->category_tabs_zoom_text;
            break;
        }
        break;
      }
      case SPACE_PROPERTIES: {
        const SpaceProperties *sbuts = static_cast<const SpaceProperties *>(area->spacedata.first);
        switch (ED_category_tabs_display_mode_get(area)) {
          case USER_CATEGORY_TABS_GLYPHS_ONLY:
            category_tabs_zoom = sbuts->category_tabs_zoom_icon;
            break;
          case USER_CATEGORY_TABS_GLYPHS_TEXT:
            category_tabs_zoom = sbuts->category_tabs_zoom_mixed;
            break;
          case USER_CATEGORY_TABS_TEXT_ONLY:
          default:
            category_tabs_zoom = sbuts->category_tabs_zoom_text;
            break;
        }
        break;
      }
      case SPACE_NODE: {
        const SpaceNode *snode = static_cast<const SpaceNode *>(area->spacedata.first);
        switch (ED_category_tabs_display_mode_get(area)) {
          case USER_CATEGORY_TABS_GLYPHS_ONLY:
            category_tabs_zoom = snode->category_tabs_zoom_icon;
            break;
          case USER_CATEGORY_TABS_GLYPHS_TEXT:
            category_tabs_zoom = snode->category_tabs_zoom_mixed;
            break;
          case USER_CATEGORY_TABS_TEXT_ONLY:
          default:
            category_tabs_zoom = snode->category_tabs_zoom_text;
            break;
        }
        break;
      }
      case SPACE_IMAGE: {
        const SpaceImage *sima = static_cast<const SpaceImage *>(area->spacedata.first);
        switch (ED_category_tabs_display_mode_get(area)) {
          case USER_CATEGORY_TABS_GLYPHS_ONLY:
            category_tabs_zoom = sima->category_tabs_zoom_icon;
            break;
          case USER_CATEGORY_TABS_GLYPHS_TEXT:
            category_tabs_zoom = sima->category_tabs_zoom_mixed;
            break;
          case USER_CATEGORY_TABS_TEXT_ONLY:
          default:
            category_tabs_zoom = sima->category_tabs_zoom_text;
            break;
        }
        break;
      }
      default:
        category_tabs_zoom = 0.0f;
        break;
    }
  }

  if (category_tabs_zoom > 0.0f) {
    return category_tabs_zoom;
  }

  switch (ED_category_tabs_display_mode_get(area)) {
    case USER_CATEGORY_TABS_GLYPHS_ONLY:
      return U.category_tabs_zoom_icon;
    case USER_CATEGORY_TABS_GLYPHS_TEXT:
      return U.category_tabs_zoom_mixed;
    case USER_CATEGORY_TABS_TEXT_ONLY:
    default:
      return U.category_tabs_zoom_text;
  }
}

/* -------------------------------------------------------------------- */
/** \name Generic Tool System Region Callbacks
 * \{ */

void ED_region_generic_tools_region_message_subscribe(const wmRegionMessageSubscribeParams *params)
{
  wmMsgBus *mbus = params->message_bus;
  ARegion *region = params->region;

  wmMsgSubscribeValue msg_sub_value_region_tag_redraw{};
  msg_sub_value_region_tag_redraw.owner = region;
  msg_sub_value_region_tag_redraw.user_data = region;
  msg_sub_value_region_tag_redraw.notify = ED_region_do_msg_notify_tag_redraw;
  WM_msg_subscribe_rna_anon_prop(mbus, WorkSpace, tools, &msg_sub_value_region_tag_redraw);
}

int ED_region_generic_tools_region_snap_size(const ARegion *region, int size, int axis)
{
  if (axis == 0) {
    /* Using Y axis avoids slight feedback loop when adjusting X. */
    const float aspect = BLI_rctf_size_y(&region->v2d.cur) /
                         (BLI_rcti_size_y(&region->v2d.mask) + 1);
    const float column = UI_TOOLBAR_COLUMN / aspect;
    const float margin = UI_TOOLBAR_MARGIN / aspect;
    const float snap_units[] = {
        column + margin,
        (2.0f * column) + margin,
        (2.7f * column) + margin,
    };
    int best_diff = std::numeric_limits<int>::max();
    int best_size = size;
    /* Only snap if less than last snap unit. */
    if (size <= snap_units[ARRAY_SIZE(snap_units) - 1]) {
      for (uint i = 0; i < ARRAY_SIZE(snap_units); i += 1) {
        const int test_size = snap_units[i];
        const int test_diff = abs(test_size - size);
        if (test_diff < best_diff) {
          best_size = test_size;
          best_diff = test_diff;
        }
      }
    }
    return best_size;
  }
  return size;
}

int ED_region_generic_panel_region_snap_size(const ARegion *region, int size, int axis)
{
  return ED_region_generic_panel_region_snap_size_with_area(nullptr, region, size, axis);
}

int ED_region_generic_panel_region_snap_size_with_area(const ScrArea *area,
                                                       const ARegion *region,
                                                       int size,
                                                       int axis)
{
  if (axis == 0) {
    if (!ui::panel_category_tabs_is_visible(region)) {
      return size;
    }

    /* Using Y axis avoids slight feedback loop when adjusting X. */
    const float aspect = BLI_rctf_size_y(&region->v2d.cur) /
                         (BLI_rcti_size_y(&region->v2d.mask) + 1);
    const float safe_aspect = std::max(aspect, 0.0001f);
    const float category_tabs_zoom = ED_category_tabs_zoom_get(area);
    const eUserPref_CategoryTabsDisplayMode display_mode = ED_category_tabs_display_mode_get(area);
    const float visual_effect_margin = (U.category_tabs_visual_effect &&
                                        display_mode == USER_CATEGORY_TABS_GLYPHS_ONLY) ?
                                           UI_TABS_VISUAL_EFFECT_MARGIN :
                                           1.0f;
    const float zoom = (1.0f / safe_aspect) * category_tabs_zoom;

    const int category_tabs_width = int(
        std::lround(double(UI_PANEL_CATEGORY_MARGIN_WIDTH * zoom * visual_effect_margin)));
    const int legacy_min_width =
        int(std::ceil(double(UI_PANEL_CATEGORY_MIN_WIDTH * UI_SCALE_FAC / safe_aspect)));
    const int category_tabs_min_width = std::max(category_tabs_width, legacy_min_width);

    return int(std::ceil(float(category_tabs_min_width) * safe_aspect / UI_SCALE_FAC));
  }
  return size;
}

/** \} */

}  // namespace blender
