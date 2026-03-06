# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(INC
  ../..

  ../include
  ../paint_cursor_sync
  ../sculpt_paint
  ../space_image
  ../../imbuf
  ../../blenlib
  ../../makesrna
  ../../blenkernel
  ../../gpu
  ../../../../intern/guardedalloc
  ${CMAKE_BINARY_DIR}/source/blender/makesrna
)

set(INC_SYS
)

set(SRC
  ED_paint_cursor_sync.cc
  paint_cursor_sync_types.hh
  paint_cursor_sync_manager.hh
  paint_cursor_sync_manager.cc

  sources/sculpt_cursor_source.hh
  sources/sculpt_cursor_source.cc

  targets/image_editor_cursor_target.hh
  targets/image_editor_cursor_target.cc
)

set(LIB
  PRIVATE bf::blenkernel
  PRIVATE bf::blenlib
  PRIVATE bf::blenloader
  PRIVATE bf::draw
  PRIVATE bf::windowmanager
)

blender_add_lib(bf_editor_paint_cursor_sync "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
