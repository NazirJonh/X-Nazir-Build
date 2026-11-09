# SPDX-FileCopyrightText: 2026 Nazir
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""The live Extract feature of Sculpt Mode.

The live Extract (``extract_live``) makes a new object from the masked faces and shapes it from a
viewport panel. The shared pieces live next to it: the scene settings (``settings``), the overlay
palette (``theme``), the viewport panel toolkit (``panel_ui``) with Extract's panel definition
(``extract_panel``), the per-object draft build (``extract_parts``), the pure mesh math
(``extract_geometry``) and the preview drawing (``extract_preview``).
"""

from __future__ import annotations

# support reloading sub-modules
if "bpy" in locals():
    from importlib import reload
    _submodules_loaded = [reload(val) for val in _submodules_loaded]
    del reload

_submodules = (
    "theme",
    "panel_ui",
    "settings",
    "extract_geometry",
    "extract_parts",
    "extract_panel",
    "extract_preview",
    "extract_live",
)

import bpy

__import__(name=__name__, fromlist=_submodules)
_namespace = globals()
_submodules_loaded = [_namespace[name] for name in _submodules]
del _namespace

# The classes are registered by `bl_operators.register()`'s generic loop over `mod.classes`.
classes = (
    settings.SculptMaskExtraSettings,
    settings.VIEW3D_PT_sculpt_mask_settings,
    panel_ui.PanelPlacementPrefs,
    *extract_live.classes,
)


def register():
    from bpy.props import PointerProperty

    if not hasattr(bpy.types.Scene, "sculpt_mask_extra"):
        bpy.types.Scene.sculpt_mask_extra = PointerProperty(
            name="Mask & Extract Settings",
            description="Extra settings for the live Extract feature",
            type=settings.SculptMaskExtraSettings,
        )
    # The saved size and place of the Extract panel (see `panel_ui.PanelPlacementPrefs`).
    if not hasattr(bpy.types.WindowManager, "sculpt_panel_prefs"):
        bpy.types.WindowManager.sculpt_panel_prefs = PointerProperty(
            name="Panel Placement",
            description="Saved size and place of the Extract viewport panel",
            type=panel_ui.PanelPlacementPrefs,
        )


def unregister():
    if hasattr(bpy.types.Scene, "sculpt_mask_extra"):
        del bpy.types.Scene.sculpt_mask_extra
    if hasattr(bpy.types.WindowManager, "sculpt_panel_prefs"):
        del bpy.types.WindowManager.sculpt_panel_prefs
