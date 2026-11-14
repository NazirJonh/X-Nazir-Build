# SPDX-FileCopyrightText: 2009-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy

from bpy.types import Menu, Panel, UIList
from bpy.app.translations import contexts as i18n_contexts
from rna_prop_ui import PropertyPanel
from bpy_extras.node_utils import find_node_input

from bl_ui.space_properties import PropertiesAnimationMixin

from bl_ui.properties_paint_common import (
    paint_shape_tool_active,
)

from bl_ui.properties_material_paint_layers import (
    draw_material_channel_mixer,
    draw_material_channel_set,
    draw_material_color_balance,
    draw_material_color_match,
    draw_material_generator_pattern_params,
    draw_material_generator_placement,
    draw_material_kind_params,
    draw_material_layer_channels,
    draw_material_layer_height,
    draw_material_mask_coverage_item,
    draw_material_mask_item,
    draw_material_smart_asset_expose,
    draw_material_smart_asset_rows,
    draw_material_update_now,
)
from bl_operators.material_paint_layers import (
    mesh_map_objects_for_material,
    mesh_map_source_member_count,
    mesh_map_summary_status,
)
# The brush glyph of the New Layered Material button: the same Material Symbols codepoint the Stack
# Layers rows and Add menu use (`GLYPH_BRUSH` there, pinned by
# `glyph_constants_stay_in_step_with_space_outliner`).
from bl_ui.space_outliner import GLYPH_BRUSH


class MATERIAL_MT_context_menu(Menu):
    bl_label = "Material Specials"

    def draw(self, _context):
        layout = self.layout

        layout.operator("material.copy", icon='COPYDOWN')
        layout.operator("object.material_slot_copy")
        layout.operator("material.paste", icon='PASTEDOWN')
        layout.operator("object.material_slot_remove_unused")
        layout.operator("object.material_slot_remove_all")


class MATERIAL_MT_layered(Menu):
    bl_label = "Layered Material"

    def draw(self, _context):
        layout = self.layout

        # The operator's poll owns the eligibility rule, so the menu cannot drift from it.
        if bpy.ops.material.layered_convert.poll():
            props = layout.operator("material.layered_convert",
                                    text="Convert, Keep Node Graph")
            props.mode = 'GRAPH'
            props = layout.operator("material.layered_convert",
                                    text="Convert, Bake to Fill Layer")
            props.mode = 'BAKE'
            layout.separator()

        if bpy.ops.material.source_cache_update.poll():
            layout.operator("material.source_cache_update",
                            text="Update Source-Graph Cache")
            layout.separator()

        if bpy.ops.material.layered_revert.poll():
            layout.operator("material.layered_revert",
                            text="Revert to Plain Material")
            layout.separator()

        layout.operator("material.new_layered", text="New Layered Material")


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

            # The Layered Material button opens a menu: the active material can become the base of
            # a stack (keeping its node graph, or baked down into a Fill layer), or a stack is
            # created from scratch. A glyph icon where the font provides one, the built-in icon
            # otherwise (background mode), like the Stack Layers Add buttons in the Outliner.
            if glyph := bpy.app.icons.glyph(GLYPH_BRUSH):
                col.menu("MATERIAL_MT_layered", text="", icon_value=glyph)
            else:
                col.menu("MATERIAL_MT_layered", text="", icon='ADD')

            col.separator()

            col.menu("MATERIAL_MT_context_menu", icon='DOWNARROW_HLT', text="")

            if is_sortable:
                col.separator()

                col.operator("object.material_slot_move", icon='TRIA_UP', text="").direction = 'UP'
                col.operator("object.material_slot_move", icon='TRIA_DOWN', text="").direction = 'DOWN'

        row = layout.row()

        if ob:
            row.template_ID(ob, "active_material", new="material.new")

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

    @classmethod
    def poll(cls, context):
        # The tab follows the Shape tool even before the selected part has a material: this panel
        # creates it. The other panels of the tab stay hidden while `context.material` is None
        # because they keep the base poll.
        if paint_shape_tool_active(context):
            return True
        return super().poll(context)

    def draw(self, context):
        layout = self.layout
        # A Shape tool does not use the active brush's material: its Default material comes from
        # the Fill or Stroke part of the shared Shape settings. The Layer source is deliberately
        # not offered here; the Shape panel's source row edits that one.
        if paint_shape_tool_active(context):
            # Always the shared tool-settings block, matching the C++ context path
            # (`buttons_context_path_brush_material`). A live session may expose a private copy
            # through `paint_shape_settings`, but this tab edits the shared settings only.
            shape = context.tool_settings.image_paint.shape
            is_stroke = shape.material_tab_part == 'STROKE'
            layout.row().prop(shape, "material_tab_part", expand=True)
            # A disabled part must not hide the whole tab. Offer the switch that enables it, and
            # keep the material slot below so a material can be assigned or dropped beforehand.
            if is_stroke and not shape.use_stroke:
                layout.row().prop(shape, "use_stroke", text="Enable Stroke", toggle=True)
            elif not is_stroke and not shape.use_fill:
                layout.row().prop(shape, "use_fill", text="Enable Fill", toggle=True)
            tex = shape.stroke_texture if is_stroke else shape.fill_texture
            # Same drop target as the Shape panel's source row: works with an empty slot, so a
            # material can be dropped in before the part has one.
            layout.row().template_ID_browser(
                tex, "material", text="Drop material", use_users=False)
            return

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


def draw_material_gradient_layer(layout, context, owner, layer, draw_channels=True):
    """The Gradient panel: the ramp, its line and the channels that read them.

    The generated graph is built from these values: the ColorRamp becomes a node in the row's
    group, and the line and midpoint are inputs the Shader Editor shows as the group's sockets.
    A Gradient row draws the ramp inside every colour channel's own subpanel, so the channel
    buttons and what each channel reads sit together. A mask item has one structural Base Color
    record and no channel widget, so its caller passes draw_channels=False and the ramp gets a
    subpanel of its own.
    """
    col = layout.column(align=True)
    # Space is an operator, not the raw RNA setter: switching it converts the line through the
    # active object's surface so the gizmo does not jump (see
    # PAINT_OT_paint_layer_gradient_space_set).
    space_row = col.row(align=True)
    for space_id, space_label in (('UV', "UV"), ('OBJECT', "Object"), ('GENERATED', "Generated")):
        op = space_row.operator(
            "paint.paint_layer_gradient_space_set",
            text=space_label,
            depress=layer.gradient_space == space_id,
        )
        op.space = space_id
        op.marker = layer.marker
    col.separator()
    col.prop(layer, "gradient_type", expand=True)
    col.separator()
    col.prop(layer, "gradient_repeat")
    # The ramp is always the source of the colors. Two colors are spread most easily with the
    # midpoint; with several, the stops of the ramp place them.
    col.separator()
    col.prop(layer, "gradient_distribution", expand=True)
    if layer.gradient_distribution == 'MIDPOINT':
        col.prop(layer, "gradient_midpoint", slider=True)

    if not draw_channels:
        layout.separator()
        header, panel = layout.panel("material_gradient_ramp", default_closed=False)
        header.label(text="Ramp")
        if panel and layer.gradient_supported and layer.gradient_ramp is not None:
            panel.template_color_ramp(layer, "gradient_ramp", expand=True)

    if draw_channels:
        layout.separator()
        draw_material_layer_channels(layout, context, owner, layer)

    # Display belongs after every channel panel and before Bake, which is a panel of its own. The
    # gizmo switch lives in its header so it stays one click away while the body is folded.
    layout.separator()
    header, panel = layout.panel("material_gradient_display", default_closed=True)
    header_row = header.row(align=True)
    header_row.label(text="Display")
    header_row.prop(layer, "gradient_gizmo", text="Show Gizmo", toggle=True, icon='GIZMO')
    if panel:
        # The line's numbers are gizmo parameters, not shading values: they only place the handles.
        # They are the first thing to reach after turning the handles on, so the body opens by
        # default rather than hiding them one more click deep.
        param_header, param_panel = panel.panel("material_gradient_gizmo_parameter",
                                                default_closed=False)
        param_header.label(text="Gizmo Parameter")
        if param_panel:
            line = param_panel.column(align=True)
            line.prop(layer, "gradient_start", text="Start")
            line.prop(layer, "gradient_end", text="End")
            space = layer.gradient_space
            if space == 'UV':
                line.label(text="Z is ignored in UV space", icon='INFO')
            elif space == 'OBJECT':
                line.label(text="Object units", icon='INFO')
            elif space == 'GENERATED':
                line.label(text="0..1, object bounding box", icon='INFO')
        sub = panel.row(align=True)
        sub.active = layer.gradient_gizmo
        sub.prop(layer, "gradient_gizmo_display", expand=True)


def draw_material_generator_layer(layout, context, owner, layer, draw_channels=True):
    """The Pattern panel of a generator row: its kind, the placement and the pattern's parameters.

    The generated graph is built from these values: the kind selector switches the schema the
    parameters draw from (the role, the name and the channel records stay), the placement is the
    first section of every generator's schema, and the pattern parameters are the rest. A Gradient
    is a generator of its own kind and keeps its panel: its line, gizmo and payload have no
    counterpart here. A mask item has no channel records, so its caller passes
    draw_channels=False and the ramp gets a subpanel of its own, as the Gradient's does.
    """
    # The kind selector comes first: everything below draws from the schema of the kind it shows.
    layout.prop(layer, "generator", text="Pattern")

    layout.separator()
    header, panel = layout.panel("material_generator_placement", default_closed=False)
    header.label(text="Placement")
    if panel:
        draw_material_generator_placement(panel, layer)

    layout.separator()
    header, panel = layout.panel("material_generator_pattern", default_closed=False)
    header.label(text="Pattern")
    if panel:
        draw_material_generator_pattern_params(panel, layer)

    if not draw_channels:
        layout.separator()
        header, panel = layout.panel("material_generator_ramp", default_closed=False)
        header.label(text="Ramp")
        if panel and layer.ramp_supported and layer.kind_ramp is not None:
            panel.template_color_ramp(layer, "kind_ramp", expand=True)

    if draw_channels:
        layout.separator()
        draw_material_layer_channels(layout, context, owner, layer)


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

    @staticmethod
    def _draw_lock(layout, row):
        layout.prop(row, "locked", text="Lock", icon='LOCKED' if row.locked else 'UNLOCKED')

    @staticmethod
    def _draw_opacity_lock(layout, row):
        """Opacity with the Lock toggle to its right, as one row."""
        line = layout.row(align=True)
        line.prop(row, "opacity", text="Opacity")
        line.prop(row, "locked", text="", icon='LOCKED' if row.locked else 'UNLOCKED')

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
            if item.gradient_supported:
                # A Gradient mask item has a line and a ramp instead of a map; its invert and blend
                # are the mask item's own, as on any other correction.
                self._draw_opacity_lock(layout, item)
                layout.prop(item, "mask_contrast", text="Contrast", slider=True)
                layout.prop(item, "blend_type", text="Blend")
                if len(item.channels):
                    layout.prop(item.channels[0], "invert", text="Invert", toggle=True)
                layout.separator()
                draw_material_gradient_layer(layout, context, owner, item, draw_channels=False)
                return
            if item.generator_supported:
                # A pattern mask item lays its ramp's grey over the row's mask: the same panel a
                # generator layer draws, with the ramp of its own and no channels to choose.
                self._draw_opacity_lock(layout, item)
                layout.prop(item, "blend_type", text="Blend")
                if len(item.channels):
                    layout.prop(item.channels[0], "invert", text="Invert", toggle=True)
                layout.separator()
                draw_material_generator_layer(layout, context, owner, item, draw_channels=False)
                return
            if item.source == 'FILTER':
                # A filter in the mask changes its one coverage: opacity and the kind's parameters,
                # and no channels to choose. A map-coverage operator draws its own mode, tolerance
                # and colours in #draw_material_mask_coverage_item.
                self._draw_opacity_lock(layout, item)
                layout.prop(item, "mask_contrast", text="Contrast", slider=True)
                layout.separator()
                if item.is_map_coverage:
                    draw_material_mask_coverage_item(layout, context, item)
                elif item.color_match_supported:
                    draw_material_color_match(layout, item.kind_params)
                else:
                    draw_material_kind_params(layout, item.kind_params)
                    if item.ramp_supported:
                        layout.template_color_ramp(item, "kind_ramp", expand=True)
                    if item.curves_supported:
                        layout.template_curve_mapping(
                            item, "kind_curves", type='COLOR', live_update=True)
                    if item.color_balance_supported:
                        draw_material_color_balance(layout, item.kind_color_balance)
                    if item.channel_mixer_supported:
                        draw_material_channel_mixer(layout, item.kind_channel_mixer)
                if item.kind_needs_bake:
                    # A CPU-computed mask effect bakes a grey of its own, like a row's effect does;
                    # the same button asks the owner row's plan for that bake at once.
                    draw_material_update_now(layout, owner)
                return
            if item.source == 'ANCHOR_POINT':
                # A marker publishes a slice and a reader lays one: no map, no channels to choose.
                if item.is_anchor_point_reader:
                    self._draw_opacity_lock(layout, item)
                    layout.prop(item, "blend_type", text="Blend")
                else:
                    self._draw_lock(layout, item)
                return
            self._draw_lock(layout, item)
            draw_material_mask_item(layout, context, item)
            return

        if layer.source == 'ANCHOR_POINT' and not layer.is_anchor_point_reader:
            # The marker is a place: it has no opacity, blend or channels of its own.
            return

        self._draw_opacity_lock(layout, layer)
        # The row's own blend is the base a channel inherits; a channel's override is edited in the
        # Outliner stack, beside its opacity.
        layout.prop(layer, "blend_type", text="Blend")
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
        # A generator is procedural: its kind, parameters and ramp replace the map controls. A
        # Gradient keeps its own panel; every other pattern draws the schema of its kind.
        if layer.source == 'GENERATOR':
            layout.separator()
            if layer.gradient_supported:
                draw_material_gradient_layer(layout, context, owner, layer)
            else:
                draw_material_generator_layer(layout, context, owner, layer)
            return
        # A filter has no map or channel records: what it is told is the parameters of its kind.
        if layer.source == 'FILTER':
            layout.separator()
            if layer.is_transform_effect:
                # A transform effect keeps its mapping as its parameter set: the Mapping panel and
                # the gizmo edit it, and there is nothing else on the row to draw.
                return
            if layer.color_match_supported:
                draw_material_color_match(layout, layer.kind_params)
            else:
                draw_material_kind_params(layout, layer.kind_params)
            if layer.kind_needs_bake:
                draw_material_update_now(layout, owner)
            if layer.ramp_supported:
                layout.template_color_ramp(layer, "kind_ramp", expand=True)
            if layer.curves_supported:
                layout.template_curve_mapping(layer, "kind_curves", type='COLOR', live_update=True)
            if layer.color_balance_supported:
                draw_material_color_balance(layout, layer.kind_color_balance)
            if layer.channel_mixer_supported:
                draw_material_channel_mixer(layout, layer.kind_channel_mixer)
            # The channels it works in, switched like a layer's; Base Color is on to start with.
            layout.separator()
            draw_material_layer_channels(layout, context, owner, layer)
            return
        # A reader has no map either: its records are only the channels it lays.
        if layer.source == 'ANCHOR_POINT':
            layout.separator()
            draw_material_layer_channels(layout, context, owner, layer)
            return
        # A folder holds what its rows hold, so it has no channel records to add: its per-channel
        # blend and opacity are edited in the Outliner stack, and this tab keeps the overall ones.
        if layer.source != 'STACK':
            layout.separator()
            draw_material_layer_channels(layout, context, owner, layer)

        # The Height record's own blend, on the channel it belongs to. Drawn last, after the
        # channels, and through its own helper so the panel body stays a list of what it shows.
        draw_material_layer_height(layout, owner, layer)


def _smart_asset_instance_root_rows(ma, instance_id):
    """The roots of one smart-asset instance: the rows its drop laid down, at any depth.

    A drop Into a folder lays its rows nested, so the top-level rows alone miss them. The walk
    stops at a root it collected: its children draw with it, and a root of the same instance
    nested under it is drawn once, as part of the root's own subtree.
    """
    rows = []

    def walk(row):
        if row.smart_asset_instance_id == instance_id:
            rows.append(row)
            return
        for child in row.children:
            walk(child)

    for row in ma.paint_layers:
        walk(row)
    return rows


def _smart_asset_instance_mask_items(ma, instance_id):
    """The mask items one Smart Mask instance laid on its target row.

    A mask drop lays no rows: its items live on the target's mask, and only they answer for the
    instance -- a duplicated item (a fresh marker) answers for no instance, the way a duplicated
    row does. The target row itself belongs to whatever row instance it did before.
    """
    items = []
    for row in ma.paint_layers:
        for item in row.mask_items:
            if item.smart_asset_instance_id == instance_id:
                items.append(item)
    return items


def _smart_asset_instance_mask_target_name(ma, inst):
    """The name of the row the Smart Mask instance landed on, or None when it is gone.

    The record names the target by its marker; the walk follows the folders the way the
    rows-collecting one does, since a drop can land on a row nested in one.
    """

    def walk(row):
        if row.marker == inst.mask_target:
            return row
        for child in row.children:
            found = walk(child)
            if found is not None:
                return found
        return None

    for row in ma.paint_layers:
        found = walk(row)
        if found is not None:
            return found.name
    return None


class LAYER_MATERIAL_PT_smart_asset_params(LayerMaterialButtonsPanel, Panel):
    """The exposed parameters of the imported Smart assets, one section per instance.

    Every drop of a Smart asset into this material is an instance of its own: the section shows
    the parameters of the rows that drop laid down, with its own Reload and Make Unique, so
    working on one drop never touches the others. The asset decided what to expose when it was
    saved; the instance edits the values here without hunting the rows in the Outliner stack.
    Reload pulls the rows fresh from the asset (keeping these values), Make Unique ends the
    binding and leaves the values as they are. The section of the instance the active row belongs
    to opens by default, and its header says so.
    """

    bl_idname = "LAYER_MATERIAL_PT_smart_asset_params"
    bl_label = "Smart Asset Parameters"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        if cls._object_mask_editing(context):
            return False
        ma = cls._owner_material(context)
        return ma is not None and ma.smart_asset_instance

    def draw(self, context):
        layout = self.layout
        ma = self._owner_material(context)
        # The instance the active row belongs to opens its section and marks its header; -1 reads
        # as "no row active" and leaves every section closed. A mask item answers for its Smart
        # Mask instance the same way a stack row does for its own.
        active_instance_id = -1
        active = ma.paint_layers.active
        if active is not None:
            active_instance_id = active.smart_asset_instance_id
        drawn_any = False
        for inst in ma.smart_asset_instances:
            drawn_any = True
            is_active = inst.id == active_instance_id
            # A row belongs to an instance through its record only: asking the row keeps the
            # duplicates (fresh markers, no instance) out of the section, and the roots the drop
            # laid into a folder come with the walk. A Smart Mask section asks the mask items the
            # same way and says which row the mask landed on; it draws no rows of its own.
            header, body = layout.panel(
                "smart_asset_instance_%d" % inst.id, default_closed=not is_active)
            if inst.is_mask:
                header.label(text=inst.label, icon='SOLO_ON' if is_active else 'MOD_MASK')
                target_name = _smart_asset_instance_mask_target_name(ma, inst)
                if target_name is not None:
                    header.label(text="on " + target_name)
                if not body:
                    continue
                items = _smart_asset_instance_mask_items(ma, inst.id)
                if not draw_material_smart_asset_rows(body, items):
                    body.label(text="The asset exposes no parameters", icon='INFO')
            else:
                header.label(text=inst.label, icon='SOLO_ON' if is_active else 'MATERIAL')
                if not body:
                    continue
                rows = _smart_asset_instance_root_rows(ma, inst.id)
                if not draw_material_smart_asset_rows(body, rows):
                    body.label(text="The asset exposes no parameters", icon='INFO')
            body.separator()
            column = body.column(align=True)
            reload = column.operator(
                "outliner.stack_layer_smart_asset_reload", icon='FILE_REFRESH')
            reload.instance_id = inst.id
            make_unique = column.operator(
                "outliner.stack_layer_smart_asset_make_unique", icon='UNLINKED')
            make_unique.instance_id = inst.id
        if not drawn_any:
            layout.label(text="The asset exposes no parameters", icon='INFO')


class LAYER_MATERIAL_PT_smart_asset_expose(LayerMaterialButtonsPanel, Panel):
    """The expose switches of a Smart asset on the asset material itself.

    Every kind parameter the asset's stack carries, with a toggle each: checked, an import marks
    and binds the parameter and the instances' panel shows it; unchecked, it arrives as a plain
    local. The asset was saved with everything exposed; this is where the author trims the list.
    """

    bl_idname = "LAYER_MATERIAL_PT_smart_asset_expose"
    bl_label = "Expose Parameters"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        if cls._object_mask_editing(context):
            return False
        ma = cls._owner_material(context)
        # The exposed-parameters list lives in the asset material's own custom properties; an
        # instance carries the import marks beside its rows instead, and no list here.
        return ma is not None and ma.get("paint_asset_params") is not None

    def draw(self, context):
        layout = self.layout
        ma = self._owner_material(context)
        if not draw_material_smart_asset_expose(layout, ma):
            layout.label(text="No parameters to expose", icon='INFO')


class LAYER_MT_mapping_reset(Menu):
    """Reset the shown row's UV mapping: all of its values, or one part at a time."""

    bl_label = "Reset Mapping"

    def draw(self, _context):
        layout = self.layout
        layout.operator("paint.layer_mapping_reset", text="Reset All").mode = 'ALL'
        layout.operator("paint.layer_mapping_reset", text="Reset Offset").mode = 'OFFSET'
        layout.operator("paint.layer_mapping_reset", text="Reset Scale").mode = 'SCALE'
        layout.operator("paint.layer_mapping_reset", text="Reset Rotation").mode = 'ROTATION'


class LAYER_MATERIAL_PT_normal_height(LayerMaterialButtonsPanel, Panel):
    """The response of the generated Normal Map and Bump nodes.

    The nodes are the stack's own, marked as such in the material's tree: this panel edits their
    Strength and Distance in place (a value the owned node takes through the sync), without going
    to the Shader Editor and without regenerating the stack. A user's own Bump or Normal Map is
    never touched.
    """

    bl_idname = "LAYER_MATERIAL_PT_normal_height"
    bl_label = "Normal & Height"
    bl_parent_id = "LAYER_MATERIAL_PT_layers"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        if cls._object_mask_editing(context):
            return False
        owner = cls._owner_material(context)
        return owner is not None and (owner.paint_layers_has_bump or
                                      owner.paint_layers_has_normal_map)

    def draw(self, context):
        owner = self._owner_material(context)
        if owner is None:
            return
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        if owner.paint_layers_has_bump:
            layout.prop(owner, "paint_layers_bump_strength", text="Bump Strength")
            layout.prop(owner, "paint_layers_bump_distance", text="Bump Distance")
        if owner.paint_layers_has_normal_map:
            layout.prop(owner, "paint_layers_normal_strength", text="Normal Strength")


class LAYER_MATERIAL_PT_mapping(LayerMaterialButtonsPanel, Panel):
    """Lookup space of the maps and sources the shown row reads: a layer, a correction or a mask
    item.

    Shown while the row can carry a mapping (RNA ``mapping_available``): a Fill -- with a map or
    not, since its mapping is a property of the row and is on from the start -- or a Material row
    with a source material. The rotation turns the lookup around Z; the tangent-space vectors of a
    Normal map are rotated (and flipped by the scale sign) to follow it. A Material row moves the
    UV at which its whole source is read, including its live source graph. Scale, Rotation and Lock
    edit the tile the gizmo draws: they turn it around its centre, the gizmo's anchor ring, so it
    keeps its place on the canvas; only Offset moves it. The row's raw ``mapping_scale`` and
    ``mapping_rotation`` keep the data's own meaning for scripts and drivers.

    UV reads the active UVs; Planar, Box, Triplanar, Cube, Cylinder and Sphere project the map
    through a frame in object or controller space. The frame's Empty (optional) places, turns and
    scales the projection, while the inline center/size/roll give it its extents. The UV
    controls below the frame adjust the projected UVs (or the UVs themselves in UV mode).
    """

    bl_idname = "LAYER_MATERIAL_PT_mapping"
    bl_label = "Mapping & Projection"
    bl_parent_id = "LAYER_MATERIAL_PT_layers"

    @classmethod
    def _shown_row(cls, context):
        """The row the Layer tab edits: the shown mask item, or else the active row.

        This is the Python mirror of ``mapping_shown_row_get`` (uv_mapping_target.cc), which the
        mapping gizmo and the mapping reset resolve through; keep both in sync.
        """
        owner, layer = cls._active_layer(context)
        if owner is None or layer is None:
            return None
        item = cls._shown_mask_item(context, owner, layer)
        return item if item is not None else layer

    @classmethod
    def poll(cls, context):
        row = cls._shown_row(context)
        return row is not None and (row.mapping_available or row.projection_supported)

    def draw_header_preset(self, _context):
        # Drawn while the panel's poll passed, so a row that carries a mapping is shown; the menu's
        # operators poll the same way and grey out with it. The preset hook is the header's
        # right-aligned slot, which puts the button at the header's far edge.
        self.layout.menu("LAYER_MT_mapping_reset", text="", icon='RECOVER_LAST')

    def _draw_projection(self, context, layout, row):
        """The main fields of a projected row: place, preset, frame size, roll and the limits."""
        layout.operator("paint.layer_projection_place",
                        text="Place on Surface",
                        icon='RESTRICT_SELECT_OFF')
        sculpt = context.tool_settings.sculpt
        if sculpt is not None:
            layout.prop(sculpt, "show_projection_gizmo", text="Show Gizmo", toggle=1)
        layout.prop(row, "projection_preset", text="Preset")

        row_size = layout.row(align=True)
        row_size.prop(row, "projection_size", index=0, text="Size X")
        row_size.prop(row, "projection_size", index=1, text="Y")
        layout.prop(row, "projection_roll", text="Roll")
        # A volume kind bounds all three axes by the clip itself and has no single projection
        # direction, so the Limit toggle and the facing fade stay Planar-only; the Depth field
        # is the frame's Size Z and stays for every space.
        # A volume space projects from every side, so the facing fade and the depth limit have no
        # single direction to act along and the projection ignores them.
        volume_space = row.projection_is_volume
        depth_row = layout.row(align=True)
        if not volume_space:
            depth_row.prop(row, "projection_depth", text="Limit", toggle=1)
        depth_row.prop(row, "projection_depth_size", text="Depth")

        if not volume_space:
            facing_row = layout.row(align=True)
            facing_row.prop(row, "projection_facing", text="Facing Fade")
            angle_col = facing_row.column()
            angle_col.enabled = row.projection_facing
            angle_col.prop(row, "projection_angle", text="Angle")
            flip_col = facing_row.column()
            flip_col.enabled = row.projection_facing
            flip_col.prop(row, "projection_facing_invert", text="Flip", toggle=1)

        layout.separator()

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        row = self._shown_row(context)
        if row is None:
            return

        layout.prop(row, "mapping_space", text="Projection")

        if row.mapping_space != 'UV':
            self._draw_projection(context, layout, row)

        if row.mapping_space == 'UV':
            # A transform effect keeps its mapping as its parameter set: a Mirror reads only the
            # fold's axes and center, so the transform sliders would promise a move it never makes.
            is_mirror_effect = row.is_transform_effect and row.filter_type == 'MIRROR'
            layout.prop(row, "mapping_enabled", text="UV Adjust")
            if not is_mirror_effect:
                col = layout.column()
                col.enabled = row.mapping_enabled
                col.prop(row, "mapping_offset", text="Offset")
                sub = col.row(align=True)
                # The tile-holding RNA properties follow the offset so the tile stays put.
                sub.prop(row, "tile_scale", text="Scale")
                sub.prop(row,
                         "tile_scale_lock",
                         text="",
                         icon='LOCKED' if row.tile_scale_lock else 'UNLOCKED',
                         icon_only=True)
                col.prop(row, "tile_rotation", text="Rotation")
                if row.is_transform_effect:
                    # Scale and Rotation turn about the pivot; moving it keeps what the effect shows.
                    col.prop(row, "mapping_pivot", text="Pivot")
                col.prop(row, "mapping_wrap", text="UV Wrap")

            if row.mapping_enabled and row.source == 'MATERIAL':
                # The wrapper moves the active UV layer (TexCoord UV and open Image Textures) of the
                # source tree and of the groups on the path to its Principled BSDF; a coordinate-driven
                # procedural texture, a named UV Map node and a group beside that path keep their own input.
                note = layout.column(align=True)
                note.enabled = False
                note.label(text="Only the active UV of the main shader path is mapped",
                           icon='BLANK1')
                note.label(text="Procedural, named-UV and side-group textures are not",
                           icon='BLANK1')
        else:
            # A projected row is addressed by its frame: the wrap and the tiling are the only mapping
            # it follows, an offset or rotation would move the lookup away from the frame. The
            # tiling is the row's mapping scale, applied to the UV only, so the footprint stays put.
            sub = layout.row(align=True)
            sub.prop(row, "mapping_scale", text="Tiling")
            sub.prop(row,
                     "tile_scale_lock",
                     text="",
                     icon='LOCKED' if row.tile_scale_lock else 'UNLOCKED',
                     icon_only=True)
            layout.prop(row, "mapping_wrap", text="UV Wrap")


def _decal_name_of_row(layer):
    """The decal a row belongs to, or None: the text in front of the last " · " separator.

    Mirrors `decal_name_of_row_name`, the one parser of the row naming scheme.
    """
    name, sep, _channel = layer.name.rpartition(" · ")
    return name if sep and name else None


def _decal_shown_name(ma):
    """The decal the decal operators act on: the active row's group, else the only one.

    Mirrors `decal_target_find`: the anchor button and the panel label then always talk about the
    same group, also when the material holds more than one decal.
    """
    if ma is None:
        return None
    active = ma.paint_layers.active
    if active is not None and _decal_name_of_row(active) is not None:
        return _decal_name_of_row(active)
    names = set()
    for layer in ma.paint_layers:
        name = _decal_name_of_row(layer)
        if name is not None:
            names.add(name)
    return names.pop() if len(names) == 1 else None


class LAYER_MATERIAL_PT_decal(LayerMaterialButtonsPanel, Panel):
    """The decal workflow on the projected rows.

    A decal is the row group one image maps through: every row is named ``<decal> · <channel>`` and
    shares one Planar frame, which "Project from View" places the way a camera at the view would
    project through. The block buttons treat the group as one: order, hide, blend, space.
    """

    bl_idname = "LAYER_MATERIAL_PT_decal"
    bl_label = "Decal"
    bl_parent_id = "LAYER_MATERIAL_PT_layers"

    @classmethod
    def poll(cls, context):
        ob = context.active_object
        return ob is not None and ob.mode == 'TEXTURE_PAINT'

    def draw_header(self, context):
        # Show the active decal name next to the panel label so the user knows which group the
        # block buttons act on without having to open the layer list.
        ob = context.active_object
        if ob is None or ob.mode != 'TEXTURE_PAINT':
            return
        ma = ob.active_material
        if ma is None:
            return
        decal_name = _decal_shown_name(ma)
        if decal_name is not None:
            self.layout.label(text=decal_name)

    def draw(self, context):
        layout = self.layout
        col = layout.column(align=True)
        row = col.row(align=True)
        row.operator("paint.decal_add", text="Add Decal...", icon='ADD')
        # Drop zone: an image dropped on this row becomes a decal through the UI drop target
        # (paint.decal_add_image); a click opens the file browser, like the Add button above.
        # "AUTO" lets the file name decide the channel (a _n/_normal suffix is a normal stamp).
        drop = col.row(align=True)
        drop.context_string_set("decal_drop_channel", "AUTO")
        drop.operator("paint.decal_add", text="Drop Image to Add Decal", icon='FILE_IMAGE')
        col.operator("paint.decal_project_from_view", text="Project from View", icon='CAMERA_DATA')
        col.operator(
            "paint.decal_from_selection",
            text="Decal from Selection",
            icon='MESH_GRID',
        )

        # The world anchor: the group's rows read their frame in the space of a controller Empty,
        # so a material shared by several objects shows the decal in one world place. Without one,
        # a shared material keeps the decal in each object's own space.
        ma = context.active_object.active_material if context.active_object else None
        decal_name = _decal_shown_name(ma)
        controller = None
        if decal_name is not None:
            controllers = {
                layer.projection_object
                for layer in ma.paint_layers
                if _decal_name_of_row(layer) == decal_name
            }
            # One controller for the whole group, or none: a mixed group is not anchored.
            if len(controllers) == 1:
                controller = next(iter(controllers))
        col = layout.column(align=True)
        if controller is None:
            col.operator(
                "paint.decal_world_anchor_set", text="Anchor to World", icon='WORLD'
            ).attach = True
            if ma is not None and ma.users > 1:
                col.label(
                    text="Material is shared by %d objects: the decal lives in each object's space"
                    % ma.users,
                    icon='ERROR',
                )
        else:
            row = col.row(align=True)
            row.label(text="Anchor: " + controller.name, icon='WORLD')
            row.operator("paint.decal_world_anchor_set", text="Release", icon='X').attach = False
        col.operator(
            "paint.decal_propagate_to_selected", text="Propagate to Selected", icon='DUPLICATE'
        )

        col = layout.column(align=True)
        row = col.row(align=True)
        row.operator("paint.decal_move", text="", icon='TRIA_UP').direction = 'UP'
        row.operator("paint.decal_move", text="", icon='TRIA_DOWN').direction = 'DOWN'
        row = col.row(align=True)
        row.operator("paint.decal_visibility_set", text="", icon='HIDE_OFF').visible = True
        row.operator("paint.decal_visibility_set", text="", icon='HIDE_ON').visible = False
        col.operator("paint.decal_remove", text="Remove Decal", icon='X')

        # The group's coverage mask: every row gets a full mask, then the Mask brush paints the
        # decal's extent (the addon's face map).
        col = layout.column(align=True)
        row = col.row(align=True)
        row.operator("paint.decal_mask_add", text="Add Mask", icon='MOD_MASK')
        row.operator("paint.decal_mask_remove", text="Remove Mask", icon='X')

        col = layout.column(align=True)
        col.label(text="Projection Space")
        row = col.row(align=True)
        row.operator("paint.decal_projection_mode_set", text="Planar").space = 'PLANAR'
        row = col.row(align=True)
        row.operator("paint.decal_projection_mode_set", text="Box").space = 'BOX'
        row.operator("paint.decal_projection_mode_set", text="Triplanar").space = 'TRIPLANAR'
        row = col.row(align=True)
        row.operator("paint.decal_projection_mode_set", text="Cylinder").space = 'CYLINDER'
        row.operator("paint.decal_projection_mode_set", text="Sphere").space = 'SPHERE'
        col.operator("paint.decal_projection_mode_set", text="Cube Atlas").space = 'CUBE'
        col.operator("paint.decal_blend_mode_set", text="Blend Mode...")

        # The Decal Master converter: offered only on a material the addon set up (the sna_*
        # properties do not exist in a plain Blender build).
        ma = context.active_object.active_material if context.active_object else None
        if ma is not None and hasattr(ma, "sna_dcl_col") and len(ma.sna_dcl_col) > 0:
            col = layout.column(align=True)
            col.label(text="Decal Master")
            col.operator(
                "material.paint_layer_decal_import_decal_master",
                text="Import Decal Master Setup",
            )


class LAYER_MATERIAL_PT_mapping_advanced(LayerMaterialButtonsPanel, Panel):
    """Advanced projection fields: center, clip, axis, blend, cube layout and the controller."""

    bl_idname = "LAYER_MATERIAL_PT_mapping_advanced"
    bl_label = "Advanced"
    bl_parent_id = "LAYER_MATERIAL_PT_mapping"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        row = LAYER_MATERIAL_PT_mapping._shown_row(context)
        return row is not None and row.mapping_space != 'UV'

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        row = LAYER_MATERIAL_PT_mapping._shown_row(context)
        if row is None:
            return

        col = layout.column(align=True)
        col.prop(row, "projection_center", text="Center")
        col.prop(row, "projection_clip", text="Clip to Frame")
        col.prop(row, "projection_fit_aspect", text="Fit Image Aspect")
        if row.projection_uses_axis:
            col.prop(row, "projection_axis", text="Axis")
        if row.projection_uses_blend:
            col.prop(row, "projection_blend", text="Normal Blend", slider=True)
        if row.projection_uses_cube_layout:
            col.prop(row, "projection_cube_layout", text="Layout")

        layout.separator()
        controller = layout.column(align=True)
        controller.label(text="Controller")
        controller.prop(row, "projection_object", text="")
        sub = controller.row(align=True)
        sub.operator("paint.layer_projection_add_controller", text="Add", icon='ADD')
        sub.operator("paint.layer_projection_select_controller",
                     text="Select",
                     icon='RESTRICT_SELECT_OFF')
        sub.operator("paint.layer_projection_controller_to_cursor",
                     text="Set to Cursor",
                     icon='CURSOR')
        obj = row.projection_object
        if obj is not None and context.scene not in obj.users_scene:
            controller.label(text="Controller is not in the current scene", icon='ERROR')
        # The constraint lives on the controller, so there is nothing to snap without one.
        snap = controller.row()
        snap.enabled = obj is not None
        snap.prop(row, "projection_snap", text="Snap to Surface")
        # Ucupaint sizes its decal by the Empty itself; this mode makes the inline Size one
        # and leaves the controller's scale as the only place the frame is sized.
        size_from_empty = controller.row()
        size_from_empty.enabled = obj is not None
        size_from_empty.prop(row, "projection_size_from_controller", text="Size from Empty")


class LAYER_MATERIAL_PT_mapping_mirror(LayerMaterialButtonsPanel, Panel):
    """Mirror fold of the Mapping panel: a subpanel whose header checkbox switches it on or off.

    Off by default. The axes and the center are kept while it is off, so switching it back on
    restores them.
    """

    bl_idname = "LAYER_MATERIAL_PT_mapping_mirror"
    bl_label = "Mirror"
    bl_parent_id = "LAYER_MATERIAL_PT_mapping"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        row = LAYER_MATERIAL_PT_mapping._shown_row(context)
        # A Transform Mapping effect keeps its pivot in the mirror center: the fold is not its own.
        return row is not None and not (row.is_transform_effect and row.filter_type == 'TRANSFORM')

    def draw_header(self, context):
        row = LAYER_MATERIAL_PT_mapping._shown_row(context)
        if row is not None:
            self.layout.prop(row, "mapping_mirror_enabled", text="")

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        row = LAYER_MATERIAL_PT_mapping._shown_row(context)
        if row is None:
            return

        # Mirror: fold around a center point on U and/or V axes. The fold makes both sides show the
        # same content in real time, useful for symmetric painting.
        layout.active = row.mapping_mirror_enabled
        mirror_col = layout.column(align=True)
        # The toggles are plain buttons: with property split on, each lands in the value column and
        # rows of different nesting no longer line up, so the split is off for this block only.
        toggle_col = mirror_col.column(align=True)
        toggle_col.use_property_split = False
        mirror_row = toggle_col.row(align=True)
        mirror_row.prop(row, "mapping_mirror_u", text="U", toggle=True)
        mirror_row.prop(row, "mapping_mirror_v", text="V", toggle=True)
        # Direction per axis: off copies the larger side onto the smaller one, on does the opposite.
        flip_row = toggle_col.row(align=True)
        sub = flip_row.row(align=True)
        sub.active = row.mapping_mirror_u
        sub.prop(row, "mapping_mirror_u_negative", text="Flip U", toggle=True)
        sub = flip_row.row(align=True)
        sub.active = row.mapping_mirror_v
        sub.prop(row, "mapping_mirror_v_negative", text="Flip V", toggle=True)
        # The center is a value of the row, so the field below can be typed before an axis is
        # switched on: the natural order (place the point, then fold around it) works. The sculpt 3D
        # cursor is projected through the active object's surface, so it places the point from
        # anywhere. The two modes that need a view of their own live where that view is: the Image
        # Editor's header places the point in the image, the 3D Viewport's header places it on the
        # mesh, and the Image Editor's mapping gizmo drags it.
        mirror_sub = mirror_col.column()
        mirror_sub.prop(row, "mapping_mirror_center", text="Center")
        from_cursor = mirror_sub.row(align=True).operator(
            "paint.layer_mapping_mirror_from_cursor",
            text="From 3D Cursor",
            icon='PIVOT_CURSOR',
        )
        # The row the button edits: the operators resolve it from this marker, so the panel edits
        # what it shows whatever the other editors happen to display.
        from_cursor.marker = row.marker
        if row.mapping_mirror_applies:
            gizmo_note = mirror_col.column(align=True)
            gizmo_note.enabled = False
            gizmo_note.label(text="Drag the mirror handle in the Image Editor", icon='BLANK1')


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
        if owner is None or layer is None:
            return False
        item = cls._shown_mask_item(context, owner, layer)
        # A mask correction that reads a material is edited here as a Material layer is.
        subject = item if item is not None else layer
        return subject.source == 'MATERIAL' and subject.material is not None

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        owner, layer = self._active_layer(context)
        if layer is None:
            return
        item = self._shown_mask_item(context, owner, layer)
        subject = item if item is not None else layer
        if subject.material is None:
            return
        if item is not None:
            # A mask takes one grey: which channel of the source it comes from.
            col = layout.column(align=True)
            col.label(text="Source Channel")
            col.prop(item, "mask_source_channel", expand=True)
            layout.separator()
        source = subject.material
        if source.library is not None:
            layout.label(text="Linked, not editable", icon='LIBRARY_DATA_DIRECT')
            layout.operator("paint.layer_source_make_local",
                            text="Make Local", icon='LIBRARY_DATA_DIRECT')
            return
        if source.node_tree is None:
            layout.label(text="The source has no nodes", icon='INFO')
            return
        panel_node_draw(layout, source.node_tree, 'OUTPUT_MATERIAL', "Surface")


class LAYER_MATERIAL_PT_anchor_point(LayerMaterialButtonsPanel, Panel):
    """The Anchor Point of the shown row: the point a reader lays, or how many readers a point has."""

    bl_idname = "LAYER_MATERIAL_PT_anchor_point"
    bl_label = "Anchor Point"
    bl_parent_id = "LAYER_MATERIAL_PT_layers"

    @classmethod
    def _shown_row(cls, context):
        """The mask item the tab shows, or else the active row."""
        owner, layer = cls._active_layer(context)
        if owner is None or layer is None:
            return None
        item = cls._shown_mask_item(context, owner, layer)
        return item if item is not None else layer

    @classmethod
    def poll(cls, context):
        row = cls._shown_row(context)
        return row is not None and row.source == 'ANCHOR_POINT'

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        row = self._shown_row(context)
        if row is None:
            return
        if row.is_anchor_point_reader:
            layout.prop(row, "anchor_point_target", text="Reads")
        else:
            layout.label(text="Read by: {:d}".format(row.anchor_point_reader_count), icon='LINKED')


class LAYER_MATERIAL_PT_bake(LayerMaterialButtonsPanel, Panel):
    """The bake cache of the active row: when its subtree is rendered into maps the stack reads.

    A baked row is a cache of the live row, not a replacement for it: it is read only while it
    matches the row's description, and the first edit to the row or to a map it reads brings the
    live row back until the cache is rendered again. Auto bakes the heavy folders nested in another
    folder that are not being edited, plus any row a Gradient takes part in once it is left -- when
    exactly that happens the material's Bake Gradient setting answers; Always bakes any row once it
    is not being edited. An Object or Generated row is one tiled map with a tile per object of the
    current scene and view layer, so it is baked only when every object can supply a surface.
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
        item = cls._shown_mask_item(context, owner, layer)
        # A mask item is normally a coverage item, but a mask Anchor Point Reader has a bake mode
        # like any other reader and needs this panel.
        return item is None or (item.source == 'ANCHOR_POINT' and item.is_anchor_point_reader)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        owner, layer = self._active_layer(context)
        if layer is None:
            return
        item = self._shown_mask_item(context, owner, layer)
        subject = item if item is not None else layer

        layout.prop(subject, "bake_mode", text="Mode")
        layout.prop(subject, "bake_size_preset", text="Size")

        issue = subject.bake_object_issue
        if issue != 'OK':
            reasons = {
                'NO_CONTEXT': "Bake needs a scene and view layer",
                'NO_OBJECT': "No visible object uses this material",
                'NO_MESH': "An object using this material has no mesh",
                'NO_UV': "An object using this material has no UV map",
                'NO_SIZE': "The row has no bake size yet",
                'TOO_MANY': "More than 8 objects use this material",
                'TOO_LARGE': "Lower the bake size to 4096 or less",
                'OVER_BUDGET': "The 2 GiB bake budget is exceeded",
                'ANIMATED': "The object deforms over time (modifier, shape keys or drivers)",
            }
            layout.label(text=reasons.get(issue, "The object bake cannot run"), icon='ERROR')

        # What an edited layer that reads another material renders for the Image Editor's composite
        # while it is edited. The settings belong to the material, so they apply to every such layer.
        # Leaving the layer renders its maps at their own size (Size above) either way.
        col = layout.column(heading="Image Editor Preview")
        col.context_pointer_set("material", owner)
        row = col.row(align=True)
        row.prop(owner, "paint_layers_preview_sync", text="Sync", toggle=True, icon='UV_SYNC_SELECT')
        row.operator("material.paint_layers_bake_now", text="Update Now", icon='FILE_REFRESH')
        sub = col.row(align=True)
        sub.active = not owner.paint_layers_preview_sync
        sub.prop(owner, "paint_layers_preview_delay", text="Delay")
        col.prop(owner, "paint_layers_preview_size", text="Resolution")
        if owner.paint_layers_is_stale:
            col.label(text="Updating...", icon='TIME')

        if subject.source == 'MATERIAL':
            # A plain value and a plain texture of the source are shown as they are. What the
            # source supplies as a graph is shown through a copy of that graph (Auto and Never)
            # or from the maps rendered from it (Always); the maps are also what the Image Editor
            # and the paint slots read.
            if subject.bake_mode == 'NEVER':
                layout.label(text="Nothing is rendered: the graph of the source is shown live", icon='INFO')
            else:
                if subject.bake_mode == 'AUTO':
                    layout.label(text="The graph of the source is shown live", icon='INFO')
                layout.label(
                    text="Maps up to date" if subject.bake_is_valid else "Maps rendering or out of date",
                    icon='CHECKMARK' if subject.bake_is_valid else 'TIME',
                )
            return
        if subject.bake_mode != 'NEVER':
            layout.label(
                text="Baked" if subject.bake_is_valid else "Not baked yet",
                icon='CHECKMARK' if subject.bake_is_valid else 'TIME',
            )


class LAYER_MT_row_result_source(Menu):
    """The rows whose result can be baked into the active row's channel maps."""

    bl_label = "Source Row"

    def draw(self, context):
        layout = self.layout
        owner = LayerMaterialButtonsPanel._owner_material(context)
        if owner is None or owner.paint_layers.active is None:
            return
        active_marker = owner.paint_layers.active.marker
        layout.context_pointer_set("material", owner)

        def draw_rows(rows, depth):
            for row in reversed(list(rows)):
                if row.marker != active_marker:
                    op = layout.operator(
                        "material.paint_layer_use_row_result",
                        text=("    " * depth) + (row.name or "Layer"),
                        icon='FILE_FOLDER' if len(row.children) else 'IMAGE_DATA',
                    )
                    op.source_marker = row.marker
                draw_rows(row.children, depth + 1)

        draw_rows(owner.paint_layers, 0)


class LAYER_MATERIAL_PT_row_result(LayerMaterialButtonsPanel, Panel):
    """Bake the result of another row into the maps of the active row."""

    bl_idname = "LAYER_MATERIAL_PT_row_result"
    bl_label = "Use Row Result"
    bl_parent_id = "LAYER_MATERIAL_PT_layers"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        if not super().poll(context):
            return False
        owner, layer = cls._active_layer(context)
        # A row that reads a material owns no maps to bake into; a Gradient owns none either.
        if owner is None or layer is None or layer.source in {'MATERIAL', 'GENERATOR'}:
            return False
        return cls._shown_mask_item(context, owner, layer) is None

    def draw(self, context):
        layout = self.layout
        owner, layer = self._active_layer(context)
        if layer is None:
            return
        layout.label(text="Bake a row's result into \"{:s}\"".format(layer.name), icon='INFO')
        layout.context_pointer_set("material", owner)
        source = owner.paint_layers.find(layer.row_result_source)
        row = layout.row(align=True)
        row.menu(
            "LAYER_MT_row_result_source",
            text=(source.name or "Layer") if source is not None else "Source Row...",
            icon='RENDER_RESULT',
        )
        sub = row.row(align=True)
        sub.enabled = source is not None
        sub.operator("material.paint_layer_use_row_result", text="", icon='FILE_REFRESH')
        if layer.row_result_source and source is None:
            layout.label(text="The source row no longer exists", icon='ERROR')


# The mesh map types come straight from the RNA enum (#MaterialPaintLayer.mesh_map_type), so a
# type added in DNA shows up here without touching this file.
_MESH_MAP_TYPES = tuple(
    (item.identifier, item.name)
    for item in bpy.types.MaterialPaintLayer.bl_rna.properties['mesh_map_type'].enum_items
    if item.identifier != ''
)

# The status keys come from the RNA enum (#ObjectMeshMapState.status); the label and icon pairs
# stay a UI mapping of their own.
_MESH_MAP_STATUS_LABEL_ICON = {
    'NONE': ("Not baked", 'DOT'),
    'VALID': ("Valid", 'CHECKMARK'),
    'STALE': ("Stale", 'FILE_REFRESH'),
    'BAKING': ("Baking", 'FILE_REFRESH'),
    'ERROR': ("Error", 'ERROR'),
}
_MESH_MAP_STATUS = {
    item.identifier: _MESH_MAP_STATUS_LABEL_ICON[item.identifier]
    for item in bpy.types.ObjectMeshMapState.bl_rna.properties['status'].enum_items
    if item.identifier != ''
}


def _mesh_map_state(context, mat, map_type):
    for state in context.object.mesh_map_states:
        if state.material == mat and state.type == map_type:
            return state
    return None


class LAYER_MATERIAL_PT_mesh_maps(LayerMaterialButtonsPanel, Panel):
    """Mesh map atlases of the stack's owner material and the active object's bake state.

    Drawn in the Layer Material tab, so the material is the owner (the active slot), never the
    source material `context.material` may point at; the bake operator bakes the active slot too.
    A mesh without any UV layer cannot be baked: the map is sampled through the active UV of the
    mesh, so the panel warns instead of offering a bake that the job would refuse.
    """

    bl_idname = "LAYER_MATERIAL_PT_mesh_maps"
    bl_label = "Mesh Maps"

    @classmethod
    def poll(cls, context):
        if cls._object_mask_editing(context):
            return False
        ob = context.object
        return (
            cls._owner_material(context) is not None and
            ob.type == 'MESH' and ob.data is not None
        )

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        mat = self._owner_material(context)
        ob = context.object

        # D: reads the mesh's active UV; there is no per-material named-UV layer to pick.
        if ob.data.uv_layers.active is None:
            layout.label(text="The object has no UV map", icon='ERROR')

        actions = layout.row(align=True)
        # The bakes are background wmJobs; a plain button would run the synchronous exec path and
        # block the UI. Clear is cheap and runs inline.
        actions.operator_context = 'INVOKE_DEFAULT'
        bake_all = actions.operator(
            "object.mesh_map_bake_all", text="Bake All Maps", icon='RENDER_STILL')
        bake_all.types = set()
        bake_all.object_scope = 'ACTIVE'
        bake_all.material_index = 0
        bake_all_objects = actions.operator(
            "object.mesh_map_bake_all", text="Bake All Objects")
        bake_all_objects.types = set()
        bake_all_objects.object_scope = 'ALL'
        bake_all_objects.material_index = 0
        actions.operator_context = 'EXEC_DEFAULT'
        actions.operator("object.mesh_map_clear", text="Clear").types = set()

        header, settings_layout = layout.panel("mesh_map_settings", default_closed=True)
        header.label(text="Settings")
        if settings_layout:
            settings = mat.mesh_map_settings
            settings_layout.use_property_split = True
            col = settings_layout.column(align=True)
            col.prop(settings, "resolution")
            col.prop(settings, "samples")
            col.prop(settings, "use_denoise")
            col.prop(settings, "margin")
            col.prop(settings, "ao_distance")
            col.prop(settings, "edge_radius")

        source = next((item for item in ob.mesh_map_sources if item.material == mat), None)
        hp_header, hp = layout.panel("mesh_map_highpoly", default_closed=True)
        hp_header.label(text="High-poly Source")
        if hp:
            if source is None:
                hp.operator("object.mesh_map_source_add", text="Use High-poly Source", icon='ADD')
            else:
                hp.prop_search(source, "high_poly", context.scene, "objects", text="Object")
                hp.prop_search(
                    source, "high_poly_collection", context.blend_data, "collections",
                    text="Collection")
                hp.label(
                    text="AO is computed per high-poly object; pieces do not occlude each other")
                hp.prop(source, "cage", text="Cage")
                hp.prop(source, "cage_extrusion")
                hp.prop(source, "max_ray_distance")
                if mesh_map_source_member_count(source) == 0:
                    hp.label(text="The source resolves to no mesh objects", icon='ERROR')
                elif source.cage is not None and not source.cage_matches(
                        depsgraph=context.view_layer.depsgraph):
                    hp.label(
                        text="The cage must have the same face count as the low-poly object",
                        icon='ERROR')
                hp.operator("object.mesh_map_source_remove", text="Remove", icon='X')

        baking = False
        for map_type, type_label in _MESH_MAP_TYPES:
            row = layout.row(align=True)
            row.label(text=type_label)
            state = _mesh_map_state(context, mat, map_type)
            if state is None:
                status_label, status_icon = _MESH_MAP_STATUS['NONE']
            else:
                status_label, status_icon = _MESH_MAP_STATUS[state.status]
                baking = baking or state.status == 'BAKING'
            row.label(text=status_label, icon=status_icon)
            slot = mat.mesh_map_slots.find(type=map_type)
            image = slot.image if slot is not None else None
            row.label(text=image.name if image is not None else "\u2014")

            row.operator_context = 'INVOKE_DEFAULT'
            bake = row.operator("object.mesh_map_bake", text="Bake", icon='RENDER_STILL')
            bake.type = map_type
            bake.material_index = 0
            row.operator_context = 'EXEC_DEFAULT'

            # No "Add Mask" here: a Mesh Map mask needs the mask-correction RNA to accept the
            # Mesh Map source, which this build does not do yet.
            row.operator(
                "material.mesh_map_add_layer", text="Add Layer", icon='ADD').type = map_type

        if baking:
            layout.label(text="Baking...", icon='FILE_REFRESH')

        objects = mesh_map_objects_for_material(context.scene, mat)
        if len(objects) > 1:
            box = layout.box()
            box.label(text="Objects using this material", icon='OUTLINER_OB_MESH')
            for other in objects:
                status, count = mesh_map_summary_status(other, mat)
                status_label, status_icon = _MESH_MAP_STATUS[status]
                line = box.row(align=True)
                line.label(
                    text="{}: {} ({} map(s))".format(other.name, status_label, count),
                    icon=status_icon,
                )
                # D: bakes through the mesh's active UV, so an object without one is unusable
                # for the shared atlas.
                if other.data is not None and other.data.uv_layers.active is None:
                    line.label(text="no active UV", icon='ERROR')

        layout.operator("object.mesh_map_refresh", text="Refresh Status", icon='FILE_REFRESH')


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

        # The baked CPU cache of a converted material's own graph, what its composite reads. The
        # one status line it gets: the mark is only ever set on a source-graph material, so the
        # property doubles as the mode check.
        if owner.source_cache_is_stale:
            col = layout.column(heading="Source-Graph Cache")
            col.context_pointer_set("material", owner)
            col.label(text="Outdated", icon='TIME')
            col.operator("material.source_cache_update", text="Update Now", icon='FILE_REFRESH')


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
        layout.prop(owner, "paint_layers_gradient_bake")


class LAYER_MATERIAL_PT_active_channel(LayerMaterialButtonsPanel, Panel):
    """The owner material's global paint channel set: which channels the stack works with.

    A channel removed here leaves each row's record and map untouched; the rows simply stop showing
    it. Base Color is an ordinary channel, but the set always keeps at least one, so the last one
    cannot be removed. The stack starts with the set chosen in the New Layered Material dialog.
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


class LAYER_MATERIAL_PT_output_texture(LayerMaterialButtonsPanel, Panel):
    """Where and how Render Output writes the final maps of the layered material."""

    bl_idname = "LAYER_MATERIAL_PT_output_texture"
    bl_label = "Output Texture"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        if cls._object_mask_editing(context):
            return False
        return cls._owner_material(context) is not None

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        owner = self._owner_material(context)
        if owner is None:
            return
        out = owner.paint_layers_output

        layout.prop(out, "destination")
        if out.destination == 'DISK':
            layout.prop(out, "directory")
        layout.prop(out, "file_format")
        layout.prop(out, "color_depth")
        layout.prop(out, "existing_mode")
        layout.prop(out, "name_pattern")

        layout.separator()
        layout.context_pointer_set("material", owner)
        # The render reads the composite of the stack, so it waits for the maps like the Bake panel.
        col = layout.column()
        col.enabled = not owner.paint_layers_is_stale
        col.operator("material.paint_layers_output_render", icon='RENDER_RESULT')
        if owner.paint_layers_is_stale:
            row = layout.row(align=True)
            row.label(text="Updating...", icon='TIME')
            row.operator("material.paint_layers_bake_now", text="Update Now", icon='FILE_REFRESH')
        if out.last_result:
            layout.label(text=out.last_result, icon='INFO')


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
    MATERIAL_MT_layered,
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
    LAYER_MATERIAL_PT_normal_height,
    LAYER_MATERIAL_PT_mapping,
    LAYER_MATERIAL_PT_decal,
    LAYER_MATERIAL_PT_mapping_advanced,
    LAYER_MATERIAL_PT_mapping_mirror,
    LAYER_MT_mapping_reset,
    LAYER_MATERIAL_PT_source_surface,
    LAYER_MATERIAL_PT_anchor_point,
    LAYER_MATERIAL_PT_bake,
    LAYER_MT_row_result_source,
    LAYER_MATERIAL_PT_row_result,
    LAYER_MATERIAL_PT_channels,
    LAYER_MATERIAL_PT_advanced,
    LAYER_MATERIAL_PT_active_channel,
    LAYER_MATERIAL_PT_output_texture,
    LAYER_MATERIAL_PT_object_mask,
    LAYER_MATERIAL_PT_smart_asset_params,
    LAYER_MATERIAL_PT_smart_asset_expose,
    LAYER_MATERIAL_PT_mesh_maps,
)


if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
