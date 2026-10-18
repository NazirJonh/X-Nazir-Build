# SPDX-FileCopyrightText: 2009-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy
from bpy.types import Menu, Panel, UIList
from bpy.app.translations import contexts as i18n_contexts
from rna_prop_ui import PropertyPanel
from bpy_extras.node_utils import find_node_input

from bl_ui.space_properties import PropertiesAnimationMixin

from bl_ui.properties_material_paint_layers import (
    draw_material_channel_set,
    draw_material_layer_channels,
    draw_material_mask_coverage_item,
    draw_material_mask_item,
)


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
    """Base for the Layer Material tab, which edits the material of the active paint layer.

    The owner material is reached through the active object's active material slot, so the tab
    follows whichever stack the object shows; `context.material` in this tab is that same owner
    (the context path holds exactly one Material). Like #BrushMaterialButtonsPanel, these panels
    share drawing through module level helpers rather than by subclassing registered panels.
    """
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "layer_material"

    @staticmethod
    def _owner_material(context):
        """The layered material that owns the stack, or None."""
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

    @staticmethod
    def _shown_mask_item(context, owner, layer):
        """The mask item the tab edits instead of the layer, or None.

        `paint_layers.active` answers the row a mask correction hangs on, so a correction is asked
        for itself. The base mask is no row of the stack: it is the layer's own mask slot, and is
        what the tab shows while the brush works in Mask mode.
        """
        item = owner.paint_layers.active_mask_item
        if item is not None:
            return item
        if (layer is not None and len(layer.mask_items) and
                context.tool_settings.paint_mode.layer_target_mode == 'MASK'):
            return layer.mask_items[0]
        return None

    @staticmethod
    def _object_mask_editing(context):
        """While the active row's object mask is edited, the tab shows only the Mask panel."""
        return context.scene.tool_settings.paint_mode.layer_object_mask_edit

    @classmethod
    def poll(cls, context):
        # No COMPAT_ENGINES test, for the same reason as the Brush Material tab: the generated tree
        # feeds the viewport whatever the scene's render engine is.
        if cls._object_mask_editing(context):
            return False
        mat = context.material
        return mat is not None and not mat.grease_pencil


class LAYER_MATERIAL_PT_layers(LayerMaterialButtonsPanel, Panel):
    """The Layer Material tab for a layered material: the active layer read from the description,
    its opacity and its channels.

    The layer add/remove/duplicate operators and the row list live in the Outliner stack; only the
    active row is edited here.
    """

    bl_idname = "LAYER_MATERIAL_PT_layers"
    bl_label = "Layer"

    @classmethod
    def poll(cls, context):
        if cls._object_mask_editing(context):
            return False
        return cls._owner_material(context) is not None

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        owner, layer = self._active_layer(context)
        if owner is None:
            return
        if layer is None:
            layout.label(text="No active layer", icon='INFO')
            return

        # A mask item has one map and no colour channels, which the row's channel widget would show
        # as filled; only its own settings are drawn.
        item = self._shown_mask_item(context, owner, layer)
        if item is not None:
            layout.label(text=item.name, icon='MOD_MASK')
            if item.source == 'FILTER':
                # A filter in the mask changes its one coverage: opacity and the kind's parameters,
                # and no channels to choose. A map-coverage operator draws its own mode, tolerance
                # and colours in #draw_material_mask_coverage_item.
                layout.prop(item, "opacity", text="Opacity")
                layout.separator()
                if item.is_map_coverage:
                    draw_material_mask_coverage_item(layout, context, item)
                else:
                    for param in item.kind_params:
                        layout.prop(param, "value", text=param.label, slider=True)
                return
            draw_material_mask_item(layout, context, item)
            return

        layout.prop(layer, "opacity", text="Opacity")
        # A Material row owns no channel records: it lays what its source material shows.
        if layer.source == 'MATERIAL':
            layout.separator()
            # The ID Browser popover: a grid with the Material asset libraries, instead of a plain
            # search list. A Material row always reads a material, so the built-in unlink and the
            # user count are left out; the pointer's own poll and setter refuse a cycle.
            layout.template_ID_browser(
                layer, "material",
                text="Source",
                use_rename=False,
                use_unlink=False,
                use_users=False,
            )
            if layer.material is None:
                layout.label(text="No source material", icon='ERROR')
            return
        # A filter has no map or channel records: what it is told is the parameters of its kind.
        if layer.source == 'FILTER':
            layout.separator()
            for param in layer.kind_params:
                layout.prop(param, "value", text=param.label, slider=True)
            # The channels it works in, switched like a layer's; Base Color is on to start with.
            layout.separator()
            draw_material_layer_channels(layout, context, owner, layer)
            return
        # A folder holds what its rows hold, so it has no channel records to add: its per-channel
        # opacity is edited in the Outliner stack, and this tab keeps the overall one.
        if layer.source != 'STACK':
            layout.separator()
            draw_material_layer_channels(layout, context, owner, layer)


class LAYER_MATERIAL_PT_mapping(LayerMaterialButtonsPanel, Panel):
    """UV mapping of the maps the shown row reads: a layer, a correction or a mask item.

    Shown only while the row reads a repeatable map (RNA ``mapping_supported``, the one BKE
    predicate the graph and the CPU share): a Fill with a map. The rotation turns the map lookup
    around Z; the tangent-space vectors of a Normal map are rotated (and flipped by the scale sign)
    to follow it.
    """

    bl_idname = "LAYER_MATERIAL_PT_mapping"
    bl_label = "Mapping"
    bl_parent_id = "LAYER_MATERIAL_PT_layers"

    @classmethod
    def _shown_row(cls, context):
        """The row the Layer tab edits: the shown mask item, or else the active row."""
        owner, layer = cls._active_layer(context)
        if owner is None or layer is None:
            return None
        item = cls._shown_mask_item(context, owner, layer)
        return item if item is not None else layer

    @classmethod
    def poll(cls, context):
        row = cls._shown_row(context)
        return row is not None and row.mapping_supported

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        row = self._shown_row(context)
        if row is None:
            return

        layout.prop(row, "mapping_enabled", text="Enabled")
        col = layout.column()
        col.enabled = row.mapping_enabled
        col.prop(row, "mapping_offset", text="Offset")
        sub = col.row(align=True)
        sub.prop(row, "mapping_scale", text="Scale")
        sub.prop(row, "mapping_scale_lock", text="", icon_only=True)
        col.prop(row, "mapping_rotation", text="Rotation")
        col.prop(row, "mapping_wrap", text="UV Wrap")


class LAYER_MATERIAL_PT_source_surface(LayerMaterialButtonsPanel, Panel):
    """The source material's Surface inputs of the active Material layer, edited in place.

    Drawing the material's own node tree here is what makes the source editable without leaving the
    tab; the same ``OUTPUT_MATERIAL`` node the shader editor shows is drawn as a property panel.
    """

    bl_idname = "LAYER_MATERIAL_PT_source_surface"
    bl_label = "Source Material"
    bl_parent_id = "LAYER_MATERIAL_PT_layers"

    @classmethod
    def poll(cls, context):
        if cls._object_mask_editing(context):
            return False
        owner, layer = cls._active_layer(context)
        if owner is None or layer is None or layer.source != 'MATERIAL':
            return False
        return cls._shown_mask_item(context, owner, layer) is None and layer.material is not None

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        _, layer = self._active_layer(context)
        if layer is None or layer.material is None:
            return
        source = layer.material
        if source.library is not None:
            layout.label(text="Linked, not editable", icon='LIBRARY_DATA_DIRECT')
            return
        if source.node_tree is None:
            layout.label(text="The source has no nodes", icon='INFO')
            return
        panel_node_draw(layout, source.node_tree, 'OUTPUT_MATERIAL', "Surface")


class LAYER_MATERIAL_PT_bake(LayerMaterialButtonsPanel, Panel):
    """The bake cache of the active row: when its subtree is rendered into maps the stack reads.

    A baked row is a cache of the live row, not a replacement for it: it is read only while it
    matches the row's description, and the first edit to the row or to a map it reads brings the
    live row back until the cache is rendered again. Auto bakes the heavy folders that are nested in
    another folder and are not being edited; Always bakes any row once it is not being edited.
    """

    bl_idname = "LAYER_MATERIAL_PT_bake"
    bl_label = "Bake"
    bl_parent_id = "LAYER_MATERIAL_PT_layers"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        if not super().poll(context):
            return False
        owner, layer = cls._active_layer(context)
        if owner is None or layer is None:
            return False
        return cls._shown_mask_item(context, owner, layer) is None

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        owner, layer = self._active_layer(context)
        if layer is None:
            return

        layout.prop(layer, "bake_mode", text="Mode")
        layout.prop(layer, "bake_size_preset", text="Size")
        if layer.source == 'MATERIAL':
            # The maps of a Material row are rendered from its source: they are what the row shows.
            if layer.bake_mode == 'NEVER':
                layout.label(text="Not shown: the row has no live form yet", icon='INFO')
            else:
                layout.label(
                    text="Rendered" if layer.bake_is_valid else "Rendering or out of date",
                    icon='CHECKMARK' if layer.bake_is_valid else 'TIME',
                )
            return
        if layer.bake_mode != 'NEVER':
            layout.label(
                text="Baked" if layer.bake_is_valid else "Not baked yet",
                icon='CHECKMARK' if layer.bake_is_valid else 'TIME',
            )


class LAYER_MATERIAL_PT_channels(LayerMaterialButtonsPanel, Panel):
    """The settings of the layered material as a whole; its sections are sub-panels."""

    bl_idname = "LAYER_MATERIAL_PT_channels"
    bl_label = "Layer Material Settings"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        if cls._object_mask_editing(context):
            return False
        return cls._owner_material(context) is not None

    def draw(self, context):
        owner = self._owner_material(context)
        if owner is None:
            return
        layout = self.layout
        layout.use_property_split = True
        layout.prop(owner, "paint_layers_image_size")


class LAYER_MATERIAL_PT_advanced(LayerMaterialButtonsPanel, Panel):
    """Settings that change how the stack is computed rather than what it contains."""

    bl_idname = "LAYER_MATERIAL_PT_advanced"
    bl_label = "Advanced"
    bl_parent_id = "LAYER_MATERIAL_PT_channels"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        return cls._owner_material(context) is not None

    def draw(self, context):
        owner = self._owner_material(context)
        if owner is None:
            return
        layout = self.layout
        layout.use_property_split = True
        layout.prop(owner, "paint_layers_composite_device")


class LAYER_MATERIAL_PT_active_channel(LayerMaterialButtonsPanel, Panel):
    """The owner material's global paint channel set: which channels the stack works with.

    A channel removed here leaves each row's record and map untouched; the rows simply stop showing
    it. Base Color is the material's constant and is always in the set. The stack starts with the
    set chosen in the New Layered Material dialog.
    """

    bl_idname = "LAYER_MATERIAL_PT_active_channel"
    bl_label = "Active Channel"
    bl_parent_id = "LAYER_MATERIAL_PT_channels"

    @classmethod
    def poll(cls, context):
        return cls._owner_material(context) is not None

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = False
        draw_material_channel_set(layout, self._owner_material(context))


# Last maskable UDIM tile, read from the RNA range of the setter so it follows
# `PAINT_LAYER_OBJECT_MASK_TILE_LAST` in `BKE_paint_layers_object_mask.hh`.
_OBJECT_MASK_TILE_LAST = (
    bpy.types.MaterialPaintLayer.bl_rna.functions["object_mask_tile_set"].parameters["tile"].hard_max
)


def paint_layer_mask_objects(context, mat):
    """Objects of the active view layer that use `mat` in any slot, each once, sorted by name.

    Only the view layer is listed: objects in other scenes using the material are not shown.
    """
    return sorted(
        (ob for ob in context.view_layer.objects
         if any(slot.material == mat for slot in ob.material_slots)),
        key=lambda ob: ob.name,
    )


class LAYER_MATERIAL_PT_object_mask(LayerMaterialButtonsPanel, Panel):
    """Which objects, or which UDIM tiles of each object, the active layer shows on. While it is
    edited it is the only panel of the Layered Material tab."""

    bl_idname = "LAYER_MATERIAL_PT_object_mask"
    bl_label = "Mesh Mask"

    @classmethod
    def poll(cls, context):
        if not cls._object_mask_editing(context):
            return False
        _owner, layer = cls._active_layer(context)
        return layer is not None

    def draw(self, context):
        layout = self.layout
        mat, layer = self._active_layer(context)
        # The way out of the mask-only tab: clears `layer_object_mask_edit`.
        header = layout.row()
        header.prop(context.scene.tool_settings.paint_mode, "layer_object_mask_edit",
                    text="Edit Mesh Mask", toggle=True)
        # Names the row being configured; same kind icons as the Outliner (folder / fill / layer).
        if layer.source == 'STACK':
            kind_icon = 'FILE_FOLDER'
        elif layer.source == 'CONSTANT':
            kind_icon = 'GP_DRAW_FILL'
        else:
            kind_icon = 'IMAGE_DATA'
        layout.label(text=layer.name, icon=kind_icon)
        layout.use_property_split = True
        layout.prop(layer, "object_mask_type", text="Mask Type")
        layout.use_property_split = False
        objects = paint_layer_mask_objects(context, mat)
        if layer.object_mask_type == 'MESH':
            col = layout.column(align=True)
            for ob in objects:
                visible = layer.object_mask_visible_get(ob)
                row = col.row(align=True)
                row.enabled = ob.library is None and ob.override_library is None
                props = row.operator(
                    "material.paint_layer_object_mask_toggle",
                    text=ob.name,
                    icon='CHECKBOX_HLT' if visible else 'CHECKBOX_DEHLT',
                    emboss=False,
                )
                props.object_name = ob.name
                props.object_session_uid = ob.session_uid
                props.marker = layer.marker
        elif layer.object_mask_type == 'UV_TILES':
            # Tile lists are cached; UV edits need an explicit rescan.
            layout.operator("material.paint_layer_object_mask_refresh_tiles", icon='FILE_REFRESH')
            for ob in objects:
                box = layout.box()
                box.label(text=ob.name, icon='OBJECT_DATA')
                tiles = layer.object_mask_tiles(ob)
                if not tiles:
                    box.label(text="No UDIM tiles", icon='INFO')
                    continue
                grid = box.grid_flow(columns=5, even_columns=True, align=True)
                for tile in tiles:
                    sub = grid.row(align=True)
                    if tile > _OBJECT_MASK_TILE_LAST:
                        sub.enabled = False
                    else:
                        sub.enabled = ob.library is None and ob.override_library is None
                    visible = tile > _OBJECT_MASK_TILE_LAST or layer.object_mask_tile_get(ob, tile)
                    props = sub.operator(
                        "material.paint_layer_object_mask_toggle",
                        text=str(tile),
                        icon='CHECKBOX_HLT' if visible else 'CHECKBOX_DEHLT',
                        emboss=False,
                    )
                    props.object_name = ob.name
                    props.object_session_uid = ob.session_uid
                    props.marker = layer.marker
                    props.tile = tile
                if any(tile > _OBJECT_MASK_TILE_LAST for tile in tiles):
                    box.label(text="Tiles above {:d} cannot be masked".format(_OBJECT_MASK_TILE_LAST), icon='INFO')


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
    BRUSH_MATERIAL_PT_context_material,
    BRUSH_MATERIAL_PT_surface,
    BRUSH_MATERIAL_PT_settings,
    BRUSH_MATERIAL_PT_settings_surface,
    BRUSH_MATERIAL_PT_viewport,
    BRUSH_MATERIAL_PT_custom_props,
    LAYER_MATERIAL_PT_layers,
    LAYER_MATERIAL_PT_mapping,
    LAYER_MATERIAL_PT_source_surface,
    LAYER_MATERIAL_PT_bake,
    LAYER_MATERIAL_PT_channels,
    LAYER_MATERIAL_PT_advanced,
    LAYER_MATERIAL_PT_active_channel,
    LAYER_MATERIAL_PT_object_mask,
)


if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
