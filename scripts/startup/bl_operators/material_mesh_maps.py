# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""The Mesh Maps operators and helpers around the layered material's shared mesh map atlases.

Split out of `material_paint_layers.py`, which keeps the paint-layer stack operators. The Mesh
Maps panel (`bl_ui.properties_material_mesh_maps`) reads the helpers here and drives these
operators; everything goes through the layered material's active slot, the same owner the panel
and the C++ bake job pick."""

import bpy
from bpy.props import EnumProperty, IntProperty, StringProperty
from bpy.types import Operator

from bl_operators.material_paint_layers import context_material


# Every map type the RNA enum offers, in RNA order; fixed after startup.
_MESH_MAP_TYPE_ENTRIES = tuple(
    (item.identifier, item.name, item.description)
    for item in bpy.types.MaterialPaintLayer.bl_rna.properties['mesh_map_type'].enum_items
    if item.identifier != ''
)


def mesh_map_type_items(mat):
    """(identifier, name, description) of the maps `mat` can bake, from the RNA enum.

    Without a material this returns the full list, so the operator's enum callback has a value to
    return before its poll rejects the call.
    """
    if mat is None:
        return _MESH_MAP_TYPE_ENTRIES
    return tuple(entry for entry in _MESH_MAP_TYPE_ENTRIES
                 if mat.mesh_map_type_is_bakeable(entry[0]))


def mesh_map_layer_name(type_id):
    """The default layer name of a map type, from the RNA enum's display name.

    'AO' keeps its short historical name, already saved in files; a type absent from the enum falls
    back to the identifier so an unknown type cannot crash the operator.
    """
    if type_id == 'AO':
        return "AO Map"
    for identifier, name, _description in _MESH_MAP_TYPE_ENTRIES:
        if identifier == type_id:
            return "{} Map".format(name)
    return "{} Map".format(type_id)


def mesh_map_owner(context):
    """The layered material whose atlas the Mesh Maps operators touch."""
    mat = context_material(context)
    return mat if (mat is not None and mat.is_layered) else None


class OBJECT_OT_mesh_map_refresh(Operator):
    bl_idname = "object.mesh_map_refresh"
    bl_label = "Refresh Mesh Map Status"
    bl_description = "Re-check mesh map bake statuses for the active object"
    # No undo step: the operator only recomputes derived statuses, nothing the user authored.
    bl_options = {'REGISTER'}

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
    """Add a Mesh Map layer of one map type to the material.

    More than one layer of the same type is allowed: they share the type's atlas, so the panel
    bakes a type once while the stack shows each row separately.
    """

    bl_idname = "material.mesh_map_add_layer"
    bl_label = "Add Mesh Map Layer"
    bl_description = "Add a Mesh Map layer to the material"
    bl_options = {'REGISTER', 'UNDO'}

    type: EnumProperty(
        name="Map Type",
        items=lambda self, context: mesh_map_type_items(mesh_map_owner(context)),
    )

    slot_uid: IntProperty(
        name="Atlas Copy",
        description="The atlas copy the layer reads; zero is the primary",
        default=0,
        min=0,
    )

    @classmethod
    def poll(cls, context):
        return mesh_map_owner(context) is not None

    def execute(self, context):
        mat = mesh_map_owner(context)
        layer = mat.paint_layers.new(source='MESH_MAP', name=mesh_map_layer_name(self.type))
        if layer is None:
            self.report({'ERROR'}, "Cannot add Mesh Map layer")
            return {'CANCELLED'}
        layer.mesh_map_type = self.type
        layer.mesh_map_slot_uid = self.slot_uid
        mat.paint_layers.active = layer
        return {'FINISHED'}


class MATERIAL_OT_mesh_map_slot_new(Operator):
    """Append a new copy of a map type: it gets its own atlas and bakes on its own"""

    bl_idname = "material.mesh_map_slot_new"
    bl_label = "Add Map Copy"
    bl_options = {'REGISTER', 'UNDO'}

    type: EnumProperty(
        name="Map Type",
        items=lambda self, context: mesh_map_type_items(mesh_map_owner(context)),
    )
    name: StringProperty(
        name="Name",
        description="The copy's name; empty uses the type",
        maxlen=64,
    )

    @classmethod
    def poll(cls, context):
        return mesh_map_owner(context) is not None

    def invoke(self, context, _event):
        return context.window_manager.invoke_props_dialog(self)

    def draw(self, _context):
        layout = self.layout
        layout.use_property_split = True
        layout.prop(self, "name")

    def execute(self, context):
        mat = mesh_map_owner(context)
        slot = mat.mesh_map_slots.new(type=self.type, name=self.name)
        if slot is None:
            self.report({'ERROR'}, "Cannot create the atlas copy")
            return {'CANCELLED'}
        self.report({'INFO'}, "Added atlas copy {:d}".format(slot.uid))
        return {'FINISHED'}


class MATERIAL_OT_mesh_map_slot_remove(Operator):
    """Remove an atlas copy; layers reading it stay but sample nothing"""

    bl_idname = "material.mesh_map_slot_remove"
    bl_label = "Remove Map Copy"
    bl_options = {'REGISTER', 'UNDO'}

    type: EnumProperty(
        name="Map Type",
        items=lambda self, context: mesh_map_type_items(mesh_map_owner(context)),
    )
    slot_uid: IntProperty(name="Atlas Copy", default=0, min=0)

    @classmethod
    def poll(cls, context):
        return mesh_map_owner(context) is not None

    def execute(self, context):
        mat = mesh_map_owner(context)
        slot = next((candidate for candidate in mat.mesh_map_slots
                     if candidate.type == self.type and candidate.uid == self.slot_uid), None)
        if slot is None:
            return {'CANCELLED'}
        if mat.mesh_map_slots.remove(slot=slot):
            self.report({'INFO'}, "Removed the atlas copy")
            return {'FINISHED'}
        return {'CANCELLED'}


class OBJECT_OT_mesh_map_source_add(Operator):
    bl_idname = "object.mesh_map_source_add"
    bl_label = "Use High-poly Source"
    bl_description = "Give this material a high-poly source object or collection"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return mesh_map_owner(context) is not None

    def execute(self, context):
        mat = mesh_map_owner(context)
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
        return mesh_map_owner(context) is not None

    def execute(self, context):
        mat = mesh_map_owner(context)
        ob = context.object
        if mat is None or ob is None:
            return {'CANCELLED'}
        ob.mesh_map_sources.remove(material=mat)
        return {'FINISHED'}


def mesh_map_objects_for_material(mat):
    """The mesh objects of the whole file that a "bake all" over `mat` covers, sources excluded.

    The rule is the bake's own (#Material.mesh_map_bake_target_objects), so the panel and
    `OBJECT_OT_mesh_map_bake_all` with `object_scope='ALL'` cannot disagree. The C++ side resolves
    the whole list in one pass, so asking for it per object would rescan the sources every time.
    """
    if mat is None:
        return []
    return list(mat.mesh_map_bake_target_objects)


class OBJECT_OT_mesh_map_source_match_by_name(Operator):
    """Pair high-poly objects to the low-poly ones of this material by name suffixes

    The high-poly object is looked up with `bpy.data.objects.get`, i.e. across the whole file, not
    just the current scene: a matching object from another scene or a linked library is paired
    too. An existing collection source on the low-poly object is replaced by the object pair; the
    report counts those replacements so the overwrite is not silent.
    """

    bl_idname = "object.mesh_map_source_match_by_name"
    bl_label = "Match by Name"
    bl_options = {'REGISTER', 'UNDO'}

    low_suffix: StringProperty(
        name="Low Suffix", description="Name ending that marks a low-poly object", default="_low",
        maxlen=64)
    high_suffix: StringProperty(
        name="High Suffix", description="Name ending that marks a high-poly object",
        default="_high", maxlen=64)

    @classmethod
    def poll(cls, context):
        return mesh_map_owner(context) is not None

    def invoke(self, context, _event):
        # The suffixes are the only decision; a props dialog keeps the panel at one button.
        return context.window_manager.invoke_props_dialog(self)

    def draw(self, _context):
        layout = self.layout
        layout.use_property_split = True
        layout.prop(self, "low_suffix")
        layout.prop(self, "high_suffix")

    def execute(self, context):
        mat = mesh_map_owner(context)
        if mat is None:
            return {'CANCELLED'}
        lows = mesh_map_objects_for_material(mat)
        if not lows:
            self.report({'ERROR'}, "No object of this material to pair")
            return {'CANCELLED'}
        paired = 0
        replaced_collections = 0
        unmatched = []
        for low in lows:
            # A high-poly object of the same material is a source, not a low-poly target: its name
            # carries the high suffix. Skipping it also keeps the pair from matching itself.
            if self.high_suffix and low.name.endswith(self.high_suffix):
                continue
            stem = low.name
            if self.low_suffix and stem.endswith(self.low_suffix):
                stem = stem[: -len(self.low_suffix)]
            high = bpy.data.objects.get(stem + self.high_suffix)
            if high is None or high.type != 'MESH' or high == low:
                unmatched.append(low.name)
                continue
            source = low.mesh_map_sources.ensure(material=mat)
            if source is None:
                unmatched.append(low.name)
                continue
            # The RNA setters keep the object-XOR-collection invariant; setting both makes the
            # result the same whichever form a previous assignment used. A collection source is
            # replaced by the object pair, which is reported below instead of happening silently.
            if source.high_poly_collection is not None:
                replaced_collections += 1
            source.high_poly = high
            source.high_poly_collection = None
            paired += 1
        report_set = {'WARNING'} if (unmatched or replaced_collections) else {'INFO'}
        message = "Paired {:d} object(s)".format(paired)
        if replaced_collections:
            message += "; replaced {:d} collection source(s) with object pairs".format(
                replaced_collections)
        if unmatched:
            message += "; without a high-poly match: {}".format(", ".join(unmatched))
        if paired:
            self.report(report_set, message)
            return {'FINISHED'}
        self.report({'WARNING'},
                    "No match for: {}".format(", ".join(unmatched) if unmatched else "any object"))
        return {'CANCELLED'}


# Cage preview #############################################################

# The preview draws the cage as a viewport overlay instead of touching the cage object's display
# settings, so a save while previewing cannot turn the temporary wireframe into file data.
_MESH_MAP_CAGE_PREVIEW_COLOR = (1.0, 0.3, 0.3, 0.9)

_mesh_map_cage_preview_handle = None
_mesh_map_cage_preview_batch = None
_mesh_map_cage_preview_key = None
_mesh_map_cage_preview_cages = ()


def _mesh_map_cage_preview_stop():
    """Remove the preview overlay and its cached batches; safe to call when nothing is shown."""
    global _mesh_map_cage_preview_handle, _mesh_map_cage_preview_batch, _mesh_map_cage_preview_key
    global _mesh_map_cage_preview_cages
    if _mesh_map_cage_preview_handle is not None:
        bpy.types.SpaceView3D.draw_handler_remove(_mesh_map_cage_preview_handle, 'WINDOW')
        _mesh_map_cage_preview_handle = None
        _mesh_map_cage_preview_batch = None
        _mesh_map_cage_preview_key = None
        _mesh_map_cage_preview_cages = ()


def _mesh_map_cage_preview_load_post(_scene, _depsgraph):
    # A new file's cages are not the ones the preview was switched on for: the draw callback would
    # quietly draw nothing, but the handler and its cache have no business outliving the file.
    _mesh_map_cage_preview_stop()


def _mesh_map_cage_preview_depsgraph_update(_scene, _depsgraph):
    """Invalidate the cached overlay when the scene updates, and drop it if its owner vanished.

    Runs outside the draw callback, so it is a safe place to touch the handler and the cache.
    Clearing the key here also catches a cage whose vertices changed without its counts or matrix
    changing; the next redraw rebuilds the batches once.
    """
    global _mesh_map_cage_preview_key
    _mesh_map_cage_preview_key = None
    if _mesh_map_cage_preview_handle is None:
        return
    try:
        if mesh_map_owner(bpy.context) is None:
            _mesh_map_cage_preview_stop()
    except Exception:
        _mesh_map_cage_preview_stop()


def _mesh_map_cage_preview_geometry_key(context):
    """The cheap identity of every previewed cage, plus the cage objects themselves.

    Only names, evaluated counts and matrices are read: no `to_mesh()` and no batches. This runs on
    every redraw, so the expensive work stays behind the key comparison in
    #_mesh_map_cage_preview_batches. Returns `(None, ())` when there is nothing to draw.
    """
    mat = mesh_map_owner(context)
    if mat is None:
        return None, ()
    depsgraph = context.evaluated_depsgraph_get()
    key_parts = []
    cages = []
    for ob in mesh_map_objects_for_material(mat):
        for source in ob.mesh_map_sources:
            if source.material != mat or source.cage is None:
                continue
            cage_eval = source.cage.evaluated_get(depsgraph)
            mesh = cage_eval.data
            if mesh is None or not mesh.vertices or not mesh.edges:
                continue
            matrix = cage_eval.matrix_world
            key_parts.append((source.cage.name, len(mesh.vertices), len(mesh.edges),
                              matrix.translation.copy(), matrix.to_quaternion().copy(),
                              matrix.to_scale().copy()))
            cages.append(source.cage)
    return (tuple(key_parts) if key_parts else None), tuple(cages)


def _mesh_map_cage_preview_batches(context):
    """The line batches of every cage of the owner material, keyed by what built them."""
    global _mesh_map_cage_preview_batch, _mesh_map_cage_preview_key
    global _mesh_map_cage_preview_cages
    key, cages = _mesh_map_cage_preview_geometry_key(context)
    if key is None:
        return None
    if key == _mesh_map_cage_preview_key and _mesh_map_cage_preview_batch is not None:
        _mesh_map_cage_preview_cages = cages
        return _mesh_map_cage_preview_batch

    import gpu
    from gpu_extras import batch as gpu_batch

    mat = mesh_map_owner(context)
    depsgraph = context.evaluated_depsgraph_get()
    batches = []
    for ob in mesh_map_objects_for_material(mat):
        for source in ob.mesh_map_sources:
            if source.material != mat or source.cage is None:
                continue
            cage_eval = source.cage.evaluated_get(depsgraph)
            mesh = cage_eval.to_mesh()
            if mesh is None:
                continue
            try:
                verts = [v.co.copy() for v in mesh.vertices]
                edges = [(e.vertices[0], e.vertices[1]) for e in mesh.edges]
            finally:
                cage_eval.to_mesh_clear()
            if not verts or not edges:
                continue
            matrix = cage_eval.matrix_world
            coords = []
            for a, b in edges:
                coords.append(matrix @ verts[a])
                coords.append(matrix @ verts[b])
            shader = gpu.shader.from_builtin('UNIFORM_COLOR')
            batches.append(gpu_batch.batch_for_shader(shader, 'LINES', {"pos": coords}))
    if not batches:
        return None
    _mesh_map_cage_preview_key = key
    _mesh_map_cage_preview_batch = batches
    _mesh_map_cage_preview_cages = cages
    return batches


def _mesh_map_cage_preview_draw():
    """The POST_VIEW overlay callback: draw every cage of the owner material as red lines."""
    global _mesh_map_cage_preview_batch, _mesh_map_cage_preview_key
    import gpu
    try:
        if mesh_map_owner(bpy.context) is None:
            # The owner can vanish between redraws. Removing a draw handler from inside its own
            # callback is unsafe, so the depsgraph handler (or the next toggle/load_post) removes
            # it; here we only skip the work and leave the viewport intact.
            return
        batches = _mesh_map_cage_preview_batches(bpy.context)
    except Exception:
        # A vanished owner or cage just stops the preview; it must never break the viewport.
        _mesh_map_cage_preview_batch = None
        _mesh_map_cage_preview_key = None
        return
    if not batches:
        return
    gpu.state.blend_set('ALPHA')
    gpu.state.line_width_set(2.0)
    shader = gpu.shader.from_builtin('UNIFORM_COLOR')
    shader.uniform_float("color", _MESH_MAP_CAGE_PREVIEW_COLOR)
    for batch in batches:
        batch.draw(shader)


class OBJECT_OT_mesh_map_cage_preview(Operator):
    """Draw the cage of this material's high-poly source as a red wireframe overlay"""

    bl_idname = "object.mesh_map_cage_preview"
    bl_label = "Preview Cage"
    bl_options = {'REGISTER'}

    @classmethod
    def poll(cls, context):
        mat = mesh_map_owner(context)
        if mat is None:
            return False
        ob = context.object
        for source in ob.mesh_map_sources if ob is not None else ():
            if source.material == mat and source.cage is not None:
                return True
        return False

    @staticmethod
    def is_running():
        return _mesh_map_cage_preview_handle is not None

    def execute(self, context):
        global _mesh_map_cage_preview_handle
        if _mesh_map_cage_preview_handle is not None:
            _mesh_map_cage_preview_stop()
            self.report({'INFO'}, "Cage preview off")
            return {'FINISHED'}
        _mesh_map_cage_preview_handle = bpy.types.SpaceView3D.draw_handler_add(
            _mesh_map_cage_preview_draw, (), 'WINDOW', 'POST_VIEW')
        self.report({'INFO'}, "Cage preview on")
        return {'FINISHED'}


def register():
    if _mesh_map_cage_preview_load_post not in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.append(_mesh_map_cage_preview_load_post)
    if _mesh_map_cage_preview_depsgraph_update not in bpy.app.handlers.depsgraph_update_post:
        bpy.app.handlers.depsgraph_update_post.append(_mesh_map_cage_preview_depsgraph_update)


def unregister():
    if _mesh_map_cage_preview_load_post in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.remove(_mesh_map_cage_preview_load_post)
    if _mesh_map_cage_preview_depsgraph_update in bpy.app.handlers.depsgraph_update_post:
        bpy.app.handlers.depsgraph_update_post.remove(_mesh_map_cage_preview_depsgraph_update)
    # A draw handler left behind would keep calling into this module after it is gone.
    _mesh_map_cage_preview_stop()


def mesh_map_summary_status(ob, mat):
    """The worst mesh-map status of `ob` for `mat`, and how many states it has."""
    states = [state for state in ob.mesh_map_states if state.material == mat]
    if len(states) == 0:
        return ('NONE', 0)
    statuses = {state.status for state in states}
    for candidate in ('BAKING', 'ERROR', 'STALE', 'VALID'):
        if candidate in statuses:
            return (candidate, len(states))
    return ('NONE', len(states))


classes = (
    OBJECT_OT_mesh_map_refresh,
    MATERIAL_OT_mesh_map_add_layer,
    MATERIAL_OT_mesh_map_slot_new,
    MATERIAL_OT_mesh_map_slot_remove,
    OBJECT_OT_mesh_map_source_add,
    OBJECT_OT_mesh_map_source_remove,
    OBJECT_OT_mesh_map_source_match_by_name,
    OBJECT_OT_mesh_map_cage_preview,
)
