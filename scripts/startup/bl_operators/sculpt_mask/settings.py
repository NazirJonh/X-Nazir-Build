# SPDX-FileCopyrightText: 2026 Nazir
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Shared settings for the live Extract feature.

A single ``bpy.types.PropertyGroup`` hung off ``Scene`` as ``sculpt_mask_extra``, so the extras
(the Extract slider values, thresholds, the Accept mode ...) are saved
in the ``.blend`` without touching DNA or the file format. Deliberately not an add-on preferences:
these are startup modules, not an add-on.

Settings only the Python UI and operators read live here, not in DNA.
The defaults declared here are the single source for the Extract tuning numbers.

Every consumer must read the group defensively -- see :func:`get` -- because it is absent when
this module is not registered.
"""

import bpy
from bpy.props import (BoolProperty, EnumProperty, FloatProperty, IntProperty,
                       PointerProperty, StringProperty)

from .extract_geometry import MASK_THRESHOLD


class SculptMaskExtraSettings(bpy.types.PropertyGroup):
    """Per-scene settings of the live Extract feature."""

    # Live Extract: values remembered between calls -- read at invoke, written at Accept.
    # Defaults match the slider neutrals, so the first run behaves as the sliders were all neutral.
    extract_scale: FloatProperty(name="Scale", default=1.0)
    extract_inflate: FloatProperty(name="Inflate", default=0.0)
    extract_thickness: FloatProperty(name="Thickness", default=0.0)
    extract_smoothness: FloatProperty(name="Smoothness", default=0.0, min=0.0, max=1.0)
    extract_offset_x: FloatProperty(name="Offset X", default=0.0)
    extract_offset_y: FloatProperty(name="Offset Y", default=0.0)
    extract_offset_z: FloatProperty(name="Offset Z", default=0.0)
    extract_offset_space: EnumProperty(
        name="Offset Space",
        description="Axes the extract's offset is measured along",
        items=[
            ('GLOBAL', "Global", "World axes"),
            ('LOCAL', "Local", "Axes of the source object"),
            ('CURSOR', "Cursor", "Axes of the 3D cursor"),
        ],
        default='GLOBAL')
    extract_placement: EnumProperty(
        name="Placement",
        description="What the extracted part becomes",
        items=[
            ('MESH', "Mesh", "Join the extracted part into the source mesh"),
            ('CHILD', "Child", "A new object parented to the source"),
            ('COLLECTION', "Collection", "A new object in the chosen collection"),
        ],
        default='COLLECTION')
    extract_grow: IntProperty(name="Grow", default=0, min=-5, max=5)
    extract_thickness_loops: IntProperty(
        name="Thickness Loops", description="Extra edge loops across the thickness of the extract",
        default=0, min=0, max=8)
    extract_target_collection: StringProperty(
        name="Target Collection",
        description="Collection the extract goes to when Collection is Source; empty keeps the "
                    "source object's own collection",
        default="")
    extract_preview: BoolProperty(
        name="Preview", description="Show the thickness loops on the extract", default=True)
    extract_collection: EnumProperty(
        name="Collection",
        description="Where to put the extracted object",
        items=[
            ('SOURCE', "Source", "Link the extract to the source object's collections"),
            ('ACTIVE', "Active", "Link the extract to the active view-layer collection"),
            ('NEW', "New", "Create a new collection for the extract"),
        ],
        default='SOURCE')
    extract_accept_mode: EnumProperty(
        name="Accept Mode",
        description="What Accept does to the source",
        items=[
            ('KEEP', "Keep", "Leave the source unchanged (default)", 0),
            ('REMOVE', "Remove", "Delete the extracted faces from the source", 1),
            ('FILL', "Fill", "Remove and patch the hole with its own polygroup", 2),
        ],
        default='KEEP')

    # Tuning thresholds (the single source of their defaults; see the module docstring).
    mask_threshold: FloatProperty(
        name="Mask Threshold",
        description="How masked a face's corners must be (all of them) to extract it",
        default=MASK_THRESHOLD, min=0.05, max=0.95)
    inflate_at_one: FloatProperty(
        name="Inflate At One",
        description="Inflate amount at slider value 1.0, as a share of the mesh size",
        default=0.05, min=0.005, max=0.5)
    thickness_at_one: FloatProperty(
        name="Thickness At One",
        description="Solidify thickness at slider value 1.0, as a share of the mesh size",
        default=0.1, min=0.005, max=1.0)
    smooth_iterations: IntProperty(
        name="Smooth Iterations",
        description="Smoothing iterations at smoothness value 1.0",
        default=30, min=1, max=100)


def settings(scene):
    """The group for `scene`, or None when the settings module is not registered."""
    return getattr(scene, "sculpt_mask_extra", None)


def get(scene, name, default=None):
    """Read one setting by name, falling back to `default` when the group or the property is
    missing -- so a disabled settings module never crashes a feature that reads it."""
    group = settings(scene)
    if group is None:
        return default
    return getattr(group, name, default)


# Whether the live Extract operator is running: the panel below is only shown meanwhile.
_extract_active = False


def set_extract_active(value):
    """Called by the Extract operator when it starts and when it ends (any way)."""
    global _extract_active
    _extract_active = bool(value)


class VIEW3D_PT_sculpt_mask_settings(bpy.types.Panel):
    """Mask & Extract settings: thresholds and defaults, shown while Extract runs."""
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "Tool"
    bl_label = "Mask & Extract Settings"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        return _extract_active and context.mode == 'SCULPT'

    def draw(self, context):
        extra = settings(context.scene)
        if extra is None:
            return
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        col = layout.column(align=True, heading="Mask")
        col.prop(extra, "mask_threshold", slider=True)
        col = layout.column(align=True, heading="Extract")
        col.prop(extra, "extract_accept_mode", text="Accept")
        col.prop(extra, "extract_collection", text="Collection")
        col.prop(extra, "extract_preview")
        col.prop(extra, "inflate_at_one", slider=True)
        col.prop(extra, "thickness_at_one", slider=True)
        col.prop(extra, "smooth_iterations", slider=True)
