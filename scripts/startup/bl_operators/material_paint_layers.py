# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""The thin operators behind the paint-layer RNA: add, remove, move, group, mask, channels,
corrections. UI-only code lives in bl_ui; these operate on Material.paint_layers by marker."""

import bpy
from bpy.app.handlers import persistent
from bpy.types import Operator


class MATERIAL_OT_paint_layers_regenerate(Operator):
    bl_idname = "material.paint_layers_regenerate"
    bl_label = "Regenerate Layer Tree"
    bl_description = "Rebuild the material's generated node tree from its layer description"

    @classmethod
    def poll(cls, context):
        mat = context.material
        return mat is not None and mat.is_layered

    def execute(self, context):
        context.material.paint_layers.regenerate()
        return {'FINISHED'}


def _paint_layers_active_layer(context):
    mat = context.material
    if mat is None or not mat.is_layered:
        return None
    return mat.paint_layers.active


def _paint_layers_uv_autofill(context):
    """Name the stack's UV layer after the object's active UV map when it is still empty."""
    mat = context.material
    obj = context.object
    if mat is not None and obj is not None:
        mat.paint_layers_uv_map_autofill(object=obj)


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
            layer = context.material.paint_layers.find(self.marker)
            if layer is not None:
                return layer
        return _paint_layers_active_layer(context)

    @classmethod
    def poll(cls, context):
        mat = context.material
        return mat is not None and mat.is_layered


class MATERIAL_OT_paint_layer_add(_PaintLayerOperator):
    bl_idname = "material.paint_layer_add"
    bl_label = "Add Paint Layer"
    bl_description = "Add a row on top of the stack"

    source: bpy.props.EnumProperty(
        name="Source",
        items=(
            ('IMAGE', "Paint", "A painted layer"),
            ('CONSTANT', "Fill", "A flat fill layer"),
            ('STACK', "Folder", "A folder grouping other layers"),
        ),
        default='IMAGE',
    )
    name: bpy.props.StringProperty(name="Name")

    def execute(self, context):
        layers = context.material.paint_layers
        _paint_layers_uv_autofill(context)
        layer = layers.new(source=self.source, name=self.name)
        if layer is not None:
            layers.active = layer
        return {'FINISHED'}


class MATERIAL_OT_paint_layer_remove(_PaintLayerOperator):
    bl_idname = "material.paint_layer_remove"
    bl_label = "Remove Paint Layer"
    bl_description = "Remove this row and everything nested under it"

    def execute(self, context):
        layer = self._layer(context)
        if layer is None:
            return {'CANCELLED'}
        context.material.paint_layers.remove(layer)
        return {'FINISHED'}


class MATERIAL_OT_paint_layer_rename(_PaintLayerOperator):
    bl_idname = "material.paint_layer_rename"
    bl_label = "Rename Paint Layer"

    name: bpy.props.StringProperty(name="Name")

    def execute(self, context):
        layer = self._layer(context)
        if layer is None:
            return {'CANCELLED'}
        layer.name = self.name
        return {'FINISHED'}


class MATERIAL_OT_paint_layer_duplicate(_PaintLayerOperator):
    bl_idname = "material.paint_layer_duplicate"
    bl_label = "Duplicate Paint Layer"

    def execute(self, context):
        layer = self._layer(context)
        if layer is None:
            return {'CANCELLED'}
        layers = context.material.paint_layers
        copy = layers.duplicate(layer)
        if copy is not None:
            layers.active = copy
        return {'FINISHED'}


class MATERIAL_OT_paint_layer_group(_PaintLayerOperator):
    bl_idname = "material.paint_layer_group"
    bl_label = "Group Paint Layers"
    bl_description = "Fold this row and another into a new folder"

    with_marker: bpy.props.StringProperty(
        name="With",
        description="UUID marker of the other row to fold in with",
        options={'SKIP_SAVE'},
    )

    def execute(self, context):
        layer = self._layer(context)
        with_layer = (
            context.material.paint_layers.find(self.with_marker) if self.with_marker else None
        )
        if layer is None or with_layer is None:
            return {'CANCELLED'}
        folder = context.material.paint_layers.group(layer, with_layer)
        if folder is not None:
            context.material.paint_layers.active = folder
        return {'FINISHED'}


class MATERIAL_OT_paint_layer_ungroup(_PaintLayerOperator):
    bl_idname = "material.paint_layer_ungroup"
    bl_label = "Ungroup Paint Layer"
    bl_description = "Lift the rows of this folder into its parent and free the folder"

    def execute(self, context):
        layer = self._layer(context)
        if layer is None:
            return {'CANCELLED'}
        context.material.paint_layers.ungroup(layer)
        return {'FINISHED'}


class MATERIAL_OT_paint_layer_move(_PaintLayerOperator):
    bl_idname = "material.paint_layer_move"
    bl_label = "Move Paint Layer"

    anchor_marker: bpy.props.StringProperty(
        name="Anchor",
        description="UUID marker of the row to move relative to; the top of the stack when unset",
        options={'SKIP_SAVE'},
    )
    place: bpy.props.EnumProperty(
        name="Place",
        items=(
            ('ABOVE', "Above", "Directly above the anchor"),
            ('BELOW', "Below", "Directly below the anchor"),
            ('INTO', "Into", "Inside the anchor folder"),
        ),
        default='ABOVE',
    )

    def execute(self, context):
        layer = self._layer(context)
        if layer is None:
            return {'CANCELLED'}
        anchor = (
            context.material.paint_layers.find(self.anchor_marker) if self.anchor_marker else None
        )
        context.material.paint_layers.move(layer, anchor, self.place)
        return {'FINISHED'}


class MATERIAL_OT_paint_layer_mask_add(_PaintLayerOperator):
    bl_idname = "material.paint_layer_mask_add"
    bl_label = "Add Paint Layer Mask"

    value: bpy.props.FloatProperty(name="Value", min=0.0, max=1.0, default=1.0)

    def execute(self, context):
        layer = self._layer(context)
        if layer is None:
            return {'CANCELLED'}
        layer.mask_add(value=self.value)
        return {'FINISHED'}


class MATERIAL_OT_paint_layer_mask_remove(_PaintLayerOperator):
    bl_idname = "material.paint_layer_mask_remove"
    bl_label = "Remove Paint Layer Mask"

    item_marker: bpy.props.StringProperty(
        name="Mask Item",
        description="UUID marker of the mask item to remove; the first one when unset",
        options={'SKIP_SAVE'},
    )

    def execute(self, context):
        layer = self._layer(context)
        if layer is None:
            return {'CANCELLED'}
        layers = context.material.paint_layers
        item = layers.find(self.item_marker) if self.item_marker else None
        if item is None:
            item = layer.mask_stack[0] if len(layer.mask_stack) else None
        if item is None:
            return {'CANCELLED'}
        layers.remove(item)
        return {'FINISHED'}


class MATERIAL_OT_paint_layer_mask_toggle(_PaintLayerOperator):
    bl_idname = "material.paint_layer_mask_toggle"
    bl_label = "Toggle Paint Layer Mask"

    item_marker: bpy.props.StringProperty(
        name="Mask Item",
        description="UUID marker of the mask item to toggle; the first one when unset",
        options={'SKIP_SAVE'},
    )

    def execute(self, context):
        layer = self._layer(context)
        if layer is None:
            return {'CANCELLED'}
        layers = context.material.paint_layers
        item = layers.find(self.item_marker) if self.item_marker else None
        if item is None:
            item = layer.mask_stack[0] if len(layer.mask_stack) else None
        if item is None:
            return {'CANCELLED'}
        item.enabled = not item.enabled
        return {'FINISHED'}


class MATERIAL_OT_paint_layer_channel_add(_PaintLayerOperator):
    bl_idname = "material.paint_layer_channel_add"
    bl_label = "Add Paint Layer Channel"

    channel: bpy.props.EnumProperty(
        name="Channel",
        items=lambda self, context: [
            (item.identifier, item.name, "")
            for item in bpy.types.Material.bl_rna.functions["bake_paint_channels"].parameters[
                "channels"].enum_items
            if item.identifier
        ],
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
        items=lambda self, context: [
            (item.identifier, item.name, "")
            for item in bpy.types.Material.bl_rna.functions["bake_paint_channels"].parameters[
                "channels"].enum_items
            if item.identifier
        ],
    )

    def execute(self, context):
        layer = self._layer(context)
        if layer is None:
            return {'CANCELLED'}
        layer.channel_remove(channel=self.channel)
        return {'FINISHED'}


class MATERIAL_OT_paint_layer_correction_add(_PaintLayerOperator):
    bl_idname = "material.paint_layer_correction_add"
    bl_label = "Add Paint Layer Correction"

    role: bpy.props.EnumProperty(
        name="Role",
        items=lambda self, context: [
            (item.identifier, item.name, "")
            for item in bpy.types.MaterialPaintLayer.bl_rna.properties["role"].enum_items
            # A correction is Effect or Mask Item; Layer is a stack row, not something this
            # operator ever creates.
            if item.identifier in ('EFFECT', 'MASK_ITEM')
        ],
    )
    source: bpy.props.EnumProperty(
        name="Source",
        items=lambda self, context: [
            (item.identifier, item.name, "")
            for item in bpy.types.MaterialPaintLayer.bl_rna.properties["source"].enum_items
            # A correction only ever paints (Image) or fills (Constant).
            if item.identifier in ('IMAGE', 'CONSTANT')
        ],
    )
    name: bpy.props.StringProperty(name="Name")

    def execute(self, context):
        layer = self._layer(context)
        if layer is None:
            return {'CANCELLED'}
        _paint_layers_uv_autofill(context)
        layer.correction_add(role=self.role, source=self.source, name=self.name)
        return {'FINISHED'}


class MATERIAL_OT_paint_layer_rebake(_PaintLayerOperator):
    bl_idname = "material.paint_layer_rebake"
    bl_label = "Rebake Paint Layer"
    bl_description = "Mark the layer's bake stale so the planner re-bakes it"

    def execute(self, context):
        layer = self._layer(context)
        if layer is None:
            return {'CANCELLED'}
        layer.bake_request()
        return {'FINISHED'}


def _mesh_map_owner(context):
    # The stack owner is the active slot's material, not `context.material`: in the Layer Material
    # tab that can be the active Material row's source. The bake operator uses the active slot too.
    ob = context.object
    mat = ob.active_material if ob is not None else None
    if mat is None or not mat.is_layered or mat.grease_pencil:
        return None
    return mat


class OBJECT_OT_mesh_map_refresh(Operator):
    bl_idname = "object.mesh_map_refresh"
    bl_label = "Refresh Mesh Map Status"
    bl_description = "Re-check mesh map bake statuses for the active object"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        ob = context.object
        return ob is not None and ob.type == 'MESH'

    def execute(self, context):
        depsgraph = context.evaluated_depsgraph_get()
        changed = context.object.mesh_map_states.refresh_all(depsgraph=depsgraph)
        self.report({'INFO'}, "Changed {} mesh map status(es)".format(changed))
        return {'FINISHED'}


class MATERIAL_OT_mesh_map_add_layer(Operator):
    bl_idname = "material.mesh_map_add_layer"
    bl_label = "Add Mesh Map Layer"
    bl_description = "Add a Mesh Map layer to the material"
    bl_options = {'REGISTER', 'UNDO'}

    type: bpy.props.EnumProperty(
        name="Map Type",
        items=(
            ('AO', "Ambient Occlusion", "Ambient occlusion map"),
            ('CURVATURE', "Curvature", "Curvature map"),
            ('NORMAL_WORLD', "Normal (World)", "World-space normal map"),
            ('NORMAL_OBJECT', "Normal (Object)", "Object-space normal map"),
            ('ID_OBJECT', "Object ID", "Object index map"),
            ('ID_MATERIAL', "Material ID", "Material index map"),
            ('EDGE', "Edge", "Edge/bevel map"),
        ),
    )

    @classmethod
    def poll(cls, context):
        return _mesh_map_owner(context) is not None

    def execute(self, context):
        names = {
            'AO': "AO Map",
            'CURVATURE': "Curvature Map",
            'NORMAL_WORLD': "Normal (World) Map",
            'NORMAL_OBJECT': "Normal (Object) Map",
            'ID_OBJECT': "Object ID Map",
            'ID_MATERIAL': "Material ID Map",
            'EDGE': "Edge Map",
        }
        mat = _mesh_map_owner(context)
        layer = mat.paint_layers.new(source='MESH_MAP', name=names[self.type])
        if layer is None:
            self.report({'ERROR'}, "Cannot add Mesh Map layer")
            return {'CANCELLED'}
        layer.mesh_map_type = self.type
        mat.paint_layers.active = layer
        return {'FINISHED'}


class MATERIAL_OT_mesh_map_add_mask(Operator):
    bl_idname = "material.mesh_map_add_mask"
    bl_label = "Add Mesh Map Mask"
    bl_description = "Add a Mesh Map mask to the active paint layer"
    bl_options = {'REGISTER', 'UNDO'}

    type: bpy.props.EnumProperty(
        name="Map Type",
        items=(
            ('AO', "Ambient Occlusion", "Ambient occlusion map"),
            ('CURVATURE', "Curvature", "Curvature map"),
            ('NORMAL_WORLD', "Normal (World)", "World-space normal map"),
            ('NORMAL_OBJECT', "Normal (Object)", "Object-space normal map"),
            ('ID_OBJECT', "Object ID", "Object index map"),
            ('ID_MATERIAL', "Material ID", "Material index map"),
            ('EDGE', "Edge", "Edge/bevel map"),
        ),
    )

    @classmethod
    def poll(cls, context):
        return _mesh_map_owner(context) is not None

    def execute(self, context):
        mat = _mesh_map_owner(context)
        layer = mat.paint_layers.active
        if layer is None:
            self.report({'ERROR'}, "No active paint layer for a Mesh Map mask")
            return {'CANCELLED'}
        names = {
            'AO': "AO Mask",
            'CURVATURE': "Curvature Mask",
            'NORMAL_WORLD': "Normal (World) Mask",
            'NORMAL_OBJECT': "Normal (Object) Mask",
            'ID_OBJECT': "Object ID Mask",
            'ID_MATERIAL': "Material ID Mask",
            'EDGE': "Edge Mask",
        }
        mask = layer.correction_add(role='MASK_ITEM', source='MESH_MAP', name=names[self.type])
        if mask is None:
            self.report({'ERROR'}, "Cannot add Mesh Map mask to the active paint layer")
            return {'CANCELLED'}
        mask.mesh_map_type = self.type
        return {'FINISHED'}


class MATERIAL_OT_mesh_map_use_active_uv(Operator):
    bl_idname = "material.mesh_map_use_active_uv"
    bl_label = "Use Active UV"
    bl_description = "Use the object's active UV layer for the material paint stack"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        ob = context.object
        return (
            _mesh_map_owner(context) is not None and
            ob.type == 'MESH' and ob.data is not None
        )

    def execute(self, context):
        mat = _mesh_map_owner(context)
        ob = context.object
        if mat.paint_layers_uv_map:
            return {'FINISHED'}
        if ob.data.uv_layers.active is None:
            self.report({'ERROR'}, "The object has no active UV layer")
            return {'CANCELLED'}
        mat.paint_layers_uv_map_autofill(object=ob)
        return {'FINISHED'}


class OBJECT_OT_mesh_map_source_add(Operator):
    bl_idname = "object.mesh_map_source_add"
    bl_label = "Use High-poly Source"
    bl_description = "Give this material a high-poly source object or collection"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return _mesh_map_owner(context) is not None

    def execute(self, context):
        mat = _mesh_map_owner(context)
        ob = context.object
        if mat is None or ob is None:
            return {'CANCELLED'}
        if ob.mesh_map_sources.ensure(material=mat) is None:
            self.report({'ERROR'}, "Cannot create a high-poly source")
            return {'CANCELLED'}
        return {'FINISHED'}


class OBJECT_OT_mesh_map_source_remove(Operator):
    bl_idname = "object.mesh_map_source_remove"
    bl_label = "Remove High-poly Source"
    bl_description = "Remove this material's high-poly source"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return _mesh_map_owner(context) is not None

    def execute(self, context):
        mat = _mesh_map_owner(context)
        ob = context.object
        if mat is None or ob is None:
            return {'CANCELLED'}
        ob.mesh_map_sources.remove(material=mat)
        return {'FINISHED'}


def mesh_map_source_objects(scene, mat):
    """The objects that are high-poly sources (object, collection members or cage) for \a mat.

    A source is geometry the low-poly samples, never a low-poly to bake: it must not show up in the
    object list and must not be treated as an owner of the shared atlas.
    """
    result = set()
    if scene is None or mat is None:
        return result
    for ob in scene.objects:
        for source in ob.mesh_map_sources:
            if source.material != mat:
                continue
            if source.high_poly is not None:
                result.add(source.high_poly)
            if source.high_poly_collection is not None:
                for member in source.high_poly_collection.all_objects:
                    result.add(member)
            if source.cage is not None:
                result.add(source.cage)
    return result


def mesh_map_objects_for_material(scene, mat):
    """The mesh objects of \a scene that use \a mat in one of their slots, sources excluded.

    Exposed for addons and the Mesh Maps panel, so both agree on which objects a "bake all"
    touches.
    """
    if scene is None or mat is None:
        return []
    sources = mesh_map_source_objects(scene, mat)
    result = []
    for ob in scene.objects:
        if ob.type != 'MESH' or ob.data is None or ob in sources:
            continue
        if any(slot.material == mat for slot in ob.material_slots):
            result.append(ob)
    return result


def mesh_map_summary_status(ob, mat):
    """The worst mesh-map status of \a ob for \a mat, and how many states it has."""
    states = [state for state in ob.mesh_map_states if state.material == mat]
    if len(states) == 0:
        return ('NONE', 0)
    statuses = {state.status for state in states}
    for candidate in ('BAKING', 'ERROR', 'STALE', 'VALID'):
        if candidate in statuses:
            return (candidate, len(states))
    return ('NONE', len(states))


def mesh_map_states_need_refresh(states):
    """Whether an object with \a states should be re-checked on a geometry change.

    Only VALID or STALE states have something to re-check: NONE was never baked, ERROR stays until an
    explicit retry, and an in-flight bake owns its status until the worker sets the final one.
    """
    return any(state.status in {'VALID', 'STALE'} for state in states)


def mesh_map_geometry_updated_objects(scene, depsgraph):
    """The scene objects whose evaluated geometry changed in \a depsgraph and have mesh-map states.

    Only the `is_updated_geometry` updates count, so moving the view or the cursor never re-hashes a
    mesh. A mesh-data update maps back to the objects that share that mesh.
    """
    result = []
    seen = set()
    if scene is None or depsgraph is None:
        return result

    def consider(ob):
        if ob is None or ob.name in seen or ob.type != 'MESH' or ob.data is None:
            return
        if len(ob.mesh_map_states) == 0:
            return
        seen.add(ob.name)
        result.append(ob)

    for update in depsgraph.updates:
        if not update.is_updated_geometry:
            continue
        id_orig = update.id
        if isinstance(id_orig, bpy.types.Object):
            consider(id_orig)
        elif isinstance(id_orig, bpy.types.Mesh):
            for ob in scene.objects:
                if ob.type == 'MESH' and ob.data == id_orig:
                    consider(ob)
    return result


def mesh_map_source_member_count(source):
    """How many mesh objects a source record resolves to (0 means the source is empty)."""
    count = 0
    if source.high_poly is not None:
        count += 1
    if source.high_poly_collection is not None:
        count += sum(1 for ob in source.high_poly_collection.all_objects if ob.type == 'MESH')
    return count


def mesh_map_has_source(ob):
    """Whether \a ob bakes at least one material from a high-poly source."""
    return any(
        source.high_poly is not None or source.high_poly_collection is not None
        for source in ob.mesh_map_sources
    )


def mesh_map_source_targets(scene):
    """Map a source object's name to the (low-poly object, material) pairs it feeds.

    The reverse of `Object.mesh_map_sources`, for the update handler: a change to a high-poly or a
    cage must re-check the low-poly objects that bake from it.
    """
    targets = {}
    if scene is None:
        return targets
    for target in scene.objects:
        if target.type != 'MESH':
            continue
        for source in target.mesh_map_sources:
            if source.material is None:
                continue
            members = []
            if source.high_poly is not None:
                members.append(source.high_poly)
            if source.high_poly_collection is not None:
                members.extend(source.high_poly_collection.all_objects)
            if source.cage is not None:
                members.append(source.cage)
            for member in members:
                targets.setdefault(member.name, []).append((target, source.material))
    return targets


def mesh_map_updated_objects(scene, depsgraph):
    """The low-poly objects whose mesh-map statuses should be re-checked after this update.

    Direct: an object with refreshable states, when its geometry changed, or its transform changed
    while it uses a high-poly source (the hash folds the relative transform).
    Reverse: a high-poly source or cage keeps no state of its own, so a geometry or transform change
    to it re-checks the low-poly objects that bake from it, for the matching material.
    """
    result = []
    seen = set()
    if scene is None or depsgraph is None:
        return result
    targets_by_source = mesh_map_source_targets(scene)

    def add(ob, material=None):
        if ob is None or ob.name in seen or ob.type != 'MESH' or ob.data is None:
            return
        states = list(ob.mesh_map_states)
        if material is not None:
            states = [state for state in states if state.material == material]
        if not mesh_map_states_need_refresh(states):
            return
        seen.add(ob.name)
        result.append(ob)

    for update in depsgraph.updates:
        is_geometry = update.is_updated_geometry
        is_transform = getattr(update, 'is_updated_transform', False)
        if not (is_geometry or is_transform):
            continue
        id_orig = update.id
        objects = []
        if isinstance(id_orig, bpy.types.Object):
            objects.append(id_orig)
        elif isinstance(id_orig, bpy.types.Mesh):
            for ob in scene.objects:
                if ob.type == 'MESH' and ob.data == id_orig:
                    objects.append(ob)
        for ob in objects:
            for target, material in targets_by_source.get(ob.name, ()):
                add(target, material)
            if is_geometry:
                add(ob)
            elif is_transform and mesh_map_has_source(ob):
                add(ob)
    return result


def mesh_map_pending_names_add(pending, objects):
    """Add the names of \a objects to the \a pending set, returning whether anything was new.

    Kept free of `bpy.app.timers` and of bpy objects other than the name, so the debounce
    accumulation can be tested on its own.
    """
    added = False
    for ob in objects:
        if ob.name not in pending:
            pending.add(ob.name)
            added = True
    return added


def mesh_map_pending_add(pending, scene, depsgraph):
    """Queue the freshly changed, refreshable objects of \a depsgraph for the debounce timer."""
    return mesh_map_pending_names_add(pending, mesh_map_updated_objects(scene, depsgraph))


# Objects whose geometry changed recently, waiting for the debounce timer to re-check them. Names,
# not references, so a deleted object is simply skipped when the timer fires.
_mesh_map_pending = set()
_mesh_map_timer_active = False

_MESH_MAP_REFRESH_DELAY = 0.3


def _mesh_map_refresh_timer():
    """One-shot timer: re-check the queued objects against the current depsgraph."""
    global _mesh_map_timer_active
    _mesh_map_timer_active = False
    names = tuple(_mesh_map_pending)
    _mesh_map_pending.clear()
    try:
        depsgraph = bpy.context.evaluated_depsgraph_get()
    except RuntimeError:
        depsgraph = None
    if depsgraph is None:
        return None
    for name in names:
        ob = bpy.data.objects.get(name)
        if ob is None or ob.type != 'MESH' or len(ob.mesh_map_states) == 0:
            continue
        ob.mesh_map_states.refresh_all(depsgraph=depsgraph)
    return None


@persistent
def mesh_map_depsgraph_update_post(scene, depsgraph=None):
    """Queue mesh-map statuses for a debounced re-check after each depsgraph update.

    A step-by-step edit in Edit/Sculpt mode fires many updates; the timer coalesces them into one
    hash check. `refresh_all` only tags the object when a status actually flips (see `mesh_maps.cc`),
    so this cannot feed itself: the shading tag produces one more update where nothing is queued.
    """
    if depsgraph is None:
        return
    global _mesh_map_timer_active
    mesh_map_pending_add(_mesh_map_pending, scene, depsgraph)
    if _mesh_map_pending and not _mesh_map_timer_active:
        _mesh_map_timer_active = True
        bpy.app.timers.register(_mesh_map_refresh_timer, first_interval=_MESH_MAP_REFRESH_DELAY)


def register():
    handlers = bpy.app.handlers.depsgraph_update_post
    if mesh_map_depsgraph_update_post not in handlers:
        handlers.append(mesh_map_depsgraph_update_post)


def unregister():
    global _mesh_map_timer_active
    handlers = bpy.app.handlers.depsgraph_update_post
    try:
        handlers.remove(mesh_map_depsgraph_update_post)
    except ValueError:
        pass
    _mesh_map_pending.clear()
    if _mesh_map_timer_active:
        try:
            bpy.app.timers.unregister(_mesh_map_refresh_timer)
        except ValueError:
            pass
        _mesh_map_timer_active = False


classes = (
    MATERIAL_OT_paint_layers_regenerate,
    MATERIAL_OT_paint_layer_add,
    MATERIAL_OT_paint_layer_remove,
    MATERIAL_OT_paint_layer_rename,
    MATERIAL_OT_paint_layer_duplicate,
    MATERIAL_OT_paint_layer_group,
    MATERIAL_OT_paint_layer_ungroup,
    MATERIAL_OT_paint_layer_move,
    MATERIAL_OT_paint_layer_mask_add,
    MATERIAL_OT_paint_layer_mask_remove,
    MATERIAL_OT_paint_layer_mask_toggle,
    MATERIAL_OT_paint_layer_channel_add,
    MATERIAL_OT_paint_layer_channel_remove,
    MATERIAL_OT_paint_layer_correction_add,
    MATERIAL_OT_paint_layer_rebake,
    OBJECT_OT_mesh_map_refresh,
    MATERIAL_OT_mesh_map_add_layer,
    MATERIAL_OT_mesh_map_add_mask,
    MATERIAL_OT_mesh_map_use_active_uv,
    OBJECT_OT_mesh_map_source_add,
    OBJECT_OT_mesh_map_source_remove,
)
