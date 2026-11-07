# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""The Sculpt Layers stack source: a `bpy.types.OutlinerStackSource` subclass.

The same sculpt layer tree the Properties editor shows, described as Stack Layers rows --
groups nest their layers, the influence rides the value column, and the eye and mask buttons
ride the row's own buttons.

The layer structure is never modified here directly: every mutation goes through the sculpt
layer operators, which own the undo steps, the sync-group fan-out and the tree invariants.
"""

import logging
from functools import lru_cache

import bpy

from bl_ui.space_outliner import stack_source_menu_register, stack_source_menu_unregister

_log = logging.getLogger(__name__)

# Kinds a row can be, as the parsed stable keys record them.
KIND_LAYER = 0
KIND_GROUP = 1

# The Material Symbols codepoint of a layer's base preview slot. The built-in icon stands in where
# no glyph font is available (background mode).
_GLYPH_BASE = 0xF5A4

# The notifier class and data `notifier_invalidates` is asked about, spelled the way
# WM_types.hh spells them: the bridge hands plain ints over, and Python has no name of its
# own for these.
_NC_GEOM = 17 << 24
_ND_DATA = 91 << 16


def _walk_tree(children, visit):
    """Depth-first walk over a layer tree, parents before their children."""
    for node in children:
        visit(node)
        if isinstance(node, bpy.types.SculptLayerGroup):
            _walk_tree(node.children, visit)


def _node_kind(node):
    return KIND_GROUP if isinstance(node, bpy.types.SculptLayerGroup) else KIND_LAYER


def _key_of(node):
    """The stable key a row of this node builds its identity from."""
    return "sculpt_group_{}".format(node.uid) if isinstance(
        node, bpy.types.SculptLayerGroup) else "sculpt_layer_{}".format(node.uid)


def parse_stable_key(key):
    """Resolve a row's stable key to its (uid, kind), or None for a key of another source."""
    prefix, sep, tail = key.rpartition('_')
    if not sep:
        return None
    if prefix == "sculpt_layer":
        kind = KIND_LAYER
    elif prefix == "sculpt_group":
        kind = KIND_GROUP
    else:
        return None
    try:
        return (int(tail), kind)
    except ValueError:
        return None


def nodes_by_uid(mesh):
    """Every node of the mesh's tree by uid, in one walk."""
    nodes = {}

    def visit(node):
        nodes[node.uid] = node

    _walk_tree(mesh.sculpt_layer_tree, visit)
    return nodes


def _selected_nodes(mesh):
    """The selected layers and folders, in tree order."""
    selected = []

    def visit(node):
        if node.select:
            selected.append(node)

    _walk_tree(mesh.sculpt_layer_tree, visit)
    return selected


def _clear_layer_selection(mesh):
    """Deselect every layer and folder of the tree."""
    def visit(node):
        node.select = False

    _walk_tree(mesh.sculpt_layer_tree, visit)


def selected_or_active(mesh):
    """The nodes a multi-target command acts on: every selected layer or folder, or the active
    layer when nothing is selected. Folders hold masks of their own, so they are targets too."""
    selected = _selected_nodes(mesh)
    if selected:
        return selected
    active = mesh.sculpt_layers_active
    return [active] if active is not None else []


def _color_tag_index(group):
    """The folder's color tag as the 0..8 palette index the row builder takes: the RNA enum names
    it ('NONE', 'COLOR1' .. 'COLOR8'), the builder wants the number."""
    tag = group.color_tag
    return int(tag[5:]) if tag.startswith('COLOR') else 0


def is_recording_row(node, active_uid):
    """Whether \a node is the layer an armed REC records into: the active layer, never a folder."""
    return (active_uid != 0 and node.uid == active_uid and
            not isinstance(node, bpy.types.SculptLayerGroup))


def mesh_of(owner):
    return owner.data if owner is not None and owner.type == 'MESH' else None


@lru_cache(maxsize=8)
def _flatten_nodes_cached(session_uid, revision, mesh):
    """The tree in the order `rows_build` lists it, for one (mesh, tree revision) pair: deepest
    subtree first, then the row that holds it. The ordinals the Outliner hands back name positions
    in exactly this list, and the list only changes shape when the tree does -- influence,
    selection and names ride the same order. The cache lives on the module (not on the source
    instance) because the bridge builds the `self` a call sees per call, so nothing an instance
    attribute holds would survive to the next one; a handful of entries is plenty, one per mesh
    being looked at."""
    flat = []

    def collect(children, depth):
        for node in reversed(children):
            if isinstance(node, bpy.types.SculptLayerGroup):
                collect(node.children, depth + 1)
            flat.append((node, depth))

    collect(mesh.sculpt_layer_tree, 0)
    return flat


def _flatten_nodes(mesh):
    return _flatten_nodes_cached(mesh.session_uid, mesh.sculpt_layer_tree_revision, mesh)


def _node_at(mesh, ordinal):
    """The node the row \a ordinal named when the current build ran, or None."""
    flat = _flatten_nodes(mesh)
    if 0 <= ordinal < len(flat):
        return flat[ordinal][0]
    return None


def finish_open_mask_edit(mesh):
    """Apply the weight mask being painted, if a mask edit session is open, so the selection can
    move to another layer. False when the session could not be closed."""
    if not any(node.mask_edit_active for node in nodes_by_uid(mesh).values()):
        return True
    return _sculpt_op("layer_mask_edit_finish")


def _sculpt_op(opname, **kwargs):
    """Run a `sculpt.*` operator and answer whether it finished. A refusal (an open mask edit, a
    stale domain, a failed poll) is reported by the operator itself and logged here once -- the
    Outliner's C side reads a failed answer as a refused edit, so an exception would only add a
    traceback through the RNA call."""
    try:
        return getattr(bpy.ops.sculpt, opname)(**kwargs) == {'FINISHED'}
    except RuntimeError as ex:
        _log.warning("sculpt.%s refused: %s", opname, ex)
        return False


def _select_only(mesh, uid):
    """Make the node \a uid the sole selected one and the active layer, so a sculpt operator that
    acts on the active layer -- rather than an explicit uid -- addresses it. False when the node is
    gone or the select did not go through: a caller must not run a layer_* operator on that answer.
    The active layer is already the node's own when it matches, including the empty (None) case."""
    node = nodes_by_uid(mesh).get(uid)
    if node is None:
        return False
    _clear_layer_selection(mesh)
    node.select = True
    active = mesh.sculpt_layers_active
    if active is None or active.uid != uid:
        if not finish_open_mask_edit(mesh):
            return False
        if not _sculpt_op("layer_select", uid=uid):
            return False
    return True


def row_make_active(obj, uid):
    """Make the layer \a uid of the mesh object \a obj the sole selected one and the active layer,
    the way a click on its row does, so a button on the row acts on a row that reads as active in
    the Outliner as well. False when the layer is gone or could not be made active."""
    mesh = mesh_of(obj)
    return mesh is not None and _select_only(mesh, uid)


class OutlinerStackSourceSculptLayers(bpy.types.OutlinerStackSource):
    """The sculpt layers of the focused mesh, as the Outliner's Stack Layers shows them"""

    bl_idname = "sculpt_layers"
    ui_name = "Sculpt Layers"
    icon = 'SCULPTMODE_HLT'
    # The Focus Stack button's icon on the View Layer's object rows, and the mode entering the
    # stack with 'enter edit mode' asks for: the mode the source picker's icon names.
    focus_button_icon = 'SCULPTMODE_HLT'
    focus_object_mode = 'SCULPT'
    value_width = 2.6
    mode_width = 0.0
    action_width = 0.0
    icon_columns = 1
    supports_row_lock = False
    # The stack belongs to the object as a whole: the header offers no material picker for it.
    use_sub_selection = False

    def has_stack(self, context, object):
        mesh = mesh_of(object)
        return mesh is not None and len(mesh.sculpt_layer_tree) > 0

    def has_stack_key(self, context, object):
        # The tree's structural revision: it moves with every change that can flip has_stack
        # (an empty tree gaining its first node, or the reverse), and never repeats in a session.
        mesh = mesh_of(object)
        return float(mesh.sculpt_layer_tree_revision) if mesh is not None else 0.0

    def owner_get(self, context, object_uid, sub_index):
        if object_uid == 0:
            # The bridge hands the global context here, but read defensively all the same.
            active = context.object if context is not None else bpy.context.object
            return active if mesh_of(active) else None
        for object in bpy.data.objects:
            if object.session_uid == object_uid:
                return object if mesh_of(object) else None
        return None

    def state_hash(self, context, owner):
        mesh = mesh_of(owner)
        if mesh is None:
            return 0.0
        # RNA floats stay exact for integers up to 2^24; the mask keeps the mixed total there.
        mask = (1 << 24) - 1
        total = 0

        def mix(value):
            nonlocal total
            total = (total * 1000003 + value) & mask

        # The tree's structural revision moves with every add, remove, move and group edit, and
        # the counters beside it are the rest of what a rebuild keys on: the active layer (the
        # REC mark and the activation), the recording and solo switches, and the mode the drag
        # handle and REC buttons gate on. The per-node state a walk here used to mix -- names,
        # influence, flags, masks, color tags -- arrives through notifier_invalidates instead:
        # one Python call per mesh-data notifier beats a walk over every node on every redraw.
        mix(int(mesh.sculpt_layer_tree_revision))
        active = mesh.sculpt_layers_active
        mix((active.uid if active is not None else 0) * 7919)
        mix(len(mesh.sculpt_layers) * 104729)
        mix(int(owner.sculpt_layers_rec_active) * 131071)
        mix(int(mesh.sculpt_layers_solo_active) * 524287)
        mix(int(owner.mode == 'SCULPT') * 8191)
        return float(total)

    def notifier_invalidates(self, context, category, data, action):
        """Whether the rows have to be re-read outright for a notifier of the given category.

        Mesh-data edits -- a rename, a flag flip, a color tag, an influence or mask change from
        another editor -- leave the tree revision the state hash keys on alone, so they answer
        true here and the rows are re-read; materials, node graphs and images touch nothing the
        rows describe and answer false. The hook carries no ID, so any mesh's data notifier
        answers true: the cost is one rebuild, not a walk per redraw."""
        return category == _NC_GEOM and data == _ND_DATA

    def rows_build(self, context, owner, rows):
        mesh = mesh_of(owner)
        if mesh is None:
            return
        in_sculpt_mode = owner.mode == 'SCULPT'
        rec_active = owner.sculpt_layers_rec_active
        solo_active = mesh.sculpt_layers_solo_active
        active = mesh.sculpt_layers_active
        active_uid = active.uid if active is not None else 0

        # The rows list top first, so a folder's contents come before the folder itself in the
        # vector: walk the tree backwards, deepest subtree first, then the row that holds it.
        flat = _flatten_nodes(mesh)
        ordinal_of = {node.uid: index for index, (node, _depth) in enumerate(flat)}

        for node, depth in flat:
            kind = _node_kind(node)

            is_group = kind == KIND_GROUP
            parent = node.parent
            parent_ordinal = ordinal_of.get(parent.uid, -1) if parent is not None else -1

            row = rows.add_row(
                name=node.name,
                icon='FILE_FOLDER' if is_group else 'DECORATE',
                depth=depth,
                parent_ordinal=parent_ordinal,
                can_hold_children=is_group,
                has_children=is_group and len(node.children) > 0,
                enabled=node.enabled,
                locked=(False if is_group else node.lock),
                color_tag=(_color_tag_index(node) if is_group else 0),
                compact=False,
                stable_key=_key_of(node),
                unsupported_reason=("" if is_group or node.is_valid else
                                    "Stale layer: the mesh changed. Use Validate Stale Layers"),
                value=node,
                value_prop="influence",
            )

            if not is_group:
                # The drag handle sits before the Influence column and shows under the pointer, as
                # in the Properties tree. It runs the very operator that tree runs -- it makes the
                # layer active, then drags its influence -- with the layer's uid. Sculpt Mode only.
                rows.add_button(
                    row_ordinal=row,
                    icon='ARROW_LEFTRIGHT',
                    tooltip="Select and Drag Sculpt Layer Influence",
                    operator="sculpt.layer_select_and_drag_influence",
                    enabled=in_sculpt_mode and node.is_valid,
                    before_value=True,
                    hover_only=True,
                    int_prop="uid",
                    int_value=node.uid,
                )

            if not is_group:
                # The layer's own preview comes first and stays when a mask is added: the mask slot
                # follows it, as a Paint Layers row shows its channels slot and then its mask slot.
                # The click finishes an open mask edit session, so the Base slot doubles as the
                # "back to sculpting" button while one is open; without a session it just frames
                # the row's base preview, as before.
                rows.add_preview_slot(
                    row_ordinal=row,
                    icon='DECORATE',
                    glyph=_GLYPH_BASE,
                    label="Base",
                    tooltip=("Apply the mask edit and return to sculpting"
                             if node.mask_edit_active else ""),
                    operator="sculpt_layers.mask_edit_finish",
                    section_id="BASE",
                )
            if node.has_mask or node.mask_edit_active:
                rows.add_preview_slot(
                    row_ordinal=row,
                    icon='MOD_MASK' if node.mask_enabled else 'MOD_SUBSURF',
                    label="Mask",
                    tooltip="Edit this mask",
                    operator="sculpt_layers.mask_edit",
                    section_id="MASK",
                    keeps_row_icon=is_group,
                )
            if not is_group:
                # REC is a column of its own, the row's last one, on every layer: Off while
                # recording is not armed, On for the layer the strokes go into. Clicking another
                # layer's mark moves the recording there.
                recording_here = rec_active and is_recording_row(node, active_uid)
                rows.add_button(
                    row_ordinal=row,
                    icon='RECORD_ON' if recording_here else 'RECORD_OFF',
                    tooltip=("Sculpting records into this layer. Click to stop recording"
                             if recording_here else "Record sculpting into this layer"),
                    operator="sculpt_layers.rec_toggle",
                    pressed=recording_here,
                    enabled=in_sculpt_mode and not solo_active,
                )

    def row_activate(self, context, owner, ordinal):
        mesh = mesh_of(owner)
        node = _node_at(mesh, ordinal) if mesh is not None else None
        if node is None:
            return False
        if isinstance(node, bpy.types.SculptLayerGroup):
            # A folder is a target for Add, Move and Delete, the same way the Properties tree
            # selects a folder without making it the active layer. The row's selection itself is
            # carried from the tree store by row_set_selected.
            return True
        active = mesh.sculpt_layers_active
        if active is not None and active.uid == node.uid:
            # Already the active layer: selecting it again would only close an open mask edit
            # session, which a second click on the mask slot has to be able to toggle off itself.
            return True
        # Another layer's mask is still being painted: apply it first, or the selection is
        # refused and the click does nothing.
        return finish_open_mask_edit(mesh) and _sculpt_op("layer_select", uid=node.uid)

    def row_is_active(self, context, owner, ordinal):
        mesh = mesh_of(owner)
        node = _node_at(mesh, ordinal) if mesh is not None else None
        if node is None or isinstance(node, bpy.types.SculptLayerGroup):
            return False
        active = mesh.sculpt_layers_active
        return active is not None and active.uid == node.uid

    def row_is_selected(self, context, owner, ordinal):
        mesh = mesh_of(owner)
        node = _node_at(mesh, ordinal) if mesh is not None else None
        return node is not None and node.select

    def row_set_selected(self, context, owner, ordinal, select):
        """The Outliner's tree store is the selection the user sees here; mirror it into the
        layer flags the Properties tree and the sculpt operators read back."""
        mesh = mesh_of(owner)
        node = _node_at(mesh, ordinal) if mesh is not None else None
        if node is None or node.select == select:
            return False
        node.select = select
        return True

    def can_reorder(self, owner):
        return True

    def row_move(self, context, owner, from_ordinal, anchor_ordinal, place):
        mesh = mesh_of(owner)
        node = _node_at(mesh, from_ordinal) if mesh is not None else None
        if node is None:
            return -1
        anchor_uid = 0
        if anchor_ordinal >= 0:
            anchor = _node_at(mesh, anchor_ordinal)
            if anchor is not None:
                anchor_uid = anchor.uid
        location = {0: 'BEFORE', 1: 'AFTER', 2: 'INTO'}.get(place, 'AFTER')
        # The operator moves the node its uid names. Selecting that node here instead -- the way
        # the Properties tree's drag does -- would write the select flags outside the undo step
        # the move records, and undo would restore the tree while keeping this drag's selection.
        if not _sculpt_op(
            "layer_move_to", node_uid=node.uid, anchor_uid=anchor_uid, location=location
        ):
            return -1
        # The move reorders the tree, so the moved node now sits at a different ordinal. The C++
        # seam re-selects and re-activates whichever ordinal it is handed and reads -1 as "the move
        # did not happen" (leaving the cached rows stale), so report the node's new position -- the
        # same walk rows_build numbers by -- rather than the stale from_ordinal.
        for index, (moved, _depth) in enumerate(_flatten_nodes(mesh)):
            if moved.uid == node.uid:
                return index
        return -1

    def row_set_enabled(self, context, owner, ordinal, enable):
        mesh = mesh_of(owner)
        node = _node_at(mesh, ordinal) if mesh is not None else None
        if node is None:
            return False
        if node.enabled == enable:
            return True
        if isinstance(node, bpy.types.SculptLayerGroup):
            return _sculpt_op("layer_group_toggle_visibility", group_uid=node.uid)
        return _sculpt_op("layer_toggle_visibility", layer_uid=node.uid)

    def row_set_color_tag(self, context, owner, ordinal, color_tag):
        mesh = mesh_of(owner)
        node = _node_at(mesh, ordinal) if mesh is not None else None
        if not isinstance(node, bpy.types.SculptLayerGroup):
            return False
        tag = 'NONE' if color_tag == 0 else 'COLOR{}'.format(max(1, min(8, color_tag)))
        return _sculpt_op("layer_group_color_tag", group_uid=node.uid, color_tag=tag)

    def row_duplicate(self, context, owner, ordinal):
        mesh = mesh_of(owner)
        node = _node_at(mesh, ordinal) if mesh is not None else None
        if node is None or isinstance(node, bpy.types.SculptLayerGroup):
            return -1
        if not _select_only(mesh, node.uid):
            return -1
        before = set(nodes_by_uid(mesh).keys())
        if not _sculpt_op("layer_duplicate"):
            return -1
        # The copy is the one node the operator added; the Outliner reads -1 as "nothing happened"
        # and leaves the cached rows stale, so report the copy's new position.
        new_uids = set(nodes_by_uid(mesh).keys()) - before
        if not new_uids:
            return -1
        for index, (moved, _depth) in enumerate(_flatten_nodes(mesh)):
            if moved.uid in new_uids:
                return index
        return -1

    def row_rename(self, context, owner, ordinal, name):
        mesh = mesh_of(owner)
        node = _node_at(mesh, ordinal) if mesh is not None else None
        if node is None:
            return False
        # The one mutation without a layer operator of its own (no sculpt.layer_rename exists),
        # so the write goes through RNA and the undo step is recorded by hand here.
        node.name = name
        bpy.ops.ed.undo_push(message="Rename Sculpt Layer")
        return True

    def row_remove(self, context, owner, ordinal):
        mesh = mesh_of(owner)
        node = _node_at(mesh, ordinal) if mesh is not None else None
        if node is None:
            return False
        if isinstance(node, bpy.types.SculptLayerGroup):
            return _sculpt_op("layer_group_delete", group_uid=node.uid)
        if not _select_only(mesh, node.uid):
            return False
        return _sculpt_op("layer_remove")


def register():
    # Registering the class creates the C++ `StackSourcePython` behind it and adds it to the
    # source registry: every outliner's caches are invalidated and the picker gains the item.
    bpy.utils.register_class(OutlinerStackSourceSculptLayers)
    from .menus import draw_header, draw_header_start
    stack_source_menu_register("sculpt_layers",
                               add_menu="OUTLINER_MT_sculpt_layer_add",
                               context_menu="OUTLINER_MT_sculpt_layer_stack_context",
                               header=draw_header,
                               header_start=draw_header_start)


def unregister():
    # Unregistering the class takes the source out of the registry: spaces left on it fall back
    # to the first built-in source, the way a file naming an unknown source does.
    stack_source_menu_unregister("sculpt_layers")
    bpy.utils.unregister_class(OutlinerStackSourceSculptLayers)
    _flatten_nodes_cached.cache_clear()
