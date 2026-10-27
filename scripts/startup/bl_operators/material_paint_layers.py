# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""The thin operators behind the paint-layer RNA: the channel records. Stack edits (add, remove,
move) live in the RNA and the Outliner; UI-only code lives in bl_ui. These operate on
Material.paint_layers by marker."""

import os

import bpy
from bpy.types import Operator, PropertyGroup


def _context_material(context):
    """The material the operators act on.

    `context.material` only exists in the Properties editor's material contexts; from the
    Outliner or a viewport the owner is the active object's active material slot."""
    material = getattr(context, "material", None)
    if material is None:
        ob = getattr(context, "object", None)
        material = ob.active_material if ob is not None else None
    return material


def _paint_layers_active_layer(context):
    mat = _context_material(context)
    if mat is None or not mat.is_layered:
        return None
    return mat.paint_layers.active


class _PaintLayerOperator(Operator):
    """Base for the thin layer operators: undo, a report and the RNA notifier, and the row they
    act on taken from the operator or the material's active layer (a marker lookup).

    The row is addressed by its UUID marker as a string, not a PointerProperty: #MaterialPaintLayer
    is a plain (non-ID) RNA struct, and Python property types only accept IDs or property groups."""

    bl_options = {'UNDO', 'REGISTER'}

    marker: bpy.props.StringProperty(
        name="Layer",
        description="UUID marker of the row to act on; the active layer when unset",
        options={'SKIP_SAVE'},
    )

    def _layer(self, context):
        if self.marker:
            mat = _context_material(context)
            # A marker that names no row must not fall back to the active layer: the caller asked
            # for a specific row, and acting on another one would edit the wrong layer.
            return mat.paint_layers.find(self.marker) if mat is not None else None
        return _paint_layers_active_layer(context)

    @classmethod
    def poll(cls, context):
        mat = _context_material(context)
        return mat is not None and mat.is_layered


# Dynamic enum items must stay referenced from Python: Blender keeps raw pointers to the strings of
# the last returned list, so a list rebuilt on every call leaves the UI reading freed memory.
_ENUM_ITEMS_CACHE = {}


def _rna_enum_items_cached(struct, prop, keep=None):
    """The (identifier, name, "") items of an RNA enum, built once and kept alive."""
    key = (struct, prop, tuple(keep) if keep is not None else None)
    items = _ENUM_ITEMS_CACHE.get(key)
    if items is None:
        items = [
            (item.identifier, item.name, "")
            for item in getattr(bpy.types, struct).bl_rna.properties[prop].enum_items
            if item.identifier and (keep is None or item.identifier in keep)
        ]
        _ENUM_ITEMS_CACHE[key] = items
    return items


def _paint_channel_enum_items():
    """Every paint channel as an enum item tuple, the shared channel vocabulary.

    The identifiers match #Material.paint_layers_channels and a layer record's channel, so the same
    strings address the global set and a layer record.
    """
    return _rna_enum_items_cached("MaterialPaintLayerChannel", "channel")


def _paint_layer_addable_channel_items(context):
    """The channels the Add Channel menu offers for the active layer.

    Only channels of the material's global set that the layer carries no record for: a channel
    outside the set can never be added through the UI, and one already on the row has nothing to
    add. Falls back to the full list when no layered material/layer is in context, so the menu is
    never empty-by-accident in a scripted call.
    """
    mat = _context_material(context)
    layer = mat.paint_layers.active if (mat is not None and mat.is_layered) else None
    if layer is None:
        return _paint_channel_enum_items()
    existing = {record.channel for record in layer.channels}
    return [
        item for item in _paint_channel_enum_items()
        if item[0] not in existing and mat.paint_layers_channel_in_set(channel=item[0])
    ]


class MATERIAL_OT_paint_layer_channel_add(_PaintLayerOperator):
    bl_idname = "material.paint_layer_channel_add"
    bl_label = "Add Paint Layer Channel"

    channel: bpy.props.EnumProperty(
        name="Channel",
        items=lambda self, context: _paint_layer_addable_channel_items(context),
    )

    def execute(self, context):
        layer = self._layer(context)
        if layer is None:
            return {'CANCELLED'}
        layer.channel_add(channel=self.channel)
        return {'FINISHED'}


class MATERIAL_OT_paint_layer_channel_remove(_PaintLayerOperator):
    bl_idname = "material.paint_layer_channel_remove"
    bl_label = "Remove Paint Layer Channel"

    channel: bpy.props.EnumProperty(
        name="Channel",
        items=lambda self, context: _paint_channel_enum_items(),
    )

    def execute(self, context):
        layer = self._layer(context)
        if layer is None:
            return {'CANCELLED'}
        try:
            layer.channel_remove(channel=self.channel)
        except RuntimeError as ex:
            self.report({'WARNING'}, str(ex))
            return {'CANCELLED'}
        return {'FINISHED'}


class MATERIAL_OT_paint_layer_channel_toggle(_PaintLayerOperator):
    """Switch one channel of a layer on or off, creating the record lazily on enable.

    An absent record is how a channel the layer never opted into reads as off; enabling has to both
    create the record and start it ENABLED (`channel_add` already does). Disabling only flips an
    existing record's state, so its map stays on the row (BKE_paint_layers_channel_set_enabled).
    """

    bl_idname = "material.paint_layer_channel_toggle"
    bl_label = "Toggle Paint Layer Channel"

    channel: bpy.props.EnumProperty(
        name="Channel",
        items=lambda self, context: _paint_channel_enum_items(),
    )
    enable: bpy.props.BoolProperty(name="Enable", default=True)

    def execute(self, context):
        layer = self._layer(context)
        if layer is None:
            return {'CANCELLED'}
        record = next((item for item in layer.channels if item.channel == self.channel), None)
        try:
            if self.enable:
                if record is None:
                    layer.channel_add(channel=self.channel)
                else:
                    layer.channel_set_enabled(channel=self.channel, enabled=True)
            elif record is not None:
                layer.channel_set_enabled(channel=self.channel, enabled=False)
        except RuntimeError as ex:
            self.report({'WARNING'}, str(ex))
            return {'CANCELLED'}
        return {'FINISHED'}


def _leave_mask_target(context, layer):
    """Back to painting the content when the mask being painted is about to go away or off.

    Only the active layer's mask is the one a stroke is aimed at, so another layer's mask going away
    leaves the mode alone, as the Outliner's seam does."""
    material = _context_material(context)
    if material is None or material.paint_layers.active is None:
        return
    if material.paint_layers.active.marker != layer.marker:
        return
    if context.scene.tool_settings.paint_mode.layer_target_mode != 'MASK':
        return
    try:
        bpy.ops.paint.layer_target_mode_set(mode='CONTENT')
    except RuntimeError:
        # No active object to switch for (the operator's poll): nothing is being painted either.
        pass


class MATERIAL_OT_paint_layer_mask_add(_PaintLayerOperator):
    """Give a layer a mask: a coverage factor laid over it. A mask of one shows the layer
    everywhere; a stroke in Mask mode paints it."""

    bl_idname = "material.paint_layer_mask_add"
    bl_label = "Add Paint Layer Mask"

    value: bpy.props.FloatProperty(
        name="Value",
        description="Constant coverage of the new mask: one shows the layer, zero hides it",
        default=1.0, min=0.0, max=1.0,
    )

    def execute(self, context):
        layer = self._layer(context)
        if layer is None:
            return {'CANCELLED'}
        try:
            layer.mask_add(value=self.value)
        except RuntimeError as ex:
            self.report({'WARNING'}, str(ex))
            return {'CANCELLED'}
        return {'FINISHED'}


class MATERIAL_OT_paint_layer_mask_remove(_PaintLayerOperator):
    """Remove a layer's mask, together with its map."""

    bl_idname = "material.paint_layer_mask_remove"
    bl_label = "Remove Paint Layer Mask"

    def execute(self, context):
        layer = self._layer(context)
        if layer is None:
            return {'CANCELLED'}
        # A stroke into a mask that is gone would go nowhere, so the brush goes back to the content.
        _leave_mask_target(context, layer)
        try:
            layer.mask_remove()
        except RuntimeError as ex:
            self.report({'WARNING'}, str(ex))
            return {'CANCELLED'}
        return {'FINISHED'}


class MATERIAL_OT_paint_layer_mask_toggle(_PaintLayerOperator):
    """Switch a layer's mask on or off, keeping the mask and its paint content."""

    bl_idname = "material.paint_layer_mask_toggle"
    bl_label = "Toggle Paint Layer Mask"

    def execute(self, context):
        layer = self._layer(context)
        if layer is None or not len(layer.mask_items):
            return {'CANCELLED'}
        item = layer.mask_items[0]
        if item.enabled:
            # A switched-off mask hides nothing: strokes into it would be invisible.
            _leave_mask_target(context, layer)
        item.enabled = not item.enabled
        return {'FINISHED'}


class MATERIAL_OT_paint_layer_mask_filter_add(_PaintLayerOperator):
    """Add a Color ID or UV Island filter over the layer's mask: it keeps the coverage only where
    an ID Map matches the listed colours, or on the captured UV islands."""

    bl_idname = "material.paint_layer_mask_filter_add"
    bl_label = "Add Mask Filter"

    filter_type: bpy.props.EnumProperty(
        name="Filter",
        items=(
            ('COLOR_ID', "Color ID", "Keep the coverage where an ID Map matches the listed colors"),
            ('UV_ISLAND', "UV Island", "Keep the coverage on the UV islands captured from the Image Editor"),
        ),
        default='COLOR_ID',
    )

    def execute(self, context):
        layer = self._layer(context)
        if layer is None:
            return {'CANCELLED'}
        try:
            # A filter works on the coverage below it, so the layer gets its white mask first, like
            # the other mask corrections.
            if not len(layer.mask_items):
                layer.mask_add(value=1.0)
            layer.mask_filter_add(filter_type=self.filter_type)
        except RuntimeError as ex:
            self.report({'WARNING'}, str(ex))
            return {'CANCELLED'}
        return {'FINISHED'}


class MATERIAL_OT_paint_layer_id_color_remove(Operator):
    """Remove a color from the active Color ID mask item."""

    bl_idname = "material.paint_layer_id_color_remove"
    bl_label = "Remove Color"
    bl_options = {'UNDO', 'REGISTER'}

    index: bpy.props.IntProperty(
        name="Index",
        description="Position of the color in the item's list",
        min=0,
        options={'SKIP_SAVE'},
    )

    @classmethod
    def poll(cls, context):
        mat = _context_material(context)
        return mat is not None and mat.is_layered and mat.paint_layers.active_mask_item is not None

    def execute(self, context):
        item = _context_material(context).paint_layers.active_mask_item
        try:
            item.id_color_remove(index=self.index)
        except RuntimeError as ex:
            self.report({'WARNING'}, str(ex))
            return {'CANCELLED'}
        return {'FINISHED'}


class MATERIAL_OT_paint_layer_object_mask_toggle(_PaintLayerOperator):
    """Show or hide the active layer on an object, or on one UDIM tile of it"""

    bl_idname = "material.paint_layer_object_mask_toggle"
    bl_label = "Toggle Layer On Object"
    bl_options = {'REGISTER', 'UNDO', 'INTERNAL'}

    object_name: bpy.props.StringProperty(name="Object")
    object_session_uid: bpy.props.IntProperty(
        name="Object Session UID",
        default=0,
        description="Session UID of the object; preferred over the name lookup",
    )
    tile: bpy.props.IntProperty(name="Tile", default=0, description="UDIM tile, or 0 for the whole object")

    @staticmethod
    def _target_object(object_name, object_session_uid):
        if object_session_uid:
            for candidate in bpy.data.objects:
                if candidate.session_uid == object_session_uid:
                    if candidate.library is not None or candidate.override_library is not None:
                        continue
                    return candidate
        # Fallback for scripted calls: only an editable local match; the RNA setter refuses the rest.
        for candidate in bpy.data.objects:
            if candidate.name == object_name and candidate.library is None and candidate.override_library is None:
                return candidate
        return None

    def execute(self, context):
        layer = self._layer(context)
        if layer is None:
            self.report({'ERROR'}, "Paint layer not found" if self.marker else "No active paint layer")
            return {'CANCELLED'}
        ob = self._target_object(self.object_name, self.object_session_uid)
        if ob is None:
            self.report({'ERROR'}, "Object \"{:s}\" not found".format(self.object_name))
            return {'CANCELLED'}
        if self.tile:
            layer.object_mask_tile_set(ob, self.tile, not layer.object_mask_tile_get(ob, self.tile))
        else:
            layer.object_mask_visible_set(ob, not layer.object_mask_visible_get(ob))
        return {'FINISHED'}


class MATERIAL_OT_paint_layer_object_mask_refresh_tiles(_PaintLayerOperator):
    """Rescan the UVs of the objects using the active layered material for occupied UDIM tiles"""

    bl_idname = "material.paint_layer_object_mask_refresh_tiles"
    bl_label = "Refresh Tiles"
    # No undo step: the tile lists are runtime-only caches.
    bl_options = {'INTERNAL'}

    def execute(self, context):
        mat = _context_material(context)
        layer = self._layer(context)
        if layer is None:
            return {'CANCELLED'}
        for ob in context.view_layer.objects:
            if any(slot.material == mat for slot in ob.material_slots):
                layer.object_mask_tiles(ob, refresh=True)
        return {'FINISHED'}


# Output Texture ##############################################################

# Python cannot read the C++ `is_color` flag of a channel, so the colour channels are mirrored from
# the channel table in `BKE_paint_material_channel_info`; every other channel is data (Non-Color).
_OUTPUT_COLOR_CHANNELS = {'BASE_COLOR', 'EMISSION'}

_OUTPUT_EXTENSIONS = {'PNG': ".png", 'TIFF': ".tif", 'OPEN_EXR': ".exr"}


def _output_clamp_color_depth(out):
    """Keep a color depth the file format writes: OpenEXR is 16 or 32 bit, PNG and TIFF 8 or 16."""
    allowed = ('16', '32') if out.file_format == 'OPEN_EXR' else ('8', '16')
    if out.color_depth not in allowed:
        out.color_depth = allowed[0]


def _output_file_format_update(self, _context):
    _output_clamp_color_depth(self)


class MaterialPaintLayersOutput(PropertyGroup):
    """Where and how Render Output writes the final maps of a layered material.

    Stored as ID properties on the material, so it is saved in the .blend and copied with it.
    """

    destination: bpy.props.EnumProperty(
        name="Destination",
        items=(
            ('DISK', "Disk", "Save the maps as files in the directory"),
            ('PACKED', "Packed", "Pack the maps into the .blend file"),
        ),
        default='DISK',
    )
    directory: bpy.props.StringProperty(
        name="Directory",
        description="Folder for the saved maps; a relative path is taken from the .blend",
        subtype='DIR_PATH',
    )
    file_format: bpy.props.EnumProperty(
        name="File Format",
        items=(
            ('PNG', "PNG", "Save as PNG"),
            ('TIFF', "TIFF", "Save as TIFF"),
            ('OPEN_EXR', "OpenEXR", "Save as OpenEXR"),
        ),
        default='PNG',
        update=_output_file_format_update,
    )
    color_depth: bpy.props.EnumProperty(
        name="Color Depth",
        description="Bits per channel; the values a format cannot write are reset",
        items=(
            ('8', "8", "8 bits per channel"),
            ('16', "16", "16 bits per channel"),
            ('32', "32", "32 bits per channel (OpenEXR only)"),
        ),
        default='8',
        update=_output_file_format_update,
    )
    existing_mode: bpy.props.EnumProperty(
        name="Existing Maps",
        items=(
            ('UPDATE', "Update Existing", "Overwrite the maps of the previous render"),
            ('INCREMENT', "Add New", "Keep the previous maps and add _v001, _v002, ..."),
        ),
        default='UPDATE',
    )
    name_pattern: bpy.props.StringProperty(
        name="Name Pattern",
        description="File and image name; {material} and {channel} are replaced",
        default="{material}_{channel}",
    )
    last_result: bpy.props.StringProperty(
        name="Last Result",
        description="Summary of the last Render Output",
        default="",
    )


def _output_channels(mat):
    """The channels of the material's set, in the order the channel enum lists them."""
    return [item[0] for item in _paint_channel_enum_items() if mat.paint_layers_channel_in_set(channel=item[0])]


_OUTPUT_SIDES = {
    'SIZE_256': 256,
    'SIZE_512': 512,
    'SIZE_1K': 1024,
    'SIZE_2K': 2048,
    'SIZE_4K': 4096,
    'SIZE_8K': 8192,
}


def _output_side(context, mat):
    """The map size. The enums give identifiers, not numbers; `SCENE` is the material's
    `Scene Default`, which is the scene's new channel size."""
    side = _OUTPUT_SIDES.get(mat.paint_layers_image_size, 0)
    if side <= 0:
        side = _OUTPUT_SIDES.get(context.scene.tool_settings.paint_mode.new_channel_image_size, 0)
    return side


def _output_base_name(out, mat, identifier):
    # Plain replacement, not `str.format`: a name pattern may contain other braces.
    name = out.name_pattern.replace("{material}", mat.name).replace("{channel}", identifier.lower())
    return bpy.path.clean_name(name) or identifier.lower()


def _output_file_path(directory, file_name, file_format):
    return os.path.join(directory, file_name + _OUTPUT_EXTENSIONS[file_format])


def _output_name_free(out, mat, identifier, suffix, directory):
    file_name = _output_base_name(out, mat, identifier) + suffix
    if file_name in bpy.data.images:
        return False
    if out.destination == 'DISK':
        return not os.path.exists(_output_file_path(directory, file_name, out.file_format))
    return True


def _output_suffix(out, mat, channels, directory):
    """The version suffix of this run: `_v001`, `_v002`, ... for Add New, empty for Update Existing.

    One number serves every channel, so the maps of a run share a version. None when no number is
    free up to 999.
    """
    if out.existing_mode != 'INCREMENT':
        return ""
    for version in range(1, 1000):
        suffix = "_v{:03d}".format(version)
        if all(_output_name_free(out, mat, identifier, suffix, directory) for identifier in channels):
            return suffix
    return None


def _output_find_image(mat, identifier):
    """The image Update Existing wrote for this channel on an earlier run, or None."""
    key = "{:s}|{:s}".format(mat.name, identifier)
    for image in bpy.data.images:
        if image.get("paint_layers_output") == key:
            return image
    return None


def _output_target_image(out, mat, side, identifier, file_name):
    """The image the channel is composited into. Returns the image and whether it was just created."""
    if out.existing_mode == 'UPDATE':
        image = _output_find_image(mat, identifier)
        if image is not None:
            if image.size[0] != side or image.size[1] != side:
                image.scale(side, side)
            image.colorspace_settings.name = "sRGB" if identifier in _OUTPUT_COLOR_CHANNELS else "Non-Color"
            return image, False
    image = bpy.data.images.new(
        file_name, side, side, alpha=True, float_buffer=(out.color_depth != '8'),
    )
    image.colorspace_settings.name = "sRGB" if identifier in _OUTPUT_COLOR_CHANNELS else "Non-Color"
    if out.existing_mode == 'UPDATE':
        # Marks the image as this channel's output for the next Update Existing run.
        image["paint_layers_output"] = "{:s}|{:s}".format(mat.name, identifier)
    return image, True


def _output_write_all(mat, out, side, channels, directory, suffix):
    """Composite and write every channel. Returns the written, skipped and failed channel lists."""
    written, skipped, failed = [], [], []
    tmp_scene = None
    if out.destination == 'DISK':
        # `Image.save_render` takes the file format, depth and colour mode from a scene, so a
        # temporary one carries the settings of this output; it is removed on every path out.
        tmp_scene = bpy.data.scenes.new("_paint_layers_output_tmp")
    try:
        if tmp_scene is not None:
            settings = tmp_scene.render.image_settings
            settings.file_format = out.file_format
            settings.color_depth = out.color_depth
            settings.color_mode = 'RGBA'
            # A new scene would tone-map the saved pixels with its default view transform
            # (AgX); the maps must be written as they are.
            tmp_scene.view_settings.look = 'None'
            tmp_scene.view_settings.exposure = 0.0
            tmp_scene.view_settings.gamma = 1.0
            tmp_scene.view_settings.use_curve_mapping = False

        for identifier in channels:
            file_name = _output_base_name(out, mat, identifier) + suffix
            image, is_new = _output_target_image(out, mat, side, identifier, file_name)
            try:
                composited = mat.paint_layers.composite(channel=identifier, image=image)
                reason = "nothing to composite"
            except RuntimeError as ex:
                composited = False
                reason = str(ex)
            if not composited:
                # A new image nothing was composited into is not kept; an updated one keeps its maps.
                if is_new:
                    bpy.data.images.remove(image)
                skipped.append("{:s} ({:s})".format(identifier, reason))
                continue

            try:
                image.file_format = out.file_format
                if out.destination == 'DISK':
                    filepath = _output_file_path(directory, file_name, out.file_format)
                    # Raw writes a data map untouched; Standard is the plain sRGB encoding a colour
                    # map needs. Neither adds a tone-mapping curve.
                    tmp_scene.view_settings.view_transform = (
                        'Standard' if identifier in _OUTPUT_COLOR_CHANNELS else 'Raw')
                    image.save_render(filepath=filepath, scene=tmp_scene)
                    image.filepath_raw = filepath
                else:
                    image.pack()
            except (RuntimeError, OSError) as ex:
                failed.append("{:s} ({:s})".format(identifier, str(ex)))
                continue
            written.append(identifier)
    finally:
        if tmp_scene is not None:
            bpy.data.scenes.remove(tmp_scene)
    return written, skipped, failed


class MATERIAL_OT_paint_layers_output_render(Operator):
    """Render the final maps of every channel of the layered material into files or packed images"""

    bl_idname = "material.paint_layers_output_render"
    bl_label = "Render Output"
    # Not undoable: the files written to disk cannot be taken back, and packed images are new
    # data-blocks that an undo step would not restore cleanly.
    bl_options = {'REGISTER'}

    @classmethod
    def poll(cls, context):
        mat = _context_material(context)
        return mat is not None and mat.is_layered and mat.library is None

    def execute(self, context):
        mat = _context_material(context)
        out = mat.paint_layers_output

        if mat.paint_layers_is_stale:
            self.report({'ERROR'}, "Wait until the layer maps are up to date, or press Update Now")
            return {'CANCELLED'}

        channels = _output_channels(mat)
        side = _output_side(context, mat)
        if not channels or side <= 0:
            self.report({'ERROR'}, "Nothing to render: the material has no channels")
            return {'CANCELLED'}

        directory = ""
        if out.destination == 'DISK':
            if not out.directory or (out.directory.startswith("//") and not bpy.data.filepath):
                self.report({'ERROR'}, "Set the output directory; a relative one needs a saved .blend")
                return {'CANCELLED'}
            directory = bpy.path.abspath(out.directory)
            try:
                os.makedirs(directory, exist_ok=True)
            except OSError as ex:
                self.report({'ERROR'}, "Cannot create the directory: {:s}".format(str(ex)))
                return {'CANCELLED'}

        suffix = _output_suffix(out, mat, channels, directory)
        if suffix is None:
            self.report({'ERROR'}, "No free version number: remove some of the _v### maps first")
            return {'CANCELLED'}

        # This path does not read the 3D view pass, but a future node path might change it, so the
        # pass is put back whatever happens here.
        pass_before = mat.paint_layers_view_pass
        try:
            written, skipped, failed = _output_write_all(mat, out, side, channels, directory, suffix)
        finally:
            if mat.paint_layers_view_pass != pass_before:
                mat.paint_layers_view_pass = pass_before

        self.report({'INFO'}, "Output Texture: {:d} written, {:d} skipped, {:d} failed".format(
            len(written), len(skipped), len(failed)))
        parts = ["{:d} written".format(len(written))]
        if skipped:
            parts.append("skipped: " + ", ".join(skipped))
        if failed:
            parts.append("failed: " + ", ".join(failed))
        out.last_result = "; ".join(parts)
        return {'FINISHED'} if written else {'CANCELLED'}


# Brush Layer source ##########################################################

# The rows offered by the open search popup. Kept at module level because the enum items callback
# has to return strings that stay alive, and the popup is built after #invoke has returned.
_brush_source_row_cache = []
# (name, library path) of the brush whose Layer source row the open popup assigns.
_brush_source_row_target = None
# The Shape row uses the same operator. Only the identity of the part is kept ('FILL' or 'STROKE'):
# the wrapped DNA address can move (undo, file load, scene change) between #invoke and #execute.
_shape_source_texture_part = None


def _brush_source_row_items(_self, _context):
    return _brush_source_row_cache


def _brush_source_row_collect(rows, depth, items):
    for row in rows:
        items.append((row.marker, "{}{}".format("    " * depth, row.name), ""))
        _brush_source_row_collect(row.children, depth + 1, items)


class PAINT_OT_brush_layer_source_row_search(Operator):
    """Pick the stack row a PBR Paint brush or a Shape part reads, by searching the Layer
    material's rows"""
    bl_idname = "paint.brush_layer_source_row_search"
    bl_label = "Select Layer Row"
    bl_options = {'INTERNAL'}
    bl_property = "row"

    row: bpy.props.EnumProperty(name="Row", items=_brush_source_row_items)

    def execute(self, context):
        # The popup runs outside the button that opened it, so the layout's context pointer is
        # gone by now. A Shape target is only the part's identity, re-resolved here; a brush
        # target is looked up again from its identity.
        global _shape_source_texture_part
        if _shape_source_texture_part is not None:
            # Imported here rather than at module level: `bl_operators` modules load early, and
            # this keeps the bl_ui import (and its cost) out of startup.
            from bl_ui.properties_paint_common import paint_shape_settings
            part = _shape_source_texture_part
            _shape_source_texture_part = None
            if not self.row:
                return {'CANCELLED'}
            shape = paint_shape_settings(context)
            tex = shape.stroke_texture if part == 'STROKE' else shape.fill_texture
            tex.layer_marker = self.row
            return {'FINISHED'}
        brush = bpy.data.brushes.get(_brush_source_row_target) if _brush_source_row_target else None
        if brush is None or brush.material_paint is None or not self.row:
            return {'CANCELLED'}
        brush.material_paint.source_layer_marker = self.row
        return {'FINISHED'}

    def invoke(self, context, _event):
        global _brush_source_row_target, _shape_source_texture_part
        _shape_source_texture_part = None
        shape_texture = getattr(context, "shape_texture", None)
        brush_paint = None if shape_texture is not None else \
            getattr(context, "brush_material_paint", None)
        owner = None
        if shape_texture is not None:
            # Store which part was clicked, not the RNA wrapper: the popup clears the button
            # context, and DNA can move before #execute.
            # Imported here rather than at module level to keep the bl_ui import out of startup;
            # see #execute.
            from bl_ui.properties_paint_common import paint_shape_settings
            shape = paint_shape_settings(context)
            pointer = shape_texture.as_pointer()
            if shape.fill_texture.as_pointer() == pointer:
                _shape_source_texture_part = 'FILL'
            elif shape.stroke_texture.as_pointer() == pointer:
                _shape_source_texture_part = 'STROKE'
            else:
                return {'CANCELLED'}
            owner = shape_texture.layer_owner
        elif brush_paint is not None:
            brush = brush_paint.id_data
            _brush_source_row_target = (brush.name, brush.library.filepath if brush.library else None)
            owner = brush_paint.source_layer_owner
        else:
            return {'CANCELLED'}
        _brush_source_row_cache.clear()
        if owner is not None and owner.is_layered:
            _brush_source_row_collect(owner.paint_layers, 0, _brush_source_row_cache)
        context.window_manager.invoke_search_popup(self)
        return {'CANCELLED'}


class PAINT_IDF_layer_material(bpy.types.IDFilter):
    """Offer only layered materials in the ID browser of a brush's Layer source"""

    bl_idname = "PAINT_IDF_layer_material"

    @classmethod
    def filter_id(cls, _context, id):
        return getattr(id, "is_layered", False)


def register():
    bpy.types.Material.paint_layers_output = bpy.props.PointerProperty(type=MaterialPaintLayersOutput)


def unregister():
    del bpy.types.Material.paint_layers_output


classes = (
    MaterialPaintLayersOutput,
    MATERIAL_OT_paint_layers_output_render,
    MATERIAL_OT_paint_layer_channel_add,
    MATERIAL_OT_paint_layer_channel_remove,
    MATERIAL_OT_paint_layer_channel_toggle,
    MATERIAL_OT_paint_layer_mask_add,
    MATERIAL_OT_paint_layer_mask_remove,
    MATERIAL_OT_paint_layer_mask_toggle,
    MATERIAL_OT_paint_layer_mask_filter_add,
    MATERIAL_OT_paint_layer_id_color_remove,
    MATERIAL_OT_paint_layer_object_mask_toggle,
    MATERIAL_OT_paint_layer_object_mask_refresh_tiles,
    PAINT_OT_brush_layer_source_row_search,
    PAINT_IDF_layer_material,
)
