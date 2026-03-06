/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "MEM_guardedalloc.h"

#include "sculpt_cursor_source.hh"

#include "../paint_cursor_sync_manager.hh"

namespace blender::editors {

SculptCursorSource::SculptCursorSource() = default;

SculptCursorSource::~SculptCursorSource() = default;

std::string_view SculptCursorSource::get_id() const
{
  return "sculpt_cursor";
}

bool SculptCursorSource::is_active() const
{
  return is_active_;
}

CursorSyncData SculptCursorSource::get_sync_data() const
{
  return data_;
}

void SculptCursorSource::set_update_callback(UpdateCallback callback)
{
  callback_ = std::move(callback);
}



}  // namespace blender::editors
