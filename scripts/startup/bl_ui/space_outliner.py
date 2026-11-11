# SPDX-FileCopyrightText: 2009-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy
from bpy.types import Header, Menu, Panel

from bpy.app.translations import (
    contexts as i18n_contexts,
)


def has_selected_ids_in_context(context):
    if getattr(context, "id", None) is not None:
        return True
    if len(context.selected_ids) > 0:
        return True

    return False


# Material Symbols codepoints, kept in sync with `paint_glyph` in
# `outliner_stack_source_paint_layers.cc` (the Add menu and the rows use the same glyphs).
# `glyph_constants_stay_in_step_with_space_outliner` in that file's test pins the C++ values, so a
# change there fails the test and sends the reader here.
GLYPH_BRUSH = 0xE3AE
_GLYPH_FILL = 0xE997
_GLYPH_GRADIENT = 0xE3E9
_GLYPH_PATTERN = 0xE3A5
_GLYPH_ANCHOR = 0xF1CD

# The add and context menus registered stack sources provide, by source idname. A source module
# (or an add-on) calls the two helpers below from its own register/unregister; the Stack Layers
# header and context menu read the active source's entries here.
_stack_source_menus = {}


def stack_source_menu_register(idname, add_menu=None, context_menu=None, header=None,
                               header_start=None):
    """Name the menus a registered stack source offers: its Add and its row context menu, and the
    optional `header(layout, context, space)` that draws the source's own header controls and
    `header_start(layout, context, space)`, drawn right after the Back button."""
    _stack_source_menus[idname] = {
        "add": add_menu,
        "context": context_menu,
        "header": header,
        "header_start": header_start,
    }


def stack_source_menu_unregister(idname):
    """Drop the menus a source registered; called from its unregister."""
    _stack_source_menus.pop(idname, None)


def _stack_source_menu_for(space, kind):
    # The menu idname the active source registered for kind, or None. A source this session
    # has none of (its add-on was disabled) reads as an unregistered one and offers nothing.
    menus = _stack_source_menus.get(space.stack_source_idname)
    if menus is not None:
        return menus.get(kind)
    return None


def _stack_source_header_for(space, kind="header"):
    # The draw callable the active source registered for its own header controls (`kind` is
    # "header" or "header_start"), or None.
    menus = _stack_source_menus.get(space.stack_source_idname)
    if menus is not None:
        return menus.get(kind)
    return None


def _stack_glyph_args(glyph, fallback_icon):
    # `icon_value` of a glyph where the font provides one, the built-in icon otherwise (background
    # mode), as keyword arguments for a layout call.
    return {"icon_value": glyph} if glyph else {"icon": fallback_icon}


def _stack_add_button(row, glyph, fallback_icon, kind, label, *, text=""):
    props = row.operator("outliner.stack_layer_add", text=text, **_stack_glyph_args(glyph, fallback_icon))
    props.type = kind
    props.label = label


class OUTLINER_HT_tool_header(Header):
    bl_space_type = 'OUTLINER'
    bl_region_type = 'TOOL_HEADER'

    @classmethod
    def poll(cls, context):
        return context.space_data.display_mode == 'STACK_LAYERS'

    def draw(self, context):
        layout = self.layout
        space = context.space_data

        if space.stack_layers_view != 'STACK':
            props = layout.operator("wm.context_set_enum", text="Outliner", icon='OUTLINER')
            props.data_path = "space_data.display_mode"
            props.value = 'VIEW_LAYER'

            # Which source's stacks the object list is drawn from. It belongs on this row, next to
            # the other Stack Layers controls, pushed to the far right edge.
            layout.separator_spacer()
            layout.prop(space, "stack_source", text="")
            return

        layout.operator("outliner.stack_layers_back", text="", icon='BACK')

        # A registered source's controls that belong next to the Back button (a source's mode
        # toggles, say), before the generic ones.
        header_start = _stack_source_header_for(space, "header_start")
        if header_start is not None:
            header_start(layout, context, space)
        if space.stack_source == 'PAINT_MATERIAL':
            # Which channel's value the rows below show and edit. Width is capped so the dropdown
            # does not stretch to the widest item and crowd the Add buttons.
            sub = layout.row()
            sub.ui_units_x = 4.0
            sub.prop(context.tool_settings.paint_mode, "stack_layer_channel", text="")

        layout.separator_spacer()

        if space.stack_source == 'PAINT_MATERIAL':
            # The Add verb is built from the source's own kinds; the paint stack offers Paint, Fill
            # and a Correction, so a quick-access button each stands in for the menu (the source's
            # kind list is not exposed through RNA yet, so the buttons are written out here).
            row = layout.row(align=True)
            # The brush glyph, shared with the Paint rows and the Add menu; the built-in fallback is
            # the same icon those name, so a background-mode button reads as they do.
            _stack_add_button(row, bpy.app.icons.glyph(GLYPH_BRUSH), 'BRUSH_DATA',
                              'PAINT', "Add Paint Layer")
            # The Fill glyph (Material Symbols "colors"), the one the Fill rows and the Add menu
            # use; the built-in icon stands in where no glyph icon exists (background mode).
            _stack_add_button(row, bpy.app.icons.glyph(_GLYPH_FILL), 'GP_DRAW_FILL',
                              'FILL', "Add Fill Layer")
            # A layer that shows what another material shows; the Add asks which one.
            props = row.operator("outliner.stack_layer_add", text="", icon='MATERIAL')
            props.type = 'MATERIAL'
            props.label = "Add Material Layer"
            # The procedural layers (a Gradient, and the patterns of the generator registry) are
            # not needed every time, so one dropdown holds them instead of a button each.
            row.menu("OUTLINER_MT_stack_layer_procedural_add", text="", icon='PLUS', use_dropdown=True)
            # Corrections hang on the selected layer or on its mask; one menu holds them all.
            row = layout.row(align=True)
            # The corrections, with the newer ones in submenus. A `menu` rather than an enum button,
            # because an enum button cannot hold a submenu; `use_dropdown` gives it the arrow the
            # enum buttons have.
            row.menu("OUTLINER_MT_stack_layer_effect_add", text="", icon='SHADERFX', use_dropdown=True)

            # Only the add is here, as a menu of the starting masks; switching a mask off and
            # taking it away live in the row's context menu, while its slot is the one showing.
            # The shared menu rather than an enum of one operator: it carries the image mask too,
            # whose entry opens the ID Browser instead of running an add.
            row = layout.row(align=True)
            # An enum menu button, like the corrections above: a header only widens the button
            # enough for the dropdown arrow on that path, and a plain `layout.menu` stays square.
            row.operator_menu_enum("outliner.stack_layer_mask_add", "type", text="",
                                   icon='MOD_MASK')

            # An empty folder to put layers into; layers join one by dropping them onto it.
            layout.separator()
            row = layout.row(align=True)
            row.operator("outliner.stack_layer_group_add", text="", icon='NEWFOLDER')
        else:
            # A registered source draws its own header controls when it has them (Add, REC, Solo
            # and the operations menu, for one that has those verbs). A source without a header
            # keeps the generic Add button it has always had.
            header = _stack_source_header_for(space)
            if header is not None:
                header(layout, context, space)
            else:
                add_menu = _stack_source_menu_for(space, "add")
                if add_menu is not None:
                    layout.menu(add_menu, text="", icon='ADD')

        if space.show_stack_layer_move_buttons:
            layout.separator()
            row = layout.row(align=True)
            row.operator("outliner.stack_layer_move", text="", icon='TRIA_UP').direction = 'UP'
            row.operator("outliner.stack_layer_move", text="", icon='TRIA_DOWN').direction = 'DOWN'

        layout.separator()

        row = layout.row(align=True)
        row.operator("outliner.stack_layer_remove", text="", icon='TRASH')


class OUTLINER_MT_stack_layer_procedural_add(Menu):
    bl_label = "Add Procedural Layer"

    def draw(self, _context):
        layout = self.layout
        # A procedural ramp built from shader nodes: a layer of its own, not an effect of the
        # selected row. Its glyph is also what the row's preview slot shows.
        _stack_add_button(layout, bpy.app.icons.glyph(_GLYPH_GRADIENT), 'COLORSET_13_VEC',
                          'GRADIENT', "Add Gradient Layer", text="Gradient Layer")
        layout.menu("OUTLINER_MT_stack_layer_pattern_add",
                    **_stack_glyph_args(bpy.app.icons.glyph(_GLYPH_PATTERN), 'TEXTURE'))


class OUTLINER_MT_stack_layer_pattern_add(Menu):
    bl_label = "Pattern Layer"

    def draw(self, _context):
        layout = self.layout
        pattern_icon = _stack_glyph_args(bpy.app.icons.glyph(_GLYPH_PATTERN), 'TEXTURE')
        # The generators are the registry's: the RNA enum is held to it by a test, and each
        # identifier is also the `type` of the Add kind that lays that pattern as a layer.
        generators = bpy.types.MaterialPaintLayer.bl_rna.properties["generator"].enum_items
        for item in generators:
            if item.identifier == 'GRADIENT':
                # Has its own entry in the procedural menu.
                continue
            props = layout.operator("outliner.stack_layer_add", text=item.name, **pattern_icon)
            props.type = item.identifier
            props.label = "Add {:s} Layer".format(item.name)


class OUTLINER_MT_stack_layer_effect_add(Menu):
    bl_label = "Add Effect"

    def draw(self, _context):
        layout = self.layout
        # What is not a filter, a pattern or an Anchor Point stays in the list; each group below is
        # a submenu (or the closing entries) of an operator of its own, so the lists never repeat
        # an entry.
        layout.operator_enum("outliner.stack_layer_effect_add", "type")
        layout.separator()
        layout.menu("OUTLINER_MT_stack_layer_effect_add_correction", text="Correction", icon='MODIFIER')
        layout.operator_menu_enum(
            "outliner.stack_layer_effect_add_new", "type", text="Filter", icon='SOLO_ON')
        # The generator registry's patterns (Noise, Voronoi, ...); the Gradient stays in the list
        # above, and the pattern glyph keeps the two apart.
        # A menu of its own rather than an enum button: `operator_menu_enum` takes no glyph value.
        layout.menu("OUTLINER_MT_stack_layer_effect_add_pattern", text="Generate Patterns",
                    **_stack_glyph_args(bpy.app.icons.glyph(_GLYPH_PATTERN), 'TEXTURE'))
        # The Anchor Point and its reader close the menu, after every submenu. The reader is off
        # while the material has no point to read; a click opens the ID Browser over the points.
        layout.separator()
        layout.operator_enum("outliner.stack_layer_effect_add_anchor", "type")
        layout.operator("outliner.stack_layer_effect_add_anchor_reader", text="Anchor Point Reader",
                        **_stack_glyph_args(bpy.app.icons.glyph(_GLYPH_ANCHOR), 'EMPTY_AXIS'))


class OUTLINER_MT_stack_layer_effect_add_correction(Menu):
    bl_label = "Correction"

    def draw(self, _context):
        self.layout.operator_enum("outliner.stack_layer_effect_add_correction", "type")


class OUTLINER_MT_stack_layer_effect_add_pattern(Menu):
    bl_label = "Generate Patterns"

    def draw(self, _context):
        self.layout.operator_enum("outliner.stack_layer_effect_add_pattern", "type")


# The map-driven mask filters: they only exist in a mask, so the Add Mask menus offer them instead of
# the Add Effect menu. `(type, label, kind identifier of stack_layer_add, icon, tooltip)`.
_STACK_MASK_FILTER_ENTRIES = (
    ('COLOR_ID', "Mask Color ID", 'MASK_FILTER_COLOR_ID', 'COLOR',
     "Keep the active layer's mask only where an ID Map matches the listed colors"),
    ('UV_ISLAND', "Mask UV Island", 'MASK_FILTER_UV_ISLAND', 'UV_ISLANDSEL',
     "Keep the active layer's mask only on the UV islands captured from the Image Editor"),
)


class OUTLINER_MT_stack_layer_mask_add(Menu):
    bl_label = "Add Mask"

    def draw(self, _context):
        layout = self.layout
        # `add` is set on every button rather than left to the operator's default: the Remove Mask
        # entry stores `add = False` as the operator's last-used value, and an unset property would
        # come back as that, so "Add Mask" would run a remove the next time it was picked.
        for text, color, icon in (
                ("Add Black Mask", 'BLACK', 'MOD_MASK'),
                ("Add White Mask", 'WHITE', 'CLIPUV_HLT')):
            props = layout.operator("outliner.stack_layer_mask", text=text, icon=icon)
            props.add = True
            props.initial_color = color
        # A mask filled by an image: the entry opens the ID Browser, and picking one there makes
        # the mask (see Material.paint_layer_add_image_mask). Nothing is created before a pick.
        layout.operator(
            "outliner.stack_layer_mask_image", text="Add Image Mask", icon='IMAGE_DATA')
        layout.separator()
        for identifier, label, kind, icon, _tip in _STACK_MASK_FILTER_ENTRIES:
            props = layout.operator("outliner.stack_layer_add", text=label, icon=icon)
            props.type = kind
            props.label = label


class OUTLINER_OT_stack_layer_mask_add(bpy.types.Operator):
    """Add a mask to the active layer"""
    bl_idname = "outliner.stack_layer_mask_add"
    bl_label = "Add Mask"
    # The nested operators run without their own undo step, so this one carries it. The image
    # entry only opens the ID Browser and finishes nothing, so it records none.
    bl_options = {'UNDO', 'INTERNAL'}

    type: bpy.props.EnumProperty(
        name="Mask",
        items=(
            ('BLACK', "Add Black Mask", "Add a mask that hides the layer until it is painted in",
             'MOD_MASK', 0),
            ('WHITE', "Add White Mask", "Add a mask that shows the layer everywhere",
             'CLIPUV_HLT', 1),
            ('IMAGE', "Add Image Mask", "Give the layer a mask that reads an image",
             'IMAGE_DATA', 2),
            *((identifier, label, tip, icon, 3 + index)
              for index, (identifier, label, _kind, icon, tip) in enumerate(_STACK_MASK_FILTER_ENTRIES)),
        ),
        # Never remembered: the entry picked last time must not be what a script gets by default.
        options={'SKIP_SAVE'},
    )

    @classmethod
    def poll(cls, _context):
        return bpy.ops.outliner.stack_layer_mask.poll()

    def execute(self, _context):
        if self.type == 'IMAGE':
            # Opens the ID Browser; the pick makes the mask (see Material.paint_layer_add_image_mask).
            bpy.ops.outliner.stack_layer_mask_image('INVOKE_DEFAULT')
            return {'CANCELLED'}
        for identifier, label, kind, _icon, _tip in _STACK_MASK_FILTER_ENTRIES:
            if self.type == identifier:
                # A filter of the mask: the Add makes the base mask first when the row has none.
                return bpy.ops.outliner.stack_layer_add(type=kind, label=label)
        # `add` is always passed: see OUTLINER_MT_stack_layer_mask_add.
        return bpy.ops.outliner.stack_layer_mask(add=True, initial_color=self.type)


class OUTLINER_MT_stack_layer_advanced(Menu):
    bl_label = "Advanced"

    def draw(self, _context):
        self.layout.operator(
            "outliner.stack_layer_use_as_brush_source",
            text="Use as Brush Source",
            icon='BRUSH_DATA',
        )


class OUTLINER_HT_header(Header):
    bl_space_type = 'OUTLINER'

    def draw(self, context):
        layout = self.layout

        space = context.space_data
        display_mode = space.display_mode
        scene = context.scene
        ks = context.scene.keying_sets.active

        layout.template_header()

        layout.prop(space, "display_mode", icon_only=True)

        if display_mode == 'DATA_API':
            OUTLINER_MT_editor_menus.draw_collapsible(context, layout)
        if display_mode == 'LIBRARY_OVERRIDES':
            layout.prop(space, "lib_override_view_mode", text="")

        layout.separator_spacer()

        filter_text_supported = True
        # No text filtering for library override hierarchies. The tree is lazy built to avoid
        # performance issues in complex files.
        if (display_mode == 'LIBRARY_OVERRIDES' and
                space.lib_override_view_mode == 'HIERARCHIES') or (
                display_mode == 'STACK_LAYERS' and space.stack_layers_view == 'STACK'):
            filter_text_supported = False

        if filter_text_supported:
            row = layout.row(align=True)
            row.prop(space, "filter_text", icon='VIEWZOOM', text="")
        elif display_mode == 'STACK_LAYERS' and space.stack_layers_view == 'STACK':
            # The material whose stack is shown, before Pin; the fixed width keeps the header
            # stable whatever the name is.
            # A source that has no sub-selection of its own (its stack belongs to the object)
            # answers so through its use_sub_selection flag: only Pin is offered for it.
            if space.stack_source_use_sub_selection:
                layout.template_enum_menu(space, "stack_focus_sub_index", width=6.0, wrap=False)
            layout.prop(
                space, "use_stack_layer_pin", text="",
                icon='PINNED' if space.use_stack_layer_pin else 'UNPINNED',
            )

        layout.separator_spacer()

        if display_mode == 'SEQUENCE':
            row = layout.row(align=True)
            row.prop(space, "use_sync_select", icon='UV_SYNC_SELECT', text="")

        row = layout.row(align=True)
        if display_mode == 'STACK_LAYERS':
            row.popover(
                panel="OUTLINER_PT_stack_layers_filter",
                text="",
            )
        elif display_mode in {'SCENES', 'VIEW_LAYER', 'LIBRARY_OVERRIDES'}:
            row.popover(
                panel="OUTLINER_PT_filter",
                text="",
            )

        if display_mode in {'LIBRARIES', 'ORPHAN_DATA'}:
            row.prop(space, "use_filter_id_type", text="", icon=(
                'FILTER_FILLED' if space.use_filter_id_type else 'FILTER'))
            sub = row.row(align=True)
            sub.active = space.use_filter_id_type
            sub.prop(space, "filter_id_type", text="", icon_only=True)

        if display_mode == 'VIEW_LAYER':
            layout.operator("outliner.collection_new", text="", icon='COLLECTION_NEW').nested = True

        elif display_mode == 'ORPHAN_DATA':
            layout.operator("outliner.orphans_purge", text="Purge")

        elif space.display_mode == 'DATA_API':
            layout.separator()

            row = layout.row(align=True)
            row.operator("outliner.keyingset_add_selected", icon='ADD', text="")
            row.operator("outliner.keyingset_remove_selected", icon='REMOVE', text="")

            if ks:
                row = layout.row()
                row.prop_search(scene.keying_sets, "active", scene, "keying_sets", text="")

                row = layout.row(align=True)
                row.operator("anim.keyframe_insert", text="", icon='KEY_HLT')
                row.operator("anim.keyframe_delete", text="", icon='KEY_DEHLT')
            else:
                row = layout.row()
                row.label(text="No Keying Set Active")


class OUTLINER_MT_editor_menus(Menu):
    bl_idname = "OUTLINER_MT_editor_menus"
    bl_label = ""

    def draw(self, context):
        layout = self.layout
        space = context.space_data

        if space.display_mode == 'DATA_API':
            layout.menu("OUTLINER_MT_edit_datablocks")


class OUTLINER_MT_smart_asset_save(Menu):
    bl_label = "Save as Smart Asset"

    def draw(self, _context):
        layout = self.layout
        for asset_type, text, icon in (
                ('MATERIAL', "Smart Material", 'ASSET_MANAGER'),
                ('LAYER', "Smart Layer", 'LAYER_ACTIVE'),
                ('MASK', "Smart Mask", 'MOD_MASK'),
        ):
            props = layout.operator(
                "outliner.stack_layer_smart_asset_save", text=text, icon=icon)
            props.type = asset_type


def _stack_layer_map_encoding_items_draw(layout, channel):
    """The encodings the channel takes, as the map-encoding operator's items. Which pair a channel
    takes is the BKE setter's to say -- ``BKE_paint_layers_channel_map_encoding_allowed`` (see
    ``BKE_paint_layers.hh``) is the one place the matrix is written, and the C++ menus filter
    through it -- so this list names the pairs and leaves the refusal to the operator's exec."""
    items = (
        ('CHANNEL', "Roughness"),
        ('GLOSSINESS', "Glossiness"),
        ('GLOSSINESS_ALPHA', "Glossiness (Alpha)"),
    ) if channel == 'ROUGHNESS' else (
        ('CHANNEL', "Level"),
        ('F0', "Specular Colour (F0)"),
        ('F0_GLOSSINESS_ALPHA', "Specular + Glossiness (Alpha)"),
    )
    for encoding, text in items:
        props = layout.operator(
            "outliner.stack_layer_channel_map_encoding", text=text)
        props.channel = channel
        props.encoding = encoding


class OUTLINER_MT_stack_layer_roughness_encoding(Menu):
    bl_label = "Set Roughness Encoding"

    def draw(self, _context):
        _stack_layer_map_encoding_items_draw(self.layout, 'ROUGHNESS')


class OUTLINER_MT_stack_layer_specular_encoding(Menu):
    bl_label = "Set Specular Encoding"

    def draw(self, _context):
        _stack_layer_map_encoding_items_draw(self.layout, 'SPECULAR')


class OUTLINER_MT_context_menu(Menu):
    bl_label = "Outliner"

    @staticmethod
    def draw_common_operators(space, layout):
        # Mark/clear asset options does not belongs in certain outliner views.
        if space.display_mode not in {'SEQUENCE', 'LIBRARY_OVERRIDES'}:
            layout.menu_contents("OUTLINER_MT_asset")

            layout.separator()

        if space.display_mode in {'LIBRARY_OVERRIDES'}:
            layout.operator(
                "outliner.liboverride_property_remove",
                text="Remove",
            )

            layout.separator()

        layout.menu("OUTLINER_MT_liboverride", icon='LIBRARY_DATA_OVERRIDE')

        layout.separator()

        layout.menu("OUTLINER_MT_context_menu_view")

        layout.separator()

        layout.menu("INFO_MT_area")

    def draw(self, context):
        space = context.space_data

        layout = self.layout

        if space.display_mode == 'STACK_LAYERS':
            # A stack row is not a data-block, so none of the common entries (assets, overrides,
            # ID management) apply to it.
            if (space.stack_layers_view == 'STACK' and
                    space.stack_source == 'PAINT_MATERIAL'):
                layout.menu("OUTLINER_MT_stack_layer_mask_add", icon='MOD_MASK')
                # The same contextual grouped menu the header draws: one line per thing, resolved
                # to the active target. Keeping it here as well would be a second vocabulary.
                layout.menu("OUTLINER_MT_stack_layer_effect_add",
                            text="Add Effect", icon='SHADERFX')
                if bpy.ops.outliner.stack_layer_use_as_brush_source.poll():
                    layout.menu("OUTLINER_MT_stack_layer_advanced")
                # Switching the mask off and taking it away act on the mask being worked on: the
                # toggle's poll answers whether the row's mask slot is the one showing.
                if bpy.ops.outliner.stack_layer_mask_toggle.poll():
                    layout.operator(
                        "outliner.stack_layer_mask_toggle", text="Toggle Mask", icon='HIDE_OFF')
                    props = layout.operator(
                        "outliner.stack_layer_mask", text="Remove Mask", icon='X')
                    props.add = False
                # Activating a row aims the brush at it; this is the way back to no target.
                if bpy.ops.outliner.stack_layer_clear_target.poll():
                    layout.operator("outliner.stack_layer_clear_target", icon='RESTRICT_SELECT_ON')
                layout.separator()
                # Copy and paste act on the active row, whatever it is: a layer, a fill or a
                # correction. The mask has verbs of its own, listed while its slot is the one showing
                # (copy) or once a mask has been copied (paste).
                layout.operator("outliner.stack_layer_copy", text="Copy", icon='COPYDOWN')
                layout.operator("outliner.stack_layer_paste", text="Paste", icon='PASTEDOWN')
                if bpy.ops.outliner.stack_layer_mask_copy.poll():
                    layout.operator(
                        "outliner.stack_layer_mask_copy", text="Copy Mask", icon='COPYDOWN')
                if bpy.ops.outliner.stack_layer_mask_paste.poll():
                    layout.operator(
                        "outliner.stack_layer_mask_paste", text="Paste Mask", icon='PASTEDOWN')
                # Carry a slot's assigned data-block to another slot: the channels of the row (an
                # image) or a Material row's source. The clipboard is the paint slots' own, so a
                # copy from a panel paste onto a row and the other way around both work.
                if bpy.ops.outliner.stack_layer_image_copy.poll():
                    layout.operator_menu_enum(
                        "outliner.stack_layer_image_copy",
                        "channel",
                        text="Copy Image",
                        icon='COPYDOWN',
                    )
                if bpy.ops.outliner.stack_layer_image_paste.poll():
                    layout.operator_menu_enum(
                        "outliner.stack_layer_image_paste",
                        "channel",
                        text="Paste Image",
                        icon='PASTEDOWN',
                    )
                # A specular map is a colour (or glossiness) where the Specular slot reads a
                # scalar: the converter makes the maps the slots read, so the poll only lists it
                # for a row whose Specular slot carries a map.
                if bpy.ops.outliner.stack_layer_specular_convert.poll():
                    layout.operator_menu_enum(
                        "outliner.stack_layer_specular_convert",
                        "kind",
                        text="Convert Specular Map",
                        icon='INDIRECT_ONLY_ON',
                    )
                # The no-bake way round: the map keeps its pixels and the slot reads it
                # differently. The quick toggle covers the common case (a glossiness map dropped
                # on Roughness); the submenus carry every pair the channel takes.
                if bpy.ops.outliner.stack_layer_channel_map_encoding.poll():
                    props = layout.operator(
                        "outliner.stack_layer_channel_map_encoding",
                        text="Roughness: Read as Glossiness",
                        icon='MOD_INVERT',
                    )
                    props.channel = 'ROUGHNESS'
                    props.toggle = True
                    layout.menu(
                        "OUTLINER_MT_stack_layer_roughness_encoding",
                        icon='MOD_INVERT',
                    )
                    layout.menu(
                        "OUTLINER_MT_stack_layer_specular_encoding",
                        icon='SHADERFX',
                    )
                if bpy.ops.outliner.stack_layer_material_copy.poll():
                    layout.operator(
                        "outliner.stack_layer_material_copy",
                        text="Copy Material",
                        icon='COPYDOWN',
                    )
                if bpy.ops.outliner.stack_layer_material_paste.poll():
                    layout.operator(
                        "outliner.stack_layer_material_paste",
                        text="Paste Material",
                        icon='PASTEDOWN',
                    )
                layout.separator()
                # The verbs below bake and cannot be undone; each poll answers for the rows the
                # menu would act on, so only the one that fits is listed.
                if bpy.ops.paint.mask_merge.poll():
                    layout.operator("paint.mask_merge", text="Merge Mask Stack", icon='MOD_MASK')
                if bpy.ops.outliner.stack_layer_corrections_merge.poll():
                    layout.operator(
                        "outliner.stack_layer_corrections_merge", text="Merge Corrections", icon='AUTOMERGE_ON')
                if bpy.ops.outliner.stack_layer_correction_to_texture.poll():
                    layout.operator(
                        "outliner.stack_layer_correction_to_texture", text="Convert to Texture", icon='TEXTURE')
                layout.separator()
                layout.operator(
                    "outliner.stack_layer_group_add", text="New Group", icon='NEWFOLDER')
                # Only a folder can be unwrapped; the operator's poll answers for the row.
                if bpy.ops.outliner.stack_layer_ungroup.poll():
                    layout.operator("outliner.stack_layer_ungroup", text="Ungroup")
                layout.separator()
                # The operator's poll is the same for every save type: it answers for the owner
                # the menu acts on, not for what is selected inside it.
                if bpy.ops.outliner.stack_layer_smart_asset_save.poll():
                    layout.menu(
                        "OUTLINER_MT_smart_asset_save",
                        text="Save as Smart Asset",
                        icon='ASSET_MANAGER',
                    )
                    layout.separator()
            elif space.stack_layers_view == 'STACK':
                # A registered source's own operations, under whatever menu it registered.
                context_menu = _stack_source_menu_for(space, "context")
                if context_menu is not None:
                    # The entries themselves, not a submenu: they are the source's own verbs.
                    layout.menu_contents(context_menu)
                    layout.separator()
            # A tag belongs to a group, not to a particular source: the poll answers for the row
            # the operator would tag, so a source that grows groups gets the swatches too.
            if bpy.ops.outliner.stack_layer_color_tag_set.poll():
                row = layout.row(align=True)
                row.operator_enum(
                    "outliner.stack_layer_color_tag_set", "color", icon_only=True)
                layout.separator()
            layout.menu("OUTLINER_MT_context_menu_view")
            layout.separator()
            layout.menu("INFO_MT_area")
            return

        if space.display_mode == 'VIEW_LAYER':
            OUTLINER_MT_collection_new.draw_without_context_menu(context, layout)
            layout.separator()

        OUTLINER_MT_context_menu.draw_common_operators(space, layout)


class OUTLINER_MT_context_menu_view(Menu):
    bl_label = "View"

    def draw(self, _context):
        layout = self.layout

        layout.operator("outliner.show_active")

        layout.separator()

        layout.operator("view2d.zoom_in")
        layout.operator("view2d.zoom_out")
        layout.operator("view2d.reset", text="Reset Zoom")

        layout.separator()

        layout.operator("outliner.expanded_toggle")
        layout.operator("outliner.show_hierarchy", text="Show Object Hierarchy")
        layout.operator("outliner.show_one_level", text="Show One Level")
        layout.operator("outliner.show_one_level", text="Hide One Level").open = False


class OUTLINER_MT_view_pie(Menu):
    bl_label = "View"

    def draw(self, _context):
        layout = self.layout

        pie = layout.menu_pie()
        pie.operator("outliner.show_hierarchy")
        pie.operator("outliner.show_active", icon='ZOOM_SELECTED')


class OUTLINER_MT_id_data(Menu):
    bl_label = "ID Data"

    @classmethod
    def poll(cls, context):
        return has_selected_ids_in_context(context)

    def draw(self, context):
        layout = self.layout

        layout.operator_enum("outliner.id_operation", "type")

        id_linked = getattr(context, "id", None)
        if id_linked and id_linked.library:
            layout.separator()
            layout.operator("outliner.id_linked_relocate", text="Relocate")


class OUTLINER_MT_edit_datablocks(Menu):
    bl_label = "Edit"

    def draw(self, _context):
        layout = self.layout

        layout.operator("outliner.keyingset_add_selected")
        layout.operator("outliner.keyingset_remove_selected")

        layout.separator()

        layout.operator("outliner.drivers_add_selected")
        layout.operator("outliner.drivers_delete_selected")


class OUTLINER_MT_collection_view_layer(Menu):
    bl_label = "View Layer"

    def draw(self, context):
        layout = self.layout

        layout.operator("outliner.collection_exclude_set")
        layout.operator("outliner.collection_exclude_clear")

        layout.operator("outliner.collection_holdout_set")
        layout.operator("outliner.collection_holdout_clear")

        if context.engine == 'CYCLES':
            layout.operator("outliner.collection_indirect_only_set")
            layout.operator("outliner.collection_indirect_only_clear")


class OUTLINER_MT_collection_visibility(Menu):
    bl_label = "Visibility"

    def draw(self, _context):
        layout = self.layout

        layout.operator("outliner.collection_isolate", text="Isolate")

        layout.separator()

        layout.operator("outliner.collection_show", text="Show", icon='HIDE_OFF')
        layout.operator("outliner.collection_show_inside", text="Show All Inside")
        layout.operator("outliner.collection_hide", text="Hide", icon='HIDE_ON')
        layout.operator("outliner.collection_hide_inside", text="Hide All Inside")

        layout.separator()

        layout.operator("outliner.collection_enable", text="Enable in Viewports", icon='RESTRICT_VIEW_OFF')
        layout.operator("outliner.collection_disable", text="Disable in Viewports")

        layout.separator()

        layout.operator("outliner.collection_enable_render", text="Enable in Render", icon='RESTRICT_RENDER_OFF')
        layout.operator("outliner.collection_disable_render", text="Disable in Render")


class OUTLINER_MT_collection(Menu):
    bl_label = "Collection"

    def draw(self, context):
        layout = self.layout

        space = context.space_data

        layout.operator("outliner.collection_new", text="New", text_ctxt=i18n_contexts.id_collection).nested = True
        layout.operator("outliner.collection_duplicate", text="Duplicate Collection")
        layout.operator("outliner.collection_duplicate_linked", text="Duplicate Linked")
        layout.operator("outliner.id_copy", text="Copy", icon='COPYDOWN')
        layout.operator("outliner.id_paste", text="Paste", icon='PASTEDOWN')

        layout.separator()

        layout.operator("outliner.delete", text="Delete", icon='X')
        layout.operator("outliner.delete", text="Delete Hierarchy").hierarchy = True

        layout.separator()

        layout.operator("outliner.collection_objects_select", text="Select Objects", icon='RESTRICT_SELECT_OFF')
        layout.operator("outliner.collection_objects_deselect", text="Deselect Objects")

        layout.separator()

        layout.operator("outliner.collection_instance", text="Instance to Scene")

        if space.display_mode != 'VIEW_LAYER':
            layout.operator("outliner.collection_link", text="Link to Scene")
        layout.operator("outliner.id_operation", text="Unlink").type = 'UNLINK'

        layout.separator()

        layout.menu("OUTLINER_MT_collection_visibility")

        if space.display_mode == 'VIEW_LAYER':
            layout.separator()
            layout.menu("OUTLINER_MT_collection_view_layer", icon='RENDERLAYERS')

        layout.separator()
        row = layout.row(align=True)
        row.operator_enum("outliner.collection_color_tag_set", "color", icon_only=True)

        layout.separator()

        layout.menu("OUTLINER_MT_id_data")

        layout.separator()

        OUTLINER_MT_context_menu.draw_common_operators(space, layout)


class OUTLINER_MT_collection_new(Menu):
    bl_label = "Collection"

    @staticmethod
    def draw_without_context_menu(_context, layout):
        layout.operator("outliner.collection_new", text="New Collection").nested = True
        layout.operator("outliner.id_paste", text="Paste Data-Blocks", icon='PASTEDOWN')

    def draw(self, context):
        layout = self.layout

        self.draw_without_context_menu(context, layout)

        layout.separator()

        OUTLINER_MT_context_menu.draw_common_operators(context.space_data, layout)


class OUTLINER_MT_object(Menu):
    bl_label = "Object"

    def draw(self, context):
        layout = self.layout

        space = context.space_data

        layout.operator("outliner.id_copy", text="Copy", icon='COPYDOWN')
        layout.operator("outliner.id_paste", text="Paste", icon='PASTEDOWN')

        layout.separator()

        layout.operator("outliner.delete", text="Delete", icon='X')
        layout.operator("outliner.delete", text="Delete Hierarchy").hierarchy = True

        layout.separator()

        layout.operator("outliner.object_operation", text="Select", icon='RESTRICT_SELECT_OFF').type = 'SELECT'
        layout.operator("outliner.object_operation", text="Select Hierarchy").type = 'SELECT_HIERARCHY'
        layout.operator("outliner.object_operation", text="Deselect").type = 'DESELECT'

        layout.separator()

        if not (space.display_mode == 'VIEW_LAYER' and not space.use_filter_collection):
            layout.operator("outliner.id_operation", text="Unlink").type = 'UNLINK'
            layout.separator()

        layout.operator("outliner.collection_new", text="New Collection").nested = True

        layout.separator()

        layout.menu("OUTLINER_MT_id_data")

        layout.separator()

        OUTLINER_MT_context_menu.draw_common_operators(space, layout)


class OUTLINER_MT_asset(Menu):
    bl_label = "Assets"

    @classmethod
    def poll(cls, context):
        return has_selected_ids_in_context(context)

    def draw(self, _context):
        layout = self.layout

        layout.operator("asset.mark", icon='ASSET_MANAGER')
        layout.operator("asset.clear", text="Clear Asset").set_fake_user = False
        layout.operator("asset.clear", text="Clear Asset (Set Fake User)").set_fake_user = True


class OUTLINER_MT_liboverride(Menu):
    bl_label = "Library Override"

    @classmethod
    def poll(cls, context):
        return has_selected_ids_in_context(context)

    def draw(self, _context):
        layout = self.layout

        layout.operator_menu_enum(
            "outliner.liboverride_operation", "selection_set",
            text="Make",
        ).type = 'OVERRIDE_LIBRARY_CREATE_HIERARCHY'
        layout.operator_menu_enum(
            "outliner.liboverride_operation",
            "selection_set",
            text="Reset",
        ).type = 'OVERRIDE_LIBRARY_RESET'
        layout.operator_menu_enum(
            "outliner.liboverride_operation", "selection_set",
            text="Clear",
        ).type = 'OVERRIDE_LIBRARY_CLEAR_SINGLE'

        layout.separator()

        layout.operator_menu_enum(
            "outliner.liboverride_troubleshoot_operation", "type",
            text="Troubleshoot",
        ).selection_set = 'SELECTED'


class OUTLINER_PT_filter(Panel):
    bl_space_type = 'OUTLINER'
    bl_region_type = 'HEADER'
    bl_label = "Options"

    def draw(self, context):
        layout = self.layout

        space = context.space_data
        display_mode = space.display_mode

        if display_mode == 'VIEW_LAYER':
            layout.label(text="Restriction Toggles")
            row = layout.row(align=True)
            row.prop(space, "show_restrict_column_enable", text="")
            row.prop(space, "show_restrict_column_select", text="")
            row.prop(space, "show_restrict_column_hide", text="")
            row.prop(space, "show_restrict_column_viewport", text="")
            row.prop(space, "show_restrict_column_render", text="")
            row.prop(space, "show_restrict_column_holdout", text="")
            row.prop(space, "show_restrict_column_indirect_only", text="")
            layout.separator()
        elif display_mode == 'SCENES':
            layout.label(text="Restriction Toggles")
            row = layout.row(align=True)
            row.prop(space, "show_restrict_column_select", text="")
            row.prop(space, "show_restrict_column_hide", text="")
            row.prop(space, "show_restrict_column_viewport", text="")
            row.prop(space, "show_restrict_column_render", text="")
            layout.separator()

        if display_mode != 'DATA_API':
            col = layout.column(align=True)
            col.prop(space, "use_sort_alpha")

        if display_mode != 'LIBRARY_OVERRIDES':
            col = layout.column(align=True)
            row = col.row(align=True)
            row.prop(space, "use_sync_select", text="Sync Selection")
            row = col.row(align=True)
            row.active = space.use_sync_select
            row.prop(space, "scroll_to_active", text="Scroll to Active")

            row = layout.row(align=True)
            row.prop(space, "show_mode_column", text="Show Mode Column")
            layout.separator()

        if display_mode == 'LIBRARY_OVERRIDES' and space.lib_override_view_mode == 'PROPERTIES' and bpy.data.libraries:
            row = layout.row()
            row.label(icon='LIBRARY_DATA_OVERRIDE')
            row.prop(space, "use_filter_lib_override_system", text="System Overrides")


class OUTLINER_PT_stack_layers_filter(Panel):
    bl_space_type = 'OUTLINER'
    bl_region_type = 'HEADER'
    bl_label = "Stack Layers"

    @classmethod
    def poll(cls, context):
        return context.space_data.display_mode == 'STACK_LAYERS'

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        space = context.space_data

        col = layout.column(heading="Columns", align=True)
        col.prop(space, "show_stack_layer_value", text="Value")
        col.prop(space, "show_stack_layer_blend", text="Mode")
        col.prop(space, "show_stack_layer_lock", text="Lock")
        col.prop(space, "use_stack_layer_visibility_left", text="Visibility Left Side")

        col = layout.column(heading="Rows", align=True)
        col.prop(space, "show_stack_items", text="Contents")
        col.prop(space, "use_stack_layer_big_rows", text="Large")
        col.prop(space, "show_stack_layer_move_buttons", text="Move Buttons")

        layout.prop(space, "use_stack_layer_pin")
        # Pin Stack keeps a stack the active object does not have, which is the opposite answer to
        # following the object, so the choice below it only means anything with Pin off.
        col = layout.column()
        col.active = not space.use_stack_layer_pin
        col.prop(space, "use_stack_layer_follow_object")


class OUTLINER_PT_options_search(Panel):
    bl_space_type = 'OUTLINER'
    bl_region_type = 'HEADER'
    bl_label = "Search"
    bl_parent_id = "OUTLINER_PT_filter"

    @classmethod
    def poll(cls, context):
        st = context.space_data
        space = context.space_data
        display_mode = space.display_mode

        filter_text_supported = True
        # Same exception for library overrides as in OUTLINER_HT_header.
        if display_mode == 'LIBRARY_OVERRIDES' and space.lib_override_view_mode == 'HIERARCHIES':
            filter_text_supported = False

        return filter_text_supported

    def draw(self, context):
        layout = self.layout
        space = context.space_data
        display_mode = space.display_mode

        col = layout.column(align=True)
        col.prop(space, "use_filter_complete", text="Exact Match")
        col.prop(space, "use_filter_case_sensitive", text="Case Sensitive")


class OUTLINER_PT_options_filter(Panel):
    bl_space_type = 'OUTLINER'
    bl_region_type = 'HEADER'
    bl_label = "Filter"
    bl_parent_id = "OUTLINER_PT_filter"

    @classmethod
    def poll(cls, context):
        space = context.space_data
        display_mode = space.display_mode
        return display_mode == 'VIEW_LAYER'

    def draw(self, context):
        layout = self.layout
        space = context.space_data
        display_mode = space.display_mode
        col = layout.column(align=True)

        row = col.row()
        row.label(icon='RENDERLAYERS')
        row.prop(space, "use_filter_view_layers", text="All View Layers")

        row = col.row()
        row.label(icon='GROUP')
        row.prop(space, "use_filter_collection", text="Collections")

        row = col.row()
        row.label(icon='OBJECT_DATAMODE')
        row.prop(space, "use_filter_object", text="Objects")
        row = col.row(align=True)
        row.label(icon='BLANK1')
        row.prop(space, "filter_state", text="")
        sub = row.row(align=True)
        sub.enabled = space.filter_state != 'ALL'
        sub.prop(space, "filter_invert", text="", icon='ARROW_LEFTRIGHT')

        sub = col.column(align=True)
        sub.active = space.use_filter_object

        row = sub.row()
        row.label(icon='BLANK1')
        row.prop(space, "use_filter_object_content", text="Object Contents")
        row = sub.row()
        row.label(icon='BLANK1')
        row.prop(space, "use_filter_children", text="Object Children")

        if bpy.data.meshes:
            row = sub.row()
            row.label(icon='MESH_DATA')
            row.prop(space, "use_filter_object_mesh", text="Meshes")
        if bpy.data.armatures:
            row = sub.row()
            row.label(icon='ARMATURE_DATA')
            row.prop(space, "use_filter_object_armature", text="Armatures")
        if bpy.data.lights:
            row = sub.row()
            row.label(icon='LIGHT_DATA')
            row.prop(space, "use_filter_object_light", text="Lights")
        if bpy.data.cameras:
            row = sub.row()
            row.label(icon='CAMERA_DATA')
            row.prop(space, "use_filter_object_camera", text="Cameras")
        if bpy.data.grease_pencils:
            row = sub.row()
            row.label(icon='STROKE')
            row.prop(space, "use_filter_object_grease_pencil", text="Grease Pencil")
        row = sub.row()
        row.label(icon='EMPTY_DATA')
        row.prop(space, "use_filter_object_empty", text="Empties")

        if (
                bpy.data.curves or
                bpy.data.metaballs or
                (hasattr(bpy.data, "hair_curves") and bpy.data.hair_curves) or
                (hasattr(bpy.data, "pointclouds") and bpy.data.pointclouds) or
                bpy.data.volumes or
                bpy.data.lightprobes or
                bpy.data.lattices or
                bpy.data.fonts or
                bpy.data.speakers
        ):
            row = sub.row()
            row.label(icon='BLANK1')
            row.prop(space, "use_filter_object_others", text="Others")


classes = (
    OUTLINER_HT_header,
    OUTLINER_HT_tool_header,
    OUTLINER_MT_stack_layer_procedural_add,
    OUTLINER_MT_stack_layer_pattern_add,
    OUTLINER_MT_stack_layer_effect_add,
    OUTLINER_MT_stack_layer_effect_add_correction,
    OUTLINER_MT_stack_layer_effect_add_pattern,
    OUTLINER_MT_stack_layer_mask_add,
    OUTLINER_OT_stack_layer_mask_add,
    OUTLINER_MT_stack_layer_advanced,
    OUTLINER_MT_editor_menus,
    OUTLINER_MT_edit_datablocks,
    OUTLINER_MT_collection,
    OUTLINER_MT_collection_new,
    OUTLINER_MT_collection_visibility,
    OUTLINER_MT_collection_view_layer,
    OUTLINER_MT_id_data,
    OUTLINER_MT_object,
    OUTLINER_MT_asset,
    OUTLINER_MT_liboverride,
    OUTLINER_MT_smart_asset_save,
    OUTLINER_MT_context_menu,
    OUTLINER_MT_context_menu_view,
    OUTLINER_MT_view_pie,
    OUTLINER_PT_filter,
    OUTLINER_PT_stack_layers_filter,
    OUTLINER_PT_options_search,
    OUTLINER_PT_options_filter,
)

if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
