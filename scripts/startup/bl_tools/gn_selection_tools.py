# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy
from bl_ui.space_toolsystem_common import ToolDef


class _defs_gn_selection:
    """GN Selection Mode tool definitions."""

    @ToolDef.from_fn
    def select():
        def draw_settings(context, layout, tool):
            props = tool.operator_properties("GN_OT_select_mode")
            if props:
                row = layout.row(align=True)
                row.prop_enum(props, "type", value=0, text="", icon='VERTEXSEL')
                row.prop_enum(props, "type", value=1, text="", icon='EDGESEL')
                row.prop_enum(props, "type", value=2, text="", icon='FACESEL')

        return dict(
            idname="gn.select",
            label="Select",
            description="Select elements in GN Selection Mode",
            icon="ops.generic.select",
            cursor='CROSSHAIR',
            keymap="GN Selection Tool: Select",
            draw_settings=draw_settings,
        )

    @ToolDef.from_fn
    def box():
        def draw_settings(_context, layout, tool):
            props = tool.operator_properties("GN_OT_select_box")
            row = layout.row()
            row.use_property_split = False
            row.prop(props, "mode", text="", expand=True, icon_only=True)

        return dict(
            idname="gn.select_box",
            label="Select Box",
            description="Box select elements in GN Selection Mode",
            icon="ops.generic.select_box",
            cursor='CROSSHAIR',
            keymap="GN Selection Tool: Box",
            draw_settings=draw_settings,
        )

    @ToolDef.from_fn
    def lasso():
        def draw_settings(_context, layout, tool):
            props = tool.operator_properties("GN_OT_select_lasso")
            row = layout.row()
            row.use_property_split = False
            row.prop(props, "mode", text="", expand=True, icon_only=True)

        return dict(
            idname="gn.select_lasso",
            label="Select Lasso",
            description="Lasso select elements in GN Selection Mode",
            icon="ops.generic.select_lasso",
            cursor='CROSSHAIR',
            keymap="GN Selection Tool: Lasso",
            draw_settings=draw_settings,
        )

    @ToolDef.from_fn
    def circle():
        def draw_settings(_context, layout, tool):
            props = tool.operator_properties("GN_OT_select_circle")
            row = layout.row()
            row.use_property_split = False
            row.prop(props, "mode", text="", expand=True, icon_only=True)
            layout.prop(props, "radius")

        def draw_cursor(_context, tool, xy):
            from gpu_extras.presets import draw_circle_2d
            props = tool.operator_properties("GN_OT_select_circle")
            radius = props.radius
            draw_circle_2d(xy, (1.0,) * 4, radius, segments=32)

        return dict(
            idname="gn.select_circle",
            label="Select Circle",
            description="Circle select elements in GN Selection Mode",
            icon="ops.generic.select_circle",
            cursor='CROSSHAIR',
            keymap="GN Selection Tool: Circle",
            draw_settings=draw_settings,
            draw_cursor=draw_cursor,
        )


def register():
    """Register GN Selection tools."""
    pass


def unregister():
    """Unregister GN Selection tools."""
    pass


if __name__ == "__main__":
    register()
