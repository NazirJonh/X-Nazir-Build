/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "MEM_guardedalloc.h"

#include "editors/include/ED_paint_cursor_sync.hh"

#include "paint_cursor_sync_manager.hh"

namespace blender::editors {

void paint_cursor_sync_init()
{
  PaintCursorSyncManager::initialize();
}

void paint_cursor_sync_exit()
{
  PaintCursorSyncManager::shutdown();
}

}  // namespace blender::editors
