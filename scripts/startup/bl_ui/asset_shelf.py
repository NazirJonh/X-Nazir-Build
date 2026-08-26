# SPDX-FileCopyrightText: 2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from bpy.types import (
    Panel,
)


class ASSETSHELF_PT_display(Panel):
    bl_label = "Display Settings"
    # Doesn't actually matter. Panel is instanced through popover only.
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_ui_units_x = 10

    def draw(self, context):
        layout = self.layout

        layout.use_property_split = True
        layout.use_property_decorate = False  # No animation.

        shelf = context.asset_shelf

        col = layout.column()

        col.prop(shelf, "preview_size_preset", expand=True)
        col.prop(shelf, "preview_size")

        col.prop(shelf, "show_names", text="Names")

        if shelf.bl_idname.startswith(("VIEW3D_AST_brush", "IMAGE_AST_brush")) or shelf.bl_idname == "VIEW3D_AST_image_texture":
            col.prop(shelf, "show_favorite_icons", text="Favorite Icons")

    @classmethod
    def poll(cls, context):
        return context.asset_shelf is not None


class ASSETSHELF_PT_filter(Panel):
    bl_label = "Filter"
    # Doesn't actually matter. Panel is instanced through popover only.
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_parent_id = "ASSETSHELF_PT_display"

    def draw(self, context):
        layout = self.layout
        prefs = context.preferences
        use_remote_asset_libraries = prefs.experimental.use_remote_asset_libraries

        # Filter option stored in the Preferences.
        if use_remote_asset_libraries:
            col = layout.column()
            col.use_property_split = True
            col.use_property_decorate = False
            col.prop(prefs.view, "asset_access", text="Access")


classes = (
    ASSETSHELF_PT_display,
    ASSETSHELF_PT_filter,
)


if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class

    for cls in classes:
        register_class(cls)
