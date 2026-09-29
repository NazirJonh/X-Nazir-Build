# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Sculpt asset insert tool ("Insert Asset", last tool of the Add Cube/Add Cone group).

A click on the sculpt surface creates the chosen source with its origin snapped onto the
surface; it follows the mouse while the left mouse button is held. Releasing the button
switches to the scale phase: the object's size follows the distance between the mouse and the
object's origin (its outline grows to the cursor), and a click accepts it and moves on to the
rotation phase (the surface snap phase is a hotkey phase, entered with G or a held Space). The
rotation phase: mouse movement spins the object around its own (up) axis, and holding X, Y or Z
rotates it around the object's own axis of that name, clamped so the object never flips past the
surface plane. S, G and R jump back to scale, surface snap and rotation, a click in the rotation
phase confirms, Esc or right click cancels. Keyboard events are consumed while the tool runs, so
neighboring keys do not trigger other sculpt tools.

A collection source always inserts a single mesh object of it (the first one, or a random one).

The final insert reuses the drag-and-drop placement (`sculpt.mesh_asset_drop_batch`), so joins, face
sets, masking and the cursor gizmo behave exactly like an asset dropped from the Asset Browser.
Mirror symmetry comes from the mesh symmetry axes and is expanded by that operator itself.
Everything placed as its own object is parented to the active sculpt object.

The reusable core (placement math, placement session, final insertion) lives in
`bpy_extras.sculpt_insert`; this file is the tool's thin layer on top of it: its settings, the
event adapter (`SculptInsertModalBase`, reusable for add-on tools of the same kind), the source
resolution and the ID filter. The tool's own panels and the extended options draw live in
`bl_ui/space_toolsystem_toolbar.py`, like the rest of the UI.
"""

import random

import bpy
from bpy.props import (
    BoolProperty,
    EnumProperty,
    FloatProperty,
    FloatVectorProperty,
    PointerProperty,
)
from bpy.types import Operator, PropertyGroup
from bpy_extras import sculpt_insert
from math import degrees, radians
from mathutils import Vector

# -- Tool identity ------------------------------------------------------------------------------
# The names and paths below are the tool's identity on top of the shared core; an add-on building
# its own insert tool changes them (and subclasses `SculptInsertModalBase`), nothing else.

# RNA path (relative to the scene) of the settings struct hosting the browser target property.
# The settings live on the scene itself: a python-defined PointerProperty is ID-property backed,
# and ToolSettings (not being an ID) has no ID-property storage to put it in.
_SETTINGS_DATA_PATH = "sculpt_insert_asset"
_INSERT_OP_IDNAME = "sculpt.insert_asset"
_RESET_OP_IDNAME = "sculpt.insert_asset_correction_reset"
_FILTER_IDNAME = "SCULPT_IDF_insert_root"

# Keyboard events that are consumed while the tool runs (see `_modal`), so that a key next to the
# handled ones does not reach the sculpt/global keymaps mid-placement. Numpad, arrows and
# modifiers are left out on purpose: they keep viewport navigation working.
_KEYBOARD_EVENT_TYPES = frozenset(
    list("ABCDEFGHIJKLMNOPQRSTUVWXYZ") +
    ['ZERO', 'ONE', 'TWO', 'THREE', 'FOUR', 'FIVE', 'SIX', 'SEVEN', 'EIGHT', 'NINE',
     'SPACE', 'TAB', 'RET', 'BACK_SPACE', 'DEL', 'SEMI_COLON', 'PERIOD', 'COMMA', 'QUOTE',
     'ACCENT_GRAVE', 'MINUS', 'EQUAL', 'SLASH', 'BACK_SLASH', 'LEFT_BRACKET', 'RIGHT_BRACKET'] +
    ["F{:d}".format(i) for i in range(1, 13)]
)

# Status bar hints per phase: (icon, text) pairs drawn in order. A text-less item is an extra key
# icon for the item before it.
_STATUS_ITEMS = {
    'PLACE': (
        ('MOUSE_MOVE', "Slide on Surface"),
        ('MOUSE_LMB', "Release to Continue"),
    ),
    'SCALE': (
        ('MOUSE_MOVE', "Scale"),
        ('MOUSE_LMB', "Accept"),
        ('EVENT_G', "Move"),
        ('EVENT_SPACEKEY', "Move (Hold)"),
        ('EVENT_R', "Rotate"),
        ('EVENT_B', "Tilt"),
    ),
    'SNAP': (
        ('MOUSE_MOVE', "Move on Surface"),
        ('MOUSE_LMB', "Accept"),
        ('EVENT_S', "Scale"),
        ('EVENT_R', "Rotate"),
        ('EVENT_B', "Tilt"),
    ),
    'ROTATE': (
        ('MOUSE_MOVE', "Rotate"),
        ('EVENT_X', "Axis"),
        ('EVENT_Y', ""),
        ('EVENT_Z', ""),
        ('EVENT_CTRL', "Snap Angle"),
        ('MOUSE_LMB', "Confirm"),
        ('EVENT_S', "Scale"),
        ('EVENT_G', "Move"),
        ('EVENT_SPACEKEY', "Move (Hold)"),
        ('EVENT_B', "Tilt"),
    ),
    'TILT': (
        ('MOUSE_MOVE', "Tilt"),
        ('EVENT_X', "Axis"),
        ('EVENT_Y', ""),
        ('EVENT_Z', ""),
        ('EVENT_CTRL', "Snap Angle"),
        ('MOUSE_LMB', "Accept"),
        ('EVENT_S', "Scale"),
        ('EVENT_G', "Move"),
        ('EVENT_R', "Rotate"),
    ),
    'COMBINED': (
        ('MOUSE_MOVE', "Scale and Direction"),
        ('EVENT_CTRL', "Snap Angle"),
        ('MOUSE_LMB', "Confirm"),
        ('EVENT_S', "Scale"),
        ('EVENT_G', "Move"),
        ('EVENT_SPACEKEY', "Move (Hold)"),
        ('EVENT_R', "Rotate"),
        ('EVENT_B', "Tilt"),
    ),
}
_STATUS_CANCEL = (('MOUSE_RMB', "Cancel"), ('EVENT_ESC', ""))

# Phase entered by a click in the stepped flow: after scale go straight to rotation;
# surface move (SNAP) is only via its hotkey (G / Space hold), so a Steps insert is
# click-drag (place), scale-click, rotate-click.
_NEXT_PHASE = {'SCALE': 'ROTATE', 'SNAP': 'ROTATE', 'TILT': 'ROTATE'}
# Phase entered by its hotkey once the initial placement is done.
_PHASE_KEYS = {'S': 'SCALE', 'G': 'SNAP', 'R': 'ROTATE', 'B': 'TILT'}

# Corrections recorded with Rec, shared by this tool's inserts (kept for the session only).
_CORRECTIONS = sculpt_insert.CorrectionStore()


def _tool_insert_type():
    """The tool's Mesh/Curve switch, or the mesh default while the settings are not there."""
    settings = getattr(bpy.context.scene, _SETTINGS_DATA_PATH, None)
    return settings.insert_type if settings is not None else 'MESH'


def _is_insertable_root(obj):
    """`sculpt_insert.is_insertable_root` for this tool's insert type and active object."""
    return sculpt_insert.is_insertable_root(
        obj, _tool_insert_type(), active_object=bpy.context.active_object)


def _hierarchy_meshes(root):
    """`sculpt_insert.hierarchy_meshes` for this tool's insert type."""
    return sculpt_insert.hierarchy_meshes(root, _tool_insert_type())


def _object_poll(self, obj):
    return _is_insertable_root(obj)


class SCULPT_IDF_insert_root(bpy.types.IDFilter):
    """Offer only objects the insert tool can place in the ID browser (Blender Data source)."""

    bl_idname = _FILTER_IDNAME

    @classmethod
    def filter_id(cls, context, id):
        if not _is_insertable_root(id):
            return False
        # The browser's "Active" collection field, unless "All" is on (or no collection is chosen):
        # objects of the chosen collection and of the collections below it are offered.
        settings = getattr(context.scene, _SETTINGS_DATA_PATH, None)
        if settings is None or settings.id_browser_filter_show_all:
            return True
        collection = settings.id_browser_filter_collection
        if collection is None:
            return True
        allowed = {collection, *collection.children_recursive}
        return any(user in allowed for user in id.users_collection)

    @classmethod
    def draw_header(cls, context, layout):
        """The row the popover reserves above the grid: the collection selector and the All
        switch that `filter_id` narrows by."""
        settings = getattr(context.scene, _SETTINGS_DATA_PATH, None)
        if settings is None:
            return
        field_row = layout.row(align=True)
        field_row.prop(
            settings, "id_browser_filter_collection", text="", icon='OUTLINER_COLLECTION')

        # Right-aligned and narrower than the field, so it reads as a switch and not as a
        # second field.
        all_row = layout.row(align=True)
        all_row.alignment = 'RIGHT'
        all_row.ui_units_x = 2.5
        all_row.prop(settings, "id_browser_filter_show_all", text="All", toggle=True)


def _refresh_id_browser(_self, _context):
    """Make an open ID browser rebuild its grid after a filter changed. A Python property update
    does not reach the popover, so ask the browser itself to rebuild. Updates only fire on user
    edits, but a script can set the properties in background mode where there is no UI to
    refresh."""
    if bpy.app.background:
        return
    bpy.ops.ui.id_browser_refresh()


def _filter_collection_updated(self, context):
    """Choosing a collection turns the All switch off, so the choice takes effect."""
    if self.id_browser_filter_collection is not None:
        self.id_browser_filter_show_all = False
    _refresh_id_browser(self, context)


_PLACEMENT_JOIN = (
    'JOIN', "Join to Mesh", "Merge the inserted geometry into the active sculpt mesh",
    'AREA_JOIN', 0)
_PLACEMENT_OBJECT = (
    'OBJECT', "New Object", "Add the inserted geometry as a new object in the scene",
    'ADD', 1)
_PLACEMENT_INSTANCE = (
    'INSTANCE', "Instance (Link)", "Add a linked copy of the object that shares its mesh data",
    'LINKED', 2)
# Dynamic enum items must stay referenced, so the lists are built once.
_PLACEMENT_ITEMS_ALL = (_PLACEMENT_JOIN, _PLACEMENT_OBJECT, _PLACEMENT_INSTANCE)
_PLACEMENT_ITEMS_ASSET = (_PLACEMENT_JOIN, _PLACEMENT_OBJECT)


def _placement_items(self, _context):
    """Object assets cannot become instances, so the browser source does not offer it."""
    return _PLACEMENT_ITEMS_ASSET if self.source_mode == 'ASSET_BROWSER' else _PLACEMENT_ITEMS_ALL


def _source_mode_updated(self, context):
    """Keep the placement within the items the new source offers: object assets cannot become
    instances, so the asset browser source falls back to Join when Instance was picked."""
    allowed = {item[0] for item in _placement_items(self, context)}
    if self.placement not in allowed:
        self.placement = 'JOIN'


# The header only offers the modes without a separate scale step; "From Zero" and "Original Size"
# live in the Extra Options popover. A single enum cannot show a subset, so the header dropdown
# uses its own property mirroring `scale_mode` for the shared values.
_SCALE_MODE_QUICK = {'DEFAULT', 'FIXED', 'LAST'}


def _scale_mode_quick_updated(self, _context):
    """Header dropdown picked one of the quick modes; apply it to the real mode."""
    # Assigning runs the other property's update even for an equal value, so only write on a real
    # change: the two callbacks would otherwise call each other until the stack overflows.
    if self.scale_mode != self.scale_mode_quick:
        self.scale_mode = self.scale_mode_quick


def _scale_mode_updated(self, _context):
    """Keep the header dropdown in sync when the real mode is one it offers."""
    if self.scale_mode in _SCALE_MODE_QUICK and self.scale_mode_quick != self.scale_mode:
        self.scale_mode_quick = self.scale_mode


class SculptInsertAssetSettings(PropertyGroup):
    """Persistent state of the sculpt asset insert tool (tool settings header)."""

    insert_type: EnumProperty(
        name="Type",
        description="What kind of objects are inserted",
        items=(
            ('MESH', "Mesh", "Insert mesh objects", 'MESH_DATA', 0),
            ('CURVE', "Curve",
             "Insert curve, surface and text objects as the mesh they evaluate to (profile, "
             "bevel or geometry nodes), baked at insert time; Instance (Link) keeps them live",
             'CURVE_DATA', 1),
        ),
        default='MESH',
    )
    source_mode: EnumProperty(
        name="Source",
        description="Where the inserted geometry comes from",
        items=(
            ('OBJECT', "Object", "Insert a copy of a mesh object from the scene", 'OBJECT_DATA', 0),
            ('COLLECTION', "Collection", "Insert one mesh object of a collection",
             'OUTLINER_COLLECTION', 1),
            ('ASSET_BROWSER', "Asset Browser", "Insert an object asset from an asset library",
             'ASSET_MANAGER', 2),
        ),
        default='OBJECT',
        update=_source_mode_updated,
    )
    object: PointerProperty(
        name="Object",
        description="Mesh object to insert a copy of",
        type=bpy.types.Object,
        poll=_object_poll,
    )
    collection: PointerProperty(
        name="Collection",
        description="Collection to take the inserted mesh object from",
        type=bpy.types.Collection,
    )
    # Read by the ID browser popover, which draws a "Select Collection" row for a target struct
    # that has this property, and by `SCULPT_IDF_insert_root` to narrow the offered objects.
    id_browser_filter_collection: PointerProperty(
        name="Active",
        description="Collection whose objects (and the ones below it) are offered when All is "
                    "off",
        type=bpy.types.Collection,
        update=_filter_collection_updated,
    )
    id_browser_filter_show_all: BoolProperty(
        name="All",
        description="Offer the objects of every collection; the chosen collection is kept",
        default=True,
        update=_refresh_id_browser,
    )
    # Assigned by the ID browser popover (the same browser the paint images use); an object
    # asset picked from a library is imported by the browser itself.
    asset_object: PointerProperty(
        name="Asset Object",
        description="Object asset to insert",
        type=bpy.types.Object,
        poll=_object_poll,
    )
    insert_mode: EnumProperty(
        name="Insert",
        description="Which item is inserted on every click",
        items=(
            ('SINGLE', "Single", "Insert the selected source (the first mesh of a collection)"),
            ('RANDOM', "Random",
             "Insert a random mesh from the collection, or a random object asset from the "
             "asset library and catalog currently browsed"),
        ),
        default='SINGLE',
    )
    placement: EnumProperty(
        name="Placement",
        description="What happens to the inserted geometry",
        items=_placement_items,
        # `items` is a function, so the default is the index of the first item ("Join") rather
        # than its identifier.
        default=0,
    )
    use_face_sets: BoolProperty(
        name="Use Face Sets",
        description="Give the inserted geometry face sets of its own; when off it joins the "
                    "mesh's default face set (joined inserts only)",
        default=True,
    )
    use_face_set_color: BoolProperty(
        name="Custom Color",
        description="Color the inserted geometry's face sets with the color below instead of the "
                    "automatic one (joined inserts only)",
        default=False,
    )
    face_set_color: FloatVectorProperty(
        name="Face Set Color",
        description="Overlay color of the inserted geometry's face sets",
        subtype='COLOR',
        size=3,
        min=0.0,
        max=1.0,
        default=(0.8, 0.8, 0.8),
    )
    replace_face_sets: BoolProperty(
        name="Replace Face Sets",
        description="Collapse the inserted geometry into a single new face set instead of "
                    "keeping the source's own face sets (joined inserts only)",
        default=False,
    )
    apply_mask: BoolProperty(
        name="Mask",
        description="Mask the active mesh's pre-existing geometry so only the inserted "
                    "geometry stays sculptable (joined inserts only)",
        default=False,
    )
    use_gizmo: BoolProperty(
        name="Gizmo",
        description="Move the sculpt 3D cursor onto the insert origin afterwards, ready for "
                    "gizmo positioning",
        default=False,
    )
    scale_mode: EnumProperty(
        name="Start Scale",
        description="How the size of the inserted object is set",
        items=(
            ('DEFAULT', "Default",
             "Use the tool's default: start at zero size and grow it with the cursor"),
            ('ZERO', "From Zero",
             "Start at zero size and grow it with the cursor, so the full-size object never shows"),
            ('ORIGINAL', "Original Size",
             "Show the object at its own size, then adjust it with the cursor"),
            ('FIXED', "Fixed", "Use the scale below without a separate scale step"),
            ('LAST', "Last",
             "Keep the scale of the previous insert, without a separate scale step"),
        ),
        default='DEFAULT',
        update=_scale_mode_updated,
    )
    # Header dropdown: the subset of `scale_mode` shown there (see `_SCALE_MODE_QUICK`).
    scale_mode_quick: EnumProperty(
        name="Start Scale",
        description="How the size of the inserted object is set",
        items=(
            ('DEFAULT', "Default",
             "Use the tool's default: start at zero size and grow it with the cursor"),
            ('FIXED', "Fixed", "Use the scale below without a separate scale step"),
            ('LAST', "Last",
             "Keep the scale of the previous insert, without a separate scale step"),
        ),
        default='DEFAULT',
        update=_scale_mode_quick_updated,
    )
    interaction_mode: EnumProperty(
        name="Adjust",
        description="How size and rotation are set after the click",
        items=(
            ('STEPS', "Steps", "Set scale and rotation one after another (G moves on surface)"),
            ('COMBINED', "Combined",
             "Mouse distance from the origin sets the size and its direction spins the object, "
             "in one step"),
        ),
        default='COMBINED',
    )
    snap_mode: EnumProperty(
        name="Snap",
        description="Which point of the object touches the surface",
        items=(
            ('ORIGIN', "Origin", "Snap the object's origin onto the surface"),
            ('BOTTOM', "Bottom",
             "Snap the bottom center of the object's bounding box onto the surface"),
        ),
        default='BOTTOM',
    )
    last_scale: FloatProperty(
        name="Last Scale",
        description="Scale of the previous insert, used by the Last scale mode",
        default=1.0,
        min=sculpt_insert.SCALE_MIN,
        max=sculpt_insert.SCALE_MAX,
        options={'HIDDEN'},
    )
    fixed_scale: FloatProperty(
        name="Scale Factor",
        description="Scale applied to every inserted object in Fixed mode",
        default=1.0,
        min=sculpt_insert.SCALE_MIN,
        max=sculpt_insert.SCALE_MAX,
        soft_max=10.0,
    )
    scale_sensitivity: FloatProperty(
        name="Scale Sensitivity",
        description="How much the size grows per mouse distance from the origin; higher values "
                    "need less mouse movement",
        default=4.0,
        min=0.1,
        max=10.0,
    )
    rotation_snap_step: FloatProperty(
        name="Rotation Snap",
        description="Angle step used while Ctrl is held during rotation",
        subtype='ANGLE',
        default=radians(10.0),
        min=radians(1.0),
        max=radians(90.0),
    )
    use_correction: BoolProperty(
        name="Correction Mode",
        description="Apply the correction below to every insert (a Tilt or the fields change it), "
                    "and restore the one recorded with Rec for a source that has one",
        default=False,
    )
    use_record: BoolProperty(
        name="Rec",
        description="Record the correction of every insert for its source; inserting that source "
                    "again in Correction Mode starts from it (kept for this session only)",
        default=False,
    )
    corr_use_offset: BoolProperty(
        name="Offset Position",
        description="Apply and record the position offset of the correction",
        default=True,
    )
    corr_use_rotation: BoolProperty(
        name="Rotation",
        description="Apply and record the rotation of the correction",
        default=True,
    )
    corr_offset: FloatVectorProperty(
        name="Offset",
        description="Position offset in the surface frame of the placement point (Z is along the "
                    "surface normal)",
        subtype='TRANSLATION',
        size=3,
        default=(0.0, 0.0, 0.0),
    )
    corr_rotation: FloatVectorProperty(
        name="Rotation",
        description="Rotation about the placement point, on top of the surface alignment; the "
                    "surface point itself stays where it is",
        subtype='EULER',
        size=3,
        default=(0.0, 0.0, 0.0),
    )


# --------------------------------------------------------------------
# Source resolution


def _ensure_in_scene(context, obj):
    """Assets imported by the browser exist in the file but are not instantiated in any scene.
    The drop operator needs a view-layer base to duplicate a source, and the user should see
    what they picked, so link unlinked objects into the scene collection."""
    if obj is None or obj.users_collection:
        return
    try:
        context.scene.collection.objects.link(obj)
    except RuntimeError:
        pass


def _object_has_faces(context, obj, depsgraph=None):
    """Whether `obj` evaluates to a mesh with faces the tool can insert.

    A geometry-nodes asset can ship an empty base mesh (the real geometry is generated), and an
    object whose evaluated geometry has no faces would build an empty sculpt-overlay batch and
    break the viewport backends that cannot draw an empty index buffer. Refusing such a source
    (instead of inserting it) keeps that from crashing Blender. Curve-like sources are evaluated
    to a mesh the same way the insert bakes them. `depsgraph` may be shared by the callers, so
    checking a whole collection does not re-fetch it per object.
    """
    if obj is None or obj.type not in sculpt_insert.insert_types(_tool_insert_type()):
        return False
    if depsgraph is None:
        depsgraph = context.evaluated_depsgraph_get()
    eval_obj = obj.evaluated_get(depsgraph)
    if obj.type != 'MESH':
        try:
            mesh = eval_obj.to_mesh()
        except Exception:
            return False
        has_faces = mesh is not None and len(mesh.polygons) > 0
        eval_obj.to_mesh_clear()
        return has_faces
    mesh = eval_obj.data
    return mesh is not None and len(mesh.polygons) > 0


def _root_has_faces(context, root, depsgraph=None):
    """Whether any mesh of the `root` hierarchy evaluates to faces."""
    return any(
        _object_has_faces(context, mesh_obj, depsgraph) for mesh_obj in _hierarchy_meshes(root))


def _collection_object(context, collection, *, is_random):
    """One insertable object of the collection (nested collections included): a random one, or the
    first. Never the whole collection, so a click inserts exactly one object (with its children).
    Roots that evaluate to no faces are skipped."""
    if not is_random:
        # Single pick: stop at the first root that has geometry, so a click does not evaluate
        # every object of the collection.
        depsgraph = context.evaluated_depsgraph_get()
        for obj in collection.all_objects:
            if _is_insertable_root(obj) and _root_has_faces(context, obj, depsgraph):
                return obj
        return None
    roots = [
        obj for obj in collection.all_objects
        if _is_insertable_root(obj) and _root_has_faces(context, obj)
    ]
    return random.choice(roots) if roots else None


def _random_asset_object(context, settings):
    """Import (or reuse) a random object asset from whatever the ID browser currently shows and
    return it. The C++ operator publishes the pick into `settings.asset_object` like a browser
    click would; the object diff catches a freshly imported data-block. Returns (object,
    is_imported): a freshly imported random pick is a throwaway carrier the caller removes after
    the insert, so repeated random inserts do not pile up objects in the scene."""
    before = set(bpy.data.objects.keys())
    bpy.ops.ui.id_browser_random_asset(
        id_type='OBJECT',
        data_path=_SETTINGS_DATA_PATH,
        prop="asset_object",
        # The same narrowing the browser applies: without it the pick comes from every object
        # data-block in the file (cameras, lights, the sculpt object itself, ...).
        filter_type=_FILTER_IDNAME)
    imported = [
        bpy.data.objects[name] for name in bpy.data.objects.keys() if name not in before
    ]

    obj = settings.asset_object
    if obj is None and imported:
        obj = imported[0]
    if obj is not None and (not _hierarchy_meshes(obj) or not _root_has_faces(context, obj)):
        # A pick without any mesh (or without geometry) cannot be inserted. Drop the carrier when
        # the browser just imported it, so it does not linger in the file; an already-present
        # local object is kept, like the drop operator's keep_source does.
        if obj in imported:
            bpy.data.objects.remove(obj, do_unlink=True)
        return None, False
    if obj is None:
        return None, False
    for mesh_obj in _hierarchy_meshes(obj):
        _ensure_in_scene(context, mesh_obj)
    return obj, obj in imported


def _resolve_source(context, settings):
    """Return (object, errors, is_temporary) for one insert: `object` is the mesh object to
    duplicate. `errors` holds a message when nothing can be inserted. `is_temporary` marks an
    object imported just for this insert, to be removed once it is done."""
    errors = []
    is_random = settings.insert_mode == 'RANDOM'

    if settings.source_mode == 'COLLECTION':
        collection = settings.collection
        if collection is None:
            errors.append("Pick a collection in the tool settings first")
            return None, errors, False
        obj = _collection_object(context, collection, is_random=is_random)
        if obj is None:
            errors.append("The collection has no mesh objects with geometry to pick from")
        return obj, errors, False

    if settings.source_mode == 'ASSET_BROWSER':
        if is_random:
            obj, is_temporary = _random_asset_object(context, settings)
            if obj is None:
                errors.append("The browsed asset source has no mesh object assets to pick from")
            return obj, errors, is_temporary
        obj = settings.asset_object
        if obj is None:
            errors.append("Pick an object asset first (right click opens the asset browser)")
            return None, errors, False
        if not _hierarchy_meshes(obj):
            errors.append("The picked asset has no mesh to insert")
            return None, errors, False
        if not _root_has_faces(context, obj):
            errors.append("The picked asset evaluates to no geometry to insert")
            return None, errors, False
        for mesh_obj in _hierarchy_meshes(obj):
            _ensure_in_scene(context, mesh_obj)
        return obj, errors, False

    obj = settings.object
    if obj is None:
        errors.append("Pick an object in the tool settings first")
    elif not _hierarchy_meshes(obj):
        errors.append("The picked object has no mesh to insert")
        obj = None
    elif not _root_has_faces(context, obj):
        errors.append("The picked object evaluates to no geometry to insert")
        obj = None
    return obj, errors, False


# --------------------------------------------------------------------
# The modal operator


def _status_draw_fn(phase, session):
    """Status bar draw function (see `WorkSpace.status_text_set`) for `phase`. The live rotation
    angle is shown while rotating/tilting, like other transform tools."""
    def draw(self, _context):
        layout = self.layout
        if phase in {'ROTATE', 'TILT'} and session is not None:
            layout.label(text="{:d}\u00b0".format(round(degrees(session.rotation_display))))
        for icon, text in _STATUS_ITEMS[phase] + _STATUS_CANCEL:
            layout.label(text=text, icon=icon)
    return draw


_AXIS_DIRECTIONS = {
    'X': Vector((1.0, 0.0, 0.0)),
    'Y': Vector((0.0, 1.0, 0.0)),
    'Z': Vector((0.0, 0.0, 1.0)),
}
# Fallback axis colors, used only if the theme cannot be read.
_AXIS_COLORS_FALLBACK = {
    'X': (1.0, 0.2, 0.2),
    'Y': (0.2, 1.0, 0.2),
    'Z': (0.2, 0.4, 1.0),
}


def _axis_colors():
    """The standard axis colors from the theme (#ThemeUserInterface.axis_x/y/z)."""
    try:
        theme = bpy.context.preferences.themes[0].user_interface
        return {'X': tuple(theme.axis_x), 'Y': tuple(theme.axis_y), 'Z': tuple(theme.axis_z)}
    except (AttributeError, IndexError, TypeError):
        return _AXIS_COLORS_FALLBACK


def _axes_draw(session):
    """Viewport draw callback: show the axis the Rotation/Tilt hotkeys currently work with, so the
    user can tell which axis is active. Only the axis picked with X/Y/Z is drawn (none while no
    key is held). The axes are the inserted object's own axes (surface alignment plus the user's
    rotation), not the world axes. The line is extended far beyond the object so it reads as an
    infinite axis, like the transform gizmo."""
    if session is None or session.loc is None:
        return
    if session.phase == 'ROTATE':
        axis = session.axis_key
    elif session.phase == 'TILT':
        axis = session.tilt_axis
    else:
        return
    if axis is None:
        return
    # The object-local frame: local +Z is the surface normal, then the user's rotation on top.
    direction = (session.user_rot @ session.base_rot) @ _AXIS_DIRECTIONS[axis]

    import gpu
    from gpu_extras.batch import batch_for_shader

    origin = session.loc
    direction = direction.normalized()
    # Long enough to read as an infinite axis across the viewport.
    length = 1.0e5
    shader = gpu.shader.from_builtin('POLYLINE_UNIFORM_COLOR')
    gpu.state.blend_set('ALPHA')
    gpu.state.depth_test_set('NONE')
    shader.uniform_float("viewportSize", gpu.state.viewport_get()[2:])
    shader.uniform_float("lineWidth", 3.0)
    shader.uniform_float("color", (*_axis_colors()[axis], 1.0))
    batch = batch_for_shader(
        shader, 'LINES', {"pos": [origin - direction * length, origin + direction * length]})
    batch.draw(shader)
    gpu.state.depth_test_set('LESS_EQUAL')
    gpu.state.blend_set('NONE')


class SCULPT_OT_insert_asset_correction_reset(Operator):
    bl_idname = _RESET_OP_IDNAME
    bl_label = "Reset"
    bl_description = "Set the correction offset and rotation back to zero"
    bl_options = {'REGISTER', 'INTERNAL'}

    @classmethod
    def poll(cls, context):
        return hasattr(context.scene, _SETTINGS_DATA_PATH)

    def execute(self, context):
        settings = getattr(context.scene, _SETTINGS_DATA_PATH)
        settings.property_unset("corr_offset")
        settings.property_unset("corr_rotation")
        return {'FINISHED'}


class SculptInsertModalBase(Operator):
    """Base class for a modal sculpt insert tool: it maps the events of the "click, drag,
    release, adjust, click" interaction onto a `bpy_extras.sculpt_insert.InsertSession` and
    cleans up after it.

    Subclasses provide `get_settings` and `get_source`, and may override `on_phase_changed`,
    `on_confirm` and `on_cancel`. `invoke` calls `begin_insert` (after checking
    `sculpt_insert.check_sculpt_target`) and then keeps the operator modal. Not registered
    itself: it has no operator identity of its own.
    """

    # Corrections recorded per source and shared between this tool's inserts; None (the
    # default) runs the session without correction recording.
    correction_store = None
    # The session is created per run in `begin_insert`.
    session = None
    # Viewport draw handler showing the Rotation/Tilt axes, added while the run is modal.
    _axes_handle = None

    def _add_axes_draw(self, context):
        if self._axes_handle is None and context.space_data is not None:
            self._axes_handle = bpy.types.SpaceView3D.draw_handler_add(
                _axes_draw, (self.session,), 'WINDOW', 'POST_VIEW')

    def _remove_axes_draw(self):
        if self._axes_handle is not None:
            bpy.types.SpaceView3D.draw_handler_remove(self._axes_handle, 'WINDOW')
            self._axes_handle = None

    def get_settings(self, context):
        """The settings object handed to the session (the `SessionSettings` protocol)."""
        raise NotImplementedError

    def get_source(self, context, settings):
        """The object to insert: return (object, errors, is_temporary), where `errors` holds a
        message when nothing can be inserted and `is_temporary` marks an object imported just
        for this insert (the session removes it when it is done)."""
        raise NotImplementedError

    def on_phase_changed(self, context, phase):
        """Called after the session switched to `phase` (and once with the initial 'PLACE'
        when the modal run starts)."""

    def on_confirm(self, context):
        """Confirm the placement: insert the session's copy set as one undo step. Returns the
        operator's return value."""
        session = self.session
        if not session.matrices:
            self._finish_cancel(context)
            return {'CANCELLED'}
        result = sculpt_insert.insert(
            context, session, self.get_settings(context), undo_message=self.bl_label)
        if result.message is not None:
            self.report(result.message_type, result.message)
        self._finish(context)
        return {result.status}

    def on_cancel(self, context):
        """Cancel the placement: remove the previews and the temporary carriers."""
        self._finish_cancel(context)

    # -- run lifecycle -----------------------------------------------------------------

    def begin_insert(self, context, event):
        """Create the session and place the source under the mouse. Returns False (with
        everything cleaned up) when nothing can be placed; `invoke` then returns CANCELLED."""
        self.session = sculpt_insert.InsertSession(context.active_object, self.correction_store)
        try:
            placed = self._begin_placement(context, event)
        except Exception:
            # Never leave preview objects behind on an unexpected failure.
            import traceback
            traceback.print_exc()
            self._finish_cancel(context)
            return False
        return placed

    def _begin_placement(self, context, event):
        settings = self.get_settings(context)
        session = self.session
        source, errors, is_temporary = self.get_source(context, settings)
        if is_temporary and source is not None:
            session.temp_source_name = source.name
        if errors:
            self.report({'ERROR'}, errors[0])
            self._finish_cancel(context)
            return False

        coord = (event.mouse_region_x, event.mouse_region_y)
        self._last_mouse = coord
        if not session.begin(context, source, settings, coord, is_temporary=is_temporary):
            self.report({'ERROR'}, session.errors[0])
            self._finish_cancel(context)
            return False
        # The plane stays visible while the surface is chosen (phase PLACE), like the interactive
        # Add Cube base step; `_enter_phase` hides it once the insert moves on.
        self._set_plane(context, visible=True)
        return True

    def _set_plane(self, context, *, visible):
        """Mirror the aiming-plane visibility into the window manager flag read by the placement
        gizmo's draw prepare (see `WIDGETGROUP_placement_draw_prepare` in view3d_placement.cc).
        The flag is runtime window-manager state: it tags no depsgraph update and is not part of
        undo steps. It is stored inverted (hide) so a zero-initialized window manager shows the
        plane."""
        context.window_manager.sculpt_insert_hide_plane = not visible

    def _enter_phase(self, context, event, phase, *, follow_cursor=False):
        """Switch the session to `phase` and report the change."""
        coord = (event.mouse_region_x, event.mouse_region_y)
        self._last_mouse = coord
        settings = self.get_settings(context)
        self.session.enter_phase(
            context, settings, coord, phase, follow_cursor=follow_cursor)
        # The placement plane only aids choosing the surface; it hides from the scale/rotate
        # stages on (like the Add Cube depth step).
        self._set_plane(context, visible=(phase == 'PLACE'))
        self.on_phase_changed(context, phase)

    def _finish(self, context):
        """End the run: drop the status hints and clear the session state."""
        self._remove_axes_draw()
        context.workspace.status_text_set(None)
        # Back to the idle tool: the plane aims the next insert again.
        self._set_plane(context, visible=True)
        context.area.tag_redraw()
        self.session.reset()

    def _finish_cancel(self, context):
        self.session.clear_previews()
        self.session.remove_temp_sources()
        self._finish(context)

    # -- event mapping -----------------------------------------------------------------

    def modal(self, context, event):
        try:
            return self._modal(context, event)
        except Exception:
            # Never leave preview objects behind on an unexpected failure.
            import traceback
            traceback.print_exc()
            self._finish_cancel(context)
            return {'CANCELLED'}

    def _modal(self, context, event):
        session = self.session

        # Origin/Bottom switch updates the snap anchor live, without resetting
        # the mesh, scale or rotation.
        try:
            session.sync_snap_mode(context, self.get_settings(context))
        except Exception:
            pass

        if event.type in {'RIGHTMOUSE', 'ESC'}:
            if event.value == 'PRESS':
                self.on_cancel(context)
                return {'CANCELLED'}
            return {'RUNNING_MODAL'}

        if event.type == 'LEFTMOUSE':
            if event.value == 'PRESS':
                if session.phase == 'COMBINED' or (session.phase == 'ROTATE' and
                                                   not session.combined):
                    return self.on_confirm(context)
                if session.combined:
                    # A single-purpose phase entered by its hotkey hands back to the gesture.
                    self._enter_phase(context, event, 'COMBINED', follow_cursor=True)
                elif session.phase in _NEXT_PHASE:
                    self._enter_phase(context, event, _NEXT_PHASE[session.phase])
            elif event.value == 'RELEASE' and session.phase == 'PLACE' and session.loc is not None:
                # A fixed scale needs no scale step, and the initial drag already was the
                # surface snap, so the fixed flow goes straight to rotation.
                if session.combined:
                    first = 'COMBINED'
                elif self.get_settings(context).scale_mode in {'FIXED', 'LAST'}:
                    first = 'ROTATE'
                else:
                    first = 'SCALE'
                self._enter_phase(context, event, first, follow_cursor=True)
            return {'RUNNING_MODAL'}

        if event.type in {'MOUSEMOVE', 'INBETWEEN_MOUSEMOVE'}:
            mouse = (event.mouse_region_x, event.mouse_region_y)
            dx = mouse[0] - self._last_mouse[0]
            self._last_mouse = mouse
            if session.phase in {'PLACE', 'SNAP'}:
                session.move_to_mouse(context, self.get_settings(context), mouse)
            elif session.phase in {'SCALE', 'COMBINED'}:
                session.scale_from_mouse(
                    context, self.get_settings(context), mouse, ctrl=event.ctrl)
            elif session.phase == 'TILT':
                session.tilt_from_mouse(
                    context, self.get_settings(context), dx, ctrl=event.ctrl)
            else:
                session.rotate_from_mouse(
                    context, self.get_settings(context), dx, ctrl=event.ctrl)
            context.area.tag_redraw()
            return {'RUNNING_MODAL'}

        if event.type in _KEYBOARD_EVENT_TYPES:
            # Every keyboard event is consumed, handled or not: a neighboring key would otherwise
            # run its sculpt/global shortcut (brush switch, undo, mode change) mid-placement, the
            # same way the asset drag-and-drop preview swallows them while hovering.
            if event.type == 'SPACE' and session.phase != 'PLACE' and session.loc is not None:
                # Holding Space moves the object like G and hands back to the previous phase
                # (with its cursor-following behavior) once released.
                if event.value == 'PRESS' and not event.is_repeat and session.space_return is None:
                    session.space_return = (session.phase, session.follow_cursor)
                    self._enter_phase(context, event, 'SNAP')
                elif event.value == 'RELEASE' and session.space_return is not None:
                    phase, follow_cursor = session.space_return
                    session.space_return = None
                    self._enter_phase(context, event, phase, follow_cursor=follow_cursor)
            elif event.type in _PHASE_KEYS and event.value == 'PRESS' and not event.is_repeat \
                    and session.phase != 'PLACE' and session.loc is not None:
                self._enter_phase(context, event, _PHASE_KEYS[event.type])
            elif event.type in {'X', 'Y', 'Z'} and session.phase == 'TILT':
                if event.value == 'PRESS' and not event.is_repeat:
                    # The chosen axis stays chosen; each axis continues from its own angle.
                    session.set_tilt_axis(event.type)
                    context.area.tag_redraw()
            elif event.type in {'X', 'Y', 'Z'} and session.phase == 'ROTATE':
                if event.value == 'PRESS' and not event.is_repeat:
                    session.set_rotate_axis(event.type)
                    self._last_mouse = (event.mouse_region_x, event.mouse_region_y)
                    context.area.tag_redraw()
                elif event.value == 'RELEASE':
                    session.set_rotate_axis(None)
                    context.area.tag_redraw()
            return {'RUNNING_MODAL'}

        # Navigation (mouse wheel, middle mouse, numpad, ...) and modifiers pass through.
        return {'PASS_THROUGH'}


class SCULPT_OT_insert_asset(SculptInsertModalBase):
    bl_idname = _INSERT_OP_IDNAME
    bl_label = "Insert Asset"
    bl_description = (
        "Insert an object or asset on the sculpt surface: click to snap its point to the "
        "surface and drag to slide it, release and move the mouse to set the size, click to "
        "accept it, then rotate it (X/Y/Z rotate around the object's own axes) and click again "
        "to confirm. S, G and R return to Scale, Surface Move and Rotate. Esc or right click "
        "cancels")
    bl_options = {'REGISTER'}

    correction_store = _CORRECTIONS

    invoke_browser: BoolProperty(
        name="Open Asset Browser",
        description="Open the asset browser for picking the insert source instead of placing",
        options={'SKIP_SAVE'},
    )

    def get_settings(self, context):
        return getattr(context.scene, _SETTINGS_DATA_PATH)

    def get_source(self, context, settings):
        return _resolve_source(context, settings)

    def on_phase_changed(self, context, phase):
        context.workspace.status_text_set(_status_draw_fn(phase, self.session))

    @classmethod
    def poll(cls, context):
        if context.area is not None and context.area.type != 'VIEW_3D':
            return False
        obj = context.active_object
        return (obj is not None and obj.type == 'MESH' and obj.mode == 'SCULPT' and
                context.region_data is not None)

    def invoke(self, context, event):
        if self.invoke_browser:
            _open_source_browser(settings=self.get_settings(context))
            return {'FINISHED'}

        if not sculpt_insert.check_sculpt_target(context):
            self.report(
                {'ERROR'},
                "Inserting assets is only supported on plain meshes (no multires or dynamic "
                "topology)")
            return {'CANCELLED'}

        if not self.begin_insert(context, event):
            return {'CANCELLED'}

        context.window_manager.modal_handler_add(self)
        self._add_axes_draw(context)
        self.on_phase_changed(context, self.session.phase)
        context.area.tag_redraw()
        return {'RUNNING_MODAL'}


def _open_source_browser(*, settings):
    """Open the ID browser popover for the current source (object, collection or asset).

    A separate operator button (not the template's built-in popover button), so the user
    can put it into Quick Favorites or assign a hotkey to it.
    """
    if settings.source_mode == 'OBJECT':
        bpy.ops.ui.id_browser_open_target(
            data_path=_SETTINGS_DATA_PATH, prop="object", filter_type=_FILTER_IDNAME)
    elif settings.source_mode == 'COLLECTION':
        bpy.ops.ui.id_browser_open_target(
            data_path=_SETTINGS_DATA_PATH, prop="collection")
    else:
        bpy.ops.ui.id_browser_open_target(
            data_path=_SETTINGS_DATA_PATH, prop="asset_object", filter_type=_FILTER_IDNAME)


class SCULPT_OT_insert_asset_open_browser(Operator):
    bl_idname = "sculpt.insert_asset_open_browser"
    bl_label = "Browser"
    bl_description = (
        "Open the browser for the current insert source (object, collection or asset), "
        "so it can be picked without using the header button")
    bl_options = {'REGISTER'}

    @classmethod
    def poll(cls, context):
        return hasattr(context.scene, _SETTINGS_DATA_PATH)

    def execute(self, context):
        settings = getattr(context.scene, _SETTINGS_DATA_PATH, None)
        if settings is None:
            return {'CANCELLED'}
        _open_source_browser(settings=settings)
        return {'FINISHED'}


# --------------------------------------------------------------------
# Registration

classes = (
    SCULPT_IDF_insert_root,
    SculptInsertAssetSettings,
    SCULPT_OT_insert_asset_correction_reset,
    SCULPT_OT_insert_asset,
    SCULPT_OT_insert_asset_open_browser,
)


def register():
    # The classes themselves are registered by `bl_operators.register` (via `classes`).
    bpy.types.Scene.sculpt_insert_asset = PointerProperty(
        name="Sculpt Insert Asset",
        description="State of the sculpt asset insert tool",
        type=SculptInsertAssetSettings,
    )


def unregister():
    if hasattr(bpy.types.Scene, "sculpt_insert_asset"):
        del bpy.types.Scene.sculpt_insert_asset
