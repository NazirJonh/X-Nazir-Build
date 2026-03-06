# SPDX-FileCopyrightText: 2009-2024 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy
from bpy.types import Menu, Panel, UIList
from rna_prop_ui import PropertyPanel

from bl_ui.space_properties import PropertiesAnimationMixin


class DataButtonsPanel:
    bl_space_type = "PROPERTIES"
    bl_region_type = "WINDOW"
    bl_context = "data"

    @classmethod
    def poll(cls, context):
        engine = context.scene.render.engine
        return (
            hasattr(context, "curves")
            and context.curves
            and (engine in cls.COMPAT_ENGINES)
        )


class DATA_PT_context_curves(DataButtonsPanel, Panel):
    bl_label = ""
    bl_options = {"HIDE_HEADER"}
    COMPAT_ENGINES = {
        "BLENDER_RENDER",
        "BLENDER_EEVEE",
        "BLENDER_WORKBENCH",
    }

    def draw(self, context):
        layout = self.layout

        ob = context.object
        curves = context.curves
        space = context.space_data

        if ob:
            layout.template_ID(ob, "data")
        elif curves:
            layout.template_ID(space, "pin_id")


class DATA_PT_curves_surface(DataButtonsPanel, Panel):
    bl_label = "Surface"
    COMPAT_ENGINES = {
        "BLENDER_RENDER",
        "BLENDER_EEVEE",
        "BLENDER_WORKBENCH",
    }

    def draw(self, context):
        layout = self.layout
        ob = context.object

        layout.use_property_split = True

        layout.prop(ob.data, "surface")
        has_surface = ob.data.surface is not None
        if has_surface:
            layout.prop_search(
                ob.data,
                "surface_uv_map",
                ob.data.surface.data,
                "uv_layers",
                text="UV Map",
                icon="GROUP_UVS",
            )
        else:
            row = layout.row()
            row.prop(ob.data, "surface_uv_map", text="UV Map")
            row.active = has_surface


# -------------------------------------------------------------------- #
# Vertex Groups


class CURVES_UL_vgroups(UIList):
    def draw_item(
        self,
        _context,
        layout,
        _data,
        item,
        icon,
        _active_data,
        _active_propname,
        _index,
    ):
        vgroup = item
        if self.layout_type in {"DEFAULT", "COMPACT"}:
            layout.prop(vgroup, "name", text="", emboss=False, icon_value=icon)
            icon = "LOCKED" if vgroup.lock_weight else "UNLOCKED"
            layout.prop(vgroup, "lock_weight", text="", icon=icon, emboss=False)
        elif self.layout_type == "GRID":
            layout.alignment = "CENTER"
            layout.label(text="", icon_value=icon)


class CURVES_MT_vertex_group_context_menu(Menu):
    bl_label = "Vertex Group Specials"

    def draw(self, context):
        layout = self.layout

        layout.operator(
            "object.vertex_group_sort",
            icon="SORTALPHA",
            text="Sort by Name",
        ).sort_type = "NAME"
        layout.operator(
            "object.vertex_group_sort",
            icon="BONE_DATA",
            text="Sort by Bone Hierarchy",
        ).sort_type = "BONE_HIERARCHY"

        layout.separator()

        layout.operator("object.vertex_group_copy", icon="DUPLICATE")
        layout.operator("object.vertex_group_copy_to_selected", text="Copy to Selected")

        layout.separator()

        layout.operator("object.vertex_group_mirror", icon="ARROW_LEFTRIGHT")
        layout.operator(
            "object.vertex_group_mirror", text="Mirror Active Group (Topology)"
        ).use_topology = True

        layout.separator()

        props = layout.operator("object.vertex_group_remove", text="Remove All")
        props.all = True
        props.all_unlocked = False

        props = layout.operator(
            "object.vertex_group_remove", text="Remove All Unlocked"
        )
        props.all = False
        props.all_unlocked = True

        layout.separator()

        layout.operator("object.vertex_group_lock_all", text="Lock All").action = "LOCK"
        layout.operator(
            "object.vertex_group_lock_all", text="Unlock All"
        ).action = "UNLOCK"


class DATA_PT_curves_vertex_groups(DataButtonsPanel, Panel):
    bl_label = "Vertex Groups"
    COMPAT_ENGINES = {
        "BLENDER_RENDER",
        "BLENDER_EEVEE",
        "BLENDER_WORKBENCH",
    }

    @classmethod
    def poll(cls, context):
        engine = context.scene.render.engine
        obj = context.object
        return obj and obj.type == "CURVES" and (engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        layout = self.layout

        ob = context.object
        group = ob.vertex_groups.active

        rows = 3
        if group:
            rows = 5

        row = layout.row()
        row.template_list(
            "CURVES_UL_vgroups",
            "",
            ob,
            "vertex_groups",
            ob.vertex_groups,
            "active_index",
            rows=rows,
        )

        col = row.column(align=True)

        # Use curves-specific operator in Weight Paint mode to initialize weights
        if ob.mode == "WEIGHT_CURVES":
            col.operator("curves.vertex_group_add", icon="ADD", text="")
        else:
            col.operator("object.vertex_group_add", icon="ADD", text="")
        props = col.operator("object.vertex_group_remove", icon="REMOVE", text="")
        props.all_unlocked = props.all = False

        col.separator()

        col.menu("CURVES_MT_vertex_group_context_menu", icon="DOWNARROW_HLT", text="")

        if group:
            col.separator()
            col.operator(
                "object.vertex_group_move", icon="TRIA_UP", text=""
            ).direction = "UP"
            col.operator(
                "object.vertex_group_move", icon="TRIA_DOWN", text=""
            ).direction = "DOWN"

        # Show assign/remove buttons in Weight Paint mode for Curves
        if ob.vertex_groups and ob.mode == "WEIGHT_CURVES":
            row = layout.row()

            sub = row.row(align=True)
            sub.operator("curves.vertex_group_assign", text="Assign")
            sub.operator("curves.vertex_group_remove_from", text="Remove")

            col = layout.column(align=True)
            col.separator()
            col.use_property_split = True
            col.use_property_decorate = False
            col.prop(context.tool_settings, "vertex_group_weight", text="Weight")
            col.prop(context.tool_settings, "use_auto_normalize", text="Auto Normalize")


# -------------------------------------------------------------------- #
# Attributes


class CURVES_MT_attribute_context_menu(Menu):
    bl_label = "Attribute Specials"

    def draw(self, _context):
        layout = self.layout

        layout.operator("geometry.attribute_convert")


class CURVES_MT_add_attribute(Menu):
    bl_label = "Add Attribute"

    @staticmethod
    def add_standard_attribute(layout, curves, name, data_type, domain):
        exists = curves.attributes.get(name) is not None

        col = layout.column()
        col.enabled = not exists
        col.operator_context = "EXEC_DEFAULT"

        props = col.operator("geometry.attribute_add", text=name)
        props.name = name
        props.data_type = data_type
        props.domain = domain

    def draw(self, context):
        layout = self.layout
        curves = context.curves

        self.add_standard_attribute(layout, curves, "radius", "FLOAT", "POINT")
        self.add_standard_attribute(layout, curves, "color", "FLOAT_COLOR", "POINT")

        layout.separator()

        layout.operator_context = "INVOKE_DEFAULT"
        layout.operator("geometry.attribute_add", text="Custom...")


class CURVES_UL_attributes(UIList):
    def filter_items(self, _context, data, property):
        attributes = getattr(data, property)
        flags = []
        indices = [i for i in range(len(attributes))]

        # Filtering by name
        if self.filter_name:
            flags = bpy.types.UI_UL_list.filter_items_by_name(
                self.filter_name,
                self.bitflag_filter_item,
                attributes,
                "name",
                reverse=self.use_filter_invert,
            )
        if not flags:
            flags = [self.bitflag_filter_item] * len(attributes)

        # Filtering internal attributes
        for idx, item in enumerate(attributes):
            flags[idx] = 0 if item.is_internal else flags[idx]

        # Reorder by name.
        if self.use_filter_sort_alpha:
            indices = bpy.types.UI_UL_list.sort_items_by_name(attributes, "name")

        return flags, indices

    def draw_item(
        self,
        _context,
        layout,
        _data,
        attribute,
        _icon,
        _active_data,
        _active_propname,
        _index,
    ):
        data_type = attribute.bl_rna.properties["data_type"].enum_items[
            attribute.data_type
        ]
        domain = attribute.bl_rna.properties["domain"].enum_items[attribute.domain]

        split = layout.split(factor=0.5)
        split.emboss = "NONE"
        row = split.row()
        row.prop(attribute, "name", text="")
        sub = split.split()
        sub.alignment = "RIGHT"
        sub.active = False
        sub.label(text=domain.name)
        sub.label(text=data_type.name)


class DATA_PT_CURVES_attributes(DataButtonsPanel, Panel):
    bl_label = "Attributes"
    COMPAT_ENGINES = {
        "BLENDER_RENDER",
        "BLENDER_EEVEE",
        "BLENDER_WORKBENCH",
    }

    def draw(self, context):
        curves = context.curves

        layout = self.layout
        row = layout.row()

        col = row.column()
        col.template_list(
            "CURVES_UL_attributes",
            "attributes",
            curves,
            "attributes",
            curves.attributes,
            "active_index",
            rows=3,
        )

        col = row.column(align=True)
        col.menu("CURVES_MT_add_attribute", icon="ADD", text="")
        col.operator("geometry.attribute_remove", icon="REMOVE", text="")

        col.separator()

        col.menu("CURVES_MT_attribute_context_menu", icon="DOWNARROW_HLT", text="")


class DATA_PT_curves_animation(
    DataButtonsPanel, PropertiesAnimationMixin, PropertyPanel, Panel
):
    COMPAT_ENGINES = {
        "BLENDER_RENDER",
        "BLENDER_EEVEE",
        "BLENDER_WORKBENCH",
    }
    _animated_id_context_property = "curves"


class DATA_PT_custom_props_curves(DataButtonsPanel, PropertyPanel, Panel):
    COMPAT_ENGINES = {
        "BLENDER_RENDER",
        "BLENDER_EEVEE",
        "BLENDER_WORKBENCH",
    }
    _context_path = "object.data"
    _property_type = bpy.types.Curves if hasattr(bpy.types, "Curves") else None


classes = (
    DATA_PT_context_curves,
    DATA_PT_curves_vertex_groups,
    DATA_PT_CURVES_attributes,
    DATA_PT_curves_surface,
    DATA_PT_curves_animation,
    DATA_PT_custom_props_curves,
    CURVES_MT_add_attribute,
    CURVES_MT_attribute_context_menu,
    CURVES_MT_vertex_group_context_menu,
    CURVES_UL_attributes,
    CURVES_UL_vgroups,
)

if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class

    for cls in classes:
        register_class(cls)
