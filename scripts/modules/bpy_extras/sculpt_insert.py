# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Reusable core of the sculpt surface insert tools.

Everything a tool needs to place object hierarchies onto the surface of a sculpt mesh and to
insert them there: the placement math, the sources (object hierarchies and baked curves), the
correction storage, the placement session (`InsertSession`, a state machine over the phases
'PLACE', 'SCALE', 'SNAP', 'ROTATE', 'TILT' and 'COMBINED' that also manages the preview
objects) and the final insertion (`insert`, one undo step through the batch form of the shared
drag-and-drop operator, `sculpt.mesh_asset_drop_batch`).

The module has no UI and takes no events: session methods take plain data (context, the
settings object, mouse coordinates and flags), and nothing here reads a specific settings path.
The settings follow the `SessionSettings` protocol -- typically the tool's own PropertyGroup,
which carries the same attributes -- so several tools (built-in or add-on) can share the
module; see `SculptInsertModalBase` in `bl_operators/sculpt_insert_asset.py` for the reference
event adapter on top of it.
"""

from collections import namedtuple

import bpy
from bpy_extras import view3d_utils
from math import acos, atan2, hypot, pi, radians
from mathutils import Euler, Matrix, Vector, geometry

try:
    import numpy
except ImportError:
    # Blender builds with `WITH_PYTHON_NUMPY` off ship no numpy; the face set reads in
    # `_preview_meshes` then fall back to plain lists.
    numpy = None

__all__ = (
    "API_VERSION",
    "InsertResult",
    "InsertSession",
    "SessionSettings",
    "CorrectionStore",
    "SCALE_MIN",
    "SCALE_MAX",
    "check_sculpt_target",
    "surface_hit",
    "mirror_passes",
    "clamped_axis_delta",
    "symmetry_pass_matrix",
    "primary_matrix",
    "copy_matrices",
    "insert_types",
    "hierarchy_meshes",
    "is_insertable_root",
    "hierarchy_items",
    "items_extent",
    "drop_batch",
    "insert",
)

# Bump when the public API below changes shape, so tools can check what they call.
API_VERSION = 1

# Scale bounds of the cursor-driven size (and of the settings that feed it).
SCALE_MIN = 0.01
SCALE_MAX = 100.0

_FLIP_GUARD_EPSILON = 0.05  # keep the object's up within ~87 degrees of the surface normal
_AXIS_SENSITIVITY = 0.008  # radians per mouse pixel while an axis key is held
_SPIN_SENSITIVITY = 0.01  # radians per mouse pixel for the spin around the object's up axis
# Distance (pixels) under which the mouse direction around the origin is too noisy to spin by.
_SPIN_DEAD_ZONE_PX = 6.0

# Name prefixes of the objects (and their preview meshes) the tool creates itself: they are
# never offered as insert sources.
_PREVIEW_OBJECT_PREFIX = "insert_preview"
_BAKE_CARRIER_PREFIX = "insert_bake"
_TOOL_OWNED_NAME_PREFIXES = (_PREVIEW_OBJECT_PREFIX, _BAKE_CARRIER_PREFIX)

_MIRROR_PASS_BITS = (1, 2, 3, 4, 5, 6, 7)  # X, Y, XY, Z, XZ, YZ, XYZ (C++ numeric pass order)
_AXIS_VECTORS = {
    'X': Vector((1.0, 0.0, 0.0)),
    'Y': Vector((0.0, 1.0, 0.0)),
    'Z': Vector((0.0, 0.0, 1.0)),
}

# Local front of an inserted object (the side that should face the viewer and the cursor). The
# surface alignment maps local +Z onto the normal, so the front is a tangent direction. Blender's
# convention is that the front of an object faces -Y.
_FRONT = Vector((0.0, -1.0, 0.0))

_MESH_TYPES = frozenset({'MESH'})
# Object types whose evaluated geometry can hold a mesh (a profile/bevel, or geometry nodes):
# the tool bakes that mesh and inserts it.
_CURVE_TYPES = frozenset({'CURVE', 'CURVES', 'SURFACE', 'FONT'})

# Outcome of `insert`: `status` is the operator return value ('FINISHED' or 'CANCELLED'), and
# `message_type` with `message` describe a failure to report ('ERROR' or 'WARNING', or None).
InsertResult = namedtuple("InsertResult", ("status", "message_type", "message"))


# --------------------------------------------------------------------
# Placement math


def surface_hit(context, active_ob, region, rv3d, coord):
    """Raycast against the sculpt object `active_ob` only. Returns (world_loc, world_normal,
    base_rot) where base_rot aligns a local +Z to the surface normal, or None."""
    depsgraph = context.evaluated_depsgraph_get()
    ob_eval = active_ob.evaluated_get(depsgraph)
    # The evaluated transform: the drop operator bakes and mirrors placements through it, so the
    # raycast must use the same one (it goes stale on the original under animation or drivers).
    matrix_world = ob_eval.matrix_world

    origin = view3d_utils.region_2d_to_origin_3d(region, rv3d, coord)
    direction = view3d_utils.region_2d_to_vector_3d(region, rv3d, coord)

    inv = matrix_world.inverted()
    origin_local = inv @ origin
    direction_local = inv.to_3x3() @ direction
    if direction_local.length_squared < 1e-12:
        return None
    direction_local.normalize()

    hit, loc_local, normal_local, _index = ob_eval.ray_cast(
        origin_local, direction_local, depsgraph=depsgraph)
    if not hit:
        return None

    loc_world = matrix_world @ loc_local
    # Inverse-transpose keeps the normal correct under non-uniform scale.
    normal_world = inv.to_3x3().transposed() @ normal_local
    if normal_world.length_squared < 1e-12:
        return None
    normal_world.normalize()

    base_rot = normal_world.to_track_quat('Z', 'Y').to_matrix()
    return loc_world, normal_world, base_rot


def mirror_passes(active_ob):
    """Valid mirror pass bit-masks for the sculpt object's symmetry axes, pass 0 (the
    unmirrored placement) first. Matches the drop operator's effective set: every non-empty
    subset of the enabled axes. The axes live on the mesh (`mesh_symmetry_xyz_get`), not in the
    sculpt tool settings."""
    mesh = active_ob.data
    axes = [mesh.use_mirror_x, mesh.use_mirror_y, mesh.use_mirror_z]
    passes = [0]
    for bits in _MIRROR_PASS_BITS:
        # A pass may only flip axes that are enabled.
        if all(enabled for i, enabled in enumerate(axes) if bits & (1 << i)):
            passes.append(bits)
    return passes


def symmetry_pass_matrix(matrix_world, pass_bits):
    """Reflection through the local axes of the object at `matrix_world` for one mirror
    symmetry pass (matches the drag-and-drop placement, `world_matrix_for_symmetry_pass` in
    sculpt_asset_drop.cc)."""
    if pass_bits == 0:
        return Matrix.Identity(4)
    to_local = matrix_world.inverted()
    flip = Matrix.Diagonal((
        -1.0 if pass_bits & 1 else 1.0,
        -1.0 if pass_bits & 2 else 1.0,
        -1.0 if pass_bits & 4 else 1.0,
        1.0,
    ))
    return matrix_world @ (flip @ to_local)


def primary_matrix(loc, user_rot, base_rot, corr_offset, corr_rot, scale, anchor):
    """World matrix placing the root frame on a surface point: `base_rot` aligns local +Z to
    the surface normal, `user_rot` is the user's rotation on top of it, the correction
    (`corr_offset`, `corr_rot` in the surface frame) turns the object about the placement point
    -- which stays put -- and shifts it, and the uniform `scale` is applied with the snap
    `anchor` moved onto the point. Each inserted object is placed by this matrix times its own
    matrix in that frame."""
    correction = Matrix.Translation(corr_offset) @ corr_rot.to_matrix().to_4x4()
    return (Matrix.Translation(loc) @
            (user_rot.to_4x4() @ base_rot.to_4x4()) @
            correction @
            Matrix.Diagonal((scale, scale, scale, 1.0)) @
            Matrix.Translation(-anchor))


def copy_matrices(matrix_world, primary, mirror_passes):
    """Every copy's world matrix for one placement: the primary matrix mirrored per symmetry
    pass, in the order the drop operator itself applies. `mirror_passes` comes from
    `mirror_passes` for the sculpt object at `matrix_world`."""
    return [
        symmetry_pass_matrix(matrix_world, pass_bits) @ primary
        for pass_bits in mirror_passes
    ]


def clamped_axis_delta(up, axis, normal, desired_delta):
    """Largest rotation around `axis` from `up` toward `desired_delta` that keeps
    dot(R(axis, angle) @ up, normal) >= epsilon, so the object never flips over."""
    u_dot_n = up.dot(normal)
    a_dot_u = axis.dot(up)
    a_dot_n = axis.dot(normal)
    p = u_dot_n - a_dot_u * a_dot_n  # coefficient of cos(angle)
    q = axis.cross(up).dot(normal)  # coefficient of sin(angle)
    c = a_dot_u * a_dot_n  # constant term
    radius = hypot(p, q)
    if radius < 1e-12:
        return 0.0 if u_dot_n < _FLIP_GUARD_EPSILON else desired_delta
    phi = atan2(q, p)
    psi = acos(max(-1.0, min(1.0, (_FLIP_GUARD_EPSILON - c) / radius)))
    low = phi - psi
    high = phi + psi
    # Both endpoints move together: f >= epsilon on [phi - psi, phi + psi] + 2*pi*k for one
    # specific k, and f(0) = dot(up, normal) is already satisfied, so 0 lies in that interval.
    while high < 0.0:
        low += 2.0 * pi
        high += 2.0 * pi
    while low > 0.0:
        low -= 2.0 * pi
        high -= 2.0 * pi
    return max(low, min(high, desired_delta))


# --------------------------------------------------------------------
# Sources


def insert_types(insert_type):
    """Object types inserted for a tool's Mesh/Curve switch: the meshes themselves, or the
    curve-like types whose evaluated geometry is baked into a mesh carrier."""
    return _CURVE_TYPES if insert_type == 'CURVE' else _MESH_TYPES


def hierarchy_meshes(root, insert_type):
    """Objects that are inserted together for `root`: the root itself when it is of an
    insertable type, and every such object below it (an Empty root only groups its children)."""
    types = insert_types(insert_type)
    objects = [root] if root.type in types else []
    objects.extend(child for child in root.children_recursive if child.type in types)
    return objects


def is_insertable_root(obj, insert_type, active_object=None):
    """An insertable object, or an Empty that has insertable children, that is not itself a
    child of another object: children are only ever inserted together with their parent.
    `active_object` (the sculpt object) and objects a tool creates for itself are never
    offered."""
    if obj is None or obj.parent is not None or obj.name.startswith(_TOOL_OWNED_NAME_PREFIXES):
        return False
    # The sculpt object itself, or a root that contains it, would be inserted into itself (the
    # copies are parented to it), so it is never offered.
    if active_object is not None and (obj == active_object or
                                      active_object in obj.children_recursive):
        return False
    types = insert_types(insert_type)
    if obj.type in types:
        return True
    return obj.type == 'EMPTY' and any(child.type in types for child in obj.children_recursive)


def hierarchy_items(root, insert_type):
    """[(mesh object, matrix)] to insert for `root`, each matrix placing the object relative to
    the root's frame: the root's world transform without its scale, so the root's own scale (and
    the children's offsets and scales) end up in the matrices and one uniform factor sizes them
    all."""
    location, rotation, _scale = root.matrix_world.decompose()
    root_inverse = Matrix.LocRotScale(location, rotation, None).inverted()
    return [(obj, root_inverse @ obj.matrix_world) for obj in hierarchy_meshes(root, insert_type)]


def _item_corners(depsgraph, obj):
    """Local-space corners of the geometry `obj` contributes. A curve object's own bound box can
    miss its bevel/extrusion, which offsets the snap, so a curve-like object is measured from the
    evaluated mesh instead. `Object.bound_box` is used where available (`Mesh` has no bound box of
    its own)."""
    obj_eval = obj.evaluated_get(depsgraph)
    if obj.type == 'MESH':
        return [Vector(corner) for corner in obj_eval.bound_box]

    try:
        mesh = obj_eval.to_mesh()
    except (RuntimeError, AttributeError):
        return []
    if mesh is None:
        return []
    xs, ys, zs = [], [], []
    for vert in mesh.vertices:
        xs.append(vert.co.x)
        ys.append(vert.co.y)
        zs.append(vert.co.z)
    obj_eval.to_mesh_clear()
    if not xs:
        return []
    x0, x1 = min(xs), max(xs)
    y0, y1 = min(ys), max(ys)
    z0, z1 = min(zs), max(zs)
    return [Vector((x, y, z)) for x in (x0, x1) for y in (y0, y1) for z in (z0, z1)]


def items_extent(depsgraph, items, *, snap_bottom):
    """(radius, anchor) of the items in the root frame: the point that touches the surface (the
    origin, or the bottom center of the joint bounding box, like the asset drag-and-drop), and
    the largest distance from it to a bounding box corner (the size the scale phase matches
    against the mouse distance)."""
    corners = [
        matrix @ corner
        for obj, matrix in items
        for corner in _item_corners(depsgraph, obj)
    ]
    anchor = Vector((0.0, 0.0, 0.0))
    if snap_bottom and corners:
        xs = [c.x for c in corners]
        ys = [c.y for c in corners]
        anchor = Vector(((min(xs) + max(xs)) * 0.5, (min(ys) + max(ys)) * 0.5,
                         min(c.z for c in corners)))
    radius = max((corner - anchor).length for corner in corners) if corners else 0.0
    return (radius if radius > 1e-6 else 1.0), anchor


def check_sculpt_target(context):
    """Whether the active object is a sculpt target the drop operator supports (a plain mesh,
    no multires or dynamic topology)."""
    obj = context.active_object
    if obj is None or obj.type != 'MESH':
        return False
    if any(modifier.type == 'MULTIRES' for modifier in obj.modifiers):
        return False
    return not obj.use_dynamic_topology_sculpting


# --------------------------------------------------------------------
# Corrections


class CorrectionStore:
    """In-memory corrections recorded per source name (`ID.name_full`), each holding
    {"offset": tuple | None, "rotation": tuple | None} -- only the parts that were switched on
    when recording. Kept for the session only, deliberately not saved in the file."""

    def __init__(self):
        self._items = {}

    def get(self, source_key):
        """The recorded correction for `source_key`, or None."""
        return self._items.get(source_key)

    def record(self, source_key, *, offset, rotation):
        """Replace the recorded correction of `source_key`; a None part records nothing."""
        self._items[source_key] = {"offset": offset, "rotation": rotation}


# --------------------------------------------------------------------
# Settings protocol


class SessionSettings:
    """Settings protocol of `InsertSession` and `insert`: any object with these attributes
    works, typically the tool's own PropertyGroup (which carries the same names). The
    attributes are read live throughout the placement; the correction fields and `last_scale`
    are also written back (see `InsertSession.load_correction` and `save_correction`), so the
    object must be mutable."""

    insert_type = 'MESH'  # 'MESH', or 'CURVE' to bake evaluated curves into mesh carriers
    placement = 'JOIN'  # 'JOIN' into the sculpt mesh, 'OBJECT', or 'INSTANCE' (linked copy)
    use_face_sets = True  # give the inserted geometry face sets of its own (joins only)
    use_face_set_color = False  # color the new face sets with `face_set_color` (joins only)
    face_set_color = (0.8, 0.8, 0.8)
    replace_face_sets = False  # collapse the inserted geometry into one new set (joins only)
    apply_mask = False  # mask the sculpt mesh's pre-existing geometry (joins only)
    use_gizmo = False  # move the sculpt 3D cursor onto the insert origin afterwards
    scale_mode = 'DEFAULT'  # 'DEFAULT'/'ZERO', 'ORIGINAL', 'FIXED' or 'LAST'
    interaction_mode = 'COMBINED'  # one scale+direction gesture, or 'STEPS'
    snap_mode = 'BOTTOM'  # which point of the object touches the surface: 'ORIGIN' or 'BOTTOM'
    last_scale = 1.0  # scale of the previous insert, used by the 'LAST' scale mode
    fixed_scale = 1.0  # scale of every insert in the 'FIXED' scale mode
    scale_sensitivity = 4.0  # how much the size grows per mouse distance from the origin
    rotation_snap_step = radians(10.0)  # angle step used while Ctrl is held during rotation
    use_correction = False  # apply the correction below to every insert
    use_record = False  # record the correction of every insert for its source
    corr_use_offset = True  # apply and record the position offset of the correction
    corr_use_rotation = True  # apply and record the rotation of the correction
    corr_offset = (0.0, 0.0, 0.0)  # position offset in the surface frame of the placement point
    corr_rotation = (0.0, 0.0, 0.0)  # rotation about the placement point, on top of the alignment


# --------------------------------------------------------------------
# The placement session


class InsertSession:
    """Placement of one insert as a state machine over the phases:

    - 'PLACE': the initial click-drag, the object slides on the surface (no previews shown
      yet),
    - 'SCALE': the mouse distance from the origin sets the size,
    - 'SNAP': the object slides on the surface again (a hotkey phase, not part of the default
      flow),
    - 'ROTATE': the mouse spins the object around its up axis, or around its own X, Y or Z axis
      while the key is held,
    - 'TILT': the mouse tilts the object about an axis of the surface frame (a correction
      rotation),
    - 'COMBINED': one gesture setting the size (mouse distance) and the facing (mouse
      direction) at once.

    In the default flow a click moves from 'PLACE' to 'SCALE' (or 'COMBINED') and from there to
    'ROTATE'; 'SNAP' and 'TILT' are entered with their hotkeys.

    The methods take plain data -- context, the settings object, region coordinates and flags
    -- never events, so any event source can drive the session. The world matrices of the copy
    set (primary and mirrored) are kept up to date in `matrices`, and the preview
    objects follow them.
    """

    def __init__(self, active_object, correction_store=None):
        """`active_object` is the sculpt mesh the placement happens on; passing a
        `CorrectionStore` enables corrections recorded per source."""
        self.active_object = active_object
        self.correction_store = correction_store
        self.reset()

    def reset(self):
        """Clear the placement state (a fresh session starts here, a finished one returns
        here)."""
        self.errors = []
        self.temp_source_name = None
        self.previews = []
        self.preview_mesh_names = []
        self.matrices = []
        self.loc = None
        self.normal = None
        self.base_rot = None
        self.user_rot = Matrix.Identity(3)
        self.items = []
        self.live_curves = False
        self.bake_names = []
        self.anchor = Vector((0.0, 0.0, 0.0))
        self.snap_bottom = True
        self.combined = False
        self.follow_cursor = False
        self.spin_total = 0.0
        self.spin_applied = 0.0
        self.radius = 1.0
        self.scale = 1.0
        self.origin_2d = Vector((0.0, 0.0))
        self.space_return = None
        self.source_key = None
        self.corr_offset = Vector((0.0, 0.0, 0.0))
        self.corr_rot = Euler((0.0, 0.0, 0.0), 'XYZ')
        # No tilt axis until one is picked with X/Y/Z.
        self.tilt_axis = None
        self.tilt_total = 0.0
        self.tilt_base = (0.0, 0.0, 0.0)
        # Angle of the current rotation/tilt gesture, for the status-bar hint.
        self.rotation_display = 0.0
        self.scale_resume = None
        self.face_bias = 0.0
        self.faced_view = False
        self.radius_px = 1.0
        self.phase = 'PLACE'
        self.axis_key = None

    # -- setup -------------------------------------------------------------------------

    def begin(self, context, source, settings, coord, *, is_temporary=False):
        """Start the placement for a resolved `source` object (the root of the hierarchy to
        insert): prepare its items, extents, start scale and correction, and put the placement
        onto the surface under the region coordinate `coord`. `is_temporary` marks a source
        imported just for this insert (nothing is recorded for it, and it is removed with the
        other temporary carriers).

        Returns False with `errors` holding a message when nothing can be placed.
        """
        self.errors = []
        if source is None:
            self.errors.append("Nothing to insert")
            return False
        # The sculpt object itself, or a root that contains it, would be inserted into itself
        # (the copies are parented to it), so it is never offered.
        if source == self.active_object or self.active_object in source.children_recursive:
            self.errors.append("The sculpt object cannot be inserted into itself")
            return False
        # A throwaway random import has a new name every time, so it has nothing to record.
        self.source_key = None if is_temporary else source.name_full
        self.load_correction(settings, source)
        self.items = hierarchy_items(source, settings.insert_type)
        # Linked curve copies keep the live curve (its modifiers and node groups), so nothing is
        # baked for them; every other placement needs a mesh to join or duplicate.
        self.live_curves = settings.insert_type == 'CURVE' and settings.placement == 'INSTANCE'
        if settings.insert_type == 'CURVE' and not self.live_curves:
            self.items = self._bake_items(context, self.items)
            if not self.items:
                self.errors.append(
                    "The curve evaluates to no mesh (give it a profile/bevel or geometry nodes)")
                return False
        self.radius, self.anchor = items_extent(
            context.evaluated_depsgraph_get(), self.items,
            snap_bottom=settings.snap_mode == 'BOTTOM')
        self.snap_bottom = (settings.snap_mode == 'BOTTOM')
        # A fixed scale has no cursor-driven size, so it always uses the stepped flow.
        self.combined = (settings.interaction_mode == 'COMBINED' and
                         settings.scale_mode not in {'FIXED', 'LAST'})
        if settings.scale_mode in {'DEFAULT', 'ZERO'}:
            self.scale = SCALE_MIN
        elif settings.scale_mode == 'FIXED':
            self.scale = settings.fixed_scale
        elif settings.scale_mode == 'LAST':
            self.scale = settings.last_scale

        hit = surface_hit(context, self.active_object, context.region, context.region_data, coord)
        if hit is None:
            # Nothing under the mouse yet: the previews appear on the first move over the
            # surface.
            return True
        self.loc, self.normal, self.base_rot = hit
        self._face_view(context)
        self._refresh(settings)
        return True

    def _face_view(self, context):
        """Turn the object around the surface normal so its front (-Y) faces the viewer, once at
        the start of a placement. The surface alignment alone (`base_rot`) only follows the world
        up, which leaves the front pointing away by 180 degrees from the front view; this removes
        that offset. Skipped when the view is along the normal (the front is in the surface plane
        then, so there is nothing to face)."""
        if self.faced_view or self.normal is None:
            return
        rv3d = context.region_data
        if rv3d is None:
            return
        # The camera looks along its local -Z, so toward the viewer is its local +Z in world space.
        # `view_rotation` maps world to view, hence the inverse (as `enter_phase` does).
        to_view = rv3d.view_matrix.inverted().to_3x3() @ Vector((0.0, 0.0, 1.0))
        to_view -= self.normal * to_view.dot(self.normal)
        if to_view.length_squared < 1e-8:
            return
        to_view.normalize()
        front = self.base_rot @ _FRONT
        angle = atan2(self.normal.dot(front.cross(to_view)), front.dot(to_view))
        self.user_rot = Matrix.Rotation(angle, 3, self.normal)
        self.faced_view = True

    def sync_snap_mode(self, context, settings):
        """Recompute the snap anchor when Origin/Bottom was switched mid-placement.

        Updates the anchor (and radius) in place and refreshes the previews, without
        resetting the mesh, scale or rotation: the same point stays on the surface,
        only which point of the object touches it changes.
        """
        snap_bottom = (settings.snap_mode == 'BOTTOM')
        if snap_bottom == self.snap_bottom:
            return
        self.snap_bottom = snap_bottom
        if not self.items:
            return
        self.radius, self.anchor = items_extent(
            context.evaluated_depsgraph_get(), self.items, snap_bottom=snap_bottom)
        if self.loc is not None:
            self._refresh(settings)

    def active_matrix_world(self):
        """Evaluated world matrix of the active sculpt object. The drop operator bakes and
        mirrors placements through the evaluated transform (the original one goes stale under
        animation, drivers or constraints), so every matrix here must be built from the same
        one."""
        depsgraph = bpy.context.evaluated_depsgraph_get()
        return self.active_object.evaluated_get(depsgraph).matrix_world

    def _mirror_flip_matrix(self, pass_bits):
        """Reflection through the active object's local axes for one mirror symmetry pass, via
        `symmetry_pass_matrix` with its evaluated transform."""
        return symmetry_pass_matrix(self.active_matrix_world(), pass_bits)

    def primary_matrix(self):
        """World matrix placing the root frame for the current state (see `primary_matrix`)."""
        return primary_matrix(
            self.loc, self.user_rot, self.base_rot, self.corr_offset, self.corr_rot,
            self.scale, self.anchor)

    def copy_matrices(self, settings, primary):
        """Every copy's world matrix for the primary placement `primary` (see `copy_matrices`),
        from the active object's symmetry axes."""
        return copy_matrices(
            self.active_matrix_world(), primary, mirror_passes(self.active_object))

    def _refresh(self, settings):
        self.matrices = self.copy_matrices(settings, self.primary_matrix())
        self._update_previews(self.matrices)

    # -- gestures ----------------------------------------------------------------------

    def move_to_mouse(self, context, settings, coord):
        """Slide the placement over the surface following the mouse (phases 'PLACE' and
        'SNAP')."""
        hit = surface_hit(context, self.active_object, context.region, context.region_data, coord)
        if hit is None:
            return
        loc, normal, base_rot = hit
        if self.phase == 'SNAP' and self.normal is not None:
            # Carry the object's orientation over to the new surface point by the smallest
            # rotation between the two normals, so it keeps standing on the surface instead of
            # keeping a rotation around the old normal (total = user_rot @ base_rot).
            carry = self.normal.rotation_difference(normal).to_matrix()
            self.user_rot = carry @ self.user_rot @ self.base_rot @ base_rot.inverted()
        self.loc, self.normal, self.base_rot = loc, normal, base_rot
        if self.phase == 'PLACE':
            # The initial click may have missed the surface; face the viewer on the first move
            # that lands on it, like `begin` would have.
            self._face_view(context)
        self._refresh(settings)

    def enter_phase(self, context, settings, coord, phase, *, follow_cursor=False):
        """Switch to `phase` ('SCALE', 'SNAP', 'ROTATE', 'TILT' or 'COMBINED'), setting up what
        it measures against. `coord` is the mouse position in region coordinates;
        `follow_cursor` makes the scale gestures keep the object's facing on the mouse
        direction."""
        region = context.region
        rv3d = context.region_data
        mouse = Vector(coord)
        origin_2d = view3d_utils.location_3d_to_region_2d(region, rv3d, self.loc)
        if origin_2d is None:
            origin_2d = mouse

        if self.phase in {'SCALE', 'COMBINED'} and phase != self.phase:
            # Also between the two: a click in the separate scale phase hands over to the
            # combined gesture, which must continue from this size and not measure it again.
            self.scale_resume = (mouse - self.origin_2d, self.radius_px)

        if not self.previews:
            # Nothing is shown during the initial click-drag, only from the first phase on.
            self._create_previews(context, settings)
        if phase in {'SCALE', 'COMBINED'}:
            # The mouse distance from the origin's screen position, measured against the screen
            # size of the source's radius at that depth, sets the scale.
            view_right = rv3d.view_matrix.inverted().to_3x3() @ Vector((1.0, 0.0, 0.0))
            edge_2d = view3d_utils.location_3d_to_region_2d(
                region, rv3d, self.loc + view_right.normalized() * self.radius)
            base_px = (edge_2d - origin_2d).length if edge_2d is not None else 0.0
            self.origin_2d = origin_2d
            self.radius_px = base_px if base_px > 1.0 else 1.0
            if self.scale_resume is None:
                # First time in a scale phase: the object is already shown at its starting size
                # (its own, or nearly zero), and the mouse is on its origin. Measuring the size
                # from the origin would make it jump to the tiny distance on the first move, so
                # start the reference away from the mouse by the distance that gives exactly the
                # current size, and every move then changes it relative to that.
                distance = self.scale * self.radius_px / settings.scale_sensitivity
                self.scale_resume = (Vector((0.0, -1.0)) * distance, self.radius_px)
            if self.scale_resume is not None:
                # Continue the earlier scale instead of measuring from the (moved) origin again:
                # the mouse keeps the offset and reference size it had when the scale phase was
                # left, so the size does not reset.
                offset, radius_px = self.scale_resume
                self.origin_2d = mouse - offset
                self.radius_px = radius_px

            # The facing follows the mouse direction around the origin, so the object's front
            # always points at the cursor (no offset kept): the insert is meant to be aimed with
            # the mouse.
            self.face_bias = 0.0

        if phase == 'TILT':
            # Each tilt continues from the correction the object has now, with the X axis active
            # right away (the user can switch to Y/Z with their hotkeys).
            self.tilt_axis = 'X'
            self.tilt_total = 0.0
            self.tilt_base = tuple(self.corr_rot)

        self.phase = phase
        self.follow_cursor = follow_cursor
        self.axis_key = None
        self.rotation_display = 0.0
        self._flash_previews()

    def scale_from_mouse(self, context, settings, coord, *, ctrl=False):
        """Set the size from the mouse distance to the origin's screen position (phases 'SCALE'
        and 'COMBINED'); `ctrl` snaps the facing while the cursor is followed."""
        mouse = Vector(coord)
        scale = (mouse - self.origin_2d).length / self.radius_px * settings.scale_sensitivity
        self.scale = max(SCALE_MIN, min(SCALE_MAX, scale))
        if self.follow_cursor:
            self._face_cursor(context, settings, coord, ctrl)
        self._refresh(settings)

    def tilt_from_mouse(self, context, settings, dx, *, ctrl=False):
        """Tilt: the mouse turns the object about the chosen axis of the surface frame, through
        the placement point, on top of the surface alignment. `dx` is the mouse movement in
        pixels; `ctrl` snaps the angle. Does nothing until an axis was picked with X/Y/Z."""
        if self.tilt_axis is None:
            return
        index = "XYZ".index(self.tilt_axis)
        self.tilt_total += dx * _AXIS_SENSITIVITY
        angle = self._snap_angle(settings, self.tilt_total) if ctrl else self.tilt_total
        self.rotation_display = angle
        values = list(self.corr_rot)
        values[index] = self.tilt_base[index] + angle
        self.corr_rot = Euler(values, 'XYZ')
        self._refresh(settings)

    def set_tilt_axis(self, axis):
        """Tilt about the surface frame axis `axis` ('X', 'Y' or 'Z') from now on; each axis
        continues from its own angle."""
        self.tilt_axis = axis
        self.tilt_total = 0.0
        self.tilt_base = tuple(self.corr_rot)
        self.rotation_display = 0.0

    def rotate_from_mouse(self, context, settings, dx, *, ctrl=False):
        """Rotate: the mouse spins the object around its up axis, or around its own X, Y or Z
        axis while the key is held (phase 'ROTATE'). `ctrl` snaps the angle."""
        if self.normal is None:
            return

        if self.axis_key is not None:
            # Rotate about the object's own axis: the world direction of the object-local axis
            # (surface alignment plus the user's rotation). It stays put while the object spins,
            # so the gesture is stable.
            axis = (self.user_rot @ self.base_rot) @ _AXIS_VECTORS[self.axis_key]
            up = self.user_rot @ self.normal
            delta = clamped_axis_delta(up, axis, self.normal, dx * _AXIS_SENSITIVITY)
            self.user_rot = Matrix.Rotation(delta, 3, axis) @ self.user_rot
            self.rotation_display += delta
        else:
            # Spin around the object's own up axis; it preserves the up direction, so the
            # object can never flip.
            up = self.user_rot @ self.normal
            if up.length_squared < 1e-12:
                return
            # The unsnapped total is kept so Ctrl snaps the accumulated angle, not each step.
            self.spin_total += dx * _SPIN_SENSITIVITY
            target = self._snap_angle(settings, self.spin_total) if ctrl else self.spin_total
            self.user_rot = Matrix.Rotation(target - self.spin_applied, 3, up.normalized()) @ \
                self.user_rot
            self.spin_applied = target
            self.rotation_display = target

        self._refresh(settings)

    def set_rotate_axis(self, axis):
        """While `axis` ('X', 'Y' or 'Z', or None) is set, rotations go around the object's own
        axis of that name instead of the object's up axis."""
        self.axis_key = axis
        if axis is not None:
            self.rotation_display = 0.0

    def _snap_angle(self, settings, angle):
        step = settings.rotation_snap_step
        return round(angle / step) * step

    def _cursor_angle(self, context, mouse):
        """Angle around the surface normal from the base front (-Y) to the direction the mouse
        lies in from the scale origin, or None when it cannot be told. Both screen points are
        intersected with the tangent plane at the placement point, so a scale origin that was
        moved away from the projected object origin (see `scale_resume`) still gives the
        direction the user has been pulling in."""
        region = context.region
        rv3d = context.region_data

        def plane_point(point):
            coord = (point.x, point.y)
            ray_origin = view3d_utils.region_2d_to_origin_3d(region, rv3d, coord)
            ray_dir = view3d_utils.region_2d_to_vector_3d(region, rv3d, coord)
            return geometry.intersect_line_plane(
                ray_origin, ray_origin + ray_dir, self.loc, self.normal)

        target = plane_point(mouse)
        origin = plane_point(self.origin_2d)
        if target is None or origin is None:
            return None
        direction = target - origin
        direction -= self.normal * direction.dot(self.normal)
        front = self.base_rot @ _FRONT
        if direction.length_squared < 1e-12 or front.length_squared < 1e-12:
            return None
        direction.normalize()
        return atan2(self.normal.dot(front.cross(direction)), front.dot(direction))

    def _face_cursor(self, context, settings, coord, ctrl):
        """Turn the object around the surface normal so its front (-Y) points toward the mouse
        (plus the offset kept when a gesture is resumed). Inside a small dead zone around the
        origin the direction is too noisy and the rotation is kept."""
        mouse = Vector(coord)
        if (mouse - self.origin_2d).length <= _SPIN_DEAD_ZONE_PX:
            return
        angle = self._cursor_angle(context, mouse)
        if angle is None:
            return
        angle += self.face_bias
        if ctrl:
            angle = self._snap_angle(settings, angle)
        self.user_rot = Matrix.Rotation(angle, 3, self.normal)

    # -- corrections -------------------------------------------------------------------

    def load_correction(self, settings, source):
        """Start the insert's correction: with Correction Mode, the fields of the settings,
        first replaced by what the store recorded for this source (only the parts it recorded --
        which also shows them in the tool's UI); otherwise none. The fields only feed the parts
        that are switched on there."""
        self.corr_offset = Vector((0.0, 0.0, 0.0))
        self.corr_rot = Euler((0.0, 0.0, 0.0), 'XYZ')
        if not settings.use_correction:
            return
        recorded = (self.correction_store.get(source.name_full)
                    if self.correction_store is not None else None)
        if recorded is not None:
            if recorded["offset"] is not None:
                settings.corr_offset = recorded["offset"]
            if recorded["rotation"] is not None:
                settings.corr_rotation = recorded["rotation"]
        if settings.corr_use_offset:
            self.corr_offset = Vector(settings.corr_offset)
        if settings.corr_use_rotation:
            self.corr_rot = Euler(settings.corr_rotation, 'XYZ')

    def save_correction(self, settings):
        """Keep the finished insert's correction in the settings fields (Correction Mode only),
        so the next insert starts from it, and record it for the source while Rec is on (only
        the parts that are switched on)."""
        if settings.use_correction:
            if settings.corr_use_offset:
                settings.corr_offset = self.corr_offset
            if settings.corr_use_rotation:
                settings.corr_rotation = tuple(self.corr_rot)
        if (settings.use_record and self.source_key is not None and
                self.correction_store is not None):
            self.correction_store.record(
                self.source_key,
                offset=tuple(self.corr_offset) if settings.corr_use_offset else None,
                rotation=tuple(self.corr_rot) if settings.corr_use_rotation else None)

    # -- preview objects ---------------------------------------------------------------

    def _update_previews(self, matrices):
        """Previews are stored copy by copy, each copy holding one object per item."""
        item_count = len(self.items)
        for index, name in enumerate(self.previews):
            obj = bpy.data.objects.get(name)
            if obj is not None:
                obj.matrix_world = matrices[index // item_count] @ self.items[index % item_count][1]

    def _preview_meshes(self, context, settings):
        """Throwaway meshes for the previews, one per inserted object, whose face sets are the
        ones the join will assign, so the face set overlay shows the inserted geometry as it
        will look: the set IDs are shifted above the active mesh's and above the objects
        inserted before (same as `prepare_asset_face_sets` in sculpt_asset_drop.cc), or
        collapsed into one new set per object with Replace Face Sets. Joins only; other
        placements keep the source's own sets.

        Each entry is (mesh or None, from_evaluated): `from_evaluated` marks a mesh copied from
        the evaluated geometry because the source's own mesh data is empty, so the preview
        object must not run its modifiers again on top of it.
        """
        join = settings.placement == 'JOIN'
        next_id = 2  # A mesh without face sets gets one set (ID 1) when joined.
        active_mesh = self.active_object.data
        if join and ".sculpt_face_set" in active_mesh.attributes:
            # The whole sculpt mesh's face sets are read just for the highest ID, so read into a
            # flat buffer when numpy is available instead of a list of boxed Python ints.
            if numpy is not None:
                values = numpy.empty(len(active_mesh.polygons), dtype=numpy.int32)
                active_mesh.attributes[".sculpt_face_set"].data.foreach_get("value", values)
                next_id = (int(values.max()) if values.size else 1) + 1
            else:
                values = [0] * len(active_mesh.polygons)
                active_mesh.attributes[".sculpt_face_set"].data.foreach_get("value", values)
                next_id = max(values, default=1) + 1

        depsgraph = context.evaluated_depsgraph_get()
        meshes = []
        for obj, _matrix in self.items:
            if obj.type != 'MESH':
                # Live curve previews copy the object itself, modifiers included.
                meshes.append((None, False))
                continue
            # A geometry-nodes source can have an empty base mesh (the geometry is generated), so
            # preview the evaluated mesh in that case: it is what the drop operator will insert,
            # and it keeps the overlay from building a batch out of a mesh with no faces.
            source_mesh = obj.data
            from_evaluated = False
            if source_mesh is None or not len(source_mesh.polygons):
                evaluated = obj.evaluated_get(depsgraph).data
                if evaluated is not None and len(evaluated.polygons):
                    source_mesh = evaluated
                    from_evaluated = True
            if source_mesh is None:
                meshes.append((None, False))
                continue
            mesh = source_mesh.copy()
            mesh.name = _PREVIEW_OBJECT_PREFIX
            self.preview_mesh_names.append(mesh.name)
            meshes.append((mesh, from_evaluated))
            num = len(mesh.polygons)
            if not join or not num:
                continue

            has_sets = ".sculpt_face_set" in mesh.attributes
            if not settings.use_face_sets:
                # Joins into the default set, which is drawn without a color.
                if has_sets:
                    mesh.attributes.remove(mesh.attributes[".sculpt_face_set"])
                continue
            if settings.replace_face_sets or not has_sets:
                new_values = [next_id] * num
                next_id += 1
            else:
                new_values = [0] * num
                mesh.attributes[".sculpt_face_set"].data.foreach_get("value", new_values)
                new_values = [value + next_id for value in new_values]
                next_id = max(new_values) + 1
            if has_sets:
                mesh.attributes.remove(mesh.attributes[".sculpt_face_set"])
            attribute = mesh.attributes.new(".sculpt_face_set", 'INT', 'FACE')
            attribute.data.foreach_set("value", new_values)
        return meshes

    def _create_previews(self, context, settings):
        scene_coll = context.scene.collection
        preview_meshes = self._preview_meshes(context, settings)
        for copy_index in range(len(self.matrices)):
            for (source, _matrix), (mesh, from_evaluated) in zip(self.items, preview_meshes):
                preview = source.copy()
                if mesh is not None:
                    preview.data = mesh
                if from_evaluated:
                    # The preview mesh already is the evaluated geometry (the source's own mesh
                    # data is empty): running the source's modifiers on it would apply the
                    # generated geometry a second time.
                    preview.modifiers.clear()
                # Marks the object for the sculpt overlay, which draws its face sets (see
                # `mesh_sync` in overlay_sculpt.hh). Only meshes with faces are marked: an empty
                # mesh has no overlay to draw and would build an empty batch.
                if mesh is not None and len(mesh.polygons):
                    preview.show_sculpt_preview = True
                preview.name = "insert_preview_{:d}".format(len(self.previews))
                preview.parent = None
                preview.hide_viewport = False
                preview.hide_render = True
                scene_coll.objects.link(preview)
                self.previews.append(preview.name)
        self._update_previews(self.matrices)

    def clear_previews(self):
        """Remove the preview objects (and their throwaway meshes)."""
        for name in self.previews:
            obj = bpy.data.objects.get(name)
            if obj is not None:
                bpy.data.objects.remove(obj, do_unlink=True)
        self.previews.clear()
        for name in self.preview_mesh_names:
            mesh = bpy.data.meshes.get(name)
            if mesh is not None:
                bpy.data.meshes.remove(mesh)
        self.preview_mesh_names.clear()

    def _flash_previews(self):
        """Play the mode-transfer highlight (Alt+Q) on the previews as feedback for a phase
        change."""
        for name in self.previews:
            obj = bpy.data.objects.get(name)
            if obj is None:
                continue
            try:
                bpy.ops.object.overlay_flash(session_uid=obj.session_uid)
            except RuntimeError:
                return

    # -- temporary carriers ------------------------------------------------------------

    def _bake_items(self, context, items):
        """Replace curve-like items with mesh carriers holding their evaluated mesh, so the
        rest of the session (preview, extent, the drop operator) only ever sees meshes. The
        carriers sit far away because the drop operator replaces their transform, and are
        removed with `remove_temp_sources`. Items evaluating to no faces are dropped."""
        depsgraph = context.evaluated_depsgraph_get()
        baked = []
        for obj, matrix in items:
            mesh = bpy.data.meshes.new_from_object(obj.evaluated_get(depsgraph), depsgraph=depsgraph)
            if mesh is None:
                continue
            if not mesh.polygons:
                bpy.data.meshes.remove(mesh)
                continue
            carrier = bpy.data.objects.new(_BAKE_CARRIER_PREFIX, mesh)
            carrier.location = (0.0, 0.0, -1.0e5)
            context.scene.collection.objects.link(carrier)
            self.bake_names.append(carrier.name)
            baked.append((carrier, matrix))
        context.view_layer.update()
        return baked

    def _remove_bake_carriers(self, keep):
        for name in self.bake_names:
            carrier = bpy.data.objects.get(name)
            if carrier is None or carrier in keep:
                continue
            mesh = carrier.data
            bpy.data.objects.remove(carrier, do_unlink=True)
            # Linked copies made by the drop operator keep sharing the baked mesh.
            if mesh is not None and mesh.users == 0:
                bpy.data.meshes.remove(mesh)
        self.bake_names.clear()

    def remove_temp_sources(self, *, keep=()):
        """Remove everything created just for this insert: the curve bake carriers and the
        throwaway carrier of a random asset pick (`temp_source_name`), except the objects in
        `keep`. Objects the drop operator created from them are independent duplicates (linked
        copies share the baked mesh, which is kept while it has users), so removing the carriers
        leaves them intact."""
        self._remove_bake_carriers(keep)
        name = self.temp_source_name
        self.temp_source_name = None
        obj = bpy.data.objects.get(name) if name else None
        if obj is not None:
            # The children were imported together with the carrier.
            carriers = [carrier for carrier in (obj, *obj.children_recursive) if carrier not in keep]
            meshes = [carrier.data for carrier in carriers if carrier.type == 'MESH']
            for carrier in carriers:
                bpy.data.objects.remove(carrier, do_unlink=True)
            # Drop the carriers' meshes too unless something (a linked copy) still uses them.
            for mesh in meshes:
                if mesh.users == 0:
                    bpy.data.meshes.remove(mesh)


# --------------------------------------------------------------------
# Final insert


def drop_batch(settings, placements, cursor_placement, *, keep_source, parent_to_active):
    """One call of the batch form of the drag-and-drop insert operator
    (`sculpt.mesh_asset_drop_batch`): `placements` is a list of (mesh object, world matrix),
    scale included, all placed as a single undo step. Mirror symmetry is expanded inside the
    operator per placement. The mask covers only the geometry that existed before the batch, and
    the face set IDs run across the placements in order. `keep_source` False removes the source
    objects (and their now-orphaned mesh data) inside the same step (for throwaway carriers),
    `parent_to_active` parents what was added as separate objects to the active sculpt object.

    Returns True when the batch was placed."""
    color_kwargs = {}
    if settings.use_face_sets and settings.use_face_set_color:
        color_kwargs = {"use_face_set_color": True, "face_set_color": tuple(settings.face_set_color)}
    items = [
        {
            # The element is an RNA `PropertyGroup`, so it also inherits the base `name`
            # property. The batch operator ignores it, but RNA requires it (and every other
            # property) when a collection is filled from a list of dicts -- without it the call
            # fails with `keyword "name" missing`.
            "name": "{:d}".format(index),
            "session_uid": obj.session_uid,
            # RNA matrices are column-major (C `mat[col][row]`), so pass the transposed rows;
            # otherwise the translation ends up in the bottom row and the drop ignores the snap.
            "matrix": [list(col) for col in matrix.transposed()],
        }
        for index, (obj, matrix) in enumerate(placements)
    ]
    try:
        result = bpy.ops.sculpt.mesh_asset_drop_batch(
            **color_kwargs,
            items=items,
            use_face_sets=settings.use_face_sets,
            join_to_active=settings.placement == 'JOIN',
            replace_face_sets=settings.replace_face_sets,
            apply_mask=settings.apply_mask,
            keep_source=keep_source,
            parent_to_active=parent_to_active,
            linked=settings.placement == 'INSTANCE',
            cursor_placement=cursor_placement,
        )
    except RuntimeError:
        return False
    return result == {'FINISHED'}


def insert(context, session, settings, *, undo_message):
    """Insert the copy set of a finished `session` into the active sculpt mesh.

    Places every item with a single `drop_batch` call (which expands the mirror passes itself,
    parents what it added as objects to the sculpt object and removes the session's throwaway
    carriers), so the whole insert is one undo step. The previews are removed first, so they never
    end up in an undo state, and the correction is saved to `settings` and the session's
    correction store. `undo_message` names the step of the live curve copies (which the operator
    does not place); the batch operator names its own.

    Returns an `InsertResult`: 'CANCELLED' with an ERROR message when nothing could be placed,
    'FINISHED' otherwise (the operator reports skipped copies as warnings itself).
    """
    placement = session.primary_matrix()
    session.save_correction(settings)
    settings.last_scale = session.scale

    # The previews must not be part of any undo state the insert operator pushes.
    session.clear_previews()

    active_ob = session.active_object
    parent_inverse = session.active_matrix_world().inverted()
    cursor_placement = 'CURSOR_TO_ORIGIN' if settings.use_gizmo else 'NONE'

    def parent_to_active(obj):
        obj.parent = active_ob
        obj.matrix_parent_inverse = parent_inverse

    if session.live_curves:
        # Linked copies of the curve objects themselves: they share the curve data and carry
        # the same modifiers, so they stay connected to their sources. Every mirror copy is
        # placed here, as the drop operator only handles meshes.
        for copy_matrix in session.copy_matrices(settings, placement):
            for source, item_matrix in session.items:
                live = source.copy()
                live.name = source.name
                live.parent = None
                live.matrix_world = copy_matrix @ item_matrix
                context.scene.collection.objects.link(live)
                parent_to_active(live)
        if settings.use_gizmo:
            # The batch moves the 3D cursor onto the insert origin for the mesh placements; the
            # live copies share that origin, so it moves onto it here too (the sculpt cursor's
            # internal positioning state itself is not reachable from Python).
            context.scene.cursor.matrix = placement.normalized()
        session.remove_temp_sources()
        bpy.ops.ed.undo_push(message=undo_message)
        return InsertResult('FINISHED', None, None)

    # Carriers baked from curves and a randomly imported asset only exist for this insert: the
    # batch removes them (and their now-orphaned mesh data) in its own undo step, so undo and
    # redo can neither resurrect nor lose them.
    discard_sources = bool(session.bake_names) or session.temp_source_name is not None
    placements = [
        (source, placement @ item_matrix)
        for source, item_matrix in session.items
    ]
    if discard_sources:
        # What the batch does not consume (the root of an imported hierarchy) goes now, for the
        # same undo reason.
        session.remove_temp_sources(keep=[source for source, _matrix in placements])

    if not drop_batch(
            settings, placements, cursor_placement,
            keep_source=not discard_sources, parent_to_active=True):
        # Nothing was inserted: leave nothing behind and let the caller report it.
        session.remove_temp_sources()
        return InsertResult('CANCELLED', 'ERROR', "Insert failed: the source could not be placed")
    return InsertResult('FINISHED', None, None)
