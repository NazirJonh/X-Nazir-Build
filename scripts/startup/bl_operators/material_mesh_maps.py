# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""The Mesh Maps operators and helpers around the layered material's shared mesh map atlases.

Split out of `material_paint_layers.py`, which keeps the paint-layer stack operators. The Mesh
Maps panel (`bl_ui.properties_material_mesh_maps`) reads the helpers here and drives these
operators; everything goes through the layered material's active slot, the same owner the panel
and the C++ bake job pick."""

import bpy
from bpy.props import EnumProperty
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
        mat.paint_layers.active = layer
        return {'FINISHED'}


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
    OBJECT_OT_mesh_map_source_add,
    OBJECT_OT_mesh_map_source_remove,
)
