# SPDX-FileCopyrightText: 2026 XNazir
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy

from . import (
    operators,
    ui,
)

_is_registered = False


def _set_addon_active(active, _attempt=0):
    """Toggle the core runtime flag that enables user-defined (custom) cursor buttons.

    The property lives on the window manager but is backed by process memory, so any
    manager works. It is set before the UI is registered so the first redraw already
    sees the feature enabled. A window manager may not exist yet in unusual startup
    contexts, so retry briefly on a timer.
    """
    wm = bpy.context.window_manager
    if wm is None:
        wms = bpy.data.window_managers
        wm = wms[0] if len(wms) else None
    if wm is not None:
        wm.sculpt_cursor_addon_active = active
        return
    if _attempt < 10:
        def retry():
            # The add-on may have been toggled while the timer was pending.
            if _is_registered == active:
                _set_addon_active(active, _attempt + 1)
            return None
        bpy.app.timers.register(retry, first_interval=0.1)


def register():
    global _is_registered
    if _is_registered:
        return
    _is_registered = True

    _set_addon_active(True)

    ui.register()
    operators.register()


def unregister():
    global _is_registered
    if not _is_registered:
        return
    _is_registered = False

    operators.unregister()
    ui.unregister()

    _set_addon_active(False)
