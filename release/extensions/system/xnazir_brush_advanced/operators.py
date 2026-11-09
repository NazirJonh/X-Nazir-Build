# SPDX-FileCopyrightText: 2026 XNazir
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy
from bpy.props import EnumProperty, StringProperty
from bpy.types import Operator


# Sculpt panels whose saved size and place can be reset: (label, `WindowManager` property). To
# list another panel, add a row here.
PANELS = (
    ("Extract panel", "sculpt_panel_prefs"),
)

_persistence_module = None


def _persistence():
    """The core persistence module (found once), or None when this build does not have it."""
    global _persistence_module
    if _persistence_module is None:
        try:
            import brush_settings_persistence
        except ImportError:
            return None
        _persistence_module = brush_settings_persistence
    return _persistence_module


def panel_prefs(context, attribute):
    """The saved placement group of a panel, or None when its tool is not registered."""
    return getattr(context.window_manager, attribute, None)


class XNAZIR_BRUSH_OT_forget(Operator):
    """Forget the remembered settings of brushes.

    Brushes keep their current values until Blender is restarted; only what is remembered for
    the next session is removed.
    """
    bl_idname = "xnazir_brush.forget_saved"
    bl_label = "Reset Saved Brush Settings"
    bl_options = {'INTERNAL'}

    mode: EnumProperty(
        name="Mode",
        items=[
            ('ALL', "All", "Every remembered brush"),
            ('LIBRARY', "Library", "Every remembered brush of one asset library"),
            ('BRUSH', "Brush", "One remembered brush"),
        ],
        default='BRUSH')
    library: StringProperty(name="Library")
    key: StringProperty(name="Brush Key")

    @classmethod
    def description(cls, _context, properties):
        note = " (brushes keep their current values until Blender is restarted)"
        return {
            'ALL': "Forget the remembered settings of every brush" + note,
            'LIBRARY': "Forget the remembered settings of every brush of this library" + note,
            'BRUSH': "Forget the remembered settings of this brush" + note,
        }[properties.mode]

    def invoke(self, context, event):
        if self.mode == 'BRUSH':
            return self.execute(context)
        return context.window_manager.invoke_confirm(self, event)

    def execute(self, _context):
        module = _persistence()
        if module is None:
            self.report({'ERROR'}, "Brush settings persistence is not available")
            return {'CANCELLED'}
        if self.mode == 'ALL':
            module.forget([key for key, _lib, _name in module.saved_brushes()])
        elif self.mode == 'LIBRARY':
            if not self.library:
                self.report({'ERROR'}, "No library given")
                return {'CANCELLED'}
            module.forget_library(self.library)
        else:
            if not self.key:
                self.report({'ERROR'}, "No brush given")
                return {'CANCELLED'}
            module.forget([self.key])
        return {'FINISHED'}


class XNAZIR_BRUSH_OT_reset_panel_layout(Operator):
    """Reset the saved size and position of a sculpt panel to its defaults"""
    bl_idname = "xnazir_brush.reset_panel_layout"
    bl_label = "Reset Panel Layout"
    bl_options = {'INTERNAL'}

    # The `WindowManager` property of the panel, one of `PANELS`.
    attribute: StringProperty(name="Panel")

    def execute(self, context):
        if self.attribute not in {attribute for _label, attribute in PANELS}:
            return {'CANCELLED'}
        prefs = panel_prefs(context, self.attribute)
        if prefs is None:
            self.report({'ERROR'}, "This panel's settings are not available")
            return {'CANCELLED'}
        for prop in prefs.bl_rna.properties:
            if prop.identifier == "rna_type" or prop.is_readonly:
                continue
            if getattr(prop, "is_array", False):
                setattr(prefs, prop.identifier, prop.default_array)
            else:
                setattr(prefs, prop.identifier, prop.default)
        return {'FINISHED'}


_classes = (
    XNAZIR_BRUSH_OT_forget,
    XNAZIR_BRUSH_OT_reset_panel_layout,
)


def register():
    for cls in _classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(_classes):
        bpy.utils.unregister_class(cls)
