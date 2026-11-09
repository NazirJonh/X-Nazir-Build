# SPDX-FileCopyrightText: 2026 XNazir
#
# SPDX-License-Identifier: GPL-2.0-or-later

import os

import bpy
from bpy.props import BoolProperty, EnumProperty
from bpy.types import AddonPreferences

from . import operators

try:
    # One definition of the defaults, in the core module.
    from brush_settings_persistence import DEFAULT_ENABLED, DEFAULT_SCOPE
except ImportError:
    DEFAULT_ENABLED = True
    DEFAULT_SCOPE = 'ESSENTIALS'


def _config_dir():
    return bpy.utils.user_resource('CONFIG')


class XNazirBrushAdvancedPreferences(AddonPreferences):
    # The core module `brush_settings_persistence` reads these two settings by name. `__package__`
    # is "bl_ext.system.xnazir_brush_advanced", the key of `preferences.addons` and the core's
    # `ADDON_ID`.
    bl_idname = __package__

    persistence_enabled: BoolProperty(
        name="Remember Brush Settings",
        description="Sculpt brushes keep their settings between sessions and files. "
        "Without this extension (or while it is disabled) the feature stays on with its defaults",
        default=DEFAULT_ENABLED)
    persistence_scope: EnumProperty(
        name="Apply To",
        description="Which asset libraries' brushes are remembered",
        items=[
            ('ESSENTIALS', "Essentials Only", "Only brushes of the Essentials library"),
            ('ALL', "All Libraries", "Brushes of every asset library"),
        ],
        default=DEFAULT_SCOPE)

    def draw(self, context):
        layout = self.layout
        self._draw_persistence(layout)
        self._draw_panel_ui(context, layout)

        layout.separator()
        row = layout.row()
        row.enabled = context.preferences.is_dirty
        row.operator("wm.save_userpref", icon='FILE_TICK',
                     text="Save Preferences" + (" *" if context.preferences.is_dirty else ""))

    def _draw_persistence(self, layout):
        persistence = operators._persistence()

        box = layout.box()
        box.label(text="Brush Settings Persistence", icon='BRUSH_DATA')
        box.label(text="Disabling this extension does not turn the feature off.", icon='INFO')
        col = box.column()
        col.use_property_split = True
        col.prop(self, "persistence_enabled")
        sub = col.column()
        sub.active = self.persistence_enabled
        sub.prop(self, "persistence_scope", expand=True)

        if persistence is None:
            box.label(text="Not available in this build", icon='ERROR')
            return
        path = os.path.join(_config_dir(), persistence.FILE_NAME)
        row = box.row()
        row.label(text=path, icon='FILE')
        row.operator("wm.path_open", text="", icon='FILE_FOLDER').filepath = _config_dir()

        brushes = persistence.saved_brushes()
        header = box.row()
        header.label(text="Remembered brushes: {} (reset keeps current values until restart)".format(
            len(brushes)))
        sub = header.row()
        sub.enabled = bool(brushes)
        props = sub.operator("xnazir_brush.forget_saved", text="Reset All", icon='TRASH')
        props.mode = 'ALL'

        libraries = {}
        for key, library, name in brushes:
            libraries.setdefault(library, []).append((key, name))
        for library, items in libraries.items():
            head, body = box.panel("xnazir_brush_lib_" + library, default_closed=True)
            head.label(text="{} ({})".format(library, len(items)), icon='ASSET_MANAGER')
            props = head.operator("xnazir_brush.forget_saved", text="Reset", icon='TRASH')
            props.mode = 'LIBRARY'
            props.library = library
            if body is None:
                continue
            for key, name in items:
                row = body.row()
                row.label(text=name, icon='BRUSH_DATA')
                props = row.operator("xnazir_brush.forget_saved", text="", icon='X')
                props.mode = 'BRUSH'
                props.key = key

    def _draw_panel_ui(self, context, layout):
        box = layout.box()
        box.label(text="Sculpt Panel UI", icon='WINDOW')
        for label, attribute in operators.PANELS:
            prefs = operators.panel_prefs(context, attribute)
            row = box.row()
            if prefs is None:
                row.label(text="{}: not available".format(label))
                continue
            row.label(text="{}: scale {:.2f}, offset {:.0f}, {:.0f}".format(
                label, prefs.ui_scale, prefs.offset_x, prefs.offset_y))
            props = row.operator("xnazir_brush.reset_panel_layout", text="", icon='LOOP_BACK')
            props.attribute = attribute


def register():
    bpy.utils.register_class(XNazirBrushAdvancedPreferences)


def unregister():
    bpy.utils.unregister_class(XNazirBrushAdvancedPreferences)
