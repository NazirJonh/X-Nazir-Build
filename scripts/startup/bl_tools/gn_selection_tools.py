# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy
from bpy.types import ToolDef


def gn_selection_toolbar_draw(context, layout, tool):
    """Draw additional options for GN Selection tool."""
    obj = context.active_object
    if not obj:
        return

    layout.label(text="Selection Domain:")

    # Use operator buttons to switch domain
    row = layout.row(align=True)
    props = row.operator("gn.select_mode", text="", icon='VERTEXSEL')
    props.type = 0  # Point/Vertex
    props = row.operator("gn.select_mode", text="", icon='EDGESEL')
    props.type = 1  # Edge
    props = row.operator("gn.select_mode", text="", icon='FACESEL')
    props.type = 2  # Face


class _GN_SelectTool:
    """Base class for GN Selection tools"""

    @staticmethod
    def gn_select_draw_settings(context, layout, tool):
        gn_selection_toolbar_draw(context, layout, tool)


@ToolDef.from_fn
def GN_SELECT():
    return dict(
        idname="gn.select",
        label="Select",
        description="Select elements in GN Selection Mode",
        icon="ops.generic.select",
        cursor='CROSSHAIR',
        keymap="GN Selection",
    )


def get_tool_list(space_type, context_mode):
    """Get the tool list for a given space type and context mode."""
    from bl_ui.space_toolsystem_common import ToolSelectPanelHelper
    return ToolSelectPanelHelper._tool_class_from_space_type(space_type).tools_from_context(context_mode)


def register():
    """Register GN Selection tools."""
    pass


def unregister():
    """Unregister GN Selection tools."""
    pass


if __name__ == "__main__":
    register()
