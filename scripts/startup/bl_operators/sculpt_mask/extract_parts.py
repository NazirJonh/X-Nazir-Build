# SPDX-FileCopyrightText: 2026 Nazir
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
One masked sculpt object and its draft extract (a "part" of the live Extract session).

This is the geometry-building layer of the feature: every part takes its source mesh, builds the
draft object with the shape modifiers on it, and bakes or discards it. It knows nothing about the
modal input or the panel -- the operator (:mod:`extract_live`) owns those, and the part reads the
shared values and choices through the ``op`` object it is constructed with (``op.values``,
``op._threshold``, ``op.placement`` ...).

The pure mesh math (which faces to keep, how to grow them) lives in :mod:`extract_geometry`; this
module is the bpy side that turns it into objects, modifiers and polygroups.
"""

import bpy
from bpy.app.translations import pgettext_rpt
from mathutils import Matrix, Vector
from sculpt_mesh_common import FACE_SET_ATTRIBUTE, MASK_ATTRIBUTE

from .extract_geometry import (
    DEFAULT_HUE,
    EDGE_GROUP,
    extracted_mesh,
    masked_faces,
    distinct_seed,
    extract_keep,
    mesh_polygroup_hues,
)

# Above this face count, skip the on-the-fly rebuild during a drag and the viewport preview.
LARGE_MESH_FACES = 500_000
# ID name limit.
MAX_NAME_LEN = 63
# Rings of the smoothing band at the edge of the extract (the Smoothness modifier is weighted by
# it); see the ring weights in ``extract_geometry``.
EDGE_BAND_RINGS = 3

# Modifier names on the draft object.
MOD_SMOOTH = "Extract Smoothness"
MOD_INFLATE = "Extract Inflate"
MOD_THICKNESS = "Extract Thickness"
# Polygroups of an accepted extract.
POLYGROUP_OUTSIDE = 1
POLYGROUP_INSIDE = 2
POLYGROUP_RIM = 3

# Why a sculpt object could not be extracted (see `_Part.prepare`).
_NO_MASK = "NO_MASK"


class Part:
    """One masked sculpt object and its draft extract. The panel's values (``op.values``) and
    choices are shared by all parts and read through ``op``."""

    def __init__(self, op, source):
        self.op = op
        self.source_name = source.name
        self.object_name = ""
        # The extraction mesh: the object's own, or an evaluated snapshot for Multires.
        self.mesh_copy = False
        self.mesh_data = source.data
        self.base_matrix = Matrix()
        self.current_keep = None
        self.draft_weights = {}
        self.material_slots = 0
        self.slot_step = 1
        self.loops_dirty = True
        self.loops_data = None
        self.large = False
        self.size = 1.0
        self.world_size = 1.0

    # -- setup ------------------------------------------------------------------
    def prepare(self, context, source):
        """Take the extraction mesh and sizes. Returns None when ready, else a reason: a message
        or `_NO_MASK`."""
        op = self.op
        if any(m.type == 'MULTIRES' and m.show_viewport for m in source.modifiers):
            # Multires goes through an evaluated snapshot, refused when it carries no mask.
            depsgraph = context.evaluated_depsgraph_get()
            eval_mesh = bpy.data.meshes.new_from_object(
                source.evaluated_get(depsgraph), depsgraph=depsgraph)
            if eval_mesh.attributes.get(MASK_ATTRIBUTE) is None:
                bpy.data.meshes.remove(eval_mesh)
                return pgettext_rpt("Apply Multires or remove the modifier to extract")
            self.mesh_data = eval_mesh
            self.mesh_copy = True
        keep = masked_faces(self.mesh_data, op._threshold)
        if keep is None or not keep.any():
            self.release_mesh_copy()
            return _NO_MASK
        self.large = len(self.mesh_data.polygons) > LARGE_MESH_FACES
        # Sizes in the object's own units: inflate and thickness follow the masked mesh's size.
        scale = max(source.matrix_world.to_scale())
        self.size = max(max(source.dimensions), 1e-6) / max(scale, 1e-6)
        # The same size in world units, for the offset sliders.
        self.world_size = self.size * max(scale, 1e-6)
        return None

    def create_draft(self, source):
        """The draft object, grown to the current values, with the three shape modifiers on it."""
        mesh, center = self.build_mesh()
        ob = bpy.data.objects.new(self.op.part_name(self), mesh)
        # The draft stays with the source until Accept, so Cancel leaves no collection behind.
        for collection in source.users_collection:
            collection.objects.link(ob)
        self.object_name = ob.name
        self.base_matrix = source.matrix_world @ Matrix.Translation(center)
        self.current_keep = self.keep()
        self.make_modifiers(ob, mesh)
        self.apply()

    # -- object / mesh helpers --------------------------------------------------
    def object(self):
        return bpy.data.objects.get(self.object_name)

    def source(self):
        return bpy.data.objects.get(self.source_name)

    def keep(self):
        op = self.op
        return extract_keep(self.mesh_data, int(round(op.values["grow"])), op._threshold)

    def placement(self):
        """The Result choice; joining into the source is not possible from a Multires snapshot."""
        if self.op.placement == 'MESH' and self.mesh_copy:
            return 'COLLECTION'
        return self.op.placement

    def build_mesh(self):
        """The extracted mesh (grown by the current Grow, edge band by the smoothing rings) and its
        center; the object is at the origin, its origin at the extracted shape's middle."""
        import numpy as np
        op = self.op
        keep = self.keep()
        if keep is None or not np.any(keep):
            keep = masked_faces(self.mesh_data, op._threshold)
        mesh, weights = extracted_mesh(self.mesh_data, keep, EDGE_BAND_RINGS)
        if len(mesh.vertices) == 0:
            # A pathological shrink that emptied the set: fall back to the raw mask.
            bpy.data.meshes.remove(mesh)
            keep = masked_faces(self.mesh_data, op._threshold)
            mesh, weights = extracted_mesh(self.mesh_data, keep, EDGE_BAND_RINGS)
        self.draft_weights = weights
        center = sum((v.co for v in mesh.vertices), Vector()) / len(mesh.vertices)
        mesh.transform(Matrix.Translation(-center))
        return mesh, center

    def make_modifiers(self, ob, mesh):
        ob.vertex_groups.clear()
        weights = self.draft_weights
        if weights:
            group = ob.vertex_groups.new(name=EDGE_GROUP)
            # One `add` per weight value, not per vertex: each call re-locks the group's data.
            for weight in dict.fromkeys(weights.values()):
                group.add([i for i, w in weights.items() if w == weight], weight, 'REPLACE')
        smooth = ob.modifiers.get(MOD_SMOOTH) or ob.modifiers.new(MOD_SMOOTH, 'SMOOTH')
        smooth.vertex_group = EDGE_GROUP
        smooth.factor = 0.5
        inflate = ob.modifiers.get(MOD_INFLATE) or ob.modifiers.new(MOD_INFLATE, 'DISPLACE')
        inflate.direction = 'NORMAL'
        inflate.mid_level = 0.0
        thickness = ob.modifiers.get(MOD_THICKNESS) or ob.modifiers.new(MOD_THICKNESS, 'SOLIDIFY')
        thickness.offset = 1.0
        thickness.use_even_offset = True
        thickness.use_rim = True
        # Solidify tells its faces apart only by material: the shell's are moved up by the number
        # of slots and the rim's by twice that. Accept turns that into polygroups.
        self.material_slots = len(mesh.materials)
        self.slot_step = max(self.material_slots, 1)
        while len(mesh.materials) < 3 * self.slot_step:
            mesh.materials.append(None)
        thickness.material_offset = self.slot_step
        thickness.material_offset_rim = 2 * self.slot_step

    def rebuild(self):
        """Swap the draft mesh for one rebuilt from Grow. Keeps the modifiers, the object and its
        link; recomputes the edge band, materials and preview cache."""
        ob = self.object()
        source = self.source()
        if ob is None or source is None:
            return
        mesh, center = self.build_mesh()
        old = ob.data
        ob.data = mesh
        mesh.name = old.name
        bpy.data.meshes.remove(old)
        self.base_matrix = source.matrix_world @ Matrix.Translation(center)
        self.current_keep = self.keep()
        self.make_modifiers(ob, mesh)
        self.loops_dirty = True
        self.apply()

    def offset_matrix(self):
        """The Move X/Y/Z sliders as a world translation, along the global, source-local or 3D
        cursor axes (in shape sizes)."""
        op = self.op
        v = op.values
        move = Vector((v["offset_x"], v["offset_y"], v["offset_z"])) * self.world_size
        if op.offset_space == 'LOCAL':
            source = self.source()
            if source is not None:
                move = source.matrix_world.to_3x3().normalized() @ move
        elif op.offset_space == 'CURSOR':
            move = bpy.context.scene.cursor.matrix.to_3x3().normalized() @ move
        return Matrix.Translation(move)

    def apply(self):
        ob = self.object()
        if ob is None:
            return
        op = self.op
        v = op.values
        ob.matrix_world = self.offset_matrix() @ self.base_matrix @ Matrix.Scale(v["scale"], 4)
        iterations = round(v["smoothness"] * op._smooth_iterations)
        smooth = ob.modifiers[MOD_SMOOTH]
        smooth.iterations = max(iterations, 1)
        smooth.factor = 0.5
        smooth.show_viewport = iterations > 0
        ob.modifiers[MOD_INFLATE].strength = v["inflate"] * op._inflate_at_one * self.size
        thickness = ob.modifiers[MOD_THICKNESS]
        thickness.thickness = v["thickness"] * op._thickness_at_one * self.size
        thickness.show_viewport = v["thickness"] != 0.0

    # -- accept / cancel --------------------------------------------------------
    def bake(self, context):
        """Bake the modifiers and the scale into a plain mesh and place it per the Result and
        Collection choices. Returns the object, or None when the draft is gone."""
        ob = self.object()
        if ob is None:
            return None
        op = self.op
        depsgraph = context.evaluated_depsgraph_get()
        mesh = bpy.data.meshes.new_from_object(ob.evaluated_get(depsgraph), depsgraph=depsgraph)
        mesh.transform(Matrix.Scale(op.values["scale"], 4))
        self.set_polygroups(mesh)
        old = ob.data
        ob.modifiers.clear()
        ob.vertex_groups.clear()
        ob.data = mesh
        mesh.name = old.name
        bpy.data.meshes.remove(old)
        ob.matrix_world = self.offset_matrix() @ self.base_matrix
        ob.name = op.part_name(self)
        # A child (and a part joined into the mesh) stays with the source's collections.
        source = self.source()
        placement = self.placement()
        if (source is not None and placement == 'COLLECTION'
                and (op.collection_mode != 'SOURCE' or op.target_collection)):
            for collection in list(ob.users_collection):
                collection.objects.unlink(ob)
            self.link(ob, source, op.collection_mode)
        elif source is not None and placement == 'CHILD':
            ob.parent = source
            # Keeps the world transform: the parent's own transform is cancelled out.
            ob.matrix_parent_inverse = source.matrix_world.inverted()
        return ob

    def finish_source(self, context, ob):
        """After the undo push: remove / fill the extracted region from the source, and join the
        part into it. Returns the name of the object (it is gone after a join)."""
        op = self.op
        source = self.source()
        name = ob.name
        if source is None:
            return name
        # The panel works on the active object; the source is made active for the mode switches.
        context.view_layer.objects.active = source
        # Remove / fill works on a plain mesh only -- a Multires snapshot's faces do not line up
        # with the base object's, so those keep the source intact.
        if op.accept_mode in ('REMOVE', 'FILL'):
            if self.mesh_copy:
                op.report({'WARNING'}, pgettext_rpt(
                    "Remove/Fill works on plain meshes only; the source was left unchanged"))
            else:
                self.edit_source_remove(source, fill=op.accept_mode == 'FILL')
        if self.placement() == 'MESH':
            self.join_into_source(context, ob, source)
        return name

    def join_into_source(self, context, ob, source):
        """Make the extracted part a part of the source mesh (Object Mode join, back to Sculpt
        Mode). On failure the part stays a separate object next to the source."""
        import bmesh
        try:
            bpy.ops.object.mode_set(mode='OBJECT')
            # Merged by hand into this part's own source: `object.join` merges whatever is
            # selected into the active object, which with several sculpted objects can be another
            # part's.
            temp = ob.data.copy()
            try:
                temp.transform(source.matrix_world.inverted() @ ob.matrix_world)
                bm = bmesh.new()
                try:
                    bm.from_mesh(source.data)
                    bm.from_mesh(temp)
                    bm.to_mesh(source.data)
                finally:
                    bm.free()
                source.data.update()
            finally:
                bpy.data.meshes.remove(temp)
            mesh = ob.data
            bpy.data.objects.remove(ob)
            if mesh.users == 0:
                bpy.data.meshes.remove(mesh)
        except (RuntimeError, ReferenceError):
            self.op.report({'WARNING'}, pgettext_rpt("Could not join the part into the mesh"))
        finally:
            try:
                context.view_layer.objects.active = source
                bpy.ops.object.mode_set(mode='SCULPT')
            except RuntimeError:
                pass

    def release_mesh_copy(self):
        """Free the Multires evaluated snapshot taken at invoke (never the source's own mesh)."""
        if self.mesh_copy and self.mesh_data is not None:
            try:
                bpy.data.meshes.remove(self.mesh_data)
            except RuntimeError:
                pass
            self.mesh_copy = False
            self.mesh_data = None

    def edit_source_remove(self, source, fill):
        """Delete the extracted region from the plain-mesh source; with ``fill``, patch the hole
        and give the patch its own polygroup. Object Mode -> bmesh -> Sculpt Mode."""
        import bmesh
        import numpy as np
        keep = self.current_keep
        if keep is None or len(keep) != len(source.data.polygons):
            return
        keep = np.asarray(keep, dtype=bool)
        try:
            bpy.ops.object.mode_set(mode='OBJECT')
        except RuntimeError:
            return
        bm = bmesh.new()
        bm.from_mesh(source.data)
        bm.faces.ensure_lookup_table()
        removed = {f for f, k in zip(bm.faces, keep) if k}
        # The border of the removed region: edges between a removed and a surviving face. Only
        # these are patched, so open boundaries the mesh already had stay open.
        border = [e for e in bm.edges
                  if any(f in removed for f in e.link_faces)
                  and any(f not in removed for f in e.link_faces)]
        bmesh.ops.delete(bm, geom=list(removed), context='FACES')
        if fill and bm.faces:
            layer = bm.faces.layers.int.get(FACE_SET_ATTRIBUTE)
            if layer is None:
                # No polygroups yet: every face keeps the default one, the patch gets its own.
                layer = bm.faces.layers.int.new(FACE_SET_ATTRIBUTE)
                for f in bm.faces:
                    f[layer] = source.data.face_sets_color_default
            new_set = max((f[layer] for f in bm.faces), default=0) + 1
            before = set(bm.faces)
            bmesh.ops.holes_fill(bm, edges=[e for e in border if e.is_valid], sides=0)
            for f in bm.faces:
                if f not in before:
                    f[layer] = new_set
        bm.to_mesh(source.data)
        bm.free()
        source.data.update()
        try:
            bpy.ops.object.mode_set(mode='SCULPT')
        except RuntimeError:
            pass

    def link(self, ob, source, mode):
        """Link `ob` to the collection chosen by `mode` (the Collection row)."""
        op = self.op
        if mode == 'ACTIVE':
            coll = bpy.context.view_layer.active_layer_collection.collection
            coll.objects.link(ob)
            return
        if mode == 'NEW':
            name = (op.obj_name + " Collection")[:MAX_NAME_LEN]
            coll = bpy.data.collections.get(name) or bpy.data.collections.new(name)
            try:
                bpy.context.scene.collection.children.link(coll)
            except RuntimeError:
                pass  # already linked
            coll.objects.link(ob)
            return
        # SOURCE: the collection picked in the panel, else the source object's collections.
        picked = next((c for c in op._scene_collections(bpy.context)
                       if c.name == op.target_collection), None) if op.target_collection else None
        if picked is not None:
            picked.objects.link(ob)
            return
        for collection in source.users_collection:
            collection.objects.link(ob)

    def set_polygroups(self, mesh):
        """Polygroups for the baked mesh: one, or with a thickness its outside, inside and rim
        (from the material offsets the shell and rim got), and its own materials back."""
        import numpy as np
        op = self.op
        faces = len(mesh.polygons)
        materials = np.zeros(faces, dtype=np.int32)
        attr = mesh.attributes.get("material_index")
        if attr is not None:
            attr.data.foreach_get("value", materials)
        part = materials // self.slot_step
        polygroups = np.full(faces, POLYGROUP_OUTSIDE, dtype=np.int32)
        thickness = op.values["thickness"]
        if thickness != 0.0:
            polygroups[part == 1] = POLYGROUP_INSIDE
            polygroups[part == 2] = POLYGROUP_RIM
        face_sets = mesh.attributes.get(FACE_SET_ATTRIBUTE)
        if face_sets is None:
            face_sets = mesh.attributes.new(FACE_SET_ATTRIBUTE, 'INT', 'FACE')
        face_sets.data.foreach_set("value", polygroups)
        source = self.source()
        avoid = mesh_polygroup_hues(source.data) if source is not None else [DEFAULT_HUE]
        mesh.face_sets_color_default = 0
        mesh.face_sets_color_seed = distinct_seed(sorted(set(polygroups.tolist())), avoid)
        if attr is not None:
            attr.data.foreach_set("value", materials % self.slot_step)
        while len(mesh.materials) > self.material_slots:
            mesh.materials.pop()
        if thickness != 0.0 and int(op.values["thickness_loops"]) > 0:
            self.add_thickness_loops(mesh, int(op.values["thickness_loops"]))
        mesh.update()

    @staticmethod
    def add_thickness_loops(mesh, loops):
        """Cut `loops` extra edge loops across the rim wall: the rim's edges that run between the
        two surfaces (the ones whose faces are all rim faces) are subdivided."""
        import bmesh
        bm = bmesh.new()
        bm.from_mesh(mesh)
        layer = bm.faces.layers.int.get(FACE_SET_ATTRIBUTE)
        if layer is not None:
            rim = {f for f in bm.faces if f[layer] == POLYGROUP_RIM}
            across = [e for e in bm.edges if e.link_faces and all(f in rim for f in e.link_faces)]
            if across:
                bmesh.ops.subdivide_edges(bm, edges=across, cuts=loops, use_grid_fill=True)
                bm.to_mesh(mesh)
        bm.free()

    def discard(self):
        """Cancel: delete the draft and free the snapshot."""
        ob = self.object()
        if ob is not None:
            mesh = ob.data
            bpy.data.objects.remove(ob)
            if mesh.users == 0:
                bpy.data.meshes.remove(mesh)
        self.release_mesh_copy()
