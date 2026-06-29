/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Internal shared state and helpers for the Extract Region tool.
 */

#pragma once

#include "sculpt_extract_region.hh"
#include "sculpt_extract_shared.hh"

struct BMFace;
struct bContext;

namespace blender::ed::sculpt_paint::extract_region {

struct ExtractRegionModalData {
  extract::ExtractSharedData shared;
  extract::ExtrudeState extrude;
  RegionSource source = RegionSource::FaceSet;
  float mask_threshold = 0.5f;
  BMFace *seed_face = nullptr;
  void *draw_handle = nullptr;
};

/* _select.cc */
BMFace *find_seed_face(extract::ExtractSharedData &shared, const float mval[2]);
void select_region(extract::ExtractSharedData &shared,
                   RegionSource source,
                   float mask_threshold,
                   BMFace *seed_face);
bool region_is_valid(const extract::ExtractSharedData &shared);

/* _hover.cc */
void sync_preview_from_hover(ExtractRegionModalData &data);

/* _region core */
void gesture_data_free(bContext *C, ExtractRegionModalData *data);

}  // namespace blender::ed::sculpt_paint::extract_region
