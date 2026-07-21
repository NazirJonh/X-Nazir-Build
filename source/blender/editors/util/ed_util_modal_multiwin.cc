/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "ED_util_modal_multiwin.hh"

#include "BKE_context.hh"

#include "DNA_screen_types.h"

#include "ED_screen.hh"

#include "WM_api.hh"
#include "WM_types.hh"

namespace blender::ed {

ModalViewportTracker::ModalViewportTracker(bContext &C,
                                           const wmEvent &event,
                                           const int space_type,
                                           const int region_type)
    : C_(C), event_(event), prev_area_(CTX_wm_area(&C)), prev_region_(CTX_wm_region(&C))
{
  wmWindow *win = CTX_wm_window(&C_);
  ScrArea *area = nullptr;
  ARegion *region = ED_screen_area_region_under_cursor(
      win, space_type, region_type, event_.xy, &area);
  apply_region(area, region);
}

ModalViewportTracker::~ModalViewportTracker()
{
  CTX_wm_area_set(&C_, prev_area_);
  CTX_wm_region_set(&C_, prev_region_);
}

void ModalViewportTracker::apply_region(ScrArea *area, ARegion *region)
{
  area_ = area;
  region_ = region;
  if (region_) {
    CTX_wm_area_set(&C_, area_);
    CTX_wm_region_set(&C_, region_);
    mval_.x = event_.xy[0] - region_->winrct.xmin;
    mval_.y = event_.xy[1] - region_->winrct.ymin;
  }
  else {
    mval_.x = event_.mval[0];
    mval_.y = event_.mval[1];
  }
}

void ModalViewportTracker::use_fallback_region(ARegion *fallback_region)
{
  if (found() || !fallback_region) {
    return;
  }
  bScreen *screen = WM_window_get_active_screen(CTX_wm_window(&C_));
  ScrArea *area = ED_screen_area_of_region(screen, fallback_region);
  if (!area) {
    return;
  }
  apply_region(area, fallback_region);
}

}  // namespace blender::ed
