# SPDX-FileCopyrightText: 2026 Phantom Authors
# SPDX-FileCopyrightText: 2026 Modifications by Nazir
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Sculpt brushes remember their settings between sessions.

A brush's settings live in the .blend file, and a fresh session usually starts from a new file,
so changes to a brush (size, strength, spacing, falloff, alpha, ...) are lost when Blender is
closed. This module mirrors the settings of every sculpt brush into ``sculpt_brush_settings.json``
in the user config directory, and puts them back as brushes are loaded, so a brush feels the same
in every file. The brush selected when Blender was closed is remembered too, and selected again
once Sculpt Mode is entered. The sculpt tool's unified size/strength are kept as well.

Only simple values are stored (numbers, booleans, text, choices and their small arrays), plus the
brush's texture slots and the names of their textures. Everything else in a brush (its nested
sub-settings blocks, its preview, asset metadata and so on) is left alone.

The control extension (``xnazir_brush_advanced``) only offers the settings for this feature
(on/off and which libraries). Without the extension, or while it is disabled, the feature is
still ON with the defaults ``DEFAULT_ENABLED`` / ``DEFAULT_SCOPE``; the only way to switch it off
is the setting inside the extension.
"""

import json
import os
import time

import bpy
from bpy.app.handlers import persistent

FILE_NAME = "sculpt_brush_settings.json"
FORMAT_VERSION = 1
# How often loaded brushes are checked for changes, and how long changes wait before the file
# is written (a slider drag changes the brush on every mouse move).
POLL_INTERVAL = 0.5
# Only the active brush is compared on every poll; all brushes are compared this often (and
# whenever the active brush changes or a file is loaded), to keep the poll cheap.
FULL_SCAN_INTERVAL = 5.0
SAVE_DELAY = 2.0
# How many polls to wait for brush assets to finish loading, or for the sculpt context to exist,
# before giving up (about ten seconds at the poll interval above).
RESTORE_ATTEMPTS = 20
# The brush's identity, and data owned by Blender rather than the user.
_SKIP_PROPS = {
    "name", "name_full", "library", "asset_data", "preview", "has_unsaved_changes",
    "users", "use_fake_user", "use_extra_user", "original", "is_evaluated",
    "is_linked", "is_library_indirect", "is_embedded_data", "is_runtime_data",
    "session_uid", "tag", "id_data", "rna_type", "output_node",
    # Which paint modes the brush is offered in is asset placement, not brush behaviour.
    "use_paint_sculpt", "use_paint_uv_sculpt", "use_paint_vertex", "use_paint_weight",
    "use_paint_image", "use_paint_grease_pencil", "use_vertex_grease_pencil",
    "use_paint_sculpt_curves",
}
# Pointers to other data-blocks, stored by name and restored when that data-block is loaded.
_ID_PROPS = ("texture", "mask_texture")
# Every other pointer property on a brush (its falloff curves, texture slots, the sub-settings
# blocks such as gpencil_settings / mesh_automasking_settings, ...) is deliberately not stored:
# none of it was persisted before, so persisting it now would change behaviour, and curves and
# collections are not simple values. Checked against the brush RNA in rna_brush.cc.
_SIMPLE_TYPES = {'BOOLEAN', 'INT', 'FLOAT', 'ENUM', 'STRING'}
_SPECIAL_KEYS = {"tool_settings", "active_brush"}
_TOOLS_PROPS = {'use_unified_size', 'size', 'strength'}

# The control extension (release/extensions/system/xnazir_brush_advanced). Without it, or while it
# is disabled, the defaults below apply. This is the single definition of the add-on ID: it is
# the key of `preferences.addons` and must match the extension's `bl_idname` (its `__package__`).
ADDON_ID = "bl_ext.system.xnazir_brush_advanced"
SCOPE_ESSENTIALS = 'ESSENTIALS'
SCOPE_ALL = 'ALL'
# The single definition of the defaults, also imported by the extension for its settings.
DEFAULT_ENABLED = True
DEFAULT_SCOPE = SCOPE_ESSENTIALS
LIBRARY_ESSENTIALS = "Essentials"
LIBRARY_LOCAL = "Local"

_state = None


class _State:
    """All mutable module state in one object; register() creates a fresh instance so a
    re-registration never inherits state from the previous one."""

    def __init__(self):
        # True once the settings file has been read successfully or confirmed missing: until
        # then nothing may be written, so a newer file is never clobbered.
        self.loaded_ok = False
        self.data = {}
        self.seen = {}
        self.tools = {}
        self.tools_loaded = False
        self.tools_attempts = 0
        self.dirty = False
        self.dirty_keys = set()
        # Records the user reset: removed from the file on the next save, even if another
        # instance wrote them.
        self.deleted_keys = set()
        # Library label of each brush key, found once per brush.
        self.labels = {}
        self.last_save = 0.0
        self.last_full_scan = 0.0
        # Name of the active brush at the last poll, to notice a switch.
        self.active_brush_name = None
        self.active = {}
        self.active_seen = None
        self.active_ready = False
        self.active_attempts = 0


# -----------------------------------------------------------------------------
# Storage

def _path():
    return os.path.join(bpy.utils.user_resource('CONFIG'), FILE_NAME)


def _read_disk():
    """The payload on disk: {} when the file is missing, None when it cannot be read."""
    try:
        with open(_path(), encoding="utf-8") as fh:
            payload = json.load(fh)
    except FileNotFoundError:
        return {}
    except (OSError, ValueError):
        return None
    return payload if isinstance(payload, dict) else None


def _brush_records(payload):
    """The brush records of a payload as {key: settings}: every entry that is a stored settings
    dict (the ``{"props": ..., "textures": ...}`` shape). Non-conforming entries are dropped."""
    brushes = payload.get("brushes")
    if not isinstance(brushes, dict):
        return {}
    records = {}
    for name, settings in brushes.items():
        if isinstance(name, str) and isinstance(settings, dict) and \
                isinstance(settings.get("props"), dict):
            records[name] = settings
    return records


def _payload_writable(payload):
    """Whether the on-disk `payload` may be read and overwritten: one of a version this module
    understands."""
    if not isinstance(payload, dict):
        return False
    version = payload.get("version", 0)
    return isinstance(version, int) and version <= FORMAT_VERSION


def _load(st):
    """Fill `st` from the settings file. Returns True when the file was read successfully or
    is missing or unreadable (a copy is kept as ``.bak`` and the session starts fresh); False
    for a newer-version file, which must never be overwritten."""
    try:
        with open(_path(), encoding="utf-8") as fh:
            payload = json.load(fh)
    except FileNotFoundError:
        st.loaded_ok = True
        return True
    except (OSError, ValueError):
        print("brush-settings: could not read the settings file, starting fresh")
        _backup_broken()
        st.loaded_ok = True
        return True

    if not isinstance(payload, dict):
        _backup_broken()
        st.loaded_ok = True
        return True

    if not _payload_writable(payload):
        print("brush-settings: settings file version {} is newer than {}".format(
            payload.get("version", 0), FORMAT_VERSION))
        return False

    st.data = _brush_records(payload)

    tools = payload.get("tool_settings")
    if isinstance(tools, dict):
        st.tools = {key: value for key, value in tools.items() if key in _TOOLS_PROPS}
    st.tools_loaded = False

    active = _valid_active(payload.get("active_brush"))
    st.active = active if active is not None else {}
    st.loaded_ok = True
    return True


def _addon_prefs():
    """The control extension's preferences, or None while it is not enabled."""
    addon = bpy.context.preferences.addons.get(ADDON_ID)
    return addon.preferences if addon is not None else None


def _enabled():
    """Whether the feature is switched on (it is, unless the extension turns it off)."""
    if _state is None:
        return False
    prefs = _addon_prefs()
    return getattr(prefs, "persistence_enabled", DEFAULT_ENABLED)


def _scope():
    """Which libraries' brushes are remembered: DEFAULT_SCOPE unless the extension says otherwise."""
    prefs = _addon_prefs()
    return getattr(prefs, "persistence_scope", DEFAULT_SCOPE)


def _save(st, force=False):
    """Write the settings file, keeping any brush this session did not touch (another Blender
    instance may have written it). Never writes unless `st.loaded_ok`, and never overwrites a
    newer file. `force` writes even while the feature is switched off (a reset must stick)."""
    if bpy.app.background:
        return
    if st is None or not st.loaded_ok:
        return
    if not force and not _enabled():
        return
    if st.tools_loaded:
        st.dirty_keys.add("tool_settings")
    if st.active:
        st.dirty_keys.add("active_brush")
    if not st.dirty_keys and not st.deleted_keys:
        return

    payload = _read_disk()
    if payload is None:
        print("brush-settings: settings file became unreadable, skipping this save")
        return
    if not _payload_writable(payload):
        print("brush-settings: not overwriting a newer settings file")
        return

    payload["version"] = FORMAT_VERSION
    if "tool_settings" in st.dirty_keys:
        payload["tool_settings"] = {name: st.tools.get(name) for name in _TOOLS_PROPS}
    if "active_brush" in st.dirty_keys:
        payload["active_brush"] = st.active
    payload["brushes"] = _merge_records(_brush_records(payload), st)

    path = _path()
    temp = path + ".tmp"
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(temp, "w", encoding="utf-8") as fh:
            json.dump(payload, fh, indent=2, sort_keys=True)
        os.replace(temp, path)
    except OSError as ex:
        print("brush-settings: could not save:", ex)
        return
    st.dirty = False
    st.dirty_keys.clear()
    st.deleted_keys.clear()


def _merge_records(disk_records, st):
    """The brush records to write: `disk_records` (which may hold brushes this session did not
    touch) with this session's changed brushes put in and its reset ones taken out."""
    merged = dict(disk_records)
    for key in list(st.dirty_keys):
        if key in _SPECIAL_KEYS:
            continue
        if key in st.data:
            merged[key] = st.data[key]
        else:
            # Another instance deleted the brush this session was tracking: forget it.
            st.dirty_keys.discard(key)
    for key in st.deleted_keys:
        merged.pop(key, None)
    return merged


def _mark_dirty(st, key):
    """Note that the record of `key` changed. A brush changed after a reset is wanted again, so it
    must not be removed by the pending reset."""
    st.dirty = True
    st.dirty_keys.add(key)
    st.deleted_keys.discard(key)


def _backup_broken():
    """Copy the unreadable file aside once, so its content is not lost."""
    path = _path()
    backup = path + ".bak"
    if os.path.exists(backup):
        return
    try:
        with open(path, "rb") as src, open(backup, "wb") as dst:
            dst.write(src.read())
        print("brush-settings: kept a copy of the unreadable file at", backup)
    except OSError as ex:
        print("brush-settings: could not back up the unreadable file:", ex)


# -----------------------------------------------------------------------------
# Reading and writing a brush's settings

def _coerce(prop, value):
    """`value` converted for `prop` (a plain value or a list for array properties), or None
    when it does not fit."""
    try:
        if prop.type == 'ENUM':
            if prop.is_enum_flag:
                # Flag enums are sets of identifiers; stored as a sorted list.
                return sorted(str(item) for item in value)
            # Some enums are context dependent and have no active item outside their mode; an
            # invalid identifier is rejected later by the setter itself.
            return str(value) or None
        if prop.type == 'STRING':
            return str(value)
        if prop.is_array:
            items = list(value)
        else:
            items = [value]
        if prop.type == 'BOOLEAN':
            converted = [bool(item) for item in items]
        elif prop.type == 'INT':
            converted = [int(item) for item in items]
        else:
            converted = [float(item) for item in items]
    except (AttributeError, TypeError, ValueError):
        return None
    if prop.is_array:
        return converted
    return converted[0]


def _simple_value(struct, prop):
    """A property's value as JSON data, or None when it can't be stored."""
    try:
        value = getattr(struct, prop.identifier)
    except (AttributeError, TypeError, ValueError):
        return None
    return _coerce(prop, value)


# The storable properties of a struct type, by its RNA identifier: the RNA definition does not
# change while Blender runs, so filtering the property list once is enough.
_simple_props_cache = {}


def _simple_props(struct):
    key = struct.bl_rna.identifier
    props = _simple_props_cache.get(key)
    if props is None:
        props = []
        for prop in struct.bl_rna.properties:
            ident = prop.identifier
            if ident.startswith("_") or ident in _SKIP_PROPS or ident in _ID_PROPS:
                continue
            # A pointer (sub-settings block, curve, texture slot) is never a simple value, so it
            # falls through the type check below.
            if prop.is_readonly or prop.type not in _SIMPLE_TYPES:
                continue
            props.append(prop)
        _simple_props_cache[key] = props
    return props


def _collect_props(struct):
    props = {}
    for prop in _simple_props(struct):
        ident = prop.identifier
        value = _simple_value(struct, prop)
        if value is not None:
            props[ident] = value
    return props


def _brush_key(brush):
    """The storage key of a brush. Brushes linked from an asset library are keyed by their library
    file name and ID name (unique across libraries, independent of where the library is
    installed); others fall back to the brush name. Old name-keyed records are still read (see
    :func:`_stored_for`).
    """
    try:
        ref = brush.library_weak_reference
        if ref is not None and ref.id_name:
            return "asset::{}::{}".format(os.path.basename(ref.filepath), ref.id_name)
    except (AttributeError, RuntimeError):
        pass
    return brush.name


def _stored_for(brush, st):
    """The saved settings of `brush`: by its key first, then by name as a fallback for files
    written before brushes were keyed by asset."""
    record = st.data.get(_brush_key(brush))
    if record is None:
        record = st.data.get(brush.name)
    return record if isinstance(record, dict) else None


def _brush_file(brush):
    """The normalized path of the .blend a brush comes from, or "" for a brush of this file."""
    path = ""
    try:
        if brush.library is not None:
            path = brush.library.filepath
        elif brush.library_weak_reference is not None:
            path = brush.library_weak_reference.filepath
    except (AttributeError, RuntimeError):
        return ""
    if not path:
        return ""
    return os.path.normcase(os.path.abspath(bpy.path.abspath(path)))


def _library_label(brush):
    """The asset library a brush belongs to: "Essentials", the name of a user asset library,
    or "Local" for a brush that does not come from a library file."""
    path = _brush_file(brush)
    if not path:
        return LIBRARY_LOCAL
    essentials = os.path.normcase(os.path.abspath(
        os.path.join(bpy.utils.system_resource('DATAFILES'), "assets")))
    if path.startswith(essentials + os.sep):
        return LIBRARY_ESSENTIALS
    for library in bpy.context.preferences.filepaths.asset_libraries:
        if not library.path:
            # `abspath("")` is the current directory, which would match unrelated files.
            continue
        root = os.path.normcase(os.path.abspath(bpy.path.abspath(library.path)))
        if path.startswith(root + os.sep):
            return library.name
    return os.path.basename(os.path.dirname(path)) or LIBRARY_LOCAL


def _collect(brush):
    textures = {}
    for ident in _ID_PROPS:
        texture = getattr(brush, ident, None)
        textures[ident] = texture.name if texture is not None else None
    return {"props": _collect_props(brush), "textures": textures}


def _apply_props(struct, props):
    for ident, value in props.items():
        prop = struct.bl_rna.properties.get(ident)
        if prop is None:
            continue
        if prop.is_readonly or prop.type not in _SIMPLE_TYPES:
            continue
        converted = _coerce(prop, value)
        if converted is None:
            continue
        if prop.type == 'ENUM' and prop.is_enum_flag:
            converted = set(converted)
        try:
            setattr(struct, ident, converted)
        except (AttributeError, TypeError, ValueError):
            # A setting from an older session (or another Blender version) may not fit.
            continue


def _apply(brush, settings):
    props = settings.get("props")
    if isinstance(props, dict):
        _apply_props(brush, props)
    textures = settings.get("textures")
    if not isinstance(textures, dict):
        return
    for ident in _ID_PROPS:
        name = textures.get(ident)
        try:
            if name is None:
                setattr(brush, ident, None)
            elif isinstance(name, str):
                texture = bpy.data.textures.get(name)
                if texture is not None:
                    setattr(brush, ident, texture)
        except (AttributeError, TypeError, ValueError):
            continue


# -----------------------------------------------------------------------------
# The selected brush

def _valid_active(value):
    """A saved active brush as stored data, or None when it can't be used."""
    if not isinstance(value, dict):
        return None
    name = value.get("name")
    if not isinstance(name, str):
        return None
    asset = value.get("asset")
    if isinstance(asset, dict):
        asset = {
            "type": asset.get("type"),
            "identifier": asset.get("identifier"),
            "relative": asset.get("relative"),
        }
        if (isinstance(asset["type"], str) and isinstance(asset["identifier"], str) and
                isinstance(asset["relative"], str) and asset["relative"]):
            return {"name": name, "asset": asset}
        return None
    return {"name": name}


def _current_active():
    """The active sculpt brush as stored data, or None outside Sculpt Mode."""
    try:
        sculpt = bpy.context.tool_settings.sculpt
        brush = sculpt.brush
    except AttributeError:
        return None
    if brush is None:
        return None
    entry = {"name": brush.name}
    try:
        ref = sculpt.brush_asset_reference
    except AttributeError:
        ref = None
    if ref is not None and ref.relative_asset_identifier:
        entry["asset"] = {
            "type": ref.asset_library_type,
            "identifier": ref.asset_library_identifier,
            "relative": ref.relative_asset_identifier,
        }
    return entry


def _restore_active(st):
    """Select the brush saved last session, once Sculpt Mode and its assets are ready."""
    if st.active_ready:
        return
    asset = st.active.get("asset") if isinstance(st.active, dict) else None
    if asset is None:
        # Nothing selectable to restore (first run, or the brush isn't an asset).
        st.active_ready = True
        st.active_seen = _current_active()
        return
    if bpy.context.mode != 'SCULPT':
        return
    if st.active_attempts >= RESTORE_ATTEMPTS:
        print("brush-settings: could not restore the sculpt brush:", st.active.get("name", ""))
        st.active_ready = True
        st.active_seen = _current_active()
        return

    current = _current_active()
    if current is not None and current.get("asset") == asset:
        st.active_ready = True
        st.active_seen = current
        return

    st.active_attempts += 1
    try:
        bpy.ops.brush.asset_activate(
            asset_library_type=asset["type"],
            asset_library_identifier=asset["identifier"],
            relative_asset_identifier=asset["relative"],
        )
    except RuntimeError as ex:
        # The brush assets may still be loading; try again on the next poll.
        if "loading is unfinished" in str(ex):
            return
        print("brush-settings: could not restore the sculpt brush:", ex)
        st.active_ready = True
        st.active_seen = _current_active()
        return

    current = _current_active()
    if current is not None and current.get("asset") == asset:
        st.active_ready = True
        st.active_seen = current


def _update_active(st):
    if not st.active_ready:
        _restore_active(st)
        return
    current = _current_active()
    if current is None:
        return
    if st.active_seen is None:
        # The first brush after startup (or a file load) isn't a choice the user made.
        st.active_seen = current
        return
    if current != st.active_seen:
        st.active_seen = current
        st.active = current
        st.dirty = True


# -----------------------------------------------------------------------------
# Keeping the JSON in step with the brushes in the app

def _sculpt_brushes():
    return [brush for brush in bpy.data.brushes if brush.use_paint_sculpt]


def _active_sculpt_brush():
    try:
        return bpy.context.tool_settings.sculpt.brush
    except AttributeError:
        return None


def _update_brushes(st, only=None):
    """Compare the sculpt brushes with their last snapshot. With `only` (a brush), just that one
    is looked at, which is what the frequent poll does; a full pass handles everything else."""
    present = set()
    scope = _scope()
    brushes = _sculpt_brushes() if only is None else [only]
    for brush in brushes:
        key = _brush_key(brush)
        label = st.labels.get(key)
        if label is None:
            label = st.labels[key] = _library_label(brush)
        if scope != SCOPE_ALL and label != LIBRARY_ESSENTIALS:
            continue
        present.add(key)
        try:
            snapshot = _collect(brush)
        except Exception as ex:  # A broken data-block shouldn't stop the others.
            print("brush-settings: could not read:", ex)
            continue
        if key not in st.seen:
            # A brush just loaded (or a new file just opened): put its saved settings back.
            stored = _stored_for(brush, st)
            if stored is not None:
                _apply(brush, stored)
                snapshot = _collect(brush)
                if "library" not in stored:
                    # A record written before libraries were tracked: add the missing info.
                    st.data[key] = dict(snapshot, library=label, name=brush.name)
                    _mark_dirty(st, key)
        elif snapshot != st.seen[key]:
            st.data[key] = dict(snapshot, library=label, name=brush.name)
            _mark_dirty(st, key)
        st.seen[key] = snapshot
    if only is not None:
        return
    for key in list(st.seen):
        if key not in present:
            del st.seen[key]


def _unified_settings():
    """The sculpt unified paint settings of the context, or None until the context exists."""
    scene = getattr(bpy.context, "scene", None)
    if scene is None:
        return None
    sculpt = getattr(scene.tool_settings, "sculpt", None)
    if sculpt is None:
        return None
    return sculpt.unified_paint_settings


def _update_tool_settings(st):
    if not st.tools_loaded:
        return
    settings = _unified_settings()
    if settings is None:
        return
    for key in _TOOLS_PROPS:
        value = getattr(settings, key)
        if st.tools.get(key) != value:
            st.tools[key] = value
            st.dirty = True
            st.dirty_keys.add("tool_settings")


def _load_tool_settings():
    """Timer callback: put the saved unified size/strength back once a context exists, retrying
    while it does not. Also called from load_post, so freshly opened files get them too."""
    st = _state
    if st is None or not _enabled():
        return None
    settings = _unified_settings()
    if settings is None:
        st.tools_attempts += 1
        if st.tools_attempts >= RESTORE_ATTEMPTS:
            print("brush-settings: could not find a sculpt context to restore tool settings in")
            st.tools_loaded = True
            return None
        return POLL_INTERVAL
    for key in _TOOLS_PROPS:
        if key in st.tools:
            try:
                setattr(settings, key, st.tools[key])
            except (AttributeError, TypeError, ValueError):
                # A value saved by another Blender version may not fit.
                continue
    st.tools_loaded = True
    st.tools_attempts = 0
    # Returning None lets the timer retire itself; no explicit unregister from inside it.
    return None


def _tick():
    st = _state
    if st is None or not _enabled():
        return POLL_INTERVAL
    # Polling only costs anything in Sculpt Mode with a window to poll for.
    window = getattr(getattr(bpy, "context", None), "window", None)
    if window is None or bpy.context.mode != 'SCULPT':
        return POLL_INTERVAL

    now = time.monotonic()
    try:
        active = _active_sculpt_brush()
        active_name = active.name if active is not None else None
        if (active is None or active_name != st.active_brush_name or
                now - st.last_full_scan >= FULL_SCAN_INTERVAL):
            st.active_brush_name = active_name
            st.last_full_scan = now
            _update_brushes(st)
        else:
            _update_brushes(st, only=active)
    except Exception as ex:  # Keep the timer alive, whatever a brush does.
        print("brush-settings: could not update:", ex)

    try:
        _update_tool_settings(st)
    except Exception as ex:
        print("brush-settings: could not update tool settings:", ex)

    try:
        _update_active(st)
    except Exception as ex:
        print("brush-settings: could not update the selected brush:", ex)

    if st.dirty and now - st.last_save >= SAVE_DELAY:
        st.last_save = now
        _save(st)

    return POLL_INTERVAL


@persistent
def _on_load(*_args):
    st = _state
    if st is None or not _enabled():
        return
    # Brushes of the file that just loaded are new to this session: restore their settings.
    st.seen.clear()
    st.last_full_scan = 0.0
    # Loading a file changes the active brush, but that isn't a choice the user made.
    st.active_seen = _current_active()
    # And the tool settings of the new file are not the saved ones yet.
    st.tools_loaded = False
    _load_tool_settings()


def register():
    global _state
    st = _State()
    _state = st
    _load(st)
    if not bpy.app.background:
        _start_runtime()


def _start_runtime():
    if _on_load not in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.append(_on_load)
    if bpy.app.timers.is_registered(_tick):
        return
    if not bpy.app.timers.is_registered(_load_tool_settings):
        # The first try is delayed; afterwards it re-arms itself until the context exists.
        bpy.app.timers.register(_load_tool_settings, first_interval=1.0, persistent=False)
    bpy.app.timers.register(_tick, first_interval=POLL_INTERVAL, persistent=True)


def _stop_runtime():
    if bpy.app.timers.is_registered(_tick):
        bpy.app.timers.unregister(_tick)
    if bpy.app.timers.is_registered(_load_tool_settings):
        bpy.app.timers.unregister(_load_tool_settings)
    if _on_load in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.remove(_on_load)


def saved_brushes():
    """The brushes with saved settings as sorted (key, library, name) tuples."""
    st = _state
    if st is None:
        return []
    records = [
        (key, record.get("library", "Unknown"), record.get("name", key))
        for key, record in st.data.items()
    ]
    return sorted(records, key=lambda item: (item[1].lower(), item[2].lower()))


def forget(keys):
    """Drop the saved settings of the given brush keys. Brushes keep their current values for
    this session; only what is remembered for the next one is removed."""
    st = _state
    if st is None:
        return
    for key in keys:
        st.data.pop(key, None)
        st.dirty_keys.discard(key)
        st.deleted_keys.add(key)
    _save(st, force=True)


def forget_library(library):
    """Drop the saved settings of every brush of one asset library."""
    forget([key for key, lib, _name in saved_brushes() if lib == library])


def unregister():
    global _state
    _stop_runtime()
    # Unregister runs on exit: save what the last timer tick may not have written yet. This is
    # the only exit-save point (no atexit).
    _save(_state)
    _state = None
