# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Sculpt Layers in the Outliner's Stack Layers display mode, as an add-on.

Registers a stack source through the Outliner Stack Layer Python API
(`bpy.types.OutlinerStackSource`): the object's sculpt layer tree as Stack Layers rows, the
Focus Stack button on the View Layer's object rows, and the row operators and menus the rows
use. Disabling the add-on takes all of it back out, and the Outliner falls back to its first
built-in source.
"""

bl_info = {
    "name": "Sculpt Layers Outliner",
    "author": "Nazir Galimov",
    "version": (1, 0, 0),
    "blender": (5, 2, 0),
    "location": "Outliner > Display Mode > Stack Layers",
    "description": "Show an object's sculpt layers as the Outliner's Stack Layers rows",
    "support": 'OFFICIAL',
    "category": "Interface",
}

if "source" in locals():
    from importlib import reload

    # The row map and the helpers the operators read live in the source module: reload it
    # before the modules that import from it.
    source = reload(source)
    menus = reload(menus)
    operators = reload(operators)
    del reload
else:
    from . import menus, operators, source


def register():
    operators.register()
    menus.register()
    source.register()


def unregister():
    source.unregister()
    menus.unregister()
    operators.unregister()
