# SPDX-FileCopyrightText: 2009-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy
from bpy.types import Menu, Operator, Panel, UIList
from bpy.app.translations import contexts as i18n_contexts
from rna_prop_ui import PropertyPanel
from bpy_extras.node_utils import find_node_input

from bl_ui.space_properties import PropertiesAnimationMixin


class MATERIAL_MT_context_menu(Menu):
    bl_label = "Material Specials"

    def draw(self, _context):
        layout = self.layout

        layout.operator("material.copy", icon='COPYDOWN')
        layout.operator("object.material_slot_copy")
        layout.operator("material.paste", icon='PASTEDOWN')
        layout.operator("object.material_slot_remove_unused")
        layout.operator("object.material_slot_remove_all")


class MATERIAL_UL_matslots(UIList):

    def draw_item(self, _context, layout, _data, item, icon, _active_data, _active_propname, _index):
        # assert(isinstance(item, bpy.types.MaterialSlot)
        # ob = data
        slot = item
        ma = slot.material

        layout.context_pointer_set("id", ma)
        layout.context_pointer_set("material_slot", slot)

        if ma:
            layout.prop(ma, "name", text="", emboss=False, icon_value=icon)
        else:
            layout.label(text="", icon_value=icon)


class MaterialButtonsPanel:
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "material"
    # COMPAT_ENGINES must be defined in each subclass, external engines can add themselves here

    @classmethod
    def poll(cls, context):
        mat = context.material
        return mat and (context.engine in cls.COMPAT_ENGINES) and not mat.grease_pencil


class MATERIAL_PT_preview(MaterialButtonsPanel, Panel):
    bl_label = "Preview"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_EEVEE'}

    def draw(self, context):
        self.layout.template_preview(context.material)


class MATERIAL_PT_custom_props(MaterialButtonsPanel, PropertyPanel, Panel):
    COMPAT_ENGINES = {
        'BLENDER_RENDER',
        'BLENDER_EEVEE',
        'BLENDER_WORKBENCH',
    }
    _context_path = "material"
    _property_type = bpy.types.Material


class EEVEE_MATERIAL_PT_context_material(MaterialButtonsPanel, Panel):
    bl_label = ""
    bl_context = "material"
    bl_options = {'HIDE_HEADER'}
    COMPAT_ENGINES = {
        'BLENDER_EEVEE',
        'BLENDER_WORKBENCH',
    }

    @classmethod
    def poll(cls, context):
        ob = context.object
        mat = context.material

        if mat and mat.grease_pencil:
            return False
        if ob and ob.type == 'GREASEPENCIL':
            return False

        return (
            (ob or mat) and
            (context.engine in cls.COMPAT_ENGINES)
        )

    def draw(self, context):
        layout = self.layout

        mat = context.material
        ob = context.object
        slot = context.material_slot
        space = context.space_data

        if ob:
            is_sortable = len(ob.material_slots) > 1
            rows = 3
            if is_sortable:
                rows = 5

            row = layout.row()

            row.template_list("MATERIAL_UL_matslots", "", ob, "material_slots", ob, "active_material_index", rows=rows)

            col = row.column(align=True)
            col.operator("object.material_slot_add", icon='ADD', text="")
            col.operator("object.material_slot_remove", icon='REMOVE', text="")

            col.separator()

            col.menu("MATERIAL_MT_context_menu", icon='DOWNARROW_HLT', text="")

            if is_sortable:
                col.separator()

                col.operator("object.material_slot_move", icon='TRIA_UP', text="").direction = 'UP'
                col.operator("object.material_slot_move", icon='TRIA_DOWN', text="").direction = 'DOWN'

        row = layout.row()

        if ob:
            row.template_ID(ob, "active_material", new="material.new")
            row.operator("material.new_layered", text="", icon='ADD')

            if slot:
                row.prop(slot, "link", icon_only=True)

            if ob.mode == 'EDIT':
                row = layout.row(align=True)
                row.operator("object.material_slot_assign", text="Assign")
                if ob.type != 'FONT':
                    row.operator("object.material_slot_select", text="Select")
                    row.operator("object.material_slot_deselect", text="Deselect")

        elif mat:
            row.template_ID(space, "pin_id")


def panel_node_draw(layout, ntree, _output_type, input_name):
    node = ntree.get_output_node('EEVEE')

    if node:
        input = find_node_input(node, input_name)
        if input:
            layout.template_node_view(ntree, node, input)
        else:
            layout.label(text="Incompatible output node")
    else:
        layout.label(text="No output node")


class EEVEE_MATERIAL_PT_surface(MaterialButtonsPanel, Panel):
    bl_label = "Surface"
    bl_context = "material"
    COMPAT_ENGINES = {'BLENDER_EEVEE'}

    def draw(self, context):
        layout = self.layout

        mat = context.material

        layout.use_property_split = True
        panel_node_draw(layout, mat.node_tree, 'OUTPUT_MATERIAL', "Surface")


class EEVEE_MATERIAL_PT_volume(MaterialButtonsPanel, Panel):
    bl_label = "Volume"
    bl_translation_context = i18n_contexts.id_id
    bl_context = "material"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_EEVEE'}

    @classmethod
    def poll(cls, context):
        engine = context.engine
        mat = context.material
        return mat and (engine in cls.COMPAT_ENGINES) and not mat.grease_pencil

    def draw(self, context):
        layout = self.layout

        layout.use_property_split = True

        mat = context.material

        panel_node_draw(layout, mat.node_tree, 'OUTPUT_MATERIAL', "Volume")


class EEVEE_MATERIAL_PT_displacement(MaterialButtonsPanel, Panel):
    bl_label = "Displacement"
    bl_context = "material"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_EEVEE'}

    @classmethod
    def poll(cls, context):
        engine = context.engine
        mat = context.material
        return mat and (engine in cls.COMPAT_ENGINES) and not mat.grease_pencil

    def draw(self, context):
        layout = self.layout

        layout.use_property_split = True

        mat = context.material

        panel_node_draw(layout, mat.node_tree, 'OUTPUT_MATERIAL', "Displacement")


class EEVEE_MATERIAL_PT_thickness(MaterialButtonsPanel, Panel):
    bl_label = "Thickness"
    bl_translation_context = i18n_contexts.id_material
    bl_context = "material"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_EEVEE'}

    @classmethod
    def poll(cls, context):
        engine = context.engine
        mat = context.material
        return mat and (engine in cls.COMPAT_ENGINES) and not mat.grease_pencil

    def draw(self, context):
        layout = self.layout

        layout.use_property_split = True

        mat = context.material

        panel_node_draw(layout, mat.node_tree, 'OUTPUT_MATERIAL', "Thickness")


def draw_material_surface_settings(layout, mat, is_eevee=True):
    col = layout.column(heading="Backface Culling")
    col.prop(mat, "use_backface_culling", text="Camera")
    col.prop(mat, "use_backface_culling_shadow", text="Shadow")
    col.prop(mat, "use_backface_culling_lightprobe_volume", text="Light Probe Volume")

    col = layout.column(align=True)

    if is_eevee:
        col.prop(mat, "displacement_method", text="Displacement")
        col = col.column(align=True)

    col.enabled = mat.displacement_method != 'BUMP'
    # Clarify that this is for displacement if the displacement method setting is not above.
    max_diplacement_text = "Max Distance" if is_eevee else "Max Displacement"
    col.prop(mat, "max_vertex_displacement", text=max_diplacement_text)

    if mat.displacement_method == 'DISPLACEMENT':
        layout.label(text="Unsupported displacement method", icon='ERROR')

    if is_eevee:
        layout.prop(mat, "use_transparent_shadow")

    col = layout.column()
    col.prop(mat, "surface_render_method", text="Render Method")
    if mat.surface_render_method == 'BLENDED':
        col.prop(mat, "use_transparency_overlap", text="Transparency Overlap")
    elif mat.surface_render_method == 'DITHERED':
        col.prop(mat, "use_raytrace_refraction", text="Raytraced Transmission")

    col = layout.column()
    col.prop(mat, "thickness_mode", text="Thickness")
    if mat.surface_render_method == 'DITHERED':
        col.prop(mat, "use_thickness_from_shadow", text="From Shadow")


def draw_material_volume_settings(layout, mat, is_eevee=True):
    layout.prop(mat, "volume_intersection_method", text="Intersection" if is_eevee else "Volume Intersection")


def draw_material_settings(self, context):
    layout = self.layout
    layout.use_property_split = True
    layout.use_property_decorate = False

    mat = context.material

    draw_material_surface_settings(layout, mat, False)
    draw_material_volume_settings(layout, mat, False)


class EEVEE_MATERIAL_PT_viewport_settings(MaterialButtonsPanel, Panel):
    bl_label = "Settings"
    bl_context = "material"
    bl_parent_id = "MATERIAL_PT_viewport"
    COMPAT_ENGINES = {'BLENDER_RENDER'}

    def draw(self, context):
        draw_material_settings(self, context)


class EEVEE_MATERIAL_PT_settings(MaterialButtonsPanel, Panel):
    bl_label = "Settings"
    bl_context = "material"
    COMPAT_ENGINES = {'BLENDER_EEVEE'}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        mat = context.material

        layout.prop(mat, "pass_index")


class EEVEE_MATERIAL_PT_settings_surface(MaterialButtonsPanel, Panel):
    bl_label = "Surface"
    bl_context = "material"
    bl_parent_id = "EEVEE_MATERIAL_PT_settings"
    COMPAT_ENGINES = {'BLENDER_EEVEE'}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        mat = context.material

        draw_material_surface_settings(layout, mat)


class EEVEE_MATERIAL_PT_settings_volume(MaterialButtonsPanel, Panel):
    bl_label = "Volume"
    bl_context = "material"
    bl_parent_id = "EEVEE_MATERIAL_PT_settings"
    COMPAT_ENGINES = {'BLENDER_EEVEE'}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        mat = context.material

        draw_material_volume_settings(layout, mat)


class MATERIAL_PT_viewport(MaterialButtonsPanel, Panel):
    bl_label = "Viewport Display"
    bl_context = "material"
    bl_options = {'DEFAULT_CLOSED'}
    bl_order = 10

    @classmethod
    def poll(cls, context):
        mat = context.material
        return mat and not mat.grease_pencil

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        mat = context.material

        col = layout.column()
        col.prop(mat, "diffuse_color", text="Color")
        col.prop(mat, "metallic")
        col.prop(mat, "roughness")


class MATERIAL_PT_lineart(MaterialButtonsPanel, Panel):
    bl_label = "Line Art"
    bl_options = {'DEFAULT_CLOSED'}
    bl_order = 10

    @classmethod
    def poll(cls, context):
        mat = context.material
        return mat and not mat.grease_pencil

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        mat = context.material
        lineart = mat.lineart

        layout.prop(lineart, "use_material_mask", text="Material Mask")

        col = layout.column(align=True)
        col.active = lineart.use_material_mask
        row = col.row(align=True, heading="Masks")
        for i in range(8):
            row.prop(lineart, "use_material_mask_bits", text=" ", index=i, toggle=True)
            if i == 3:
                row = col.row(align=True)

        row = layout.row(align=True, heading="Custom Occlusion")
        row.prop(lineart, "mat_occlusion", text="Levels")

        row = layout.row(heading="Intersection Priority")
        row.prop(lineart, "use_intersection_priority_override", text="")
        subrow = row.row()
        subrow.active = lineart.use_intersection_priority_override
        subrow.prop(lineart, "intersection_priority", text="")


class MATERIAL_PT_animation(MaterialButtonsPanel, Panel, PropertiesAnimationMixin):
    COMPAT_ENGINES = {
        'BLENDER_RENDER',
        'BLENDER_EEVEE',
        'BLENDER_WORKBENCH',
    }

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        # MaterialButtonsPanel.poll ensures this is not None.
        material = context.material

        col = layout.column(align=True)
        col.label(text="Material")
        self.draw_action_and_slot_selector(context, col, material)

        if node_tree := material.node_tree:
            col = layout.column(align=True)
            col.label(text="Shader Node Tree")
            self.draw_action_and_slot_selector(context, col, node_tree)


class BrushMaterialButtonsPanel:
    """Base for the Brush Material tab, which edits the active PBR Paint brush's source material.

    The material is reached through the brush rather than through an object material slot, so the
    context path is Brush -> Material and there is no object, no slot and no pinning here (the
    Properties editor's pinned root is deliberately ignored for this tab).

    The panels below deliberately do not subclass the equivalent "material" tab panels: those are
    registered classes, and registering a subclass of one breaks the original's callbacks. Shared
    drawing goes through the module level helpers instead, the same way the rest of this file
    shares code between panels.
    """
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "brush_material"

    @classmethod
    def poll(cls, context):
        # Deliberately no COMPAT_ENGINES test, unlike MaterialButtonsPanel: the source material is
        # baked through the EEVEE-based node preview path whatever the scene's render engine is,
        # so filtering these panels by engine would hide settings that still take effect.
        mat = context.material
        return mat is not None and not mat.grease_pencil


class BRUSH_MATERIAL_PT_context_material(BrushMaterialButtonsPanel, Panel):
    bl_idname = "BRUSH_MATERIAL_PT_context_material"
    bl_label = ""
    bl_options = {'HIDE_HEADER'}

    def draw(self, context):
        layout = self.layout
        # Both the tab's context path and context.brush resolve through the active paint, so a
        # brush with material_paint is expected here. Guarded anyway: a UI script raising is far
        # worse than a degraded row, and the two lookups are not literally the same code path.
        brush = getattr(context, "brush", None)

        row = layout.row()
        if brush is not None and brush.material_paint is not None:
            row.template_ID(brush.material_paint, "source_material")
        else:
            row.label(text=context.material.name, icon='MATERIAL')


class BRUSH_MATERIAL_PT_surface(BrushMaterialButtonsPanel, Panel):
    bl_idname = "BRUSH_MATERIAL_PT_surface"
    bl_label = "Surface"

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        panel_node_draw(layout, context.material.node_tree, 'OUTPUT_MATERIAL', "Surface")


class BRUSH_MATERIAL_PT_settings(BrushMaterialButtonsPanel, Panel):
    bl_idname = "BRUSH_MATERIAL_PT_settings"
    bl_label = "Settings"

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        layout.prop(context.material, "pass_index")


class BRUSH_MATERIAL_PT_settings_surface(BrushMaterialButtonsPanel, Panel):
    bl_idname = "BRUSH_MATERIAL_PT_settings_surface"
    bl_label = "Surface"
    bl_parent_id = "BRUSH_MATERIAL_PT_settings"

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        draw_material_surface_settings(layout, context.material)


class BRUSH_MATERIAL_PT_viewport(BrushMaterialButtonsPanel, Panel):
    bl_idname = "BRUSH_MATERIAL_PT_viewport"
    bl_label = "Viewport Display"
    bl_options = {'DEFAULT_CLOSED'}
    bl_order = 10

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        mat = context.material

        col = layout.column()
        col.prop(mat, "diffuse_color", text="Color")
        col.prop(mat, "metallic")
        col.prop(mat, "roughness")


class BRUSH_MATERIAL_PT_custom_props(BrushMaterialButtonsPanel, PropertyPanel, Panel):
    bl_idname = "BRUSH_MATERIAL_PT_custom_props"
    _context_path = "material"
    _property_type = bpy.types.Material


class LayerMaterialButtonsPanel:
    """Base for the Layer Material tab, which edits the material the active Material paint layer
    was baked from.

    The material is reached through the scene's paint channel bindings, so there is no slot to pick
    from and no pinning. Editing it re-bakes the layer's maps on its own.
    Like #BrushMaterialButtonsPanel, these panels share drawing through module level helpers rather
    than by subclassing registered panels.
    """
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "layer_material"

    @staticmethod
    def _owner_material(context):
        """The material that owns the stack, or None.

        In this tab `context.material` can be the active Material layer's *source* material, not the
        owner: `buttons_context_path_layer_material` points the context at `layer.material` for a
        Material row. Panels that need the stack or its active row must go through the object's
        active material slot instead.
        """
        ob = context.object
        mat = ob.active_material if ob is not None else None
        return mat if (mat is not None and mat.is_layered) else None

    @staticmethod
    def _active_layer(context):
        """The active row of the owning material, or `(None, None)` when there is none."""
        owner = LayerMaterialButtonsPanel._owner_material(context)
        if owner is None:
            return None, None
        return owner, owner.paint_layers.active

    @classmethod
    def poll(cls, context):
        # No COMPAT_ENGINES test, for the same reason as the Brush Material tab: the bake goes
        # through EEVEE whatever the scene's render engine is.
        mat = context.material
        return mat is not None and not mat.grease_pencil


class LAYER_MATERIAL_PT_layers(LayerMaterialButtonsPanel, Panel):
    """The Layer Material tab for a *layered* material: the active layer read from the description,
    its channels and values, and the generated tree's state. The binding-driven panel below is for
    the old graph path and hides itself for a layered material."""

    bl_idname = "LAYER_MATERIAL_PT_layers"
    bl_label = "Paint Layers"

    @classmethod
    def poll(cls, context):
        return cls._owner_material(context) is not None

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        owner = self._owner_material(context)
        if owner is None:
            return

        col = layout.column(align=True)
        col.prop(owner, "paint_layers_locked", text="Locked")
        row = col.row()
        row.enabled = owner.paint_layers_tree_is_stale
        row.operator("material.paint_layers_regenerate", text="Regenerate", icon='FILE_REFRESH')
        if owner.paint_layers_tree_is_stale:
            layout.label(text="Tree out of step with the layers", icon='ERROR')

        row = layout.row(align=True)
        row.operator_menu_enum("material.paint_layer_add", "source", text="Add", icon='ADD')
        row.operator("material.paint_layer_remove", text="", icon='REMOVE')
        row.operator("material.paint_layer_duplicate", text="", icon='DUPLICATE')

        row = layout.row()
        row.operator_menu_enum("material.paint_layer_add_material", "source",
                               text="New Material Layer", icon='MATERIAL')
        row.operator_menu_enum("material.paint_layer_use_row_result", "source",
                               text="Use Row Result", icon='RENDER_STILL')
        layout.operator("material.paint_layer_add_custom", text="New Custom Layer",
                        icon='NODETREE')

        _, layer = self._active_layer(context)
        if layer is None:
            layout.separator()
            layout.label(text="No active layer", icon='INFO')
            return

        layout.separator()
        layout.prop(layer, "name", text="Layer")
        layout.prop(layer, "blend_type", text="Blend")
        layout.prop(layer, "opacity", text="Opacity")
        if layer.source == 'CONSTANT':
            layout.prop(layer, "fill_color")

        box = layout.box()
        box.label(text="Mask", icon='MOD_MASK')
        for item in layer.mask_stack:
            row = box.row(align=True)
            row.operator(
                "material.paint_layer_mask_toggle",
                text="",
                icon='CHECKBOX_HLT' if item.enabled else 'CHECKBOX_DEHLT',
            ).item_marker = item.marker
            row.label(text=item.name if item.name else "Mask")
            row.operator("material.paint_layer_mask_remove", text="", icon='X').item_marker = \
                item.marker
        box.operator("material.paint_layer_mask_add", text="Add Mask", icon='ADD')

        box = layout.box()
        box.label(text="Channels", icon='IMAGE_RGB')
        for ch in layer.channels:
            row = box.row(align=True)
            row.label(text=ch.channel)
            if ch.image is not None:
                row.label(text=ch.image.name, icon='IMAGE_DATA')
            else:
                row.label(text="No map", icon='INFO')
            op = row.operator("material.paint_layer_channel_remove", text="", icon='X')
            op.channel = ch.channel
        row = box.row(align=True)
        row.operator_menu_enum("material.paint_layer_channel_add", "channel",
                               text="Add Channel", icon='ADD')
        row.operator_menu_enum("material.paint_layer_correction_add", "role",
                               text="Add Correction", icon='ADD')
        if layer.source == 'NODE_GROUP':
            layout.operator_menu_enum("material.paint_layer_custom_channel_add", "channel",
                                      text="Add Custom Channel", icon='ADD')

        if len(layer.issues):
            box = layout.box()
            box.label(text="Issues", icon='ERROR')
            for issue in layer.issues:
                box.label(text=issue.text)


class LAYER_MATERIAL_PT_source_material(LayerMaterialButtonsPanel, Panel):
    """The source material of the active Material layer: the material it bakes its channels from.

    ``layer.material`` is a plain RNA pointer to a Material, not a material slot, so the picker
    changes what the layer bakes without touching the object's slots.
    """

    bl_idname = "LAYER_MATERIAL_PT_source_material"
    bl_label = "Source Material"

    @classmethod
    def poll(cls, context):
        _, layer = cls._active_layer(context)
        return layer is not None and layer.source == 'MATERIAL'

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        _, layer = self._active_layer(context)
        if layer is None:
            return

        row = layout.row()
        row.prop(layer, "material", text="")
        if layer.material is not None and layer.material.library is not None:
            layout.label(text="Linked, not editable", icon='LIBRARY_DATA_DIRECT')

        col = layout.column(align=True)
        col.prop(layer, "bake_mode", text="Bake")
        col.prop(layer, "bake_size", text="Resolution")
        row = col.row()
        row.enabled = layer.material is not None
        row.operator("material.paint_layer_rebake", text="Rebake", icon='FILE_REFRESH')
        # One RNA answer for the row's state; the Outliner stack shows the same property.
        status = layer.live_status
        if status == 'REFUSED':
            reason = layer.live_status_refusal_reason
            layout.label(text="Refused: " + (reason if reason else "unknown"), icon='ERROR')
        elif status == 'BAKING':
            layout.label(text="Baking...", icon='FILE_REFRESH')
        elif status == 'BAKED':
            layout.label(text="Baked", icon='IMAGE_DATA')
        else:
            layout.label(text="Live", icon='HIDE_OFF')


class LAYER_MATERIAL_PT_source_surface(LayerMaterialButtonsPanel, Panel):
    """The source material's Surface inputs, edited in place.

    Drawing the material's own node tree here is what makes the source editable without leaving the
    tab; the same ``OUTPUT_MATERIAL`` node the shader editor shows is drawn as a property panel.
    """

    bl_idname = "LAYER_MATERIAL_PT_source_surface"
    bl_parent_id = "LAYER_MATERIAL_PT_source_material"
    bl_label = "Surface"

    @classmethod
    def poll(cls, context):
        _, layer = cls._active_layer(context)
        return (
            layer is not None and
            layer.source == 'MATERIAL' and
            layer.material is not None and
            layer.material.node_tree is not None
        )

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        _, layer = self._active_layer(context)
        if layer is None or layer.material is None:
            return
        if layer.material.library is not None:
            layout.label(text="Linked, not editable", icon='LIBRARY_DATA_DIRECT')
            return
        panel_node_draw(layout, layer.material.node_tree, 'OUTPUT_MATERIAL', "Surface")


class LAYER_MATERIAL_PT_custom_layer(LayerMaterialButtonsPanel, Panel):
    """The node group of the active Custom layer and its stored input values."""

    bl_idname = "LAYER_MATERIAL_PT_custom_layer"
    bl_label = "Custom Group"

    @classmethod
    def poll(cls, context):
        _, layer = cls._active_layer(context)
        return layer is not None and layer.source == 'NODE_GROUP'

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        _, layer = self._active_layer(context)
        if layer is None:
            return

        layout.prop(layer, "custom_group", text="Group")
        props = layer.properties
        if props is None:
            return
        # The group's role-less inputs are stored as IDProperty members; draw each as its own row.
        keys = getattr(props, "keys", None)
        if keys is None:
            return
        col = layout.column(align=True)
        for key in keys():
            if key == "rna_type":
                continue
            col.prop(props, '["%s"]' % key, text=key)


class LAYER_MATERIAL_PT_custom_props(LayerMaterialButtonsPanel, PropertyPanel, Panel):
    bl_idname = "LAYER_MATERIAL_PT_custom_props"
    _context_path = "material"
    _property_type = bpy.types.Material


class MATERIAL_PT_paint_layers(MaterialButtonsPanel, Panel):
    """Layer-stack controls of a layered material: who owns the generated tree, and whether it is
    in step with the description."""

    bl_label = "Layered Material"
    bl_context = "material"
    COMPAT_ENGINES = {'BLENDER_EEVEE', 'CYCLES'}

    @classmethod
    def poll(cls, context):
        mat = context.material
        return mat is not None and mat.is_layered and not mat.grease_pencil

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        mat = context.material

        col = layout.column(heading="Tree")
        col.prop(mat, "paint_layers_locked", text="Locked")
        row = layout.row()
        row.enabled = mat.paint_layers_tree_is_stale
        row.operator("material.paint_layers_regenerate", text="Regenerate", icon='FILE_REFRESH')
        if mat.paint_layers_tree_is_stale:
            layout.label(
                text="The node tree is out of step with the layers; Regenerate to rebuild it",
                icon='ERROR',
            )


classes = (
    MATERIAL_MT_context_menu,
    MATERIAL_UL_matslots,
    MATERIAL_PT_preview,
    EEVEE_MATERIAL_PT_context_material,
    EEVEE_MATERIAL_PT_surface,
    EEVEE_MATERIAL_PT_volume,
    EEVEE_MATERIAL_PT_displacement,
    EEVEE_MATERIAL_PT_thickness,
    EEVEE_MATERIAL_PT_settings,
    EEVEE_MATERIAL_PT_settings_surface,
    EEVEE_MATERIAL_PT_settings_volume,
    MATERIAL_PT_lineart,
    MATERIAL_PT_viewport,
    EEVEE_MATERIAL_PT_viewport_settings,
    MATERIAL_PT_animation,
    MATERIAL_PT_custom_props,
    MATERIAL_PT_paint_layers,
    BRUSH_MATERIAL_PT_context_material,
    BRUSH_MATERIAL_PT_surface,
    BRUSH_MATERIAL_PT_settings,
    BRUSH_MATERIAL_PT_settings_surface,
    BRUSH_MATERIAL_PT_viewport,
    BRUSH_MATERIAL_PT_custom_props,
    LAYER_MATERIAL_PT_layers,
    LAYER_MATERIAL_PT_source_material,
    LAYER_MATERIAL_PT_source_surface,
    LAYER_MATERIAL_PT_custom_layer,
    LAYER_MATERIAL_PT_custom_props,
)


if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
