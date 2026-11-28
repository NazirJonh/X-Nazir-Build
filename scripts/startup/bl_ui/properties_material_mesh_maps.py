# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""The Mesh Maps panel of the Layer Material tab: the bake-all buttons, the per-map rows and
the shared bake settings.

Split out of `properties_material.py`, which keeps the rest of the Layer Material tab. This module
registers its panel through `classes` and is listed in `bl_ui/__init__.py` after
`properties_material`, like `properties_material_paint_layers.py`; the tab's base class lives in
that module so neither file imports the other."""

import bpy

from bpy.types import Panel

from bl_operators.material_mesh_maps import (
    OBJECT_OT_mesh_map_cage_preview,
    mesh_map_objects_for_material,
    mesh_map_summary_status,
    mesh_map_type_items,
)
from bl_ui.properties_material_paint_layers import LayerMaterialButtonsPanel


# The status keys come from the RNA enum (#ObjectMeshMapState.status); the label and icon pairs
# stay a UI mapping of their own. Unknown statuses fall back to a placeholder rather than raising,
# so a status added to RNA later does not break startup.
_MESH_MAP_STATUS_LABEL_ICON = {
    'NONE': ("Not baked", 'DOT'),
    'VALID': ("Valid", 'CHECKMARK'),
    'STALE': ("Stale", 'FILE_REFRESH'),
    'BAKING': ("Baking", 'TIME'),
    'ERROR': ("Error", 'ERROR'),
}
_MESH_MAP_STATUS_FALLBACK = ("Unknown", 'QUESTION')
_MESH_MAP_STATUS = {
    item.identifier: _MESH_MAP_STATUS_LABEL_ICON.get(item.identifier, _MESH_MAP_STATUS_FALLBACK)
    for item in bpy.types.ObjectMeshMapState.bl_rna.properties['status'].enum_items
    if item.identifier != ''
}


def mesh_map_status_ui(status):
    """The (label, icon) pair of a status, with a fallback for one added to RNA later."""
    return _MESH_MAP_STATUS.get(status, _MESH_MAP_STATUS_FALLBACK)


class LAYER_MATERIAL_PT_mesh_maps(LayerMaterialButtonsPanel, Panel):
    """Mesh map atlases of the stack's owner material and the active object's bake state.

    Drawn in the Layer Material tab, whose context material is the owner (the active slot) the bake
    operator bakes. A mesh without any UV layer cannot be baked: the map is sampled through the
    active UV of the mesh, so the panel warns instead of offering a bake that the job would refuse.
    """

    bl_idname = "LAYER_MATERIAL_PT_mesh_maps"
    bl_label = "Mesh Maps"
    bl_options = {'DEFAULT_CLOSED'}
    # Sits right above Output Texture, after the settings panels.
    bl_order = 1

    @classmethod
    def poll(cls, context):
        if cls._object_mask_editing(context):
            return False
        ob = context.object
        return (
            cls._owner_material(context) is not None and
            ob.type == 'MESH' and ob.data is not None
        )

    def _draw_actions(self, layout):
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

    def _draw_settings(self, layout, mat):
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
            col.prop(settings, "margin_type")
            col.prop(settings, "ao_distance")
            col.prop(settings, "edge_radius")
            col.prop(settings, "thickness_rays")
            col.prop(settings, "position_normalization")
            col.prop(settings, "use_directx_normal")
            col.prop(settings, "curvature_method")
            if settings.curvature_method == 'NORMAL_FILTER':
                col.prop(settings, "curvature_radius")
                col.prop(settings, "curvature_scale")
            col.prop(settings, "dilate_iterations")

            # Quality: the settings that act on a subset of the maps only, grouped under their own
            # sub-header with the scope spelled out in the labels. The panel is shared by every
            # map type, so the controls stay visible instead of hiding per type.
            quality = settings_layout.column(align=True)
            quality.label(text="Quality")
            quality.prop(settings, "supersampling", text="Supersampling (Render Pairs)")
            quality.prop(settings, "use_global_ao", text="Global AO (AO Only)")

    def _draw_high_poly(self, layout, context, ob, mat):
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
                if not mat.mesh_map_settings.use_global_ao:
                    hp.label(
                        text="AO is computed per high-poly object; pieces do not occlude each "
                        "other")
                hp.prop(source, "cage", text="Cage")
                hp.prop(source, "cage_extrusion")
                hp.prop(source, "max_ray_distance")
                hp.prop(source, "max_rear_ray_distance")
                hp.prop(source, "use_ignore_backfacing")
                if source.cage is not None:
                    hp.prop(source, "skew")
                hp.operator(
                    "object.mesh_map_cage_preview",
                    text="Preview Cage",
                    icon='XRAY',
                    depress=OBJECT_OT_mesh_map_cage_preview.is_running(),
                )
                if source.resolved_object_count() == 0:
                    hp.label(text="The source resolves to no mesh objects", icon='ERROR')
                elif source.cage is not None and not source.cage_matches(
                        depsgraph=context.view_layer.depsgraph):
                    hp.label(
                        text="The cage must have the same face count as the low-poly object",
                        icon='ERROR')
                hp.operator("object.mesh_map_source_remove", text="Remove", icon='X')
                hp.operator(
                    "object.mesh_map_source_match_by_name",
                    text="Match by Name",
                    icon='LINKED',
                )

    def _draw_map_row(self, layout, mat, map_type, type_label, states):
        """One expandable baker per map type: a header with the primary's status and a quick bake
        over every copy, then one sub-row per atlas copy with its own bake and overrides."""
        primary_state = states.get((map_type, 0))
        status = primary_state.status if primary_state is not None else 'NONE'
        status_label, status_icon = mesh_map_status_ui(status)

        baker_header, baker = layout.panel(
            "mesh_map_baker_" + map_type.lower(), default_closed=(status == 'NONE'))
        baker_header.label(text=type_label)
        baker_header.label(text=status_label, icon=status_icon)
        baker_header.operator_context = 'INVOKE_DEFAULT'
        bake = baker_header.operator("object.mesh_map_bake", text="", icon='RENDER_STILL')
        bake.type = map_type
        bake.material_index = 0
        baker_header.operator_context = 'EXEC_DEFAULT'

        if not baker:
            return

        type_slots = [slot for slot in mat.mesh_map_slots if slot.type == map_type]
        if not type_slots:
            # No slot yet: the type's bake still works (it ensures the primary). Offer the
            # primary layer so a fresh material is usable without a bake first.
            placeholder = baker.row(align=True)
            placeholder.label(text=type_label, icon='DOT')
            placeholder.operator(
                "material.mesh_map_add_layer", text="Add Layer", icon='ADD').type = map_type
        for type_slot in type_slots:
            slot_state = states.get((map_type, type_slot.uid))
            slot_status = slot_state.status if slot_state is not None else 'NONE'
            slot_status_label, slot_status_icon = mesh_map_status_ui(slot_status)
            display = type_slot.name if type_slot.name else (
                type_label + ("" if type_slot.uid == 0 else " (copy)"))

            slot_header, slot_body = baker.panel(
                "mesh_map_slot_%s_%d" % (map_type.lower(), type_slot.uid),
                default_closed=(type_slot.uid != 0))
            slot_header.label(
                text=display,
                icon='IMAGE_DATA' if type_slot.image is not None else 'DOT',
            )
            slot_header.label(text=slot_status_label, icon=slot_status_icon)
            slot_header.operator_context = 'INVOKE_DEFAULT'
            slot_bake = slot_header.operator(
                "object.mesh_map_bake_all", text="", icon='RENDER_STILL')
            slot_bake.types = {map_type}
            slot_bake.slot_uid = type_slot.uid
            slot_bake.material_index = 0
            slot_header.operator_context = 'EXEC_DEFAULT'
            if type_slot.uid != 0:
                remove_slot = slot_header.operator(
                    "material.mesh_map_slot_remove", text="", icon='X', emboss=False)
                remove_slot.type = map_type
                remove_slot.slot_uid = type_slot.uid

            if slot_body:
                # The per-map overrides: a copy can bake at its own resolution, sample count,
                # merge margin and denoise state without touching the others.
                slot_body.use_property_split = True
                slot_body.prop(type_slot, "name")
                override_col = slot_body.column(align=True)
                override_col.prop(type_slot, "use_resolution_override")
                if type_slot.use_resolution_override:
                    override_col.prop(type_slot, "resolution_override")
                override_col.prop(type_slot, "use_samples_override")
                if type_slot.use_samples_override:
                    override_col.prop(type_slot, "samples_override")
                override_col.prop(type_slot, "use_margin_override")
                if type_slot.use_margin_override:
                    override_col.prop(type_slot, "margin_override")
                override_col.prop(type_slot, "use_denoise_override")
                if type_slot.use_denoise_override:
                    override_col.prop(type_slot, "denoise_override")

                action_row = slot_body.row(align=True)
                add_layer = action_row.operator(
                    "material.mesh_map_add_layer", text="Add Layer", icon='ADD')
                add_layer.type = map_type
                add_layer.slot_uid = type_slot.uid

        add_copy = baker.operator(
            "material.mesh_map_slot_new", text="Add Copy", icon='ADD')
        add_copy.type = map_type

        clear_row = baker.row(align=True)
        clear_one = clear_row.operator("object.mesh_map_clear", text="Clear", icon='X')
        clear_one.types = {map_type}

    def _draw_objects(self, layout, mat):
        objects = mesh_map_objects_for_material(mat)
        if len(objects) > 1:
            box = layout.box()
            box.label(text="Objects using this material", icon='OUTLINER_OB_MESH')
            for other in objects:
                status, count = mesh_map_summary_status(other, mat)
                status_label, status_icon = mesh_map_status_ui(status)
                line = box.row(align=True)
                line.label(
                    text="{}: {} ({} map(s))".format(other.name, status_label, count),
                    icon=status_icon,
                )
                # D: bakes through the mesh's active UV, so an object without one is unusable
                # for the shared atlas.
                if other.data is not None and other.data.uv_layers.active is None:
                    line.label(text="no active UV", icon='ERROR')

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        mat = self._owner_material(context)
        ob = context.object

        # D: reads the mesh's active UV; there is no per-material named-UV layer to pick.
        if ob.data.uv_layers.active is None:
            layout.label(text="The object has no UV map", icon='ERROR')

        self._draw_actions(layout)
        self._draw_settings(layout, mat)
        self._draw_high_poly(layout, context, ob, mat)

        states = {(state.type, state.slot_uid): state
                  for state in ob.mesh_map_states if state.material == mat}
        for map_type, type_label, _description in mesh_map_type_items(mat):
            self._draw_map_row(layout, mat, map_type, type_label, states)

        if any(state.status == 'BAKING' for state in states.values()):
            layout.label(text="Baking...", icon='FILE_REFRESH')

        self._draw_objects(layout, mat)

        layout.operator("object.mesh_map_refresh", text="Refresh Status", icon='FILE_REFRESH')


# Registered by `bl_ui/__init__.py` through `_modules`, like every other startup UI module.
classes = (
    LAYER_MATERIAL_PT_mesh_maps,
)
