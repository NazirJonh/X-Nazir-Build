# SPDX-FileCopyrightText: 2009-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy
import json
import os
import re
import shutil
import time
from contextlib import contextmanager
from datetime import datetime
from bpy.types import (
    Header,
    Menu,
    Operator,
    Panel,
    PropertyGroup,
    UIList,
)
from bpy.app.translations import (
    contexts as i18n_contexts,
    pgettext_iface as iface_,
    pgettext_rpt as rpt_,
)
from bl_ui.utils import PresetPanel

# Import glyph library for integration
try:
    from bl_ui.glyph_library import get_glyph_library
except ImportError:
    get_glyph_library = None

if not hasattr(bpy.types.WindowManager, "category_tag_glyph_hex"):
    bpy.types.WindowManager.category_tag_glyph_hex = bpy.props.StringProperty(default="", options={'HIDDEN'})


# -----------------------------------------------------------------------------
# Tag System - Infrastructure Utilities (CleanPanels patterns)

TAG_DEBUG = True
TAG_BACKUP_ENABLED = False  # Отключено временно для отладки


# Единый источник режимов тегов (должен соответствовать CategoryTagMode в DNA/RNA).
_CATEGORY_TAG_MODES = (
    ("OBJECT_MODE", "OBJECT", 0, "Object Mode", 'OBJECT_DATAMODE'),
    ("EDIT_MODE", "EDIT", 1, "Edit Mode", 'EDITMODE_HLT'),
    ("SCULPT_MODE", "SCULPT", 2, "Sculpt Mode", 'SCULPTMODE_HLT'),
    ("VERTEX_PAINT", "VERTEX_PAINT", 3, "Vertex Paint", 'VPAINT_HLT'),
    ("WEIGHT_PAINT", "WEIGHT_PAINT", 4, "Weight Paint", 'WPAINT_HLT'),
    ("TEXTURE_PAINT", "TEXTURE_PAINT", 5, "Texture Paint", 'TPAINT_HLT'),
    ("UV_EDIT", "UV_EDIT", 6, "UV Edit", 'UV'),
    ("POSE_MODE", "POSE", 7, "Pose Mode", 'POSE_HLT'),
)
_CATEGORY_TAG_MODE_NAME_TO_FLAG = {name: (1 << bit) for name, _id, bit, _label, _icon in _CATEGORY_TAG_MODES}
_CATEGORY_TAG_MODE_FLAG_TO_NAME = {(1 << bit): name for name, _id, bit, _label, _icon in _CATEGORY_TAG_MODES}
_CATEGORY_TAG_MODE_ID_TO_BIT = {mode_id: bit for _name, mode_id, bit, _label, _icon in _CATEGORY_TAG_MODES}
_CATEGORY_TAG_ALL_MODE_FLAGS = sum((1 << bit) for _name, _id, bit, _label, _icon in _CATEGORY_TAG_MODES)
_CATEGORY_TAG_DEFAULT_MODE_FLAGS = (1 << 0) | (1 << 1) | (1 << 2)
_CATEGORY_TAG_FILTER_ENUM_TO_FLAG = {
    0: 0,
    "ALL": 0,
    1: (1 << 0),
    "OBJECT_MODE": (1 << 0),
    "OBJECT": (1 << 0),
    2: (1 << 1),
    "EDIT_MODE": (1 << 1),
    "EDIT": (1 << 1),
    3: (1 << 2),
    "SCULPT_MODE": (1 << 2),
    "SCULPT": (1 << 2),
    4: (1 << 3),
    "VERTEX_PAINT": (1 << 3),
    5: (1 << 4),
    "WEIGHT_PAINT": (1 << 4),
    6: (1 << 5),
    "TEXTURE_PAINT": (1 << 5),
    7: (1 << 6),
    "UV_EDIT": (1 << 6),
    8: (1 << 7),
    "POSE_MODE": (1 << 7),
    "POSE": (1 << 7),
}

def tag_log(message, level="INFO"):
    """Logging for tag system operations."""
    if TAG_DEBUG or level == "ERROR":
        print(f"[TAGS][{level}] {message}")


def _get_tag_filter_mode_flag_from_wm(wm):
    """Convert WindowManager.category_tag_filter_mode enum index to CategoryTagMode bit flag."""
    enum_value = getattr(wm, "category_tag_filter_mode", 0)
    if isinstance(enum_value, str):
        return _CATEGORY_TAG_FILTER_ENUM_TO_FLAG.get(enum_value, 0)
    try:
        return _CATEGORY_TAG_FILTER_ENUM_TO_FLAG.get(int(enum_value), 0)
    except (TypeError, ValueError):
        return 0


def get_current_tag_mode_flag(context):
    """Get the current object mode as a CategoryTagMode bitmask.
    
    This is the Python equivalent of the C++ get_current_tag_mode_flag() function.
    Returns a bit flag corresponding to the current object mode.
    """
    area = getattr(context, "area", None)
    if area is not None:
        if area.type == 'NODE_EDITOR':
            snode = context.space_data
            if snode is not None:
                if snode.tree_type == 'GeometryNodeTree':
                    return _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("EDIT_MODE", 0)
                if snode.tree_type == 'ShaderNodeTree':
                    return _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("OBJECT_MODE", 0)
        elif area.type == 'IMAGE_EDITOR':
            sima = context.space_data
            if sima is not None:
                if sima.mode == 'PAINT':
                    return _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("TEXTURE_PAINT", 0)
                if sima.mode == 'UV':
                    return _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("UV_EDIT", 0)

    ob = context.active_object
    if not ob:
        return _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("OBJECT_MODE", 0)
    
    mode = ob.mode
    mode_map = {
        'OBJECT': _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("OBJECT_MODE", 0),
        'EDIT': _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("EDIT_MODE", 0),
        'SCULPT': _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("SCULPT_MODE", 0),
        'VERTEX_PAINT': _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("VERTEX_PAINT", 0),
        'WEIGHT_PAINT': _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("WEIGHT_PAINT", 0),
        'TEXTURE_PAINT': _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("TEXTURE_PAINT", 0),
        'PAINT_UV': _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("UV_EDIT", 0),
        'POSE': _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("POSE_MODE", 0),
    }
    return mode_map.get(mode, _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("OBJECT_MODE", 0))


@contextmanager
def safe_file_write(filepath):
    """Atomic file write with rollback on error and retry logic for Windows."""
    import time
    temp_path = f"{filepath}.tmp"
    max_retries = 3
    retry_delay = 0.1  # 100ms

    try:
        with open(temp_path, 'w', encoding='utf-8') as f:
            yield f

        # Atomic rename with retry on Windows (file may be locked)
        for attempt in range(max_retries):
            try:
                os.replace(temp_path, filepath)
                tag_log(f"Saved: {filepath}")
                return
            except PermissionError as e:
                if attempt < max_retries - 1:
                    tag_log(f"Retry {attempt + 1}/{max_retries} for {filepath}: {e}")
                    time.sleep(retry_delay)
                    retry_delay *= 2  # Exponential backoff
                else:
                    raise e
    except Exception as e:
        # Remove temp file on error
        if os.path.exists(temp_path):
            try:
                os.remove(temp_path)
            except:
                pass
        tag_log(f"Failed to save {filepath}: {e}", "ERROR")
        raise e


def load_json_safely(filepath, default_structure):
    """Load JSON with fallback to defaults on corruption."""
    if not os.path.exists(filepath):
        tag_log(f"File not found: {filepath}, creating defaults")
        return default_structure

    try:
        with open(filepath, 'r', encoding='utf-8') as f:
            data = json.load(f)
        tag_log(f"Loaded: {filepath}")
        return data
    except json.JSONDecodeError as e:
        tag_log(f"JSON corrupted: {e}", "ERROR")
        # Rename corrupted file for recovery
        os.rename(filepath, f"{filepath}.corrupted_{int(time.time())}")
        return default_structure
    except Exception as e:
        tag_log(f"Load error: {e}", "ERROR")
        return default_structure


def create_backup(filepath):
    """Create timestamped backup before overwriting."""
    if not TAG_BACKUP_ENABLED:
        return None
    if not os.path.exists(filepath):
        return None

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    backup_path = f"{filepath}.backup_{timestamp}"

    # Keep only last 5 backups
    backup_dir = os.path.dirname(filepath)
    basename = os.path.basename(filepath)
    existing_backups = sorted([
        f for f in os.listdir(backup_dir)
        if f.startswith(basename) and ".backup_" in f
    ])

    while len(existing_backups) >= 5:
        old_backup = os.path.join(backup_dir, existing_backups.pop(0))
        os.remove(old_backup)
        tag_log(f"Removed old backup: {old_backup}")

    shutil.copy(filepath, backup_path)
    tag_log(f"Created backup: {backup_path}")
    return backup_path


# -----------------------------------------------------------------------------
# Category Glyph Mappings - JSON-based storage

# Default category glyph mappings (Material Symbols)
# Structure: {category: {"glyph": str, "display_name": str, "color": [r, g, b],
#                        "default_glyph": str, "default_display_name": str}}
DEFAULT_CATEGORY_GLYPHS = {
    # Common categories
    "Item": {"glyph": "\ueb75", "display_name": "", "color": [0.0, 0.0, 0.0],
             "default_glyph": "\ueb75", "default_display_name": ""},       # visibility
    "View": {"glyph": "\uf4c9", "display_name": "", "color": [0.0, 0.0, 0.0],
             "default_glyph": "\uf4c9", "default_display_name": ""},       # visibility
    "Edit": {"glyph": "\ue3c9", "display_name": "", "color": [0.0, 0.0, 0.0],
             "default_glyph": "\ue3c9", "default_display_name": ""},       # edit
    "Tool": {"glyph": "\uea3b", "display_name": "", "color": [0.0, 0.0, 0.0],
             "default_glyph": "\uea3b", "default_display_name": ""},       # construction
    "Asset": {"glyph": "\ue2c7", "display_name": "", "color": [0.0, 0.0, 0.0],
             "default_glyph": "\ue2c7", "default_display_name": ""},      # folder
    "Options": {"glyph": "\ue5d4", "display_name": "", "color": [0.0, 0.0, 0.0],
                "default_glyph": "\ue5d4", "default_display_name": ""},    # settings

    # Editor-specific
    "Animation": {"glyph": "\uf8f0", "display_name": "", "color": [0.0, 0.0, 0.0],
                  "default_glyph": "\uf8f0", "default_display_name": ""},  # motion_photos_on
    "Physics": {"glyph": "\ue3d4", "display_name": "", "color": [0.0, 0.0, 0.0],
                "default_glyph": "\ue3d4", "default_display_name": ""},    # science
    "World": {"glyph": "\ue88e", "display_name": "", "color": [0.0, 0.0, 0.0],
              "default_glyph": "\ue88e", "default_display_name": ""},      # public
    "Material": {"glyph": "\ue429", "display_name": "", "color": [0.0, 0.0, 0.0],
                 "default_glyph": "\ue429", "default_display_name": ""},   # palette
    "Modifiers": {"glyph": "\ue429", "display_name": "", "color": [0.0, 0.0, 0.0],
                  "default_glyph": "\ue429", "default_display_name": ""},  # palette
    "Texture": {"glyph": "\ue40a", "display_name": "", "color": [0.0, 0.0, 0.0],
                "default_glyph": "\ue40a", "default_display_name": ""},    # texture
    "Particles": {"glyph": "\ue3d4", "display_name": "", "color": [0.0, 0.0, 0.0],
                  "default_glyph": "\ue3d4", "default_display_name": ""},  # science
    "Curve": {"glyph": "\ue148", "display_name": "", "color": [0.0, 0.0, 0.0],
              "default_glyph": "\ue148", "default_display_name": ""},      # timeline
    "Image": {"glyph": "\ue204", "display_name": "", "color": [0.0, 0.0, 0.0],
             "default_glyph": "\ue204", "default_display_name": ""},       # image
    "Mesh": {"glyph": "\ue204", "display_name": "", "color": [0.0, 0.0, 0.0],
             "default_glyph": "\ue204", "default_display_name": ""},       # category
    "Object": {"glyph": "\ue8d4", "display_name": "", "color": [0.0, 0.0, 0.0],
               "default_glyph": "\ue8d4", "default_display_name": ""},     # select_all
    "Scene": {"glyph": "\ue8f9", "display_name": "", "color": [0.0, 0.0, 0.0],
              "default_glyph": "\ue8f9", "default_display_name": ""},      # dashboard
    "Render": {"glyph": "\ue439", "display_name": "", "color": [0.0, 0.0, 0.0],
               "default_glyph": "\ue439", "default_display_name": ""},     # photo_camera
    "Script": {"glyph": "\ue86f", "display_name": "", "color": [0.0, 0.0, 0.0],
               "default_glyph": "\ue86f", "default_display_name": ""},     # terminal
    "Sound": {"glyph": "\ue3a1", "display_name": "", "color": [0.0, 0.0, 0.0],
              "default_glyph": "\ue3a1", "default_display_name": ""},      # speaker
    "Surface": {"glyph": "\ue76c", "display_name": "", "color": [0.0, 0.0, 0.0],
                "default_glyph": "\ue76c", "default_display_name": ""},    # waves
    "Volume": {"glyph": "\ue2c8", "display_name": "", "color": [0.0, 0.0, 0.0],
               "default_glyph": "\ue2c8", "default_display_name": ""},     # folder_open
    "Constraints": {"glyph": "\ue8d2", "display_name": "", "color": [0.0, 0.0, 0.0],
                    "default_glyph": "\ue8d2", "default_display_name": ""}, # rule
    "Data": {"glyph": "\ue23e", "display_name": "", "color": [0.0, 0.0, 0.0],
             "default_glyph": "\ue23e", "default_display_name": ""},       # database
    "Node": {"glyph": "\ue1b8", "display_name": "", "color": [0.0, 0.0, 0.0],
             "default_glyph": "\ue1b8", "default_display_name": ""},       # account_tree
}

# In-memory cache of glyph mappings
_glyph_cache = {}
_glyph_cache_loaded = False

# Last discovery source per category name (used for canonicalization priority).
_last_discovered_category_sources = {}

# In-memory cache of all tags (tag_name -> {glyph, color})
_all_tags_cache = {}

# Guard flag to prevent recursive sync calls
_sync_in_progress = False

# Flag to indicate initial load is complete (callbacks should not save during load)
_initial_load_complete = False

# Tag order for preserving manual ordering
_tag_order_cache = []

# Category order cache: tag_combination -> [category_id1, category_id2, ...]
_category_orders_cache = {}

# Current JSON format version
CURRENT_JSON_VERSION = 7  # Bumped for category icon persistence support

# JSON file name in config directory
GLYPHS_FILENAME = "category_glyphs.json"

# Priority order for reserved categories per space type.
# Categories not in this list are sorted alphabetically at the end of the reserved block.
# IMPORTANT: Names must correspond to reserved categories in DEFAULT_CATEGORY_GLYPHS.
RESERVED_CATEGORY_PRIORITY = {
    'VIEW_3D': [
        "Item", "Tool", "View", "Animation", "Edit", "Asset", "Options",
        "Modifiers", "Physics", "Material", "World", "Scene",
        "Render" 
    ],
    'PROPERTIES': [
        "Item", "Tool", "View", "Physics", "Material", "World", "Scene",
        "Render", "Options", "Texture", "Output"
    ],
    'NODE_EDITOR': [
        "Item", "Tool", "Options", "Node", "Group"
    ],
    'IMAGE_EDITOR': [
        "Item", "Tool", "View", "Image", "Mask", "Scopes"
    ],
    'SEQUENCE_EDITOR': [
        "Item", "Tool", "View", "Strip"
    ],
    'CLIP_EDITOR': [
        "Item", "Tool", "View", "Mask", "Tracking"
    ],
    'TEXT_EDITOR': [
        "Tool", "View", "Options", "Text"
    ],
    'DOPESHEET_EDITOR': [
        "Item", "Tool", "View", "Animation"
    ],
    'GRAPH_EDITOR': [
        "Item", "Tool", "View", "Animation"
    ],
    'NLA_EDITOR': [
        "Item", "Tool", "View", "Animation"
    ],
    # Default fallback for unknown space types
    'DEFAULT': [
        "Item", "Tool", "View", "Edit", "Asset", "Options"
    ]
}


def get_reserved_category_priority(category_id: str, space_type: str) -> int:
    """
    Get priority index for a reserved category in a specific space type.
    Lower index = higher priority (appears earlier).
    Returns -1 if category is not in the priority list (sort alphabetically).
    """
    priority_list = RESERVED_CATEGORY_PRIORITY.get(
        space_type,
        RESERVED_CATEGORY_PRIORITY.get('DEFAULT', [])
    )
    try:
        return priority_list.index(category_id)
    except ValueError:
        return -1  # Unknown category - sort alphabetically at end


def _get_glyphs_filepath():
    """Get the path to the glyph mappings JSON file."""
    import bpy.utils
    config_dir = bpy.utils.user_resource('CONFIG')
    if config_dir:
        return os.path.join(config_dir, GLYPHS_FILENAME)
    return None


def _normalize_category_key(value: str) -> str:
    """Normalize category/package name for robust matching (e.g. OpenVAT <-> openvat)."""
    return re.sub(r"[^a-z0-9]+", "", str(value).lower())


_CATEGORY_DISCOVERY_SOURCE_PRIORITY = {
    "panel_discovered": 0,
    "manifest_name": 1,
    "manifest_id": 2,
    "package_dir": 3,
    "unknown": 99,
}


def _get_discovery_source_priority(source: str) -> int:
    return _CATEGORY_DISCOVERY_SOURCE_PRIORITY.get(source, _CATEGORY_DISCOVERY_SOURCE_PRIORITY["unknown"])


def _pick_canonical_category_name(candidates, source_map):
    """Pick one canonical category name among aliases sharing the same normalized key."""
    if not candidates:
        return ""

    def _style_rank(name: str) -> int:
        # Prefer human-readable/mixed-case variants over lowercase technical ids.
        if any(ch.isupper() for ch in name):
            return 0
        if any(ch.isspace() for ch in name):
            return 1
        return 2

    return sorted(
        candidates,
        key=lambda name: (
            _get_discovery_source_priority(source_map.get(name, "unknown")),
            _style_rank(name),
            len(name),
            name.lower(),
            name,
        ),
    )[0]


def _is_single_edit_apart(a: str, b: str) -> bool:
    """Return True when strings differ by exactly one insertion/deletion/substitution."""
    if a == b:
        return False
    len_a = len(a)
    len_b = len(b)
    if abs(len_a - len_b) > 1:
        return False

    # Substitution case.
    if len_a == len_b:
        diff = 0
        for ca, cb in zip(a, b):
            if ca != cb:
                diff += 1
                if diff > 1:
                    return False
        return diff == 1

    # Insertion/deletion case.
    if len_a > len_b:
        a, b = b, a
        len_a, len_b = len_b, len_a

    i = j = 0
    mismatches = 0
    while i < len_a and j < len_b:
        if a[i] == b[j]:
            i += 1
            j += 1
            continue
        mismatches += 1
        if mismatches > 1:
            return False
        j += 1

    return True


def _manifest_field_match_keys(field_value: str):
    """Build robust normalized match keys from a manifest text field."""
    keys = set()
    if not isinstance(field_value, str):
        return keys

    value = field_value.strip()
    if not value:
        return keys

    normalized_full = _normalize_category_key(value)
    if normalized_full:
        keys.add(normalized_full)

    # Support values like "Huge Menace <hello@hugemenace.co>".
    email = ""
    name_and_email = re.match(r"^\s*([^<]+?)\s*<([^>]+)>\s*$", value)
    if name_and_email:
        display_name = name_and_email.group(1).strip()
        email = name_and_email.group(2).strip()
        display_key = _normalize_category_key(display_name)
        if display_key:
            keys.add(display_key)
    else:
        email_match = re.search(r"([A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,})", value)
        if email_match:
            email = email_match.group(1).strip()

    if email and "@" in email:
        _local, _sep, domain = email.partition("@")
        domain_labels = [label for label in re.split(r"[^A-Za-z0-9]+", domain) if label]
        if domain_labels:
            # Prefer second-level domain key: "hugemenace.co" -> "hugemenace".
            if len(domain_labels) >= 2:
                sld_key = _normalize_category_key(domain_labels[-2])
                if sld_key:
                    keys.add(sld_key)
            # Also keep a compact domain form without TLD for subdomain cases.
            compact_domain = "".join(domain_labels[:-1]) if len(domain_labels) > 1 else domain_labels[0]
            compact_domain_key = _normalize_category_key(compact_domain)
            if compact_domain_key:
                keys.add(compact_domain_key)

    return keys


def _extension_manifest_match_keys(pkg_path: str, pkg_name: str):
    """Collect normalized keys from extension folder + manifest for category matching."""
    keys = set()
    if pkg_name:
        keys.add(_normalize_category_key(pkg_name))

    manifest_path = os.path.join(pkg_path, "blender_manifest.toml")
    if not os.path.isfile(manifest_path):
        return keys

    try:
        import tomllib

        with open(manifest_path, "rb") as fh:
            manifest = tomllib.load(fh)

        for field_name in ("id", "name", "tagline", "maintainer", "publisher"):
            field_value = manifest.get(field_name)
            if isinstance(field_value, str) and field_value.strip():
                keys.update(_manifest_field_match_keys(field_value))
    except Exception as e:
        print(f"[GLYPH ICON AUTO DEBUG] manifest parse failed: pkg_path={pkg_path!r}, error={e}")

    return keys


def _auto_detect_extension_icon_path(category: str):
    """Try to find extension icon file for a category in user EXTENSIONS folders.

    Returns: (icon_path, provider) or ("", "") when not found.
    """
    print(f"[GLYPH ICON AUTO DEBUG] detect start: category={category!r}")
    try:
        extensions_dir = bpy.utils.user_resource('EXTENSIONS')
    except Exception:
        print(f"[GLYPH ICON AUTO DEBUG] detect abort: category={category!r}, reason=user_resource_exception")
        return "", ""

    if not extensions_dir or not os.path.isdir(extensions_dir):
        print(f"[GLYPH ICON AUTO DEBUG] detect abort: category={category!r}, extensions_dir={extensions_dir!r}, exists=False")
        return "", ""

    target_key = _normalize_category_key(category)
    if not target_key:
        print(f"[GLYPH ICON AUTO DEBUG] detect abort: category={category!r}, normalized_key is empty")
        return "", ""

    print(f"[GLYPH ICON AUTO DEBUG] detect context: category={category!r}, target_key={target_key!r}, extensions_dir={extensions_dir!r}")

    icon_filenames = ("icon.png", "icon.webp", "icon.jpg", "icon.jpeg")

    try:
        for repo_name in os.listdir(extensions_dir):
            repo_path = os.path.join(extensions_dir, repo_name)
            if (not os.path.isdir(repo_path)) or repo_name.startswith('.'):
                continue

            for pkg_name in os.listdir(repo_path):
                pkg_path = os.path.join(repo_path, pkg_name)
                if not os.path.isdir(pkg_path):
                    continue

                match_keys = _extension_manifest_match_keys(pkg_path, pkg_name)
                exact_match = target_key in match_keys
                fuzzy_match_key = ""
                if not exact_match:
                    # Strict fuzzy fallback for near-typo category names.
                    # Example: "HugeMenance" -> "hugemenace".
                    if len(target_key) >= 8:
                        for key in match_keys:
                            if not key or abs(len(key) - len(target_key)) > 1:
                                continue
                            if key[:5] != target_key[:5]:
                                continue
                            if _is_single_edit_apart(target_key, key):
                                fuzzy_match_key = key
                                break
                    if not fuzzy_match_key:
                        continue

                print(
                    f"[GLYPH ICON AUTO DEBUG] package match: category={category!r}, repo={repo_name!r}, "
                    f"pkg={pkg_name!r}, pkg_path={pkg_path!r}, keys={sorted(match_keys)!r}, "
                    f"match_type={'exact' if exact_match else 'fuzzy'}, fuzzy_key={fuzzy_match_key!r}"
                )

                for icon_name in icon_filenames:
                    icon_path = os.path.join(pkg_path, icon_name)
                    if os.path.isfile(icon_path):
                        print(f"[GLYPH ICON AUTO DEBUG] detect hit: category={category!r}, icon={icon_path!r}, provider='extension_auto'")
                        return icon_path, "extension_auto"
    except Exception:
        print(f"[GLYPH ICON AUTO DEBUG] detect abort: category={category!r}, reason=scan_exception")
        return "", ""

    print(f"[GLYPH ICON AUTO DEBUG] detect miss: category={category!r}")
    return "", ""


def _glyph_to_unicode_escape(glyph):
    """Convert a glyph character to \\uXXXX format for reliable JSON storage."""
    if not glyph:
        return ""
    # Convert to Unicode escape sequence
    result = ""
    for char in glyph:
        code_point = ord(char)
        if code_point > 127:  # Non-ASCII characters
            result += f"\\u{code_point:04x}"
        else:
            result += char
    return result


def _unicode_escape_to_glyph(escape_str):
    """Convert \\uXXXX format back to glyph character."""
    if not escape_str:
        return ""

    # Pattern to match \uXXXX
    pattern = r'\\u([0-9a-fA-F]{4})'

    def replace_escape(match):
        code_point = int(match.group(1), 16)
        return chr(code_point)

    return re.sub(pattern, replace_escape, escape_str)


def _category_order_encode(category_list):
    """Encode category order entries for JSON storage (glyphs as \\uXXXX)."""
    if not isinstance(category_list, list):
        return category_list
    encoded = []
    for category in category_list:
        if isinstance(category, str):
            encoded.append(_glyph_to_unicode_escape(category))
        else:
            encoded.append(category)
    return encoded


def _category_order_decode(category_list):
    """Decode category order entries from JSON storage (\\uXXXX -> glyphs)."""
    if not isinstance(category_list, list):
        return category_list
    decoded = []
    for category in category_list:
        if isinstance(category, str) and "\\u" in category:
            decoded.append(_unicode_escape_to_glyph(category))
        else:
            decoded.append(category)
    return decoded


def _hex_to_glyph(hex_str):
    """Convert hex string (e.g., 'f3c1' or '0xf3c1') to Unicode glyph character.

    Args:
        hex_str: Hex string with optional '0x' prefix

    Returns:
        Unicode character, or empty string if input is empty,
        or original string if conversion fails
    """
    if not hex_str:
        return ""
    # Strip optional 0x prefix
    if hex_str.lower().startswith('0x'):
        hex_str = hex_str[2:]
    try:
        code_point = int(hex_str, 16)
        return chr(code_point)
    except (ValueError, OverflowError):
        tag_log(f"Invalid hex string for glyph: '{hex_str}'", "WARN")
        return hex_str  # Return as-is if not valid hex


def _glyph_to_hex(glyph):
    """Convert Unicode glyph character to hex string (e.g., 'f3c1').

    Note: Only single-character glyphs are fully supported.
    For multi-character strings, only the first character is converted.
    """
    if not glyph:
        return ""
    if len(glyph) == 1:
        return format(ord(glyph), 'x')
    # For multi-character strings, return first char's hex
    tag_log(f"Multi-character glyph '{repr(glyph)}', using first char only", "WARN")
    return format(ord(glyph[0]), 'x')


def _is_valid_category_name(name):
    """Check if a category name is valid (not a glyph or empty)."""
    if not name:
        return False
    # Category name should not be a single Unicode character (glyph)
    # and should contain at least one ASCII letter or digit
    if len(name) <= 2:
        # Single or double character - check if it's a glyph (high Unicode)
        for char in name:
            if ord(char) > 0xE000:  # Private Use Area - likely a glyph
                return False
    return any(c.isalnum() for c in name)


def _is_single_glyph(name):
    """Check if a string is a single glyph character (high Unicode)."""
    if not name:
        return False
    # Check if it's a single character in the Private Use Area (glyph)
    if len(name) == 1 and ord(name) > 0xE000:
        return True
    # Also check for UTF-8 encoded glyph (might be 1-4 bytes)
    if len(name) <= 4:
        for char in name:
            if ord(char) > 0xE000:
                return True
    return False


def _mode_names_to_flags(mode_names):
    """Convert list of mode names to bitmask."""
    flags = 0
    for name in mode_names:
        flags |= _CATEGORY_TAG_MODE_NAME_TO_FLAG.get(name, 0)
    return flags


def _flags_to_mode_names(flags):
    """Convert bitmask to list of mode names."""
    names = []
    for bit, name in _CATEGORY_TAG_MODE_FLAG_TO_NAME.items():
        if flags & bit:
            names.append(name)
    return names


def _normalize_category_data(category_data):
    """Normalize category data to the new format with glyph, display_name, color, defaults, and tags."""
    default_entry = {
        "glyph": "", "display_name": "", "color": [0.0, 0.0, 0.0],
        "default_glyph": "", "default_display_name": "", "base_type": "text_only",
        "tags": [],  # NEW: array of tag names
        "mode_flags": [],  # NEW: array of mode names
        "glyph_mode": "auto",
        "icon_source": "auto",
        "icon_key": "",
        "icon_path": "",
        "icon_provider": "",
    }

    if isinstance(category_data, str):
        # Old format: just a glyph string (may be in \uXXXX format)
        glyph = _unicode_escape_to_glyph(category_data) if '\\u' in category_data else category_data
        base_type = "glyph_only" if _is_single_glyph(glyph) else "glyph_text"
        return {"glyph": glyph, "display_name": "", "color": [0.0, 0.0, 0.0],
                "default_glyph": glyph, "default_display_name": "", "base_type": base_type,
                "tags": [],
                "glyph_mode": "auto",
                "icon_source": "auto", "icon_key": "", "icon_path": "", "icon_provider": ""}
    elif isinstance(category_data, dict):
        # New format: dict with glyph, display_name, color, default_glyph, default_display_name, base_type, tags
        entry = default_entry.copy()

        # Current values
        if "glyph" in category_data:
            glyph_str = category_data["glyph"]
            if glyph_str and '\\u' in glyph_str:
                entry["glyph"] = _unicode_escape_to_glyph(glyph_str)
            else:
                entry["glyph"] = glyph_str
        if "display_name" in category_data:
            entry["display_name"] = category_data["display_name"]
        if "color" in category_data:
            color = category_data["color"]
            if isinstance(color, (list, tuple)) and len(color) >= 3:
                entry["color"] = list(color[:3])

        # Default values (for reset)
        # IMPORTANT: Distinguish between "field is missing" vs "field is present but intentionally empty".
        # Empty default_glyph is meaningful for text_only categories (fallback letter behavior).
        if "default_glyph" in category_data:
            glyph_str = category_data["default_glyph"]
            if glyph_str and '\\u' in glyph_str:
                entry["default_glyph"] = _unicode_escape_to_glyph(glyph_str)
            else:
                entry["default_glyph"] = glyph_str or ""
        else:
            # Backward compatibility for legacy data where default_glyph field does not exist.
            entry["default_glyph"] = entry["glyph"]

        if "default_display_name" in category_data:
            entry["default_display_name"] = category_data["default_display_name"]
        else:
            # If no default_display_name, use empty string
            entry["default_display_name"] = ""

        # Base type (for reset): glyph_only, glyph_text, or text_only
        if "base_type" in category_data:
            entry["base_type"] = category_data["base_type"]
        else:
            # Determine base_type from current values
            if entry["glyph"]:
                entry["base_type"] = "glyph_text"
            else:
                entry["base_type"] = "text_only"

        # Safety correction for previously serialized incorrect state:
        # text_only categories must reset to fallback letter, so default_glyph must be empty.
        if entry["base_type"] == "text_only":
            entry["default_glyph"] = ""

        # NEW: Tags
        if "tags" in category_data:
            tags = category_data["tags"]
            if isinstance(tags, list):
                entry["tags"] = [str(t) for t in tags]

        # Icon persistence (v7): accept both nested block and legacy flat keys.
        icon_block = category_data.get("icon", {}) if isinstance(category_data.get("icon", {}), dict) else {}

        glyph_mode = category_data.get("glyph_mode", "auto")
        if not isinstance(glyph_mode, str):
            glyph_mode = "auto"
        glyph_mode = glyph_mode.lower()
        if glyph_mode not in {"auto", "first_letter"}:
            glyph_mode = "auto"
        entry["glyph_mode"] = glyph_mode

        icon_source = icon_block.get("source", category_data.get("icon_source", "auto"))
        if not isinstance(icon_source, str):
            icon_source = "auto"
        icon_source = icon_source.lower()
        if icon_source not in {"auto", "manual", "off"}:
            icon_source = "auto"
        entry["icon_source"] = icon_source

        icon_key = icon_block.get("key", category_data.get("icon_key", ""))
        entry["icon_key"] = str(icon_key) if icon_key is not None else ""

        icon_path = icon_block.get("path", category_data.get("icon_path", ""))
        entry["icon_path"] = str(icon_path) if icon_path is not None else ""

        icon_provider = icon_block.get("provider", category_data.get("icon_provider", ""))
        entry["icon_provider"] = str(icon_provider) if icon_provider is not None else ""

        return entry
    else:
        return default_entry


# -----------------------------------------------------------------------------
# JSON Migration Functions

def migrate_v1_to_v2(data):
    """Migrate v1 (string values) to v2 (dict with glyph/color)."""
    tag_log("Migrating JSON v1 → v2")
    mappings = {}
    for cat_name, glyph_str in data.get("mappings", {}).items():
        if isinstance(glyph_str, str):
            mappings[cat_name] = _normalize_category_data(glyph_str)
    return {"version": 2, "mappings": mappings}


def migrate_v2_to_v3(data):
    """Migrate v2 to v3 (add all_tags and tags arrays)."""
    tag_log("Migrating JSON v2 → v3")
    data["all_tags"] = {}  # Empty tag registry
    for cat_data in data.get("mappings", {}).values():
        if isinstance(cat_data, dict):
            cat_data["tags"] = []
    data["version"] = 3
    return data


def migrate_v3_to_v4(data):
    """Migrate v3 to v4 (add tag_order for manual ordering)."""
    tag_log("Migrating JSON v3 → v4")
    # tag_order will be populated on next save from WM collection order
    data["tag_order"] = []
    data["version"] = 4
    return data


def migrate_v4_to_v5(data):
    """Migrate from v4 to v5: add mode_flags to tags."""
    tag_log("Migrating JSON v4 → v5")
    # Add empty mode_flags to all tags (empty = all modes active)
    if "tags" in data:
        for tag in data["tags"]:
            if isinstance(tag, dict) and "mode_flags" not in tag:
                tag["mode_flags"] = []  # Empty = all modes
    data["version"] = 5
    return data


def migrate_v5_to_v6(data):
    """Migrate version 5 to 6: Add category_orders support."""
    # Add empty category_orders dict if not present
    if "category_orders" not in data:
        data["category_orders"] = {}
        print("[MIGRATION] v5->v6: Added category_orders section")
    data["version"] = 6
    return data


def migrate_v6_to_v7(data):
    """Migrate version 6 to 7: Add icon persistence block for each category mapping."""
    mappings = data.get("mappings", {})
    if isinstance(mappings, dict):
        for _category, category_data in mappings.items():
            if not isinstance(category_data, dict):
                continue

            icon = category_data.get("icon")
            if not isinstance(icon, dict):
                icon = {}

            icon_source = icon.get("source", category_data.get("icon_source", "auto"))
            if not isinstance(icon_source, str):
                icon_source = "auto"
            icon_source = icon_source.lower()
            if icon_source not in {"auto", "manual", "off"}:
                icon_source = "auto"

            icon["source"] = icon_source
            icon["key"] = str(icon.get("key", category_data.get("icon_key", "")) or "")
            icon["path"] = str(icon.get("path", category_data.get("icon_path", "")) or "")
            icon["provider"] = str(icon.get("provider", category_data.get("icon_provider", "")) or "")
            category_data["icon"] = icon

    data["version"] = 7
    return data


MIGRATORS = {
    1: migrate_v1_to_v2,
    2: migrate_v2_to_v3,
    3: migrate_v3_to_v4,
    4: migrate_v4_to_v5,
    5: migrate_v5_to_v6,
    6: migrate_v6_to_v7,
}


def migrate_json_data(data):
    """Migrate data to current version with validation."""
    version = data.get("version", 1)

    while version < CURRENT_JSON_VERSION:
        migrator = MIGRATORS.get(version)
        if not migrator:
            raise ValueError(f"Unknown JSON version: {version}")
        data = migrator(data)
        version = data.get("version", version + 1)
        tag_log(f"Migrated to version {version}")

    # Validate final structure
    if "all_tags" not in data:
        data["all_tags"] = {}
    if "mappings" not in data:
        data["mappings"] = {}
    if "category_orders" not in data:
        data["category_orders"] = {}

    return data


def _load_glyph_mappings_from_file():
    """Load glyph mappings from JSON file with migration support."""
    global _glyph_cache, _glyph_cache_loaded, _all_tags_cache

    filepath = _get_glyphs_filepath()
    print(f"[GLYPH] JSON storage path: {filepath}")

    default_structure = {
        "version": CURRENT_JSON_VERSION,
        "all_tags": {},
        "mappings": {},
        "category_orders": {}
    }

    if not filepath:
        print(f"[GLYPH] No valid config path, using defaults")
        _glyph_cache = DEFAULT_CATEGORY_GLYPHS.copy()
        _all_tags_cache = {}
        _glyph_cache_loaded = True
        return False

    data = load_json_safely(filepath, default_structure)

    # Migrate if needed
    if data.get("version", 1) < CURRENT_JSON_VERSION:
        create_backup(filepath)  # Backup before migration
        data = migrate_json_data(data)
        _save_glyph_mappings_to_file(data)  # Save migrated data

    # Load into caches
    _glyph_cache = {}
    raw_mappings = data.get('mappings', {})
    raw_orders = data.get("category_orders", {})
    order_categories = set()
    for _tag_key, category_list in raw_orders.items():
        if isinstance(category_list, list):
            decoded_list = _category_order_decode(category_list)
            for category in decoded_list:
                if isinstance(category, str):
                    order_categories.add(category)

    def _has_user_customizations_raw(category_data):
        """Check if category has user customizations (display_name, color, or tags)."""
        if isinstance(category_data, dict):
            display_name = category_data.get("display_name", "")
            color = category_data.get("color", [0.0, 0.0, 0.0])
            tags = category_data.get("tags", [])
            glyph_mode = str(category_data.get("glyph_mode", "auto")).lower()
            icon_source = str(category_data.get("icon_source", "auto")).lower()
            icon_key = category_data.get("icon_key", "")
            icon_path = category_data.get("icon_path", "")
            icon_provider = category_data.get("icon_provider", "")
            icon_customized = (icon_source != "auto") or bool(icon_key) or bool(icon_path) or bool(icon_provider)
            glyph_mode_customized = glyph_mode != "auto"
            return bool(display_name) or color != [0.0, 0.0, 0.0] or bool(tags) or icon_customized or glyph_mode_customized
        return False

    skipped_count = 0
    for category, category_data in raw_mappings.items():
        # Skip invalid category names (glyphs as names) ONLY if they have no user customizations
        # AND they are not referenced by any category order list.
        if not _is_valid_category_name(category):
            normalized = _normalize_category_data(category_data)
            if (not _has_user_customizations_raw(normalized)) and (category not in order_categories):
                skipped_count += 1
                continue
            # If referenced by order list or has user customizations, load it even with invalid name
            if category in order_categories:
                print(f"[GLYPH] Loading invalid category from order list: {repr(category)}")
            else:
                print(f"[GLYPH] Loading category with user customizations: {repr(category)}")

        _glyph_cache[category] = _normalize_category_data(category_data)

    # Ensure invalid categories referenced in order lists are kept in cache
    for category in order_categories:
        if category not in _glyph_cache and not _is_valid_category_name(category):
            _glyph_cache[category] = _normalize_category_data({})
            print(f"[GLYPH] Adding missing invalid category from order list: {repr(category)}")

    # Load all_tags cache - convert hex glyphs to Unicode
    raw_tags = data.get("all_tags", {})
    _all_tags_cache = {}
    for tag_name, tag_data in raw_tags.items():
        if isinstance(tag_data, dict):
            _all_tags_cache[tag_name] = {
                "glyph": _hex_to_glyph(tag_data.get("glyph", "")),
                "color": tag_data.get("color", [0.0, 0.0, 0.0]),
                "mode_flags": tag_data.get("mode_flags", _CATEGORY_TAG_DEFAULT_MODE_FLAGS)
            }
        else:
            _all_tags_cache[tag_name] = tag_data

    # Load tag order for preserving manual ordering
    global _tag_order_cache
    _tag_order_cache = data.get("tag_order", [])

    # Load category orders
    global _category_orders_cache
    _category_orders_cache = {}
    for tag_key, category_list in raw_orders.items():
        if isinstance(category_list, list):
            _category_orders_cache[tag_key] = _category_order_decode(category_list)

    _glyph_cache_loaded = True
    if skipped_count > 0:
        print(f"[GLYPH] Skipped {skipped_count} categories with invalid names and no customizations")
    print(f"[GLYPH] Loaded {len(_glyph_cache)} mappings, {len(_all_tags_cache)} tags from {filepath}")
    return True


def _save_glyph_mappings_to_file(data=None):
    """Save glyph mappings to JSON file with glyphs in \\uXXXX format."""
    global _glyph_cache, _all_tags_cache

    filepath = _get_glyphs_filepath()
    if not filepath:
        tag_log("No filepath for saving", "ERROR")
        return False

    # Create backup before overwriting
    create_backup(filepath)

    if data is None:
        # Ensure directory exists
        config_dir = os.path.dirname(filepath)
        if not os.path.exists(config_dir):
            print(f"[GLYPH] Creating config directory: {config_dir}")
            os.makedirs(config_dir, exist_ok=True)

        def _has_user_customizations(category_data):
            """Check if category has user customizations (display_name, color, or tags)."""
            if isinstance(category_data, dict):
                display_name = category_data.get("display_name", "")
                color = category_data.get("color", [0.0, 0.0, 0.0])
                tags = category_data.get("tags", [])
                glyph_mode = str(category_data.get("glyph_mode", "auto")).lower()
                icon_source = str(category_data.get("icon_source", "auto")).lower()
                icon_key = category_data.get("icon_key", "")
                icon_path = category_data.get("icon_path", "")
                icon_provider = category_data.get("icon_provider", "")
                icon_customized = (icon_source != "auto") or bool(icon_key) or bool(icon_path) or bool(icon_provider)
                glyph_mode_customized = glyph_mode != "auto"
                # Check if display_name is not empty, color is not default black, has tags, or icon customized.
                return bool(display_name) or color != [0.0, 0.0, 0.0] or bool(tags) or icon_customized or glyph_mode_customized
            return False

        # Convert glyphs to Unicode escape format for reliable storage
        mappings_to_save = {}
        skipped_count = 0
        order_categories = set()
        for _tag_key, category_list in _category_orders_cache.items():
            if isinstance(category_list, list):
                for category in category_list:
                    if isinstance(category, str):
                        order_categories.add(category)
        for category, category_data in _glyph_cache.items():
            # Skip invalid category names (glyphs as names) ONLY if they have no user customizations
            if not _is_valid_category_name(category):
                if (not _has_user_customizations(category_data)) and (category not in order_categories):
                    skipped_count += 1
                    continue
                # If referenced by order list or has user customizations, save it even with invalid name
                if category in order_categories:
                    print(f"[GLYPH] Saving invalid category from order list: {repr(category)}")
                else:
                    print(f"[GLYPH] Saving category with user customizations: {repr(category)}")

            if isinstance(category_data, dict):
                # Debug: print tags for all categories with tags
                cat_tags = category_data.get("tags", [])
                if cat_tags:
                    print(f"[GLYPH SAVE] Category '{category}' has tags: {cat_tags}")

                mappings_to_save[category] = {
                    "glyph": _glyph_to_unicode_escape(category_data.get("glyph", "")),
                    "display_name": category_data.get("display_name", ""),
                    "color": category_data.get("color", [0.0, 0.0, 0.0]),
                    "default_glyph": _glyph_to_unicode_escape(category_data.get("default_glyph", "")),
                    "default_display_name": category_data.get("default_display_name", ""),
                    "base_type": category_data.get("base_type", "text_only"),
                    "tags": category_data.get("tags", []),  # NEW: Save tags
                    "glyph_mode": category_data.get("glyph_mode", "auto"),
                    "icon": {
                        "source": category_data.get("icon_source", "auto"),
                        "key": category_data.get("icon_key", ""),
                        "path": category_data.get("icon_path", ""),
                        "provider": category_data.get("icon_provider", ""),
                    },
                }
            elif isinstance(category_data, str):
                # Old format - convert to new format
                glyph = _unicode_escape_to_glyph(category_data) if '\\u' in category_data else category_data
                base_type = "glyph_only" if _is_single_glyph(glyph) else "glyph_text"
                mappings_to_save[category] = {
                    "glyph": _glyph_to_unicode_escape(glyph),
                    "display_name": "",
                    "color": [0.0, 0.0, 0.0],
                    "default_glyph": _glyph_to_unicode_escape(glyph),
                    "default_display_name": "",
                    "base_type": base_type,
                    "tags": [],
                    "glyph_mode": "auto",
                    "icon": {
                        "source": "auto",
                        "key": "",
                        "path": "",
                        "provider": "",
                    },
                }

        if skipped_count > 0:
            print(f"[GLYPH] Skipped {skipped_count} categories with invalid names and no customizations")

        # Convert tag glyphs to hex for storage
        tags_to_save = {}
        for tag_name, tag_data in _all_tags_cache.items():
            if isinstance(tag_data, dict):
                tags_to_save[tag_name] = {
                    "glyph": _glyph_to_hex(tag_data.get("glyph", "")),
                    "color": tag_data.get("color", [0.0, 0.0, 0.0]),
                    "mode_flags": tag_data.get("mode_flags", _CATEGORY_TAG_DEFAULT_MODE_FLAGS)
                }
            else:
                tags_to_save[tag_name] = tag_data

        # Get tag order from wm.category_tags (preserves manual ordering)
        tag_order = []
        try:
            wm = bpy.context.window_manager
            if hasattr(wm, 'category_tags') and _is_collection_safe(wm.category_tags):
                tag_order = [tag.name for tag in wm.category_tags]
        except Exception:
            pass

        data = {
            'version': CURRENT_JSON_VERSION,
            'all_tags': tags_to_save,
            'tag_order': tag_order,  # Save tag order
            'mappings': mappings_to_save,
            'category_orders': {
                tag_key: _category_order_encode(category_list)
                for tag_key, category_list in _category_orders_cache.items()
            }  # Save category orders (glyphs as \uXXXX)
        }

    try:
        with safe_file_write(filepath) as f:
            json.dump(data, f, indent=2, ensure_ascii=False)
        tag_log(f"Saved {len(_glyph_cache)} categories, {len(_all_tags_cache)} tags")
        print(f"[GLYPH SAVE] Successfully saved to {filepath}")
        return True
    except Exception as e:
        tag_log(f"Save failed: {e}", "ERROR")
        print(f"[GLYPH SAVE] Error: {e}")
        import traceback
        traceback.print_exc()
        return False


# -----------------------------------------------------------------------------
# Category Order Management Functions

def get_category_order(tag_combination):
    """
    Get category order for a specific tag combination.
    Args:
        tag_combination: String like "" (no filter), "Modeling", or "Animation;Modeling"
    Returns:
        List of category IDs in order, or empty list if not found
    """
    global _category_orders_cache, _glyph_cache_loaded
    if not _glyph_cache_loaded:
        _load_glyph_mappings_from_file()
    return _category_orders_cache.get(tag_combination, []).copy()


def set_category_order(tag_combination, category_list):
    """
    Set category order for a specific tag combination.
    Args:
        tag_combination: String like "" (no filter), "Modeling", or "Animation;Modeling"
        category_list: List of category IDs in order
    """
    global _category_orders_cache
    try:
        print(f"[CAT ORDER][PY] set_category_order: tag='{tag_combination}' count={len(category_list)}")
        # Print a compact preview to avoid huge logs
        preview = ", ".join([repr(x) for x in category_list[:12]])
        if len(category_list) > 12:
            preview += f", ... (+{len(category_list)-12})"
        print(f"[CAT ORDER][PY] order: [{preview}]")
    except Exception as e:
        print(f"[CAT ORDER][PY] set_category_order log failed: {e}")
    _category_orders_cache[tag_combination] = category_list.copy()
    # Trigger save
    _save_glyph_mappings_to_file()


def clear_category_order(tag_combination):
    """Clear category order for a specific tag combination."""
    global _category_orders_cache
    if tag_combination in _category_orders_cache:
        del _category_orders_cache[tag_combination]
        _save_glyph_mappings_to_file()


def get_all_category_orders():
    """Get all category orders (for debugging/export)."""
    global _category_orders_cache, _glyph_cache_loaded
    if not _glyph_cache_loaded:
        _load_glyph_mappings_from_file()
    return _category_orders_cache.copy()


# -----------------------------------------------------------------------------
# Tag Management Functions

def get_all_tags():
    """Get all available tags as dict."""
    global _all_tags_cache, _glyph_cache_loaded
    if not _glyph_cache_loaded:
        _load_glyph_mappings_from_file()
    return _all_tags_cache.copy()


def get_tag_names():
    """Get list of tag names only."""
    return list(get_all_tags().keys())


def get_tags_for_category_ui(category):
    """
    Get all tags formatted for C++ UI display.
    Returns string: "name|glyph|is_active|r,g,b;name2|glyph2|is_active2|r,g,b;..."
    - name: tag name
    - glyph: unicode glyph character
    - is_active: 1 if assigned to category, 0 otherwise
    - r,g,b: RGB color values (0.0-1.0)
    """
    all_tags = get_all_tags()
    category_tags = set(get_category_tags(category))

    tag_log(f"get_tags_for_category_ui('{category}'): {len(all_tags)} tags found")

    parts = []
    for name, data in all_tags.items():
        glyph = data.get("glyph", "")
        is_active = "1" if name in category_tags else "0"
        color = data.get("color", [0.0, 0.0, 0.0])
        # Format color as r,g,b with 3 decimal places
        color_str = f"{color[0]:.3f},{color[1]:.3f},{color[2]:.3f}"
        # Use | as separator between fields, ; between tags
        parts.append(f"{name}|{glyph}|{is_active}|{color_str}")

    result = ";".join(parts)
    tag_log(f"get_tags_for_category_ui result: '{result}'")
    return result


def get_tag_data(tag_name):
    """Get glyph and color for a specific tag."""
    tags = get_all_tags()
    return tags.get(tag_name, {"glyph": "", "color": [0.0, 0.0, 0.0]})


# Default glyph hex for tags when none is specified (FontAwesome tag icon)
DEFAULT_TAG_GLYPH_HEX = "e866"


def generate_unique_tag_name():
    """Generate a unique tag name (Tag, Tag.001, Tag.002, etc.)."""
    tags = get_all_tags()
    base_name = "New Tag"
    if base_name not in tags:
        return base_name

    # Check for existing numbered names
    i = 1
    while True:
        name = f"{base_name}.{i:03d}"
        if name not in tags:
            return name
        i += 1


def create_tag(tag_name, glyph="", color=None, mode_flags=None, auto_save=True):
    """
    Create a new tag.

    Args:
        tag_name: Name for the new tag
        glyph: Unicode glyph character
        color: RGB color tuple (0.0-1.0)
        mode_flags: Bitmask of modes where tag is active (None = use default)

    Returns:
        (success: bool, message: str)
    """
    global _all_tags_cache, _tag_order_cache

    if not tag_name:
        return False, "Tag name cannot be empty"

    if len(tag_name) > 32:
        return False, "Tag name too long (max 32 chars)"

    if tag_name in _all_tags_cache:
        return False, f"Tag '{tag_name}' already exists"

    # Use default glyph if none provided - convert hex to Unicode
    if not glyph:
        glyph = _hex_to_glyph(DEFAULT_TAG_GLYPH_HEX)

    _all_tags_cache[tag_name] = {
        "glyph": glyph,
        "color": list(color) if color else [0.0, 0.0, 0.0],
        "mode_flags": mode_flags if mode_flags is not None else _CATEGORY_TAG_DEFAULT_MODE_FLAGS
    }

    # Always add new tags to the end of the order list
    if tag_name not in _tag_order_cache:
        _tag_order_cache.append(tag_name)

    tag_log(f"Created tag: {tag_name}")

    if auto_save:
        _auto_save_tags()

    return True, f"Tag '{tag_name}' created"


def update_tag(tag_name, glyph=None, color=None, auto_save=True):
    """Update an existing tag's glyph and/or color."""
    global _all_tags_cache

    if tag_name not in _all_tags_cache:
        return False, f"Tag '{tag_name}' not found"

    if glyph is not None:
        _all_tags_cache[tag_name]["glyph"] = glyph
    if color is not None:
        _all_tags_cache[tag_name]["color"] = list(color)

    tag_log(f"Updated tag: {tag_name}")

    if auto_save:
        _auto_save_tags()

    return True, f"Tag '{tag_name}' updated"


def delete_tag(tag_name, auto_save=True):
    """Delete a tag from registry and all category assignments."""
    global _all_tags_cache, _glyph_cache, _tag_order_cache

    if tag_name not in _all_tags_cache:
        return False, f"Tag '{tag_name}' not found"

    del _all_tags_cache[tag_name]

    # Remove from order cache
    if tag_name in _tag_order_cache:
        _tag_order_cache.remove(tag_name)

    # Remove from all categories
    for cat_name, cat_data in _glyph_cache.items():
        if "tags" in cat_data and tag_name in cat_data["tags"]:
            cat_data["tags"].remove(tag_name)
            tag_log(f"Removed '{tag_name}' from category '{cat_name}'")

    tag_log(f"Deleted tag: {tag_name}")

    if auto_save:
        _auto_save_tags()

    return True, f"Tag '{tag_name}' deleted"


def get_category_tags(category):
    """Get list of tag names assigned to a category."""
    global _glyph_cache, _glyph_cache_loaded
    if not _glyph_cache_loaded:
        _load_glyph_mappings_from_file()
    cat_data = _glyph_cache.get(category, {})
    return list(cat_data.get("tags", []))


def set_category_tags(category, tags, auto_save=True):
    """Set tags for a category (replaces existing)."""
    global _glyph_cache, _all_tags_cache

    if category not in _glyph_cache:
        # If we are setting empty tags on a non-existent category, don't create it.
        # This prevents "empty" overrides from being created during cancel/restore.
        if not tags:
            tag_log(f"set_category_tags: No tags to set for new category '{category}', skipping creation")
            return True, "No tags to set"
        # Create entry if not exists
        _glyph_cache[category] = _normalize_category_data({})

    # Validate tags exist
    valid_tags = [t for t in tags if t in _all_tags_cache]
    invalid_tags = set(tags) - set(valid_tags)

    if invalid_tags:
        tag_log(f"Warning: Unknown tags ignored: {invalid_tags}", "WARN")

    _glyph_cache[category]["tags"] = valid_tags
    tag_log(f"Set tags for '{category}': {valid_tags}")

    # Update WM override for sync
    update_category_tags_in_wm(category)

    if auto_save:
        _auto_save_tags()

    return True, f"Tags set for '{category}'"


def add_category_tag(category, tag_name, auto_save=True):
    """Add a single tag to a category."""
    global _glyph_cache, _all_tags_cache

    if tag_name not in _all_tags_cache:
        return False, f"Tag '{tag_name}' not found"

    if category not in _glyph_cache:
        _glyph_cache[category] = _normalize_category_data({})

    if "tags" not in _glyph_cache[category]:
        _glyph_cache[category]["tags"] = []

    if tag_name in _glyph_cache[category]["tags"]:
        return True, f"Tag '{tag_name}' already assigned to '{category}'"

    _glyph_cache[category]["tags"].append(tag_name)
    tag_log(f"Added tag '{tag_name}' to '{category}'")

    # Update WM override for sync
    update_category_tags_in_wm(category)

    if auto_save:
        _auto_save_tags()

    return True, f"Tag '{tag_name}' added to '{category}'"


def remove_category_tag(category, tag_name, auto_save=True):
    """Remove a single tag from a category."""
    global _glyph_cache

    if category not in _glyph_cache:
        return False, f"Category '{category}' not found"

    cat_tags = _glyph_cache[category].get("tags", [])

    if tag_name not in cat_tags:
        return True, f"Tag '{tag_name}' not assigned to '{category}'"

    cat_tags.remove(tag_name)
    tag_log(f"Removed tag '{tag_name}' from '{category}'")

    # Update WM override for sync
    update_category_tags_in_wm(category)

    if auto_save:
        _auto_save_tags()

    return True, f"Tag '{tag_name}' removed from '{category}'"


def toggle_category_tag(category, tag_name, auto_save=True):
    """Toggle a tag on/off for a category."""
    tags = get_category_tags(category)
    if tag_name in tags:
        return remove_category_tag(category, tag_name, auto_save)
    else:
        return add_category_tag(category, tag_name, auto_save)


def toggle_category_tag_no_save(category, tag_name):
    """Toggle a tag on/off for a category WITHOUT auto-saving to JSON.
    
    This is used for live preview in the edit dialog. Changes are only
    persisted when the user clicks Save.
    """
    tags = get_category_tags(category)
    if tag_name in tags:
        return remove_category_tag(category, tag_name, auto_save=False)
    else:
        return add_category_tag(category, tag_name, auto_save=False)


def restore_category_tags_from_string(category, tags_string):
    """Restore category tags from a semicolon-separated string.

    This is used when cancelling the edit dialog to revert changes.
    """
    if not tags_string:
        tags = []
    else:
        tags = [t.strip() for t in tags_string.split(';') if t.strip()]

    tag_log(f"Restoring tags for '{category}' from string: '{tags_string}' -> {tags}")
    return set_category_tags(category, tags, auto_save=False)


def update_category_tags_in_wm(category):
    """Update the tags for a category in WM for UI display.
    
    Tags are stored in _glyph_cache and JSON for persistence.
    They are also synced to WM category_glyph_overrides for C++ UI display.
    """
    try:
        wm = bpy.context.window_manager
        if wm is None or not hasattr(wm, 'category_glyph_overrides'):
            tag_log(f"update_category_tags_in_wm: WM or overrides not available", "ERROR")
            return
        
        current_tags = get_category_tags(category)
        tag_log(f"update_category_tags_in_wm: category='{category}', tags={current_tags}")
        
        override_item = None
        for item in wm.category_glyph_overrides:
            if item.category == category:
                override_item = item
                break
        
        if override_item is None:
            # Only create a new override if there are actually tags to store.
            # If tags are empty and no override exists, we don't need to create one.
            if not current_tags:
                tag_log(f"update_category_tags_in_wm: No override and no tags, skipping creation for '{category}'")
                return
            override_item = wm.category_glyph_overrides.new(category=category)
            tag_log(f"update_category_tags_in_wm: Created new override for '{category}'")
        
        if hasattr(override_item, 'tags'):
            override_item.tags = ";".join(current_tags)
            tag_log(f"update_category_tags_in_wm: Set WM override tags for '{category}' to '{override_item.tags}'")
            
        tag_log(f"update_category_tags_in_wm: Completed for '{category}'")
    except Exception as e:
        tag_log(f"update_category_tags_in_wm: Error: {e}", "ERROR")
        import traceback
        traceback.print_exc()


def get_categories_for_tag(tag_name):
    """Get a list of all categories that use a specific tag."""
    global _glyph_cache, _glyph_cache_loaded
    
    # Ensure cache is loaded
    if not _glyph_cache_loaded:
        _load_glyph_mappings_from_file()
        
    categories = []
    for cat_name, cat_data in _glyph_cache.items():
        if isinstance(cat_data, dict) and tag_name in cat_data.get("tags", []):
            categories.append(cat_name)
    return sorted(categories)


def get_category_display_name(category):
    """Get the display name for a category (user-defined or default)."""
    global _glyph_cache, _glyph_cache_loaded
    
    # Ensure cache is loaded
    if not _glyph_cache_loaded:
        _load_glyph_mappings_from_file()
        
    if category in _glyph_cache:
        cat_data = _glyph_cache[category]
        if isinstance(cat_data, dict):
            # 1. User defined display name
            display_name = cat_data.get("display_name", "")
            if display_name:
                return display_name
            # 2. Default display name
            default_name = cat_data.get("default_display_name", "")
            if default_name:
                return default_name
    # 3. Fallback to ID
    return category


def get_category_glyph_data(category):
    """Get glyph, color, and display name for a category."""
    global _glyph_cache, _glyph_cache_loaded
    
    # Ensure cache is loaded
    if not _glyph_cache_loaded:
        _load_glyph_mappings_from_file()
        
    if category in _glyph_cache:
        cat_data = _glyph_cache[category]
        if isinstance(cat_data, str):
            # Old format - just glyph
            return cat_data, [0.0, 0.0, 0.0], category
        elif isinstance(cat_data, dict):
            glyph = cat_data.get("glyph", "")
            color = cat_data.get("color", [0.0, 0.0, 0.0])
            display_name = cat_data.get("display_name", "") or cat_data.get("default_display_name", "") or category
            return glyph, color, display_name
            
    return "", [0.0, 0.0, 0.0], category


class USERPREF_OT_category_tag_remove_from_category(Operator):
    """Remove this tag from a specific category"""
    bl_idname = "wm.category_tag_remove_from_category"
    bl_label = "Remove Tag from Category"
    bl_options = {'REGISTER', 'INTERNAL'}

    category: bpy.props.StringProperty(name="Category")
    tag_name: bpy.props.StringProperty(name="Tag")

    def execute(self, context):
        success, message = remove_category_tag(self.category, self.tag_name, auto_save=True)
        if success:
            self.report({'INFO'}, message)
            context.area.tag_redraw()
            return {'FINISHED'}
        self.report({'ERROR'}, message)
        return {'CANCELLED'}


# -----------------------------------------------------------------------------
# Category Filtering by Tags

def _is_reserved_category_name(category_name):
    """Single source-of-truth check for reserved categories.

    Reserved = categories defined in DEFAULT_CATEGORY_GLYPHS.
    """
    return category_name in DEFAULT_CATEGORY_GLYPHS

# TODO: Add preference U.category_filter_hide_reserved (bool, default false)
# When true, reserved tabs also respect tag filtering


def is_category_visible_by_tags(category_name, active_filter_tags):
    """
    Check if a category should be visible based on tag filtering.

    Rules:
    1. Reserved categories are ALWAYS visible (by default)
    2. If active_filter_tags is empty or None, show all categories
    3. Category is visible if it has at least one of the active tags (OR logic)

    Args:
        category_name: Name of the category to check
        active_filter_tags: List/set of active tag names, or None/empty for all

    Returns:
        bool: True if category should be visible
    """
    # Reserved categories are always visible
    # TODO: Check U.category_filter_hide_reserved preference
    if _is_reserved_category_name(category_name):
        return True

    # No filtering - show all
    if not active_filter_tags:
        return True

    # Convert to set for fast lookup
    active_set = set(active_filter_tags)

    # "All" filter shows everything
    if "All" in active_set:
        return True

    # Check tag intersection (OR logic)
    category_tags = set(get_category_tags(category_name))

    # Categories without tags are only visible with "All" filter
    if not category_tags:
        return False

    return bool(category_tags & active_set)


def filter_categories_by_tags(category_list, active_filter_tags):
    """
    Filter a list of categories by active tags.

    Args:
        category_list: List of category names
        active_filter_tags: List/set of active tag names

    Returns:
        List of visible category names
    """
    return [
        cat for cat in category_list
        if is_category_visible_by_tags(cat, active_filter_tags)
    ]


def get_category_glyph(category):
    """Get the glyph for a category name."""
    global _glyph_cache, _glyph_cache_loaded

    if not _glyph_cache_loaded:
        _load_glyph_mappings_from_file()

    entry = _glyph_cache.get(category, None)
    if entry and isinstance(entry, dict):
        return entry.get("glyph", None)
    return None


def get_category_data(category):
    """Get the full data (glyph, display_name, color, defaults) for a category name."""
    global _glyph_cache, _glyph_cache_loaded

    if not _glyph_cache_loaded:
        _load_glyph_mappings_from_file()

    return _glyph_cache.get(category, None)


def get_default_glyph(category):
    """Get the default glyph for a category name."""
    global _glyph_cache, _glyph_cache_loaded

    if not _glyph_cache_loaded:
        _load_glyph_mappings_from_file()

    entry = _glyph_cache.get(category, None)
    if entry and isinstance(entry, dict):
        result = entry.get("default_glyph", entry.get("glyph", ""))
        print(f"[GLYPH] get_default_glyph('{category}') = '{result}'")
        return result
    print(f"[GLYPH] get_default_glyph('{category}') = '' (not found)")
    return ""


def get_default_display_name(category):
    """Get the default display name for a category name."""
    global _glyph_cache, _glyph_cache_loaded

    if not _glyph_cache_loaded:
        _load_glyph_mappings_from_file()

    entry = _glyph_cache.get(category, None)
    if entry and isinstance(entry, dict):
        result = entry.get("default_display_name", "")
        print(f"[GLYPH] get_default_display_name('{category}') = '{result}'")
        return result
    print(f"[GLYPH] get_default_display_name('{category}') = '' (not found)")
    return ""


def set_category_glyph(category, glyph, save=True):
    """Set the glyph for a category name."""
    global _glyph_cache

    if category not in _glyph_cache:
        _glyph_cache[category] = {
            "glyph": "", "display_name": "", "color": [0.0, 0.0, 0.0],
            "default_glyph": "", "default_display_name": "",
            "glyph_mode": "auto",
            "icon_source": "auto", "icon_key": "", "icon_path": "", "icon_provider": "",
        }

    # Ensure the entry has all required fields
    entry = _glyph_cache[category]
    if "default_glyph" not in entry:
        entry["default_glyph"] = entry.get("glyph", "")
    if "default_display_name" not in entry:
        entry["default_display_name"] = entry.get("display_name", "")
    if "glyph_mode" not in entry:
        entry["glyph_mode"] = "auto"

    if glyph:
        _glyph_cache[category]["glyph"] = glyph
    else:
        # Remove the category if glyph is empty
        if category in _glyph_cache:
            del _glyph_cache[category]

    if save:
        _save_glyph_mappings_to_file()


def set_category_data(category,
                      glyph=None,
                      display_name=None,
                      color=None,
                      tags=None,
                      glyph_mode=None,
                      icon_source=None,
                      icon_key=None,
                      icon_path=None,
                      icon_provider=None,
                      save=True):
    """Set the full data (glyph, display_name, color, tags, icon fields) for a category name."""
    global _glyph_cache

    if category not in _glyph_cache:
        _glyph_cache[category] = {
            "glyph": "", "display_name": "", "color": [0.0, 0.0, 0.0],
            "default_glyph": "", "default_display_name": "",
            "glyph_mode": "auto",
            "icon_source": "auto", "icon_key": "", "icon_path": "", "icon_provider": "",
        }

    # Ensure the entry has all required fields
    entry = _glyph_cache[category]
    if "default_glyph" not in entry:
        entry["default_glyph"] = entry.get("glyph", "")
    if "default_display_name" not in entry:
        entry["default_display_name"] = entry.get("display_name", "")
    if "glyph_mode" not in entry:
        entry["glyph_mode"] = "auto"

    if glyph is not None:
        _glyph_cache[category]["glyph"] = glyph
    if display_name is not None:
        _glyph_cache[category]["display_name"] = display_name
    if color is not None:
        _glyph_cache[category]["color"] = list(color[:3]) if len(color) >= 3 else [0.0, 0.0, 0.0]
    if tags is not None:
        # Parse tags string (comma-separated or single tag)
        if isinstance(tags, str):
            if tags:
                _glyph_cache[category]["tags"] = [t.strip() for t in tags.split(',') if t.strip()]
            else:
                _glyph_cache[category]["tags"] = []
        elif isinstance(tags, list):
            _glyph_cache[category]["tags"] = tags

    if glyph_mode is not None:
        glyph_mode_norm = str(glyph_mode).lower()
        if glyph_mode_norm not in {"auto", "first_letter"}:
            glyph_mode_norm = "auto"
        _glyph_cache[category]["glyph_mode"] = glyph_mode_norm

    if icon_source is not None:
        icon_source_norm = str(icon_source).lower()
        if icon_source_norm not in {"auto", "manual", "off"}:
            icon_source_norm = "auto"
        _glyph_cache[category]["icon_source"] = icon_source_norm
    if icon_key is not None:
        _glyph_cache[category]["icon_key"] = str(icon_key)
    if icon_path is not None:
        _glyph_cache[category]["icon_path"] = str(icon_path)
    if icon_provider is not None:
        _glyph_cache[category]["icon_provider"] = str(icon_provider)

    if save:
        _save_glyph_mappings_to_file()


def reset_category_to_defaults(category, save=True):
    """Reset a single category to its default values (glyph and display_name).

    Color is always reset to [0.0, 0.0, 0.0].
    Returns the default glyph and display_name.
    """
    global _glyph_cache, _glyph_cache_loaded

    print(f"[GLYPH RESET] reset_category_to_defaults called for: '{category}'")

    if not _glyph_cache_loaded:
        _load_glyph_mappings_from_file()

    if category not in _glyph_cache:
        print(f"[GLYPH RESET] Category '{category}' not found in cache!")
        return "", ""

    entry = _glyph_cache[category]
    print(f"[GLYPH RESET] Current entry: {entry}")

    default_glyph = entry.get("default_glyph", entry.get("glyph", ""))
    default_display_name = entry.get("default_display_name", "")

    print(f"[GLYPH RESET] default_glyph='{default_glyph}', default_display_name='{default_display_name}'")

    # Reset to defaults
    entry["glyph"] = default_glyph
    entry["display_name"] = default_display_name
    entry["color"] = [0.0, 0.0, 0.0]

    print(f"[GLYPH RESET] After reset entry: {entry}")

    if save:
        _save_glyph_mappings_to_file()

    return default_glyph, default_display_name


def get_all_category_glyphs():
    """Get all category glyph mappings."""
    global _glyph_cache, _glyph_cache_loaded

    if not _glyph_cache_loaded:
        _load_glyph_mappings_from_file()

    return _glyph_cache.copy()


def reset_category_glyphs_to_defaults():
    """Reset all glyph mappings to defaults."""
    global _glyph_cache

    _glyph_cache = DEFAULT_CATEGORY_GLYPHS.copy()
    _save_glyph_mappings_to_file()


def _integrate_glyph_library():
    """Integrate glyph library with DEFAULT_CATEGORY_GLYPHS."""
    if not get_glyph_library:
        return
    
    try:
        library = get_glyph_library()
        if not library or not library.is_loaded:
            return
        
        # For each category in DEFAULT_CATEGORY_GLYPHS, try to get the glyph name from library
        for category, glyph_data in DEFAULT_CATEGORY_GLYPHS.items():
            glyph_unicode = glyph_data.get('glyph', '')
            if glyph_unicode:
                # Try to find the glyph name in the library
                glyph_obj = library.get_by_codepoint(glyph_unicode)
                if glyph_obj:
                    # Store the glyph name for reference
                    glyph_data['glyph_name'] = glyph_obj.get('name', '')
                    glyph_data['glyph_category'] = glyph_obj.get('category', '')
    except Exception as e:
        print(f"[GLYPH] Warning: Could not integrate glyph library: {e}")


def _discover_active_categories():
    """Discover all active categories from registered panels including addon panels."""
    global _last_discovered_category_sources

    discovered_categories = set()
    discovered_sources = {}
    panel_samples = []

    def _record_discovered(category, source):
        if not category:
            return

        discovered_categories.add(category)
        old_source = discovered_sources.get(category)
        if (old_source is None) or (
            _get_discovery_source_priority(source) < _get_discovery_source_priority(old_source)
        ):
            discovered_sources[category] = source

    def _append_panel_sample(source, panel_name, panel_obj):
        if len(panel_samples) >= 40:
            return
        try:
            category = getattr(panel_obj, 'bl_category', '')
            if not category:
                return
            space_type = getattr(panel_obj, 'bl_space_type', '')
            region_type = getattr(panel_obj, 'bl_region_type', '')
            panel_samples.append((source, panel_name, category, space_type, region_type))
        except Exception:
            return

    try:
        import bpy.types
        from bpy.types import Panel

        # Method 1: Check all registered types that are Panel subclasses
        for type_name in dir(bpy.types):
            try:
                type_obj = getattr(bpy.types, type_name)
                # Check if it's a Panel class with bl_category
                if hasattr(type_obj, 'bl_category') and type_obj.bl_category:
                    _record_discovered(type_obj.bl_category, "panel_discovered")
                    _append_panel_sample("bpy.types", type_name, type_obj)
            except (AttributeError, TypeError):
                continue

        # Method 2: Use Panel.__subclasses__() to find all registered panels
        try:
            for panel_class in Panel.__subclasses__():
                if hasattr(panel_class, 'bl_category') and panel_class.bl_category:
                    _record_discovered(panel_class.bl_category, "panel_discovered")
                    _append_panel_sample("Panel.__subclasses__", getattr(panel_class, "__name__", "<unknown>"), panel_class)
        except Exception as e:
            print(f"[GLYPH] Warning: Could not get Panel subclasses: {e}")

        # Method 3: Check _bli_register_classes if available (internal Blender registry)
        try:
            import _bpy
            if hasattr(_bpy, 'types'):
                for attr_name in dir(_bpy.types):
                    try:
                        attr = getattr(_bpy.types, attr_name)
                        if hasattr(attr, 'bl_category') and attr.bl_category:
                            _record_discovered(attr.bl_category, "panel_discovered")
                            _append_panel_sample("_bpy.types", attr_name, attr)
                    except (AttributeError, TypeError):
                        continue
        except ImportError:
            pass

        # Method 4: Seed categories from enabled extension add-ons.
        # This covers cases where extension is enabled but its panels are not yet
        # discoverable through bpy.types / Panel subclasses at this moment.
        try:
            prefs = getattr(bpy.context, "preferences", None)
            addons = getattr(prefs, "addons", None) if prefs else None
            if addons:
                extensions_dir = bpy.utils.user_resource('EXTENSIONS')
                for addon in addons:
                    module_name = getattr(addon, "module", "")
                    if not isinstance(module_name, str) or not module_name.startswith("bl_ext."):
                        continue

                    module_parts = module_name.split(".")
                    # Expected format: bl_ext.<repo_name>.<package_name>
                    if len(module_parts) < 3:
                        continue

                    repo_name = module_parts[1]
                    pkg_name = ".".join(module_parts[2:])
                    if pkg_name:
                        _record_discovered(pkg_name, "package_dir")
                        print(
                            f"[GLYPH DISCOVER DEBUG] extension addon seed: "
                            f"module={module_name!r}, category={pkg_name!r}, source='module'"
                        )

                    if not extensions_dir:
                        continue

                    manifest_path = os.path.join(extensions_dir, repo_name, pkg_name, "blender_manifest.toml")
                    if not os.path.isfile(manifest_path):
                        continue

                    try:
                        import tomllib
                        with open(manifest_path, "rb") as fh:
                            manifest = tomllib.load(fh)

                        for key_name in ("name", "id"):
                            value = manifest.get(key_name)
                            if isinstance(value, str) and value.strip():
                                seed = value.strip()
                                if key_name == "name":
                                    _record_discovered(seed, "manifest_name")
                                else:
                                    _record_discovered(seed, "manifest_id")
                                print(
                                    f"[GLYPH DISCOVER DEBUG] extension addon seed: "
                                    f"module={module_name!r}, category={seed!r}, source='manifest.{key_name}'"
                                )
                    except Exception as e:
                        print(
                            f"[GLYPH DISCOVER DEBUG] extension manifest read failed: "
                            f"module={module_name!r}, path={manifest_path!r}, error={e}"
                        )
        except Exception as e:
            print(f"[GLYPH DISCOVER DEBUG] extension addon seeding failed: {e}")

        # Method 5: Seed categories from installed extension packages with a root icon file.
        # This is a fallback for cases where addon enable events do not immediately expose
        # panel classes or prefs.addons entries for extension packages.
        try:
            extensions_dir = bpy.utils.user_resource('EXTENSIONS')
            if extensions_dir and os.path.isdir(extensions_dir):
                icon_filenames = ("icon.png", "icon.webp", "icon.jpg", "icon.jpeg")

                for repo_name in os.listdir(extensions_dir):
                    repo_path = os.path.join(extensions_dir, repo_name)
                    if (not os.path.isdir(repo_path)) or repo_name.startswith('.'):
                        continue

                    for pkg_name in os.listdir(repo_path):
                        pkg_path = os.path.join(repo_path, pkg_name)
                        if not os.path.isdir(pkg_path):
                            continue

                        has_root_icon = any(os.path.isfile(os.path.join(pkg_path, icon_name)) for icon_name in icon_filenames)
                        if not has_root_icon:
                            continue

                        if pkg_name:
                            _record_discovered(pkg_name, "package_dir")
                            print(
                                f"[GLYPH DISCOVER DEBUG] extension package seed: "
                                f"repo={repo_name!r}, category={pkg_name!r}, source='package_with_icon'"
                            )

                        manifest_path = os.path.join(pkg_path, "blender_manifest.toml")
                        if not os.path.isfile(manifest_path):
                            continue

                        try:
                            import tomllib
                            with open(manifest_path, "rb") as fh:
                                manifest = tomllib.load(fh)

                            for key_name in ("name", "id"):
                                value = manifest.get(key_name)
                                if isinstance(value, str) and value.strip():
                                    seed = value.strip()
                                    if key_name == "name":
                                        _record_discovered(seed, "manifest_name")
                                    else:
                                        _record_discovered(seed, "manifest_id")
                                    print(
                                        f"[GLYPH DISCOVER DEBUG] extension package seed: "
                                        f"repo={repo_name!r}, category={seed!r}, source='manifest.{key_name}_with_icon'"
                                    )
                        except Exception as e:
                            print(
                                f"[GLYPH DISCOVER DEBUG] extension package manifest read failed: "
                                f"pkg_path={pkg_path!r}, error={e}"
                            )
        except Exception as e:
            print(f"[GLYPH DISCOVER DEBUG] extension package seeding failed: {e}")

        if panel_samples:
            print(f"[GLYPH DISCOVER DEBUG] panel samples collected: {len(panel_samples)} (showing up to 20)")
            for source, panel_name, category, space_type, region_type in panel_samples[:20]:
                print(
                    f"[GLYPH DISCOVER DEBUG] sample: source={source}, panel={panel_name}, "
                    f"bl_category={category!r}, space={space_type!r}, region={region_type!r}"
                )
        else:
            print("[GLYPH DISCOVER DEBUG] panel samples collected: 0")

        _last_discovered_category_sources = discovered_sources
        print(f"[GLYPH] Discovered {len(discovered_categories)} active categories: {sorted(discovered_categories)}")
        return discovered_categories

    except Exception as e:
        _last_discovered_category_sources = {}
        print(f"[GLYPH] Error discovering categories: {e}")
        import traceback
        traceback.print_exc()
        return set()


def _merge_discovered_categories():
    """Merge discovered categories with cached mappings, adding defaults for new ones."""
    global _glyph_cache, _category_orders_cache

    discovered = _discover_active_categories()
    if not discovered:
        return False

    discovered_source_map = dict(_last_discovered_category_sources)

    def _clone_category_data(data):
        if not isinstance(data, dict):
            return _normalize_category_data({})
        cloned = dict(data)
        if isinstance(cloned.get("color"), list):
            cloned["color"] = list(cloned["color"])
        if isinstance(cloned.get("tags"), list):
            cloned["tags"] = list(cloned["tags"])
        return cloned

    def _is_default_color(color):
        return not isinstance(color, list) or color == [0.0, 0.0, 0.0]

    def _merge_alias_into_canonical(canonical_name, alias_name):
        if canonical_name == alias_name:
            return False
        canonical_data = _glyph_cache.get(canonical_name)
        alias_data = _glyph_cache.get(alias_name)
        if alias_data is None:
            return False

        changed = False
        if canonical_data is None:
            _glyph_cache[canonical_name] = _clone_category_data(alias_data)
            canonical_data = _glyph_cache[canonical_name]
            changed = True

        # Prefer keeping existing canonical values, but fill missing important fields from alias.
        for field_name in ("glyph", "display_name", "icon_path", "icon_provider", "icon_source", "icon_key"):
            if not canonical_data.get(field_name) and alias_data.get(field_name):
                canonical_data[field_name] = alias_data.get(field_name)
                changed = True

        if _is_default_color(canonical_data.get("color")) and isinstance(alias_data.get("color"), list) and not _is_default_color(alias_data.get("color")):
            canonical_data["color"] = list(alias_data.get("color"))
            changed = True

        canonical_tags = canonical_data.get("tags", []) if isinstance(canonical_data.get("tags"), list) else []
        alias_tags = alias_data.get("tags", []) if isinstance(alias_data.get("tags"), list) else []
        merged_tags = []
        for tag in canonical_tags + alias_tags:
            if tag not in merged_tags:
                merged_tags.append(tag)
        if merged_tags != canonical_tags:
            canonical_data["tags"] = merged_tags
            changed = True

        # Remap category references in persisted category orders.
        for tag_key, order_list in _category_orders_cache.items():
            if not isinstance(order_list, list):
                continue

            remapped = [canonical_name if category == alias_name else category for category in order_list]
            deduped = []
            for category in remapped:
                if category not in deduped:
                    deduped.append(category)
            if deduped != order_list:
                _category_orders_cache[tag_key] = deduped
                changed = True

        del _glyph_cache[alias_name]
        print(
            f"[GLYPH DISCOVER DEBUG] canonicalized alias in cache: alias={alias_name!r} -> canonical={canonical_name!r}"
        )
        return True

    # Group discovered names by normalized key and choose one canonical key per group.
    discovered_groups = {}
    for category in discovered:
        normalized_key = _normalize_category_key(category) or category
        discovered_groups.setdefault(normalized_key, []).append(category)

    canonical_by_group = {
        group_key: _pick_canonical_category_name(candidates, discovered_source_map)
        for group_key, candidates in discovered_groups.items()
    }

    # Existing cache aliases for active discovered groups are merged into the canonical entry.
    cache_changed = False
    for group_key, canonical_name in canonical_by_group.items():
        aliases_in_cache = [
            cache_name
            for cache_name in list(_glyph_cache.keys())
            if (_normalize_category_key(cache_name) or cache_name) == group_key and cache_name != canonical_name
        ]
        for alias_name in aliases_in_cache:
            cache_changed = _merge_alias_into_canonical(canonical_name, alias_name) or cache_changed

    # Find categories that are in the discovered set but not in cache
    new_categories = discovered - set(_glyph_cache.keys())

    # Canonicalize only new candidates: one mapping per normalized key.
    canonical_new_categories = []
    suppressed_aliases = []
    for group_key, candidates in discovered_groups.items():
        if not any(candidate in new_categories for candidate in candidates):
            continue

        canonical_name = canonical_by_group[group_key]
        if canonical_name not in _glyph_cache:
            canonical_new_categories.append(canonical_name)

        for candidate in candidates:
            if candidate in new_categories and candidate != canonical_name:
                suppressed_aliases.append((candidate, canonical_name))

    new_categories = set(canonical_new_categories)

    if suppressed_aliases:
        print(f"[GLYPH DISCOVER DEBUG] suppressed {len(suppressed_aliases)} alias categories during merge")
        for alias_name, canonical_name in sorted(suppressed_aliases)[:40]:
            print(
                f"[GLYPH DISCOVER DEBUG] alias suppressed: alias={alias_name!r}, canonical={canonical_name!r}"
            )

    if new_categories:
        print(f"[GLYPH] Found {len(new_categories)} new categories: {sorted(new_categories)}")

        # Add new categories with appropriate glyphs from DEFAULT_CATEGORY_GLYPHS
        for category in new_categories:
            # First check if category exists in DEFAULT_CATEGORY_GLYPHS
            if category in DEFAULT_CATEGORY_GLYPHS:
                default_data = DEFAULT_CATEGORY_GLYPHS[category]
                glyph = default_data.get("glyph", "")
            else:
                glyph = None
            if glyph is None:
                # Check if category name is a single glyph (addon category with glyph as name)
                if _is_single_glyph(category):
                    glyph = category  # Use the glyph from category name
                else:
                    # No glyph - leave empty to use fallback letter (first char of category name)
                    glyph = ""

            # Determine base_type
            if glyph and _is_single_glyph(category):
                base_type = "glyph_only"
            elif glyph:
                base_type = "glyph_text"
            else:
                base_type = "text_only"

            _glyph_cache[category] = {
                "glyph": glyph,
                "display_name": "",
                "color": [0.0, 0.0, 0.0],
                "default_glyph": glyph,
                "default_display_name": "",
                "base_type": base_type,
                "glyph_mode": "auto",
                "icon_source": "auto",
                "icon_key": "",
                "icon_path": "",
                "icon_provider": "",
            }

            detected_icon_path, detected_provider = _auto_detect_extension_icon_path(category)
            if detected_icon_path:
                _glyph_cache[category]["icon_path"] = detected_icon_path
                _glyph_cache[category]["icon_provider"] = detected_provider or "extension_auto"
                print(
                    f"[GLYPH ICON AUTO DEBUG] merge new category auto-icon: "
                    f"category={category!r}, path={detected_icon_path!r}, provider={_glyph_cache[category]['icon_provider']!r}"
                )
            else:
                print(f"[GLYPH ICON AUTO DEBUG] merge new category no icon: category={category!r}")

            print(f"[GLYPH] Added new category '{category}' with glyph '{glyph}', base_type={base_type}")

        # Save updated cache to file
        if _save_glyph_mappings_to_file():
            print(f"[GLYPH] Saved {len(new_categories)} new category mappings to JSON")
            return True
        else:
            print(f"[GLYPH] Failed to save new category mappings")

    elif cache_changed:
        if _save_glyph_mappings_to_file():
            print("[GLYPH] Saved cache canonicalization updates to JSON")
            return True
        else:
            print("[GLYPH] Failed to save cache canonicalization updates")

    else:
        print(f"[GLYPH] No new categories found (all {len(discovered)} are cached)")

    return len(new_categories) > 0 or cache_changed


def _is_collection_safe(collection):
    """Check if a bpy_prop_collection is safe to access without triggering crashes."""
    try:
        # Test access without triggering iteration - just check if we can get the RNA type
        # This is a minimal operation that shouldn't trigger ListBase traversal
        _ = collection.bl_rna
        return True
    except (AttributeError, RuntimeError, ReferenceError):
        return False


def sync_glyph_mappings_to_wm():
    """Sync in-memory glyph mappings to window manager collection.

    Note: The collections are cleared in C++ code before file save and after file load
    to prevent crashes from garbage pointers. This function only adds new items.
    """
    global _glyph_cache, _glyph_cache_loaded, _sync_in_progress

    # Prevent recursive calls
    if _sync_in_progress:
        print("[GLYPH SYNC] sync_glyph_mappings_to_wm: sync already in progress, skipping")
        return False

    _sync_in_progress = True
    try:
        return _sync_glyph_mappings_to_wm_impl()
    finally:
        _sync_in_progress = False


def _sync_glyph_mappings_to_wm_impl():
    """Implementation of sync_glyph_mappings_to_wm."""
    global _glyph_cache, _glyph_cache_loaded

    print(f"[GLYPH SYNC] sync_glyph_mappings_to_wm called, cache has {len(_glyph_cache)} entries")

    # Re-check categories each sync to catch addons/extensions enabled after startup.
    # This also triggers icon auto-detection for newly discovered categories.
    try:
        discovered_changes = _merge_discovered_categories()
        print(f"[GLYPH SYNC] late discovery merge result: {discovered_changes}")
    except Exception as e:
        print(f"[GLYPH SYNC] late discovery merge failed: {e}")

    def _has_user_customizations(category_data):
        """Check if category has user customizations (display_name, color, or tags)."""
        if isinstance(category_data, dict):
            display_name = category_data.get("display_name", "")
            color = category_data.get("color", [0.0, 0.0, 0.0])
            tags = category_data.get("tags", [])
            icon_source = str(category_data.get("icon_source", "auto")).lower()
            icon_key = category_data.get("icon_key", "")
            icon_path = category_data.get("icon_path", "")
            icon_provider = category_data.get("icon_provider", "")
            icon_customized = (icon_source != "auto") or bool(icon_key) or bool(icon_path) or bool(icon_provider)
            return bool(display_name) or color != [0.0, 0.0, 0.0] or bool(tags) or icon_customized
        return False

    try:
        wm = bpy.context.window_manager
        if wm is None or not hasattr(wm, 'category_glyph_mappings'):
            print("[GLYPH] WindowManager or collections not available")
            return False

        # Clear existing mappings to avoid duplicates.
        # IMPORTANT: use RNA clear() (C-side) to handle potentially corrupted ListBase
        # safely after loading older/foreign blend files.
        wm.category_glyph_mappings.clear()
        print("[GLYPH SYNC] Cleared existing mappings")

        # Sync available tags (definitions) into wm.category_tags if available
        if hasattr(wm, "category_tags"):
            # Same safety rationale as above: do not iterate Python-side over a potentially
            # corrupted RNA collection right after file load.
            wm.category_tags.clear()
            print("[GLYPH SYNC] Cleared existing tag definitions")

            # Use tag_order if available, otherwise use cache insertion order (added to end)
            if _tag_order_cache:
                # Use saved order, but only include tags that still exist
                tag_names = [t for t in _tag_order_cache if t in _all_tags_cache]
                # Add any new tags not in order list (at the end)
                new_tags = [t for t in _all_tags_cache.keys() if t not in _tag_order_cache]
                tag_names.extend(new_tags)
            else:
                # Default: insertion order (added to end)
                tag_names = list(_all_tags_cache.keys())

            for tag_name in tag_names:
                tag_data = _all_tags_cache[tag_name]
                glyph_hex = _glyph_to_hex(tag_data.get("glyph", "")) if isinstance(tag_data, dict) else ""
                color_val = tag_data.get("color", [0.0, 0.0, 0.0]) if isinstance(tag_data, dict) else [0.0, 0.0, 0.0]
                print(f"[DEBUG PY] Creating tag '{tag_name}' with glyph='{glyph_hex}'")
                tag_item = wm.category_tags.new(name=tag_name)
                tag_item.glyph = glyph_hex
                print(f"[DEBUG PY] Set tag '{tag_name}' glyph to '{glyph_hex}' -> tag_item.glyph='{tag_item.glyph}'")
                tag_item.color = (color_val[0], color_val[1], color_val[2])
                # НОВОЕ: Sync mode flags
                mode_flags_val = tag_data.get("mode_flags", _CATEGORY_TAG_DEFAULT_MODE_FLAGS) if isinstance(tag_data, dict) else _CATEGORY_TAG_DEFAULT_MODE_FLAGS
                tag_item.mode_flags = mode_flags_val
            print(f"[GLYPH SYNC] Synced {len(wm.category_tags)} tag definitions to WM")

        # Add current mappings from cache
        added_count = 0
        skipped_invalid = 0
        try:
            for category, category_data in _glyph_cache.items():
                try:
                    # Skip invalid category names ONLY if they have no user customizations
                    if not _is_valid_category_name(category):
                        if not _has_user_customizations(category_data):
                            skipped_invalid += 1
                            continue

                    # Normalize data to ensure it has all required fields
                    if isinstance(category_data, str):
                        # Old format - convert to new format
                        normalized_data = {"glyph": category_data, "display_name": "", "color": [0.0, 0.0, 0.0]}
                    elif isinstance(category_data, dict):
                        normalized_data = category_data
                    else:
                        continue

                    glyph_val = normalized_data.get("glyph", "")
                    display_name_val = normalized_data.get("display_name", "")
                    color_val = normalized_data.get("color", [0.0, 0.0, 0.0])
                    default_glyph_val = normalized_data.get("default_glyph", glyph_val)
                    default_display_name_val = normalized_data.get("default_display_name", "")
                    tags_val = normalized_data.get("tags", [])
                    icon_source_str = str(normalized_data.get("icon_source", "auto")).lower()
                    glyph_mode_str = str(normalized_data.get("glyph_mode", "auto")).lower()
                    icon_key_val = str(normalized_data.get("icon_key", ""))
                    icon_path_val = str(normalized_data.get("icon_path", ""))
                    icon_provider_val = str(normalized_data.get("icon_provider", ""))

                    if icon_source_str == "auto" and (not icon_key_val) and (not icon_path_val):
                        print(
                            f"[GLYPH ICON AUTO DEBUG] sync attempt: category={category!r}, "
                            f"icon_source={icon_source_str!r}, key={icon_key_val!r}, path={icon_path_val!r}, provider={icon_provider_val!r}"
                        )
                        detected_icon_path, detected_provider = _auto_detect_extension_icon_path(category)
                        if detected_icon_path:
                            icon_path_val = detected_icon_path
                            if not icon_provider_val:
                                icon_provider_val = detected_provider
                            print(
                                f"[GLYPH ICON AUTO DEBUG] sync hit: category={category!r}, "
                                f"path={icon_path_val!r}, provider={icon_provider_val!r}"
                            )
                        else:
                            print(f"[GLYPH ICON AUTO DEBUG] sync miss: category={category!r}")

                    icon_source_to_enum = {
                        "auto": "AUTO",
                        "manual": "MANUAL",
                        "off": "OFF",
                    }
                    icon_source_val = icon_source_to_enum.get(icon_source_str, "AUTO")

                    glyph_mode_to_enum = {
                        "auto": "AUTO",
                        "first_letter": "FIRST_LETTER",
                    }
                    glyph_mode_val = glyph_mode_to_enum.get(glyph_mode_str, "AUTO")

                    item = wm.category_glyph_mappings.new(category=category)
                    item.glyph = glyph_val
                    item.display_name = display_name_val
                    item.color = (color_val[0], color_val[1], color_val[2])
                    item.default_glyph = default_glyph_val
                    item.default_display_name = default_display_name_val
                    if hasattr(item, "icon_source"):
                        item.icon_source = icon_source_val
                    if hasattr(item, "glyph_mode"):
                        item.glyph_mode = glyph_mode_val
                    if hasattr(item, "icon_key"):
                        item.icon_key = icon_key_val
                    if hasattr(item, "icon_path"):
                        item.icon_path = icon_path_val
                    if hasattr(item, "icon_provider"):
                        item.icon_provider = icon_provider_val
                    # Note: is_reserved is read-only (computed in C++), don't try to set it
                    # Sync tags to WM for UI display (semicolon-separated string)
                    if hasattr(item, "tags") and isinstance(tags_val, (list, tuple)):
                        tags_str = ";".join([t for t in tags_val if isinstance(t, str) and t])
                        item.tags = tags_str
                        # Debug: log categories with tags
                        if tags_str:
                            print(f"[GLYPH SYNC] Synced tags for category repr={repr(category)}, len={len(category)}, ord={[ord(c) for c in category]}, tags='{tags_str}'")
                    added_count += 1

                    # Debug: show what was synced for key categories
                    if category in ["Item", "View", "Tool", "Edit"]:
                        print(f"[GLYPH SYNC] Synced '{category}': glyph='{glyph_val}', display_name='{display_name_val}'")
                except Exception as e:
                    print(f"[GLYPH] Error adding mapping for {category}: {e}")

        except Exception as e:
            print(f"[GLYPH] Critical error during mapping addition: {e}")
            import traceback
            traceback.print_exc()
            return False

        if skipped_invalid > 0:
            print(f"[GLYPH] Skipped {skipped_invalid} categories with invalid names and no customizations")
        print(f"[GLYPH] Successfully synced {added_count}/{len(_glyph_cache)} mappings to WM")
        return added_count > 0
    except Exception as e:
        print(f"[GLYPH] Error syncing to WM: {e}")
        import traceback
        traceback.print_exc()
        return False


def register_category_glyph_mappings():
    """Register glyph mappings. Loads from file, discovers addon categories, and syncs to WM."""
    global _glyph_cache_loaded, _initial_load_complete

    # Reset flag at start of registration
    _initial_load_complete = False

    if not _glyph_cache_loaded:
        _load_glyph_mappings_from_file()

    # Integrate with glyph library if available
    _integrate_glyph_library()

    # Discover and merge any new categories from active addons
    try:
        _merge_discovered_categories()
    except Exception as e:
        print(f"[GLYPH] Error during category discovery: {e}")
        # Continue even if discovery fails - we can still sync existing categories

    result = sync_glyph_mappings_to_wm()

    # Mark initial load as complete - callbacks can now save
    _initial_load_complete = True
    print("[GLYPH] Initial load complete, auto-save callbacks enabled")

    return result


def sync_wm_to_glyph_cache():
    """Sync glyph mappings from window manager collection back to cache and JSON.

    This function reads user changes from category_glyph_overrides and
    category_glyph_mappings in WM and saves them to the JSON file.
    """
    global _glyph_cache, _all_tags_cache, _sync_in_progress, _initial_load_complete

    # Don't save during initial load
    if not _initial_load_complete:
        print("[GLYPH SYNC] sync_wm_to_glyph_cache: initial load not complete, skipping save")
        return False

    # Prevent recursive calls
    if _sync_in_progress:
        print("[GLYPH SYNC] sync_wm_to_glyph_cache: sync already in progress, skipping")
        return False

    _sync_in_progress = True
    try:
        return _sync_wm_to_glyph_cache_impl()
    finally:
        _sync_in_progress = False


def _sync_wm_to_glyph_cache_impl():
    """Implementation of sync_wm_to_glyph_cache."""
    global _glyph_cache, _all_tags_cache

    try:
        wm = bpy.context.window_manager
        if wm is None or not hasattr(wm, 'category_glyph_mappings'):
            print("[GLYPH] Cannot sync from WM: WindowManager not available")
            return False

        # Check if collections are safe to access
        if not _is_collection_safe(wm.category_glyph_mappings):
            print("[GLYPH] Cannot sync from WM: collections not safe")
            return False

        changes_detected = False
        print("[GLYPH] Starting sync from WM to cache...")

        # Sync from category_glyph_mappings (default mappings)
        try:
            mappings_count = 0
            for item in wm.category_glyph_mappings:
                category = item.category
                if not category or category == "__test__":
                    continue

                mappings_count += 1

                # Get current cached data or create new entry
                if category not in _glyph_cache:
                    _glyph_cache[category] = {
                        "glyph": "", "display_name": "", "color": [0.0, 0.0, 0.0],
                        "glyph_mode": "auto",
                        "icon_source": "auto", "icon_key": "", "icon_path": "", "icon_provider": "",
                    }

                cached_entry = _glyph_cache[category]

                # Check if any values changed
                if cached_entry.get("glyph", "") != item.glyph:
                    cached_entry["glyph"] = item.glyph
                    changes_detected = True

                if cached_entry.get("display_name", "") != item.display_name:
                    cached_entry["display_name"] = item.display_name
                    changes_detected = True

                cached_color = cached_entry.get("color", [0.0, 0.0, 0.0])
                item_color = list(item.color[:3])
                if cached_color != item_color:
                    cached_entry["color"] = item_color
                    changes_detected = True

                # Tags are NOT synced from WM to cache to avoid truncation issues
                # Tags are stored only in _glyph_cache and JSON

                # Also sync default values.
                # Must sync even when empty string to allow clearing stale values.
                if hasattr(item, 'default_glyph'):
                    item_default_glyph = item.default_glyph or ""
                    if cached_entry.get("default_glyph", "") != item_default_glyph:
                        cached_entry["default_glyph"] = item_default_glyph
                        changes_detected = True

                if hasattr(item, 'default_display_name'):
                    item_default_display_name = item.default_display_name or ""
                    if cached_entry.get("default_display_name", "") != item_default_display_name:
                        cached_entry["default_display_name"] = item_default_display_name
                        changes_detected = True

                if hasattr(item, 'icon_source'):
                    icon_source_raw = getattr(item, "icon_source", "AUTO")
                    if isinstance(icon_source_raw, str):
                        icon_source_val = icon_source_raw.lower()
                        if icon_source_val not in {"auto", "manual", "off"}:
                            icon_source_val = "auto"
                    else:
                        icon_source_from_int = {
                            0: "auto",
                            1: "manual",
                            2: "off",
                        }
                        icon_source_val = icon_source_from_int.get(int(icon_source_raw), "auto")
                    if cached_entry.get("icon_source", "auto") != icon_source_val:
                        cached_entry["icon_source"] = icon_source_val
                        changes_detected = True

                if hasattr(item, 'glyph_mode'):
                    glyph_mode_raw = getattr(item, "glyph_mode", "AUTO")
                    if isinstance(glyph_mode_raw, str):
                        glyph_mode_val = glyph_mode_raw.lower()
                        if glyph_mode_val not in {"auto", "first_letter"}:
                            glyph_mode_val = "auto"
                    else:
                        glyph_mode_from_int = {0: "auto", 1: "first_letter"}
                        glyph_mode_val = glyph_mode_from_int.get(int(glyph_mode_raw), "auto")
                    if cached_entry.get("glyph_mode", "auto") != glyph_mode_val:
                        cached_entry["glyph_mode"] = glyph_mode_val
                        changes_detected = True

                if hasattr(item, 'icon_key'):
                    icon_key_val = item.icon_key or ""
                    if cached_entry.get("icon_key", "") != icon_key_val:
                        cached_entry["icon_key"] = icon_key_val
                        changes_detected = True

                if hasattr(item, 'icon_path'):
                    icon_path_val = item.icon_path or ""
                    if cached_entry.get("icon_path", "") != icon_path_val:
                        cached_entry["icon_path"] = icon_path_val
                        changes_detected = True

                if hasattr(item, 'icon_provider'):
                    icon_provider_val = item.icon_provider or ""
                    if cached_entry.get("icon_provider", "") != icon_provider_val:
                        cached_entry["icon_provider"] = icon_provider_val
                        changes_detected = True

            print(f"[GLYPH] Processed {mappings_count} items from category_glyph_mappings")
        except Exception as e:
            print(f"[GLYPH] Error reading from category_glyph_mappings: {e}")

        # Sync tag definitions from wm.category_tags (if available)
        # Only update cache if WM has data - don't wipe cache with empty WM data
        if hasattr(wm, "category_tags") and _is_collection_safe(wm.category_tags):
            try:
                new_tags_cache = {}
                for tag_item in wm.category_tags:
                    tag_name = getattr(tag_item, "name", "")
                    if not tag_name:
                        continue
                    glyph_hex = getattr(tag_item, "glyph", "") or ""
                    glyph = _hex_to_glyph(glyph_hex) if glyph_hex else ""
                    color = list(getattr(tag_item, "color", (0.0, 0.0, 0.0))[:3])
                    # НОВОЕ: Sync mode flags
                    mode_flags = getattr(tag_item, "mode_flags", _CATEGORY_TAG_DEFAULT_MODE_FLAGS)
                    new_tags_cache[tag_name] = {"glyph": glyph, "color": color, "mode_flags": mode_flags}

                # Only update if WM has tags OR our cache is empty (initial load)
                if new_tags_cache and _all_tags_cache != new_tags_cache:
                    _all_tags_cache = new_tags_cache
                    changes_detected = True
                    print(f"[GLYPH] Synced {len(_all_tags_cache)} tag definitions from WM")
                elif not new_tags_cache and not _all_tags_cache:
                    # Both empty, nothing to do
                    pass
                elif not new_tags_cache and _all_tags_cache:
                    # WM is empty but cache has data - sync cache TO WM instead
                    print(f"[GLYPH] WM category_tags empty, preserving {len(_all_tags_cache)} cached tags")
            except Exception as e:
                print(f"[GLYPH] Error reading from category_tags: {e}")

        # Sync from category_glyph_overrides (user overrides)
        if _is_collection_safe(wm.category_glyph_overrides):
            try:
                overrides_count = 0
                for item in wm.category_glyph_overrides:
                    category = item.category
                    if not category or category == "__test__":
                        continue

                    overrides_count += 1
                    print(f"[GLYPH SYNC] Override found: category='{category}', glyph='{item.glyph}', display_name='{item.display_name}', color={list(item.color[:3])}")

                    # Get current cached data or create new entry
                    if category not in _glyph_cache:
                        _glyph_cache[category] = {
                            "glyph": "", "display_name": "", "color": [0.0, 0.0, 0.0], "tags": [],
                            "glyph_mode": "auto",
                            "icon_source": "auto", "icon_key": "", "icon_path": "", "icon_provider": "",
                        }

                    cached_entry = _glyph_cache[category]

                    # Overrides take precedence
                    if item.glyph:
                        if cached_entry.get("glyph", "") != item.glyph:
                            cached_entry["glyph"] = item.glyph
                            changes_detected = True
                            print(f"[GLYPH SYNC] Updated glyph for '{category}'")

                    if item.display_name:
                        if cached_entry.get("display_name", "") != item.display_name:
                            cached_entry["display_name"] = item.display_name
                            changes_detected = True
                            print(f"[GLYPH SYNC] Updated display_name for '{category}'")

                    # Always save color from override (even if zero - user explicitly set it)
                    item_color = list(item.color[:3])
                    if cached_entry.get("color", [0.0, 0.0, 0.0]) != item_color:
                        cached_entry["color"] = item_color
                        changes_detected = True
                        print(f"[GLYPH SYNC] Updated color for '{category}' to {item_color}")

                    # Tags are NOT synced from WM to cache to avoid truncation issues
                    # Tags are stored only in _glyph_cache and JSON

                    if hasattr(item, 'icon_source'):
                        icon_source_raw = getattr(item, "icon_source", "AUTO")
                        if isinstance(icon_source_raw, str):
                            icon_source_val = icon_source_raw.lower()
                            if icon_source_val not in {"auto", "manual", "off"}:
                                icon_source_val = "auto"
                        else:
                            icon_source_from_int = {
                                0: "auto",
                                1: "manual",
                                2: "off",
                            }
                            icon_source_val = icon_source_from_int.get(int(icon_source_raw), "auto")
                        if cached_entry.get("icon_source", "auto") != icon_source_val:
                            cached_entry["icon_source"] = icon_source_val
                            changes_detected = True

                    if hasattr(item, 'glyph_mode'):
                        glyph_mode_raw = getattr(item, "glyph_mode", "AUTO")
                        if isinstance(glyph_mode_raw, str):
                            glyph_mode_val = glyph_mode_raw.lower()
                            if glyph_mode_val not in {"auto", "first_letter"}:
                                glyph_mode_val = "auto"
                        else:
                            glyph_mode_from_int = {0: "auto", 1: "first_letter"}
                            glyph_mode_val = glyph_mode_from_int.get(int(glyph_mode_raw), "auto")
                        if cached_entry.get("glyph_mode", "auto") != glyph_mode_val:
                            cached_entry["glyph_mode"] = glyph_mode_val
                            changes_detected = True

                    if hasattr(item, 'icon_key'):
                        icon_key_val = item.icon_key or ""
                        if cached_entry.get("icon_key", "") != icon_key_val:
                            cached_entry["icon_key"] = icon_key_val
                            changes_detected = True

                    if hasattr(item, 'icon_path'):
                        icon_path_val = item.icon_path or ""
                        if cached_entry.get("icon_path", "") != icon_path_val:
                            cached_entry["icon_path"] = icon_path_val
                            changes_detected = True

                    if hasattr(item, 'icon_provider'):
                        icon_provider_val = item.icon_provider or ""
                        if cached_entry.get("icon_provider", "") != icon_provider_val:
                            cached_entry["icon_provider"] = icon_provider_val
                            changes_detected = True

                print(f"[GLYPH SYNC] Processed {overrides_count} items from category_glyph_overrides")
            except Exception as e:
                print(f"[GLYPH SYNC] Error reading from category_glyph_overrides: {e}")
                import traceback
                traceback.print_exc()

        # Save to JSON if changes were detected
        if changes_detected:
            if _save_glyph_mappings_to_file():
                print(f"[GLYPH] Saved {len(_glyph_cache)} category mappings from WM to JSON")
            else:
                print("[GLYPH] Failed to save category mappings from WM")
        else:
            # No changes detected, but still save to ensure tags are persisted
            # This is important when Save is clicked after tag changes
            if _save_glyph_mappings_to_file():
                print(f"[GLYPH] Saved {len(_glyph_cache)} category mappings to JSON (no changes detected)")
            else:
                print("[GLYPH] Failed to save category mappings to JSON")

        return changes_detected

    except Exception as e:
        print(f"[GLYPH] Error syncing from WM: {e}")
        return False


def is_zero_v3(color):
    """Check if a 3-component color is all zeros."""
    return color[0] == 0.0 and color[1] == 0.0 and color[2] == 0.0


class USERPREF_OT_save_category_glyphs(Operator):
    """Save category glyph settings to preferences file"""
    bl_idname = "wm.save_category_glyphs"
    bl_label = "Save Category Glyphs"
    bl_options = {'REGISTER', 'INTERNAL'}

    def execute(self, context):
        # Sync from WM to cache and save to JSON
        sync_wm_to_glyph_cache()
        return {'FINISHED'}


class USERPREF_OT_sync_category_glyphs(Operator):
    """Sync category glyph settings from window manager to file"""
    bl_idname = "wm.sync_category_glyphs"
    bl_label = "Sync Category Glyphs"
    bl_options = {'REGISTER', 'INTERNAL'}

    def execute(self, context):
        # Sync from WM to cache and save to JSON
        if sync_wm_to_glyph_cache():
            self.report({'INFO'}, "Category glyphs synchronized")
        else:
            self.report({'WARNING'}, "No changes to synchronize")
        return {'FINISHED'}


# -----------------------------------------------------------------------------
# Tag System - PropertyGroups and Operators

_auto_save_pending = False


def _auto_save_tags():
    """Mark that tags need to be saved (called from update callbacks).
    Does NOT sync to WM - only schedules a JSON save."""
    global _auto_save_pending
    _auto_save_pending = True
    # Use timer to batch saves (no sync - WM already has the data)
    if not bpy.app.timers.is_registered(_deferred_save):
        bpy.app.timers.register(_deferred_save, first_interval=0.5)


def _deferred_save():
    """Deferred save to batch multiple changes."""
    global _auto_save_pending
    if _auto_save_pending:
        _save_tags_to_json()
        _auto_save_pending = False
    return None  # Don't repeat timer


def _sync_mode_flags_from_wm_to_cache():
    """Sync mode flags from Python CategoryTagItem to _all_tags_cache.
    This captures UI changes to mode checkboxes before saving."""
    global _all_tags_cache
    try:
        wm = bpy.context.window_manager
        if not wm or not hasattr(wm, 'category_tags'):
            return
        for tag_item in wm.category_tags:
            tag_name = tag_item.name
            if tag_name in _all_tags_cache and isinstance(_all_tags_cache[tag_name], dict):
                _all_tags_cache[tag_name]["mode_flags"] = tag_item.mode_flags
    except Exception as e:
        print(f"[GLYPH] Error syncing mode flags: {e}")


def _save_tags_to_json():
    """Save tags to JSON file."""
    # НОВОЕ: First sync mode flags from WM items to cache (captures UI changes)
    _sync_mode_flags_from_wm_to_cache()
    sync_glyph_mappings_to_wm()
    _save_glyph_mappings_to_file()


def _save_tag_order_only():
    """Save only tag order to JSON without rebuilding WM collection.
    This avoids potential memory issues when moving tags."""
    global _all_tags_cache, _tag_order_cache, _category_orders_cache

    filepath = _get_glyphs_filepath()
    if not filepath:
        print("[TAG ORDER] No filepath for saving")
        return False

    # Create backup
    create_backup(filepath)

    try:
        # Load existing data to preserve other fields
        with open(filepath, 'r', encoding='utf-8') as f:
            data = json.load(f)
    except Exception:
        # If load fails, create minimal data
        data = {'version': CURRENT_JSON_VERSION}

    # Update tag_order from cache
    data['tag_order'] = list(_tag_order_cache)

    # Save back to file
    try:
        with safe_file_write(filepath) as f:
            json.dump(data, f, indent=2, ensure_ascii=False)
        print(f"[TAG ORDER] Saved order: {_tag_order_cache}")
        return True
    except Exception as e:
        print(f"[TAG ORDER] Save failed: {e}")
        return False


class CategoryTagItem(PropertyGroup):
    """Single tag with glyph and color."""
    name: bpy.props.StringProperty(
        name="Name",
        description="Tag name",
        maxlen=32
    )
    glyph: bpy.props.StringProperty(
        name="Glyph",
        description="Unicode glyph character",
        default="",
        update=lambda self, ctx: _auto_save_tags()  # Auto-save
    )
    color: bpy.props.FloatVectorProperty(
        name="Color",
        subtype='COLOR_GAMMA',
        size=3,
        min=0.0,
        max=1.0,
        default=(0.0, 0.0, 0.0),
        update=lambda self, ctx: _auto_save_tags()  # Auto-save
    )

    # НОВОЕ: Режимы для фильтрации
    mode_object: bpy.props.BoolProperty(
        name="Object Mode",
        default=True,
        update=lambda self, ctx: _auto_save_tags()
    )
    mode_edit: bpy.props.BoolProperty(
        name="Edit Mode",
        default=True,
        update=lambda self, ctx: _auto_save_tags()
    )
    mode_sculpt: bpy.props.BoolProperty(
        name="Sculpt Mode",
        default=True,
        update=lambda self, ctx: _auto_save_tags()
    )
    mode_vertex_paint: bpy.props.BoolProperty(
        name="Vertex Paint",
        default=False,
        update=lambda self, ctx: _auto_save_tags()
    )
    mode_weight_paint: bpy.props.BoolProperty(
        name="Weight Paint",
        default=False,
        update=lambda self, ctx: _auto_save_tags()
    )
    mode_texture_paint: bpy.props.BoolProperty(
        name="Texture Paint",
        default=False,
        update=lambda self, ctx: _auto_save_tags()
    )
    mode_uv_edit: bpy.props.BoolProperty(
        name="UV Edit",
        default=False,
        update=lambda self, ctx: _auto_save_tags()
    )
    mode_pose: bpy.props.BoolProperty(
        name="Pose Mode",
        default=False,
        update=lambda self, ctx: _auto_save_tags()
    )

    def get_mode_flags(self):
        """Convert boolean mode properties to bitmask."""
        flags = 0
        if self.mode_object:
            flags |= 1 << 0  # OBJECT_MODE
        if self.mode_edit:
            flags |= 1 << 1  # EDIT_MODE
        if self.mode_sculpt:
            flags |= 1 << 2  # SCULPT_MODE
        if self.mode_vertex_paint:
            flags |= 1 << 3  # VERTEX_PAINT
        if self.mode_weight_paint:
            flags |= 1 << 4  # WEIGHT_PAINT
        if self.mode_texture_paint:
            flags |= 1 << 5  # TEXTURE_PAINT
        if self.mode_uv_edit:
            flags |= 1 << 6  # UV_EDIT
        if self.mode_pose:
            flags |= 1 << 7  # POSE_MODE
        return flags

    def set_mode_flags(self, flags):
        """Set boolean mode properties from bitmask."""
        self.mode_object = bool(flags & (1 << 0))
        self.mode_edit = bool(flags & (1 << 1))
        self.mode_sculpt = bool(flags & (1 << 2))
        self.mode_vertex_paint = bool(flags & (1 << 3))
        self.mode_weight_paint = bool(flags & (1 << 4))
        self.mode_texture_paint = bool(flags & (1 << 5))
        self.mode_uv_edit = bool(flags & (1 << 6))
        self.mode_pose = bool(flags & (1 << 7))


class CategoryTagAssignment(PropertyGroup):
    """Assignment of a tag to a category."""
    tag_name: bpy.props.StringProperty(name="Tag Name")


class TagModeItem:
    """Wrapper class for tag mode editing.
    Provides boolean properties for UI that sync with _all_tags_cache.
    """
    def __init__(self, tag_name):
        self._tag_name = tag_name
        self._load_from_cache()

    def _load_from_cache(self):
        """Load mode flags from cache."""
        global _all_tags_cache
        tag_data = _all_tags_cache.get(self._tag_name, {})
        mode_flags = tag_data.get("mode_flags", _CATEGORY_TAG_DEFAULT_MODE_FLAGS) if isinstance(tag_data, dict) else _CATEGORY_TAG_DEFAULT_MODE_FLAGS

        self._mode_object = bool(mode_flags & (1 << 0))
        self._mode_edit = bool(mode_flags & (1 << 1))
        self._mode_sculpt = bool(mode_flags & (1 << 2))
        self._mode_vertex_paint = bool(mode_flags & (1 << 3))
        self._mode_weight_paint = bool(mode_flags & (1 << 4))
        self._mode_texture_paint = bool(mode_flags & (1 << 5))
        self._mode_uv_edit = bool(mode_flags & (1 << 6))
        self._mode_pose = bool(mode_flags & (1 << 7))

    def _save_to_cache(self):
        """Save mode flags to cache."""
        global _all_tags_cache
        if self._tag_name in _all_tags_cache and isinstance(_all_tags_cache[self._tag_name], dict):
            flags = 0
            if self._mode_object:
                flags |= 1 << 0
            if self._mode_edit:
                flags |= 1 << 1
            if self._mode_sculpt:
                flags |= 1 << 2
            if self._mode_vertex_paint:
                flags |= 1 << 3
            if self._mode_weight_paint:
                flags |= 1 << 4
            if self._mode_texture_paint:
                flags |= 1 << 5
            if self._mode_uv_edit:
                flags |= 1 << 6
            if self._mode_pose:
                flags |= 1 << 7
            _all_tags_cache[self._tag_name]["mode_flags"] = flags

    @property
    def mode_object(self):
        return self._mode_object

    @mode_object.setter
    def mode_object(self, value):
        self._mode_object = value
        self._save_to_cache()

    @property
    def mode_edit(self):
        return self._mode_edit

    @mode_edit.setter
    def mode_edit(self, value):
        self._mode_edit = value
        self._save_to_cache()

    @property
    def mode_sculpt(self):
        return self._mode_sculpt

    @mode_sculpt.setter
    def mode_sculpt(self, value):
        self._mode_sculpt = value
        self._save_to_cache()

    @property
    def mode_vertex_paint(self):
        return self._mode_vertex_paint

    @mode_vertex_paint.setter
    def mode_vertex_paint(self, value):
        self._mode_vertex_paint = value
        self._save_to_cache()

    @property
    def mode_weight_paint(self):
        return self._mode_weight_paint

    @mode_weight_paint.setter
    def mode_weight_paint(self, value):
        self._mode_weight_paint = value
        self._save_to_cache()

    @property
    def mode_texture_paint(self):
        return self._mode_texture_paint

    @mode_texture_paint.setter
    def mode_texture_paint(self, value):
        self._mode_texture_paint = value
        self._save_to_cache()

    @property
    def mode_uv_edit(self):
        return self._mode_uv_edit

    @mode_uv_edit.setter
    def mode_uv_edit(self, value):
        self._mode_uv_edit = value
        self._save_to_cache()

    @property
    def mode_pose(self):
        return self._mode_pose

    @mode_pose.setter
    def mode_pose(self, value):
        self._mode_pose = value
        self._save_to_cache()

    def _glyph_update(self, context):
        """Callback for glyph property change via RNA (e.g. from C++ picker)."""
        if self.name:
            update_tag(self.name, glyph=_hex_to_glyph(self.glyph))
            # Sync back to cache and trigger redraw
            sync_wm_to_glyph_cache()
            context.area.tag_redraw()

    def _color_update(self, context):
        """Callback for color property change via RNA."""
        if self.name:
            update_tag(self.name, color=list(self.color))
            # Sync back to cache and trigger redraw
            sync_wm_to_glyph_cache()
            context.area.tag_redraw()


def get_tag_mode_item(tag_name):
    """Get a TagModeItem for the given tag name."""
    return TagModeItem(tag_name)


def get_tag_name_by_index(idx):
    """Get tag name by index from wm.category_tags."""
    wm = bpy.context.window_manager
    if wm and hasattr(wm, 'category_tags') and 0 <= idx < len(wm.category_tags):
        return wm.category_tags[idx].name
    return None


def _get_mode_flags_for_tag(tag_name):
    """Get mode flags for a tag from _all_tags_cache."""
    global _all_tags_cache
    if tag_name in _all_tags_cache:
        tag_data = _all_tags_cache[tag_name]
        if isinstance(tag_data, dict):
            return tag_data.get("mode_flags", _CATEGORY_TAG_DEFAULT_MODE_FLAGS)
    return _CATEGORY_TAG_DEFAULT_MODE_FLAGS


def _set_mode_flags_for_tag(tag_name, mode_flags):
    """Set mode flags for a tag in _all_tags_cache."""
    global _all_tags_cache
    if tag_name in _all_tags_cache and isinstance(_all_tags_cache[tag_name], dict):
        _all_tags_cache[tag_name]["mode_flags"] = mode_flags


def with_context_check(func):
    """Decorator to verify context before RNA operations."""
    def wrapper(self, context):
        if context is None:
            self.report({'ERROR'}, "No context available")
            return {'CANCELLED'}
        if context.window_manager is None:
            self.report({'ERROR'}, "Window manager not available")
            return {'CANCELLED'}
        return func(self, context)
    return wrapper


class USERPREF_OT_category_tag_filter_set_mode(Operator):
    """Set category tag filter mode: Current Mode or All Tags"""
    bl_idname = "wm.category_tag_filter_set_mode"
    bl_label = "Set Tag Filter Mode"
    bl_options = {'REGISTER', 'INTERNAL'}

    use_current_mode: bpy.props.BoolProperty(
        name="Use Current Mode",
        description="Filter tags by current object mode (True) or show all tags (False)",
        default=True
    )

    @with_context_check
    def execute(self, context):
        wm = context.window_manager

        if self.use_current_mode:
            # Get current object mode and convert to filter mode enum string
            current_mode_flag = get_current_tag_mode_flag(context)
            # Map mode flags to RNA enum string identifiers
            mode_flag_to_enum = {
                _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("OBJECT_MODE", 0): "OBJECT_MODE",
                _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("EDIT_MODE", 0): "EDIT_MODE",
                _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("SCULPT_MODE", 0): "SCULPT_MODE",
                _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("VERTEX_PAINT", 0): "VERTEX_PAINT",
                _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("WEIGHT_PAINT", 0): "WEIGHT_PAINT",
                _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("TEXTURE_PAINT", 0): "TEXTURE_PAINT",
                _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("UV_EDIT", 0): "UV_EDIT",
                _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("POSE_MODE", 0): "POSE_MODE",
            }
            wm.category_tag_filter_mode = mode_flag_to_enum.get(current_mode_flag, "OBJECT_MODE")
        else:
            # Show all tags
            wm.category_tag_filter_mode = "ALL"

        # Trigger UI update
        context.area.tag_redraw()
        return {'FINISHED'}


class USERPREF_OT_category_tag_create(Operator):
    """Create a new category tag and assign it to the current category"""
    bl_idname = "wm.category_tag_create"
    bl_label = "Create Tag"
    bl_options = {'REGISTER', 'INTERNAL'}

    name: bpy.props.StringProperty(
        name="Name",
        description="Tag name",
        maxlen=32
    )
    category: bpy.props.StringProperty(
        name="Category",
        description="Category to assign the tag to",
        default="",
        options={'HIDDEN'}
    )
    glyph: bpy.props.StringProperty(
        name="Glyph",
        description="Unicode glyph",
        default=""
    )
    glyph_search: bpy.props.StringProperty(
        name="Glyph Search",
        description="Search query for glyph picker",
        default=""
    )
    color: bpy.props.FloatVectorProperty(
        name="Color",
        subtype='COLOR_GAMMA',
        size=3,
        min=0.0,
        max=1.0,
        default=(0.0, 0.0, 0.0)
    )
    current_mode_only: bpy.props.BoolProperty(
        name="Current Mode Only",
        description="Show tag only in the current object mode (otherwise shows in default modes)",
        default=True
    )

    @with_context_check
    def execute(self, context):
        # DEBUG: Проверяем значение glyph при сохранении
        print(f"[DEBUG CREATE_TAG execute] self.glyph = '{self.glyph}'")
        print(f"[DEBUG CREATE_TAG execute] self.name = '{self.name}'")

        if not self.name:
            self.report({'ERROR'}, "Tag name cannot be empty")
            return {'CANCELLED'}

        # Convert hex glyph to Unicode character
        glyph = _hex_to_glyph(self.glyph) if self.glyph else ""
        print(f"[DEBUG CREATE_TAG execute] Converted glyph = '{glyph}'")

        # Determine mode_flags based on current_mode_only checkbox
        if self.current_mode_only:
            mode_flags = get_current_tag_mode_flag(context)
        else:
            mode_flags = _CATEGORY_TAG_DEFAULT_MODE_FLAGS

        success, message = create_tag(
            self.name,
            glyph,
            list(self.color),
            mode_flags=mode_flags,
            auto_save=True
        )
        if success:
            # If category is specified, assign the tag to it
            if self.category:
                add_category_tag(self.category, self.name, auto_save=True)
                tag_log(f"Auto-assigned tag '{self.name}' to category '{self.category}'")

            self.report({'INFO'}, message)

            # Sync to WM to update the UI list immediately
            sync_glyph_mappings_to_wm()
            # Also ensure saved to file
            _auto_save_tags()

            # Set active index to the new tag
            for i, t in enumerate(context.window_manager.category_tags):
                if t.name == self.name:
                    context.window_manager.category_tags_active_index = i
                    break

            context.area.tag_redraw()
            return {'FINISHED'}

        self.report({'ERROR'}, message)
        return {'CANCELLED'}

    def draw(self, context):
        print(
            f"[DEBUG CREATE_TAG draw] self={self!r}, "
            f"name='{self.name}', category='{self.category}', glyph='{self.glyph}', "
            f"glyph_search='{self.glyph_search}'"
        )
        layout = self.layout
        layout.use_property_split = True
        layout.prop(self, "name")

        # Full glyph selector (search + input + preview), aligned with C++ templates.
        layout.template_glyph_selector(
            data=self.properties,
            glyph_property="glyph",
            search_property="glyph_search",
            color_property="color",
            category=self.category or "",
            show_preview=True,
            show_search=True,
            show_code=True,
        )

        # Color presets with glyph buttons
        layout.label(text="Color:")
        row = layout.row()
        row.template_color_glyph_presets(self.properties, "color")

        # Current mode only checkbox (last item)
        layout.separator()
        layout.prop(self, "current_mode_only")

    def invoke(self, context, event):
        print(
            f"[DEBUG CREATE_TAG invoke] self={self!r}, "
            f"incoming category='{self.category}', name='{self.name}'"
        )
        context.window_manager.category_tag_glyph_hex = ""
        self.glyph_search = ""
        # Set default glyph for tags (not category glyph)
        self.glyph = DEFAULT_TAG_GLYPH_HEX
        self.current_mode_only = True
        print(
            f"[DEBUG CREATE_TAG invoke] prepared glyph='{self.glyph}', "
            f"glyph_search='{self.glyph_search}', category='{self.category}'"
        )
        return context.window_manager.invoke_props_dialog(self, width=405)


class USERPREF_OT_category_tag_add(Operator):
    """Add a new tag with default name (for quick list addition)"""
    bl_idname = "wm.category_tag_add"
    bl_label = "Add Tag"
    bl_options = {'REGISTER', 'INTERNAL'}

    @with_context_check
    def execute(self, context):
        # Generate unique default name
        tag_name = generate_unique_tag_name()

        # Create tag with default glyph and color
        glyph = _hex_to_glyph(DEFAULT_TAG_GLYPH_HEX)
        success, message = create_tag(
            tag_name,
            glyph,
            [0.0, 0.0, 0.0]
        )

        if success:
            # Sync to WM to update the UI list immediately
            sync_glyph_mappings_to_wm()

            # Set active index to the new tag
            for i, t in enumerate(context.window_manager.category_tags):
                if t.name == tag_name:
                    context.window_manager.category_tags_active_index = i
                    break

            context.area.tag_redraw()
            self.report({'INFO'}, f"Added '{tag_name}'")
            return {'FINISHED'}

        self.report({'ERROR'}, message)
        return {'CANCELLED'}


class USERPREF_OT_category_tag_edit(Operator):
    """Edit an existing tag"""
    bl_idname = "wm.category_tag_edit"
    bl_label = "Edit Tag"
    bl_options = {'REGISTER', 'INTERNAL'}

    name: bpy.props.StringProperty(name="Tag Name")
    glyph: bpy.props.StringProperty(name="Glyph")
    color: bpy.props.FloatVectorProperty(
        name="Color",
        subtype='COLOR_GAMMA',
        size=3,
        min=0.0,
        max=1.0
    )

    def invoke(self, context, event):
        # Load current values - convert Unicode glyph to hex for display
        tag_data = get_tag_data(self.name)
        self.glyph = _glyph_to_hex(tag_data["glyph"])
        self.color = tag_data["color"]
        context.window_manager.category_tag_glyph_hex = ""
        return context.window_manager.invoke_props_dialog(self, width=350)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.prop(self, "name")

        # Glyph Code input row with Paste button
        layout.template_glyph_input_row(
            self.properties,      # data
            "glyph",              # glyph_property
            None,                 # search_property (not used for Edit Tag)
            has_search=False,     # no search field in Edit Tag
            has_code=True,        # show Code field
            category=""           # no category context for Edit Tag
        )

        # Color presets with glyph buttons
        layout.label(text="Color:")
        row = layout.row()
        row.template_color_glyph_presets(self.properties, "color")

        # Glyph preview - show current glyph with color
        glyph = _hex_to_glyph(self.glyph) if self.glyph else ""
        if glyph:
            layout.template_glyph_preview(
                glyph_unicode=glyph,
                data=self.properties,
                color_property="color",
                size_multiplier=2.0
            )

    @with_context_check
    def execute(self, context):
        # DEBUG: Проверяем значение glyph при сохранении
        print(f"[DEBUG EDIT_TAG execute] self.glyph = '{self.glyph}'")
        print(f"[DEBUG EDIT_TAG execute] self.name = '{self.name}'")

        # Convert hex glyph to Unicode character
        glyph = _hex_to_glyph(self.glyph) if self.glyph else ""
        print(f"[DEBUG EDIT_TAG execute] Converted glyph = '{glyph}'")
        success, message = update_tag(
            self.name,
            glyph,
            list(self.color),
            auto_save=True
        )
        if success:
            self.report({'INFO'}, message)

            # Sync to WM to update the UI list immediately
            sync_glyph_mappings_to_wm()
            # Also ensure saved to file
            _auto_save_tags()

            context.area.tag_redraw()
            return {'FINISHED'}
        self.report({'ERROR'}, message)
        return {'CANCELLED'}


class USERPREF_OT_category_tag_delete(Operator):
    """Delete a tag and remove from all categories"""
    bl_idname = "wm.category_tag_delete"
    bl_label = "Delete Tag"
    bl_options = {'REGISTER', 'INTERNAL'}

    def invoke(self, context, event):
        wm = context.window_manager
        active_idx = wm.category_tags_active_index
        tags = wm.category_tags

        if not (0 <= active_idx < len(tags)):
            self.report({'ERROR'}, "No tag selected")
            return {'CANCELLED'}

        tag_name = tags[active_idx].name
        return context.window_manager.invoke_confirm(
            self,
            event,
            message=f"Delete tag '{tag_name}'?\nIt will be removed from all categories."
        )

    @with_context_check
    def execute(self, context):
        wm = context.window_manager
        active_idx = wm.category_tags_active_index
        tags = wm.category_tags

        if not (0 <= active_idx < len(tags)):
            self.report({'ERROR'}, "No tag selected")
            return {'CANCELLED'}

        tag = tags[active_idx]
        tag_name = tag.name

        success, message = delete_tag(tag_name)
        if success:
            # Sync to WM to update the UI list immediately
            sync_glyph_mappings_to_wm()

            # Adjust active index if needed
            tags = wm.category_tags
            if len(tags) > 0:
                wm.category_tags_active_index = min(active_idx, len(tags) - 1)
            else:
                wm.category_tags_active_index = 0

            self.report({'INFO'}, message)
            context.area.tag_redraw()
            return {'FINISHED'}

        self.report({'ERROR'}, message)
        return {'CANCELLED'}


class USERPREF_OT_category_tag_move(Operator):
    """Move a tag up or down in the list"""
    bl_idname = "wm.category_tag_move"
    bl_label = "Move Tag"
    bl_options = {'REGISTER', 'INTERNAL'}

    direction: bpy.props.EnumProperty(
        name="Direction",
        description="Move direction",
        items=[
            ('UP', "Up", "Move tag up"),
            ('DOWN', "Down", "Move tag down"),
        ],
        default='UP'
    )

    @with_context_check
    def execute(self, context):
        wm = context.window_manager
        active_idx = wm.category_tags_active_index
        tags = wm.category_tags

        print(f"[TAG MOVE] Execute called: direction={self.direction}, active_idx={active_idx}, len(tags)={len(tags)}")

        if not (0 <= active_idx < len(tags)):
            self.report({'ERROR'}, "No tag selected")
            return {'CANCELLED'}

        # Calculate new index
        if self.direction == 'UP':
            if active_idx == 0:
                print(f"[TAG MOVE] Already at top, cancelling")
                return {'CANCELLED'}
            new_idx = active_idx - 1
        else:  # DOWN
            if active_idx >= len(tags) - 1:
                print(f"[TAG MOVE] Already at bottom, cancelling")
                return {'CANCELLED'}
            new_idx = active_idx + 1

        print(f"[TAG MOVE] Moving tag from index {active_idx} to {new_idx}")

        # Save all tags data
        tags_data = []
        for i, tag in enumerate(tags):
            print(f"[TAG MOVE]   Tag {i}: {tag.name}")
            tags_data.append({
                'name': tag.name,
                'glyph': tag.glyph,
                'color': tuple(tag.color),
                'mode_flags': tag.mode_flags,
            })

        # Swap the two items in the list
        tags_data[active_idx], tags_data[new_idx] = tags_data[new_idx], tags_data[active_idx]
        print(f"[TAG MOVE] After swap: {[t['name'] for t in tags_data]}")

        # Clear and rebuild collection in new order
        while len(tags) > 0:
            tags.remove(tags[0])

        for data in tags_data:
            new_tag = tags.new()
            new_tag.name = data['name']
            new_tag.glyph = data['glyph']
            new_tag.color = data['color']
            new_tag.mode_flags = data['mode_flags']

        print(f"[TAG MOVE] Rebuilt collection with {len(tags)} tags")
        wm.category_tags_active_index = new_idx

        # Update tag order cache for persistence
        global _tag_order_cache
        _tag_order_cache = [t['name'] for t in tags_data]
        print(f"[TAG MOVE] Updated tag_order_cache: {_tag_order_cache}")

        # Save directly to JSON (no sync - WM already has correct order)
        _save_tags_to_json()
        context.area.tag_redraw()
        self.report({'INFO'}, f"Moved tag {self.direction}")
        return {'FINISHED'}


def tag_enum_items_callback(self, context):
    """Dynamic enum callback for tag selection."""
    tags = get_tag_names()
    if not tags:
        return [('__none__', "No tags available", "Create a tag first")]
    return [(tag, tag, f"Tag: {tag}") for tag in tags]


class USERPREF_OT_category_tag_toggle(Operator):
    """Toggle a tag on/off for the current category"""
    bl_idname = "wm.category_tag_toggle"
    bl_label = "Toggle Category Tag"
    bl_options = {'REGISTER', 'INTERNAL'}

    category: bpy.props.StringProperty(name="Category")
    tag_name: bpy.props.StringProperty(
        name="Tag",
        description="Tag name to toggle",
        maxlen=32
    )

    @with_context_check
    def execute(self, context):
        if not self.tag_name:
            self.report({'WARNING'}, "No tag specified.")
            return {'CANCELLED'}
        # Use no-save version for live preview in edit dialog
        # Changes are persisted only when user clicks Save
        success, message = toggle_category_tag_no_save(
            self.category,
            self.tag_name
        )
        if success:
            context.area.tag_redraw()
            return {'FINISHED'}
        self.report({'ERROR'}, message)
        return {'CANCELLED'}


class USERPREF_OT_category_tag_filter_set(Operator):
    """Set the active tag filter"""
    bl_idname = "wm.category_tag_filter_set"
    bl_label = "Set Tag Filter"
    bl_options = {'REGISTER', 'INTERNAL'}

    tags: bpy.props.StringProperty(
        name="Tags",
        description="Comma-separated tag names, or 'All' for no filter"
    )

    def execute(self, context):
        # Parse comma-separated tags
        if self.tags.lower() == "all":
            tag_list = []
        else:
            tag_list = [t.strip() for t in self.tags.split(",") if t.strip()]

        # Store in preferences
        context.preferences.category_filter_tags = tag_list
        context.area.tag_redraw()
        return {'FINISHED'}


@bpy.app.handlers.persistent
def _on_load_post(dummy):
    """Load glyph mappings after file load."""
    register_category_glyph_mappings()


@bpy.app.handlers.persistent
def _on_save_pre(dummy):
    """Sync glyph mappings from WM to JSON before saving preferences."""
    sync_wm_to_glyph_cache()


@bpy.app.handlers.persistent
def _on_version_update(dummy):
    """Sync category glyphs after Blender version update or addon enable/disable."""
    # Re-discover categories in case new addons were enabled
    try:
        discovered_before = _discover_active_categories()
        cache_before = set(_glyph_cache.keys())
        print(
            f"[GLYPH VERSION UPDATE DEBUG] before merge: "
            f"discovered={len(discovered_before)}, cache={len(cache_before)}, "
            f"missing_in_cache={sorted(discovered_before - cache_before)}"
        )

        merge_result = _merge_discovered_categories()

        discovered_after = _discover_active_categories()
        cache_after = set(_glyph_cache.keys())
        print(
            f"[GLYPH VERSION UPDATE DEBUG] after merge: "
            f"merge_result={merge_result}, discovered={len(discovered_after)}, cache={len(cache_after)}, "
            f"added_to_cache={sorted(cache_after - cache_before)}, "
            f"still_missing={sorted(discovered_after - cache_after)}"
        )

        sync_glyph_mappings_to_wm()
    except Exception as e:
        print(f"[GLYPH] Error during version update sync: {e}")


# Register handlers
if _on_load_post not in bpy.app.handlers.load_post:
    bpy.app.handlers.load_post.append(_on_load_post)

if _on_save_pre not in bpy.app.handlers.save_pre:
    bpy.app.handlers.save_pre.append(_on_save_pre)

if hasattr(bpy.app.handlers, "version_update"):
    if _on_version_update not in bpy.app.handlers.version_update:
        bpy.app.handlers.version_update.append(_on_version_update)
        print("[GLYPH SYNC] Registered version_update handler for addon/category rediscovery")
else:
    print("[GLYPH SYNC] WARNING: bpy.app.handlers.version_update is unavailable")

# Load mappings on module import
_load_glyph_mappings_from_file()


# -----------------------------------------------------------------------------
# Main Header

class USERPREF_HT_header(Header):
    bl_space_type = 'PREFERENCES'

    @staticmethod
    def draw_buttons(layout, context):
        prefs = context.preferences

        layout.operator_context = 'EXEC_AREA'

        if prefs.use_preferences_save and (not bpy.app.use_userpref_skip_save_on_exit):
            pass
        else:
            # Show '*' to let users know the preferences have been modified.
            # It is shown to the left so that it is visible when the sidebar is narrow,
            # and for consistency with unsaved files in the title bar.
            layout.operator(
                "wm.save_userpref",
                text=("* " if prefs.is_dirty else "") + iface_("Save Preferences"),
                translate=False,
            )

    def draw(self, context):
        layout = self.layout
        layout.operator_context = 'EXEC_AREA'

        layout.template_header()

        USERPREF_MT_editor_menus.draw_collapsible(context, layout)

        layout.separator_spacer()

        self.draw_buttons(layout, context)


# -----------------------------------------------------------------------------
# Main Navigation Bar

class USERPREF_PT_navigation_bar(Panel):
    bl_label = "Preferences Navigation"
    bl_space_type = 'PREFERENCES'
    bl_region_type = 'UI'
    bl_category = "Navigation"
    bl_options = {'HIDE_HEADER'}

    def draw(self, context):
        layout = self.layout
        view = context.space_data

        prefs = context.preferences

        layout.prop(view, "search_filter", icon='VIEWZOOM', text="")
        layout.separator(factor=0.1)

        col = layout.column()

        col.scale_x = 1.3
        col.scale_y = 1.3
        if view.search_filter:
            col.prop_tabs_enum(
                prefs,
                "active_section",
                data_highlight=view,
                property_highlight="tab_search_results",
                expand_as='ROW')
        else:
            col.prop(prefs, "active_section", expand=True)


class USERPREF_MT_editor_menus(Menu):
    bl_idname = "USERPREF_MT_editor_menus"
    bl_label = ""

    def draw(self, _context):
        layout = self.layout
        layout.menu("USERPREF_MT_view")
        layout.menu("USERPREF_MT_save_load", text="Preferences")


class USERPREF_MT_view(Menu):
    bl_label = "View"

    def draw(self, context):
        layout = self.layout
        view = context.space_data

        layout.prop(view, "show_region_ui")
        layout.separator()

        layout.menu("INFO_MT_area")


class USERPREF_MT_save_load(Menu):
    bl_label = "Save & Load"

    def draw(self, context):
        layout = self.layout

        prefs = context.preferences

        row = layout.row()
        row.active = not bpy.app.use_userpref_skip_save_on_exit
        row.prop(prefs, "use_preferences_save", text="Auto-Save Preferences")

        layout.separator()

        layout.operator_context = 'EXEC_AREA'
        if prefs.use_preferences_save:
            layout.operator("wm.save_userpref", text="Save Preferences")

        layout.operator_context = 'INVOKE_AREA'
        sub_revert = layout.column(align=True)
        # NOTE: regarding `factory_startup`. To correctly show the active state of this menu item,
        # the user preferences themselves would need to have a `factory_startup` state.
        # Since showing an active menu item whenever factory-startup is used is not such a problem, leave this as-is.
        sub_revert.active = prefs.is_dirty or bpy.app.factory_startup
        sub_revert.operator("wm.read_userpref", text="Revert to Saved Preferences")

        app_template = prefs.app_template
        if app_template:
            display_name = bpy.path.display_name(iface_(app_template))
            layout.operator("wm.read_factory_userpref", text="Load Factory Blender Preferences")
            props = layout.operator(
                "wm.read_factory_userpref",
                text=iface_("Load Factory {:s} Preferences").format(display_name),
                translate=False,
            )
            props.use_factory_startup_app_template_only = True
            del display_name
        else:
            layout.operator("wm.read_factory_userpref", text="Load Factory Preferences")


class USERPREF_PT_save_preferences(Panel):
    bl_label = "Save Preferences"
    bl_space_type = 'PREFERENCES'
    bl_region_type = 'EXECUTE'
    bl_options = {'HIDE_HEADER'}

    @classmethod
    def poll(cls, context):
        # Hide when header is visible
        for region in context.area.regions:
            if region.type == 'HEADER' and region.height <= 1:
                return True

        return False

    def draw(self, context):
        layout = self.layout.row()
        layout.operator_context = 'EXEC_AREA'

        layout.menu("USERPREF_MT_save_load", text="", icon='COLLAPSEMENU')

        USERPREF_HT_header.draw_buttons(layout, context)


# -----------------------------------------------------------------------------
# Min-In Helpers

# Panel mix-in.
class CenterAlignMixIn:
    """
    Base class for panels to center align contents with some horizontal margin.
    Deriving classes need to implement a ``draw_centered(context, layout)`` function.
    """

    def draw(self, context):
        layout = self.layout
        width = context.region.width
        ui_scale = context.preferences.system.ui_scale
        # No horizontal margin if region is rather small.
        is_wide = width > (350 * ui_scale)

        layout.use_property_split = True
        layout.use_property_decorate = False  # No animation.

        row = layout.row()
        if is_wide:
            row.label()  # Needed so col below is centered.

        col = row.column()
        col.ui_units_x = 50

        # Implemented by sub-classes.
        self.draw_centered(context, col)

        if is_wide:
            row.label()  # Needed so col above is centered.


# -----------------------------------------------------------------------------
# Interface Panels

class InterfacePanel:
    bl_space_type = 'PREFERENCES'
    bl_region_type = 'WINDOW'
    bl_context = "interface"


class USERPREF_PT_interface_display(InterfacePanel, CenterAlignMixIn, Panel):
    bl_label = "Display"

    def draw_centered(self, context, layout):
        prefs = context.preferences
        view = prefs.view

        col = layout.column()

        col.prop(view, "ui_scale", text="Resolution Scale")
        col.prop(view, "ui_line_width", text="Line Width")
        col.prop(view, "show_splash", text="Splash Screen")
        col.prop(view, "show_developer_ui")

        col.separator()

        col = layout.column(heading="Tooltips", align=True)
        col.prop(view, "show_tooltips", text="User Tooltips")
        sub = col.column()
        sub.active = view.show_tooltips
        sub.prop(view, "show_tooltips_python")

        col.separator()

        col = layout.column(heading="Search", align=True)
        col.prop(prefs, "use_recent_searches", text="Sort by Most Recent")
        col.prop(prefs, "show_hidden_ids", text="Show Hidden")


class USERPREF_PT_interface_text(InterfacePanel, CenterAlignMixIn, Panel):
    bl_label = "Text Rendering"
    bl_options = {'DEFAULT_CLOSED'}

    def draw_centered(self, context, layout):
        prefs = context.preferences
        view = prefs.view

        flow = layout.grid_flow(row_major=False, columns=0, even_columns=True, even_rows=False, align=False)

        flow.prop(view, "use_text_antialiasing", text="Anti-Aliasing")
        sub = flow.column()
        sub.active = view.use_text_antialiasing
        sub.prop(view, "use_text_render_subpixelaa", text="Subpixel Anti-Aliasing")
        sub.prop(view, "text_hinting", text="Hinting")

        flow.prop(view, "font_path_ui")
        flow.prop(view, "font_path_ui_mono")


class USERPREF_PT_interface_translation(InterfacePanel, CenterAlignMixIn, Panel):
    bl_label = "Language"
    bl_translation_context = i18n_contexts.id_windowmanager

    @classmethod
    def poll(cls, _context):
        return bpy.app.build_options.international

    def draw_centered(self, context, layout):
        prefs = context.preferences
        view = prefs.view

        layout.prop(view, "language")

        col = layout.column(heading="Translate", heading_ctxt=i18n_contexts.editor_preferences)
        col.active = (bpy.app.translations.locale != "en_US")
        col.prop(view, "use_translate_tooltips", text="Tooltips")
        col.prop(view, "use_translate_interface", text="Interface")
        col.prop(view, "use_translate_reports", text="Reports")
        col.prop(view, "use_translate_new_dataname", text="New Data")


class USERPREF_PT_interface_accessibility(InterfacePanel, CenterAlignMixIn, Panel):
    bl_label = "Accessibility"
    bl_options = {'DEFAULT_CLOSED'}

    def draw_centered(self, context, layout):
        prefs = context.preferences
        view = prefs.view

        flow = layout.grid_flow(row_major=False, columns=0, even_columns=True, even_rows=False, align=False)

        flow.prop(view, "use_reduce_motion")


class USERPREF_PT_interface_editors(InterfacePanel, CenterAlignMixIn, Panel):
    bl_label = "Editors"

    def draw_centered(self, context, layout):
        prefs = context.preferences
        view = prefs.view
        system = prefs.system

        col = layout.column()
        col.prop(system, "use_region_overlap")

        col = layout.column(heading="Show", align=True)
        col.prop(view, "show_area_handle")
        col.prop(view, "show_number_arrows", text="Numeric Input Arrows")
        col.prop(view, "show_navigate_ui")

        col = layout.column()
        col.prop(view, "border_width")
        col.prop(view, "color_picker_type")
        col.row().prop(view, "header_align")
        col.prop(view, "factor_display_type")


class USERPREF_PT_interface_temporary_windows(InterfacePanel, CenterAlignMixIn, Panel):
    bl_label = "Temporary Editors"
    bl_parent_id = "USERPREF_PT_interface_editors"
    bl_options = {'DEFAULT_CLOSED'}

    def draw_centered(self, context, layout):
        prefs = context.preferences
        view = prefs.view

        col = layout.column()
        col.prop(view, "render_display_type", text="Render In")
        col.prop(view, "filebrowser_display_type", text="File Browser")
        col.prop(view, "preferences_display_type", text="Preferences")


class USERPREF_PT_interface_statusbar(InterfacePanel, CenterAlignMixIn, Panel):
    bl_label = "Status Bar"
    bl_parent_id = "USERPREF_PT_interface_editors"
    bl_options = {'DEFAULT_CLOSED'}

    def draw_centered(self, context, layout):
        prefs = context.preferences
        view = prefs.view

        col = layout.column(heading="Show")
        col.prop(view, "show_statusbar_stats", text="Scene Statistics")
        col.prop(view, "show_statusbar_scene_duration", text="Scene Duration")
        col.prop(view, "show_statusbar_memory", text="System Memory")
        col.prop(view, "show_statusbar_vram", text="Video Memory")
        col.prop(view, "show_extensions_updates", text="Extensions Updates")
        col.prop(view, "show_statusbar_version", text="Blender Version")


class USERPREF_PT_interface_menus(InterfacePanel, CenterAlignMixIn, Panel):
    bl_label = "Menus"
    bl_options = {'DEFAULT_CLOSED'}

    def draw_centered(self, context, layout):
        prefs = context.preferences
        view = prefs.view
        col = layout.column()
        col.prop(view, "menu_close_leave")


class USERPREF_PT_interface_menus_mouse_over(InterfacePanel, CenterAlignMixIn, Panel):
    bl_label = "Open on Mouse Over"
    bl_parent_id = "USERPREF_PT_interface_menus"

    def draw_header(self, context):
        prefs = context.preferences
        view = prefs.view

        self.layout.prop(view, "use_mouse_over_open", text="")

    def draw_centered(self, context, layout):
        prefs = context.preferences
        view = prefs.view

        layout.active = view.use_mouse_over_open

        flow = layout.grid_flow(row_major=False, columns=0, even_columns=True, even_rows=False, align=False)

        flow.prop(view, "open_toplevel_delay", text="Top Level")
        flow.prop(view, "open_sublevel_delay", text="Sub Level")


class USERPREF_PT_interface_menus_pie(InterfacePanel, CenterAlignMixIn, Panel):
    bl_label = "Pie Menus"
    bl_parent_id = "USERPREF_PT_interface_menus"

    def draw_centered(self, context, layout):
        prefs = context.preferences
        view = prefs.view

        flow = layout.grid_flow(row_major=False, columns=0, even_columns=True, even_rows=False, align=False)

        flow.prop(view, "pie_animation_timeout")
        flow.prop(view, "pie_tap_timeout")
        flow.prop(view, "pie_initial_timeout")
        flow.prop(view, "pie_menu_radius")
        flow.prop(view, "pie_menu_threshold")
        flow.prop(view, "pie_menu_confirm")


# -----------------------------------------------------------------------------
# Editing Panels

class EditingPanel:
    bl_space_type = 'PREFERENCES'
    bl_region_type = 'WINDOW'
    bl_context = "editing"


class USERPREF_PT_edit_objects(EditingPanel, Panel):
    bl_label = "Objects"

    def draw(self, context):
        pass


class USERPREF_PT_edit_objects_new(EditingPanel, CenterAlignMixIn, Panel):
    bl_label = "New Objects"
    bl_parent_id = "USERPREF_PT_edit_objects"

    def draw_centered(self, context, layout):
        prefs = context.preferences
        edit = prefs.edit

        flow = layout.grid_flow(row_major=False, columns=0, even_columns=True, even_rows=False, align=False)

        flow.prop(edit, "material_link", text="Link Materials To")
        flow.prop(edit, "object_align", text="Align To")
        flow.prop(edit, "use_enter_edit_mode", text="Enter Edit Mode")
        flow.prop(edit, "collection_instance_empty_size", text="Instance Empty Size")


class USERPREF_PT_edit_objects_duplicate_data(EditingPanel, CenterAlignMixIn, Panel):
    bl_label = "Copy on Duplicate"
    bl_parent_id = "USERPREF_PT_edit_objects"
    bl_options = {'DEFAULT_CLOSED'}

    def draw_centered(self, context, layout):
        prefs = context.preferences
        edit = prefs.edit

        layout.use_property_split = False

        flow = layout.grid_flow(row_major=False, columns=0, even_columns=True, even_rows=False, align=True)

        datablock_types = (
            ("use_duplicate_action", "Action", 'ACTION', ""),
            ("use_duplicate_armature", "Armature", 'OUTLINER_DATA_ARMATURE', ""),
            ("use_duplicate_camera", "Camera", 'OUTLINER_DATA_CAMERA', ""),
            ("use_duplicate_curve", "Curve", 'OUTLINER_DATA_CURVE', ""),
            ("use_duplicate_curves", "Curves", 'OUTLINER_DATA_CURVES', ""),
            ("use_duplicate_grease_pencil", "Grease Pencil", 'OUTLINER_OB_GREASEPENCIL', ""),
            ("use_duplicate_lattice", "Lattice", 'OUTLINER_DATA_LATTICE', ""),
            (None, None, None, None),
            ("use_duplicate_light", "Light", 'OUTLINER_DATA_LIGHT', ""),
            ("use_duplicate_lightprobe", "Light Probe", 'OUTLINER_DATA_LIGHTPROBE', ""),
            ("use_duplicate_material", "Material", 'MATERIAL_DATA', ""),
            ("use_duplicate_mesh", "Mesh", 'OUTLINER_DATA_MESH', ""),
            ("use_duplicate_metaball", "Metaball", 'OUTLINER_DATA_META', ""),
            ("use_duplicate_node_tree", "Node Tree", 'NODETREE', ""),
            ("use_duplicate_particle", "Particle", 'PARTICLES', ""),
            (None, None, None, None),
            ("use_duplicate_pointcloud", "Point Cloud", 'OUTLINER_DATA_POINTCLOUD', ""),
            ("use_duplicate_speaker", "Speaker", 'OUTLINER_DATA_SPEAKER', ""),
            ("use_duplicate_surface", "Surface", 'OUTLINER_DATA_SURFACE', ""),
            ("use_duplicate_text", "Text", 'OUTLINER_DATA_FONT', ""),
            ("use_duplicate_volume", "Volume", 'OUTLINER_DATA_VOLUME', "i18n_contexts.id_id"),
        )

        col = flow.column()

        for prop, type_name, type_icon, type_ctx in datablock_types:
            if prop is None:
                col = flow.column()
                continue

            row = col.row()

            row_checkbox = row.row()
            row_checkbox.prop(edit, prop, text="", text_ctxt=type_ctx)

            row_label = row.row()
            row_label.label(text=type_name, icon=type_icon)


class USERPREF_PT_edit_cursor(EditingPanel, CenterAlignMixIn, Panel):
    bl_label = "3D Cursor"

    def draw_centered(self, context, layout):
        prefs = context.preferences
        edit = prefs.edit

        col = layout.column(heading="Cursor")
        col.prop(edit, "use_mouse_depth_cursor", text="Surface Project")
        col.prop(edit, "use_cursor_lock_adjust", text="Lock Adjust")


class USERPREF_PT_edit_gpencil(EditingPanel, CenterAlignMixIn, Panel):
    bl_label = "Grease Pencil"
    bl_options = {'DEFAULT_CLOSED'}

    def draw_centered(self, context, layout):
        prefs = context.preferences
        edit = prefs.edit

        col = layout.column(heading="Distance")
        col.prop(edit, "grease_pencil_manhattan_distance", text="Manhattan")
        col.prop(edit, "grease_pencil_euclidean_distance", text="Euclidean")


class USERPREF_PT_edit_annotations(EditingPanel, CenterAlignMixIn, Panel):
    bl_label = "Annotations"

    def draw_centered(self, context, layout):
        prefs = context.preferences
        edit = prefs.edit

        col = layout.column()
        col.prop(edit, "grease_pencil_default_color", text="Default Color")
        col.prop(edit, "grease_pencil_eraser_radius", text="Eraser Radius")


class USERPREF_PT_edit_weight_paint(EditingPanel, CenterAlignMixIn, Panel):
    bl_label = "Weight Paint"
    bl_options = {'DEFAULT_CLOSED'}

    def draw_centered(self, context, layout):
        prefs = context.preferences
        view = prefs.view

        layout.use_property_split = False

        layout.prop(view, "use_weight_color_range", text="Custom Gradient")

        col = layout.column()
        col.active = view.use_weight_color_range
        col.template_color_ramp(view, "weight_color_range", expand=True)


class USERPREF_PT_edit_text_editor(EditingPanel, CenterAlignMixIn, Panel):
    bl_label = "Text Editor"
    bl_options = {'DEFAULT_CLOSED'}

    def draw_centered(self, context, layout):
        prefs = context.preferences
        edit = prefs.edit

        layout.prop(edit, "use_text_edit_auto_close")


class USERPREF_PT_edit_node_editor(EditingPanel, CenterAlignMixIn, Panel):
    bl_label = "Node Editor"
    bl_options = {'DEFAULT_CLOSED'}

    def draw_centered(self, context, layout):
        prefs = context.preferences
        edit = prefs.edit

        col = layout.column(heading="Auto-Offset")
        row = col.row()
        row.prop(edit, "node_use_insert_offset", text="")
        subrow = row.row()
        subrow.prop(edit, "node_margin", text="")
        subrow.active = edit.node_use_insert_offset

        layout.prop(edit, "node_preview_resolution", text="Preview Resolution")


class USERPREF_PT_edit_sequence_editor(EditingPanel, CenterAlignMixIn, Panel):
    bl_label = "Video Sequencer"
    bl_options = {'DEFAULT_CLOSED'}

    def draw_centered(self, context, layout):
        prefs = context.preferences
        edit = prefs.edit

        layout.prop(edit, "connect_strips_by_default")


class USERPREF_PT_edit_misc(EditingPanel, CenterAlignMixIn, Panel):
    bl_label = "Miscellaneous"
    bl_options = {'DEFAULT_CLOSED'}

    def draw_centered(self, context, layout):
        prefs = context.preferences
        edit = prefs.edit

        col = layout.column()
        col.prop(edit, "sculpt_paint_overlay_color", text="Sculpt Overlay Color")


# -----------------------------------------------------------------------------
# Animation Panels

class AnimationPanel:
    bl_space_type = 'PREFERENCES'
    bl_region_type = 'WINDOW'
    bl_context = "animation"


class USERPREF_PT_animation_timeline(AnimationPanel, CenterAlignMixIn, Panel):
    bl_label = "Timeline"

    def draw_centered(self, context, layout):
        prefs = context.preferences
        view = prefs.view
        edit = prefs.edit

        col = layout.column()
        col.prop(edit, "use_negative_frames")

        col.prop(view, "view2d_grid_spacing_min", text="Minimum Grid Spacing")
        col.prop(view, "timecode_style")
        col.prop(view, "view_frame_type")
        if view.view_frame_type == 'SECONDS':
            col.prop(view, "view_frame_seconds")
        elif view.view_frame_type == 'KEYFRAMES':
            col.prop(view, "view_frame_keyframes")


class USERPREF_PT_animation_keyframes(AnimationPanel, CenterAlignMixIn, Panel):
    bl_label = "Keyframes"

    def draw_centered(self, context, layout):
        prefs = context.preferences
        edit = prefs.edit

        layout.prop(edit, "key_insert_channels", expand=True)

        row = layout.row(align=True, heading="Only Insert Needed")
        row.prop(edit, "use_keyframe_insert_needed", text="Manual", toggle=1)
        row.prop(edit, "use_auto_keyframe_insert_needed", text="Auto", toggle=1)

        col = layout.column(heading="Keyframing")
        col.prop(edit, "use_visual_keying")

        col = layout.column(heading="Auto-Keyframing")
        col.prop(edit, "use_auto_keying", text="Enable in New Scenes")
        col.prop(edit, "use_auto_keying_warning", text="Show Warning")
        col.prop(edit, "use_keyframe_insert_available", text="Only Insert Available")


class USERPREF_PT_animation_fcurves(AnimationPanel, CenterAlignMixIn, Panel):
    bl_label = "F-Curves"

    def draw_centered(self, context, layout):
        prefs = context.preferences
        edit = prefs.edit

        flow = layout.grid_flow(row_major=False, columns=0, even_columns=True, even_rows=False, align=False)

        flow.prop(edit, "fcurve_unselected_alpha", text="Unselected Opacity")
        flow.prop(edit, "fcurve_new_auto_smoothing", text="Default Smoothing Mode")
        flow.prop(edit, "keyframe_new_interpolation_type", text="Default Interpolation")
        flow.prop(edit, "keyframe_new_handle_type", text="Default Handles")
        flow.prop(edit, "use_insertkey_xyz_to_rgb", text="XYZ to RGB")
        flow.prop(edit, "use_anim_channel_group_colors")
        flow.prop(edit, "show_only_selected_curve_keyframes")
        flow.prop(edit, "use_fcurve_high_quality_drawing")


# -----------------------------------------------------------------------------
# System Panels

class SystemPanel:
    bl_space_type = 'PREFERENCES'
    bl_region_type = 'WINDOW'
    bl_context = "system"


class USERPREF_PT_system_sound(SystemPanel, CenterAlignMixIn, Panel):
    bl_label = "Sound"
    bl_options = {'DEFAULT_CLOSED'}

    def draw_centered(self, context, layout):
        prefs = context.preferences
        system = prefs.system

        layout.prop(system, "audio_device", expand=False)

        sub = layout.grid_flow(row_major=False, columns=0, even_columns=False, even_rows=False, align=False)
        sub.active = system.audio_device not in {'NONE', 'None'}
        sub.prop(system, "audio_channels", text="Channels")
        sub.prop(system, "audio_mixing_buffer", text="Mixing Buffer")
        sub.prop(system, "audio_sample_rate", text="Sample Rate")
        sub.prop(system, "audio_sample_format", text="Sample Format")


class USERPREF_PT_system_cycles_devices(SystemPanel, CenterAlignMixIn, Panel):
    bl_label = "Cycles Render Devices"

    def draw_centered(self, context, layout):
        prefs = context.preferences

        col = layout.column()
        col.use_property_split = False

        if bpy.app.build_options.cycles:
            addon = prefs.addons.get("cycles")
            if addon is None:
                layout.label(text="Enable Cycles Render Engine add-on to use Cycles", icon='INFO')
            else:
                addon.preferences.draw_impl(col, context)
            del addon
        else:
            layout.label(text="Cycles is disabled in this build", icon='INFO')


class USERPREF_PT_system_display_graphics(SystemPanel, CenterAlignMixIn, Panel):
    bl_label = "Display Graphics"

    @classmethod
    def poll(cls, _context):
        import platform
        return platform.system() != "Darwin"

    def draw_centered(self, context, layout):
        prefs = context.preferences
        system = prefs.system
        import gpu
        import sys

        col = layout.column()
        col.prop(system, "gpu_backend", text="Backend")
        if system.gpu_backend == 'VULKAN':
            col = layout.column()
            col.enabled = gpu.platform.backend_type_get() == 'VULKAN'
            col.prop(system, "gpu_preferred_device")

        if system.gpu_backend != gpu.platform.backend_type_get():
            layout.label(text="A restart of Blender is required", icon='INFO')

        if system.gpu_backend == 'VULKAN':
            if sys.platform == "win32" and gpu.platform.device_type_get() == 'QUALCOMM':
                col = layout.column()
                col.label(text="Current Vulkan backend limitations:", icon='INFO')
                col.label(text="\u2022 Windows on ARM requires driver 31.0.112.0 or higher", icon='BLANK1')


class USERPREF_PT_system_os_settings(SystemPanel, CenterAlignMixIn, Panel):
    bl_label = "Operating System Settings"

    @classmethod
    def poll(cls, _context):
        # macOS isn't supported.
        from sys import platform
        if platform == "darwin":
            return False
        return True

    @staticmethod
    def _draw_associate_supported_or_label(context, layout):
        from sys import platform
        if platform[:3] == "win":
            if context.preferences.system.is_microsoft_store_install:
                layout.label(text="Microsoft Store installation")
                layout.label(text="Use Windows 'Default Apps' to associate with blend files")
                return False
        else:
            # Linux.
            if not bpy.app.portable:
                layout.label(text="System Installation")
                layout.label(text="File association is handled by the package manager")
                return False

            import os
            if os.environ.get("SNAP"):
                layout.label(text="Snap Package Installation")
                layout.label(text="File association is handled by the package manager")
                return False

        return True

    def draw_centered(self, context, layout):
        if self._draw_associate_supported_or_label(context, layout):
            layout.label(text="Open blend files with this Blender version")
            split = layout.split(factor=0.5)
            split.alignment = 'LEFT'
            split.operator("preferences.associate_blend", text="Register")
            split.operator("preferences.unassociate_blend", text="Unregister")
            layout.prop(bpy.context.preferences.system, "register_all_users", text="For All Users")


class USERPREF_PT_system_network(SystemPanel, CenterAlignMixIn, Panel):
    bl_label = "Network"

    def draw_centered(self, context, layout):
        prefs = context.preferences
        system = prefs.system

        row = layout.row()
        row.prop(system, "use_online_access", text="Allow Online Access")

        # Show when the preference has been overridden and doesn't match the current preference.
        runtime_online_access = bpy.app.online_access
        if system.use_online_access != runtime_online_access:
            row = layout.split(factor=0.4)
            row.label(text="")
            if runtime_online_access:
                text = iface_("Enabled on startup, overriding the preference.")
            else:
                text = iface_("Disabled on startup, overriding the preference.")
            row.label(text=text, translate=False)

        layout.row().prop(system, "network_timeout", text="Time Out")
        layout.row().prop(system, "network_connection_limit", text="Connection Limit")


class USERPREF_PT_system_memory(SystemPanel, CenterAlignMixIn, Panel):
    bl_label = "Memory & Limits"

    def draw_centered(self, context, layout):
        import sys

        prefs = context.preferences
        system = prefs.system
        edit = prefs.edit

        col = layout.column()
        col.prop(edit, "undo_steps", text="Undo Steps")
        col.prop(edit, "undo_memory_limit", text="Undo Memory Limit")
        col.prop(edit, "use_global_undo")

        layout.separator()

        col = layout.column()
        col.prop(system, "scrollback", text="Console Scrollback Lines")

        layout.separator()

        col = layout.column()
        col.prop(system, "texture_time_out", text="Texture Time Out")
        col.prop(system, "texture_collection_rate", text="Garbage Collection Rate")

        layout.separator()

        col = layout.column()
        col.prop(system, "vbo_time_out", text="VBO Time Out")
        col.prop(system, "vbo_collection_rate", text="Garbage Collection Rate")

        if sys.platform != "darwin":
            layout.separator()
            col = layout.column(align=True)
            col.active = system.gpu_backend != 'VULKAN'
            col.row().prop(system, "shader_compilation_method", expand=True)
            label = iface_("Threads") if system.shader_compilation_method == 'THREAD' else iface_("Subprocesses")
            col.prop(system, "gpu_shader_workers", text=label, translate=False)

        layout.separator()

        col = layout.column()
        col.prop(system, "geometry_nodes_stack_limit")


class USERPREF_PT_system_video_sequencer(SystemPanel, CenterAlignMixIn, Panel):
    bl_label = "Video Sequencer"

    def draw_centered(self, context, layout):
        prefs = context.preferences
        system = prefs.system

        layout.prop(system, "memory_cache_limit")

        layout.separator()

        layout.prop(system, "sequencer_proxy_setup")


# -----------------------------------------------------------------------------
# Viewport Panels

class ViewportPanel:
    bl_space_type = 'PREFERENCES'
    bl_region_type = 'WINDOW'
    bl_context = "viewport"


class USERPREF_PT_viewport_display(ViewportPanel, CenterAlignMixIn, Panel):
    bl_label = "Display"

    def draw_centered(self, context, layout):
        prefs = context.preferences
        view = prefs.view

        col = layout.column(heading="Text Info Overlay")
        col.prop(view, "show_object_info", text="Object Info")
        col.prop(view, "show_view_name", text="View Name")

        col = layout.column(heading="Playback Frame Rate (FPS)")
        row = col.row()
        row.prop(view, "show_playback_fps", text="")
        subrow = row.row()
        subrow.active = view.show_playback_fps
        subrow.prop(view, "playback_fps_samples", text="Samples")

        layout.separator()

        col = layout.column()
        col.prop(view, "gizmo_size")
        col.prop(view, "lookdev_sphere_size")

        col.separator()

        col.prop(view, "mini_axis_type", text="3D Viewport Axes")

        if view.mini_axis_type == 'MINIMAL':
            col.prop(view, "mini_axis_size", text="Size")
            col.prop(view, "mini_axis_brightness", text="Brightness")

        if view.mini_axis_type == 'GIZMO':
            col.prop(view, "gizmo_size_navigate_v3d", text="Size")

        layout.separator()
        col = layout.column(heading="Fresnel")
        col.prop(view, "use_fresnel_edit")


class USERPREF_PT_viewport_quality(ViewportPanel, CenterAlignMixIn, Panel):
    bl_label = "Quality"

    def draw_centered(self, context, layout):
        prefs = context.preferences
        system = prefs.system

        col = layout.column()
        col.prop(system, "viewport_aa")

        col = layout.column(heading="Smooth Wires")
        col.prop(system, "use_overlay_smooth_wire", text="Overlay")
        col.prop(system, "use_edit_mode_smooth_wire", text="Edit Mode")


class USERPREF_PT_viewport_textures(ViewportPanel, CenterAlignMixIn, Panel):
    bl_label = "Textures"

    def draw_centered(self, context, layout):
        prefs = context.preferences
        system = prefs.system

        col = layout.column()
        col.prop(system, "gl_texture_limit", text="Limit Size")
        col.prop(system, "anisotropic_filter")
        col.prop(system, "gl_clip_alpha", slider=True)
        col.prop(system, "image_draw_method", text="Image Display Method")


class USERPREF_PT_viewport_subdivision(ViewportPanel, CenterAlignMixIn, Panel):
    bl_label = "Subdivision"
    bl_options = {'DEFAULT_CLOSED'}

    def draw_centered(self, context, layout):
        prefs = context.preferences
        system = prefs.system

        layout.prop(system, "use_gpu_subdivision")


# -----------------------------------------------------------------------------
# Theme Panels

class ThemePanel:
    bl_space_type = 'PREFERENCES'
    bl_region_type = 'WINDOW'
    bl_context = "themes"


class USERPREF_MT_interface_theme_presets(Menu):
    # NOTE: this label is currently not used, see: !134844.
    bl_label = "Presets"

    preset_subdir = "interface_theme"
    preset_operator = "script.execute_preset"
    preset_type = 'XML'
    preset_xml_map = (
        ("preferences.themes[0]", "Theme"),
        ("preferences.ui_styles[0]", "ThemeStyle"),
    )
    # Prevent untrusted XML files "escaping" from these types.
    preset_xml_secure_types = {
        "Theme",
        "ThemeBoneColorSet",
        "ThemeClipEditor",
        "ThemeCollectionColor",
        "ThemeCommon",
        "ThemeCommonAnim",
        "ThemeCommonCurves",
        "ThemeConsole",
        "ThemeDopeSheet",
        "ThemeFileBrowser",
        "ThemeFontStyle",
        "ThemeGradientColors",
        "ThemeGraphEditor",
        "ThemeImageEditor",
        "ThemeInfo",
        "ThemeNLAEditor",
        "ThemeNodeEditor",
        "ThemeOutliner",
        "ThemePreferences",
        "ThemeProperties",
        "ThemeRegions",
        "ThemeRegionsAssetShelf",
        "ThemeRegionsChannels",
        "ThemeRegionsScrubbing",
        "ThemeRegionsSidebars",
        "ThemeSequenceEditor",
        "ThemeSpaceGeneric",
        "ThemeSpaceGradient",
        "ThemeSpaceListGeneric",
        "ThemeSpreadsheet",
        "ThemeStatusBar",
        "ThemeStripColor",
        "ThemeStyle",
        "ThemeTextEditor",
        "ThemeTopBar",
        "ThemeUserInterface",
        "ThemeView3D",
        "ThemeWidgetColors",
        "ThemeWidgetStateColors",
    }

    draw = Menu.draw_preset

    @staticmethod
    def reset_cb(_context, _filepath):
        bpy.ops.preferences.reset_default_theme()

    @staticmethod
    def post_cb(context, filepath):
        context.preferences.themes[0].filepath = filepath


class USERPREF_PT_theme(ThemePanel, Panel):
    bl_label = "Themes"
    bl_options = {'HIDE_HEADER'}

    def draw(self, context):
        import os

        layout = self.layout

        split = layout.split(factor=0.6)

        row = split.row(align=True)

        # Unlike most presets (which use the classes bl_label),
        # themes store the path, use this when set.
        if filepath := context.preferences.themes[0].filepath:
            preset_label = bpy.path.display_name(os.path.basename(filepath))
        else:
            # If the `filepath` is empty, assume the theme was reset and use the default theme name as a label.
            # This would typically be:
            # `preset_label = USERPREF_MT_interface_theme_presets.bl_label`
            # However the operator to reset the preferences doesn't clear the value,
            # so it's simplest to hard-code "Presets" here.
            preset_label = "Presets"

        row.menu("USERPREF_MT_interface_theme_presets", text=preset_label)
        del filepath, preset_label

        row.operator("wm.interface_theme_preset_add", text="", icon='ADD')
        row.operator("wm.interface_theme_preset_remove", text="", icon='REMOVE')
        row.operator("wm.interface_theme_preset_save", text="", icon='FILE_TICK')

        row = split.row(align=True)
        row.operator("preferences.theme_install", text="Install...", icon='IMPORT')
        row.operator("preferences.reset_default_theme", text="Reset", icon='LOOP_BACK')


class USERPREF_PT_theme_user_interface(ThemePanel, CenterAlignMixIn, Panel):
    bl_label = "User Interface"
    bl_options = {'DEFAULT_CLOSED'}

    def draw_header(self, _context):
        layout = self.layout

        layout.label(icon='WORKSPACE')

    def draw(self, context):
        pass


# Base class for dynamically defined widget color panels.
# This is not registered.
class PreferenceThemeWidgetColorPanel:
    bl_parent_id = "USERPREF_PT_theme_user_interface"

    def draw(self, context):
        theme = context.preferences.themes[0]
        ui = theme.user_interface
        widget_style = getattr(ui, self.wcol)
        layout = self.layout

        layout.use_property_split = True

        flow = layout.grid_flow(row_major=False, columns=0, even_columns=True, even_rows=False, align=False)

        col = flow.column(align=True)
        col.prop(widget_style, "text")
        col.prop(widget_style, "text_sel", text="Selected")
        col.prop(widget_style, "item", slider=True)

        col = flow.column(align=True)
        col.prop(widget_style, "inner", slider=True)
        col.prop(widget_style, "inner_sel", text="Selected", slider=True)

        col = flow.column(align=True)
        col.prop(widget_style, "outline")
        col.prop(widget_style, "outline_sel", text="Selected", slider=True)

        col.separator()

        col.prop(widget_style, "roundness")


# Base class for dynamically defined widget color panels.
# This is not registered.
class PreferenceThemeWidgetShadePanel:

    def draw(self, context):
        theme = context.preferences.themes[0]
        ui = theme.user_interface
        widget_style = getattr(ui, self.wcol)
        layout = self.layout

        layout.use_property_split = True

        col = layout.column(align=True)
        col.active = widget_style.show_shaded
        col.prop(widget_style, "shadetop", text="Shade Top")
        col.prop(widget_style, "shadedown", text="Down")

    def draw_header(self, context):
        theme = context.preferences.themes[0]
        ui = theme.user_interface
        widget_style = getattr(ui, self.wcol)

        self.layout.prop(widget_style, "show_shaded", text="")


class USERPREF_PT_theme_interface_panel(ThemePanel, CenterAlignMixIn, Panel):
    bl_label = "Panel"
    bl_options = {'DEFAULT_CLOSED'}
    bl_parent_id = "USERPREF_PT_theme_user_interface"

    def draw_centered(self, context, layout):
        theme = context.preferences.themes[0]
        ui = theme.user_interface

        flow = layout.grid_flow(row_major=False, columns=2, even_columns=True, even_rows=False, align=False)

        col = flow.column()
        col.prop(ui, "panel_header", text="Header")

        col = col.column(align=True)
        col.prop(ui, "panel_back", text="Background")
        col.prop(ui, "panel_sub_back", text="Sub-Panel")

        col = col.column()
        col.prop(ui, "panel_active", text="Active")

        col = flow.column(align=True)
        col.prop(ui, "panel_title", text="Title")
        col.prop(ui, "panel_text", text="Text")

        col = col.column()
        col.prop(ui, "panel_outline", text="Outline")
        col.prop(ui, "panel_roundness", text="Roundness")


class USERPREF_PT_theme_interface_state(ThemePanel, CenterAlignMixIn, Panel):
    bl_label = "State"
    bl_options = {'DEFAULT_CLOSED'}
    bl_parent_id = "USERPREF_PT_theme_user_interface"

    def draw_centered(self, context, layout):
        theme = context.preferences.themes[0]
        ui_state = theme.user_interface.wcol_state

        flow = layout.grid_flow(row_major=False, columns=0, even_columns=True, even_rows=False, align=False)

        col = flow.column(align=True)

        col.prop(ui_state, "error")
        col.prop(ui_state, "warning")
        col.prop(ui_state, "info")
        col.prop(ui_state, "success")

        col = flow.column(align=True)
        col.prop(ui_state, "inner_anim")
        col.prop(ui_state, "inner_anim_sel", text="Selected")

        col = flow.column(align=True)
        col.prop(ui_state, "inner_driven")
        col.prop(ui_state, "inner_driven_sel", text="Selected")

        col = flow.column(align=True)
        col.prop(ui_state, "inner_key")
        col.prop(ui_state, "inner_key_sel", text="Selected")

        col = flow.column(align=True)
        col.prop(ui_state, "inner_overridden")
        col.prop(ui_state, "inner_overridden_sel", text="Selected")

        col = flow.column(align=True)
        col.prop(ui_state, "inner_changed")
        col.prop(ui_state, "inner_changed_sel", text="Selected")

        col = flow.column(align=True)
        col.prop(ui_state, "blend")


class USERPREF_PT_theme_interface_styles(ThemePanel, CenterAlignMixIn, Panel):
    bl_label = "Styles"
    bl_options = {'DEFAULT_CLOSED'}
    bl_parent_id = "USERPREF_PT_theme_user_interface"

    def draw_centered(self, context, layout):
        theme = context.preferences.themes[0]
        ui = theme.user_interface

        flow = layout.grid_flow(row_major=False, columns=0, even_columns=True, even_rows=False, align=False)

        col = flow.column(align=True)
        col.prop(ui, "editor_border")
        col.prop(ui, "editor_outline")
        col.prop(ui, "editor_outline_active")

        col = flow.column()
        col.prop(ui, "widget_text_cursor")

        col = flow.column(align=True)
        col.prop(ui, "icon_alpha")
        col.prop(ui, "icon_saturation", text="Saturation")

        flow.separator()

        col = flow.column()
        col.prop(ui, "widget_emboss")

        col = flow.column(align=True)
        col.prop(ui, "menu_shadow_fac")
        col.prop(ui, "menu_shadow_width", text="Shadow Width")


class USERPREF_PT_theme_interface_transparent_checker(ThemePanel, CenterAlignMixIn, Panel):
    bl_label = "Transparent Checkerboard"
    bl_options = {'DEFAULT_CLOSED'}
    bl_parent_id = "USERPREF_PT_theme_user_interface"

    def draw_centered(self, context, layout):
        theme = context.preferences.themes[0]
        ui = theme.user_interface

        flow = layout.grid_flow(row_major=False, columns=0, even_columns=True, even_rows=False, align=False)

        col = flow.column(align=True)
        col.prop(ui, "transparent_checker_primary")
        col.prop(ui, "transparent_checker_secondary")

        col = flow.column()
        col.prop(ui, "transparent_checker_size")


class USERPREF_PT_theme_interface_gizmos(ThemePanel, CenterAlignMixIn, Panel):
    bl_label = "Axis & Gizmo Colors"
    bl_options = {'DEFAULT_CLOSED'}
    bl_parent_id = "USERPREF_PT_theme_user_interface"

    def draw_centered(self, context, layout):
        theme = context.preferences.themes[0]
        ui = theme.user_interface

        flow = layout.grid_flow(row_major=False, columns=0, even_columns=True, even_rows=True, align=False)

        col = flow.column(align=True)
        col.prop(ui, "axis_x", text="Axis X")
        col.prop(ui, "axis_y", text="Y")
        col.prop(ui, "axis_z", text="Z")
        col.prop(ui, "axis_w", text="W")

        col = flow.column()
        col.prop(ui, "gizmo_primary")
        col.prop(ui, "gizmo_secondary", text="Secondary")
        col.prop(ui, "gizmo_view_align", text="View Align")

        col = flow.column()
        col.prop(ui, "gizmo_a")
        col.prop(ui, "gizmo_b", text="B")


class USERPREF_PT_theme_interface_icons(ThemePanel, CenterAlignMixIn, Panel):
    bl_label = "Icon Colors"
    bl_options = {'DEFAULT_CLOSED'}
    bl_parent_id = "USERPREF_PT_theme_user_interface"

    def draw_centered(self, context, layout):
        theme = context.preferences.themes[0]
        ui = theme.user_interface

        flow = layout.grid_flow(row_major=False, columns=0, even_columns=True, even_rows=False, align=False)

        flow.prop(ui, "icon_scene")
        flow.prop(ui, "icon_collection")
        flow.prop(ui, "icon_object")
        flow.prop(ui, "icon_object_data")
        flow.prop(ui, "icon_modifier")
        flow.prop(ui, "icon_shading")
        flow.prop(ui, "icon_folder")
        flow.prop(ui, "icon_autokey")
        flow.prop(ui, "icon_border_intensity")


class USERPREF_PT_theme_text_style(ThemePanel, CenterAlignMixIn, Panel):
    bl_label = "Text Style"
    bl_options = {'DEFAULT_CLOSED'}

    @staticmethod
    def _ui_font_style(layout, font_style):
        layout.use_property_split = True
        flow = layout.grid_flow(row_major=False, columns=0, even_columns=True, even_rows=False, align=False)

        col = flow.column()
        col.prop(font_style, "points")
        col.prop(font_style, "character_weight", text="Weight", text_ctxt=i18n_contexts.id_text)

        col = flow.column(align=True)
        col.prop(font_style, "shadow_offset_x", text="Shadow Offset X")
        col.prop(font_style, "shadow_offset_y", text="Y")

        col = flow.column(align=True)
        col.prop(font_style, "shadow")
        col.prop(font_style, "shadow_alpha", text="Alpha")
        col.prop(font_style, "shadow_value", text="Brightness")

    def draw_header(self, _context):
        layout = self.layout

        layout.label(icon='FONTPREVIEW')

    def draw_centered(self, context, layout):
        style = context.preferences.ui_styles[0]

        layout.label(text="Panel Title")
        self._ui_font_style(layout, style.panel_title)

        layout.separator()

        layout.label(text="Widget")
        self._ui_font_style(layout, style.widget)

        layout.separator()

        layout.label(text="Tooltip")
        self._ui_font_style(layout, style.tooltip)


class USERPREF_PT_theme_bone_color_sets(ThemePanel, CenterAlignMixIn, Panel):
    bl_label = "Bone Color Sets"
    bl_options = {'DEFAULT_CLOSED'}

    def draw_header(self, _context):
        layout = self.layout

        layout.label(icon='COLOR')

    def draw_centered(self, context, layout):
        theme = context.preferences.themes[0]

        layout.use_property_split = True

        for i, ui in enumerate(theme.bone_color_sets, 1):
            layout.label(text=iface_("Color Set {:d}").format(i), translate=False)

            flow = layout.grid_flow(row_major=False, columns=0, even_columns=True, even_rows=False, align=True)

            flow.prop(ui, "normal")
            flow.prop(ui, "select", text="Selected")
            flow.prop(ui, "active")
            flow.prop(ui, "show_colored_constraints")


class USERPREF_PT_theme_collection_colors(ThemePanel, CenterAlignMixIn, Panel):
    bl_label = "Collection Colors"
    bl_options = {'DEFAULT_CLOSED'}

    def draw_header(self, _context):
        layout = self.layout

        layout.label(icon='GROUP')

    def draw_centered(self, context, layout):
        theme = context.preferences.themes[0]

        layout.use_property_split = True

        flow = layout.grid_flow(row_major=False, columns=2, even_columns=True, even_rows=False, align=False)
        for i, ui in enumerate(theme.collection_color, 1):
            flow.prop(ui, "color", text=iface_("Color {:d}").format(i), translate=False)


class USERPREF_PT_theme_strip_colors(ThemePanel, CenterAlignMixIn, Panel):
    bl_label = "Strip Color Tags"
    bl_options = {'DEFAULT_CLOSED'}

    def draw_header(self, _context):
        layout = self.layout

        layout.label(icon='SEQ_STRIP_DUPLICATE')

    def draw_centered(self, context, layout):
        theme = context.preferences.themes[0]

        layout.use_property_split = True

        flow = layout.grid_flow(row_major=False, columns=2, even_columns=True, even_rows=False, align=False)
        for i, ui in enumerate(theme.strip_color, 1):
            flow.prop(ui, "color", text=iface_("Color {:d}").format(i), translate=False)


class USERPREF_PT_theme_glyph_colors(ThemePanel, CenterAlignMixIn, Panel):
    bl_label = "Glyph Colors"
    bl_options = {'DEFAULT_CLOSED'}

    def draw_header(self, _context):
        layout = self.layout

        layout.label(icon='COLOR')

    def draw_centered(self, context, layout):
        theme = context.preferences.themes[0]

        layout.use_property_split = True

        flow = layout.grid_flow(row_major=False, columns=2, even_columns=True, even_rows=False, align=False)
        for i, ui in enumerate(theme.glyph_color, 1):
            flow.prop(ui, "color", text=iface_("Color {:d}").format(i), translate=False)


# Base class for dynamically defined theme-space panels.
# This is not registered.
class PreferenceThemeSpacePanel:
    @staticmethod
    def _theme_generic(layout, themedata):

        layout.use_property_split = True

        flow = layout.grid_flow(row_major=False, columns=0, even_columns=True, even_rows=False, align=False)

        props_type = {}

        for prop in themedata.rna_type.properties:
            if prop.identifier == "rna_type":
                continue

            props_type.setdefault((prop.type, prop.subtype), []).append(prop)

        for props_type, props_ls in sorted(props_type.items()):
            if props_type[0] == 'POINTER':
                continue

            for prop in props_ls:
                flow.prop(themedata, prop.identifier)

    def draw_header(self, _context):
        icon = getattr(self, "icon", 'NONE')
        if icon != 'NONE':
            layout = self.layout
            layout.label(icon=icon)

    def draw(self, context):
        layout = self.layout
        theme = context.preferences.themes[0]

        datapath_list = self.datapath.split(".")
        data = theme
        for datapath_item in datapath_list:
            data = getattr(data, datapath_item)
        PreferenceThemeSpacePanel._theme_generic(layout, data)


class ThemeGenericClassGenerator:

    @staticmethod
    def generate_panel_classes_for_wcols():
        wcols = [
            ("Box", "wcol_box"),
            ("Curve", "wcol_curve"),
            ("List Item", "wcol_list_item"),
            ("Menu", "wcol_menu"),
            ("Menu Background", "wcol_menu_back"),
            ("Menu Item", "wcol_menu_item"),
            ("Number Field", "wcol_num"),
            ("Option", "wcol_option"),
            ("Pie Menu", "wcol_pie_menu"),
            ("Progress Bar", "wcol_progress"),
            ("Pulldown", "wcol_pulldown"),
            ("Radio Buttons", "wcol_radio"),
            ("Regular", "wcol_regular"),
            ("Scroll Bar", "wcol_scroll"),
            ("Tab", "wcol_tab"),
            ("Text", "wcol_text"),
            ("Toggle", "wcol_toggle"),
            ("Tool", "wcol_tool"),
            ("Toolbar Item", "wcol_toolbar_item"),
            ("Tooltip", "wcol_tooltip"),
            ("Value Slider", "wcol_numslider"),
        ]

        for (name, wcol) in wcols:
            panel_id = "USERPREF_PT_theme_interface_" + wcol
            yield type(panel_id, (PreferenceThemeWidgetColorPanel, ThemePanel, Panel), {
                "bl_label": name,
                "bl_options": {'DEFAULT_CLOSED'},
                "draw": PreferenceThemeWidgetColorPanel.draw,
                "wcol": wcol,
            })

            panel_shade_id = "USERPREF_PT_theme_interface_shade_" + wcol
            yield type(panel_shade_id, (PreferenceThemeWidgetShadePanel, ThemePanel, Panel), {
                "bl_label": "Shaded",
                "bl_options": {'DEFAULT_CLOSED'},
                "bl_parent_id": panel_id,
                "draw": PreferenceThemeWidgetShadePanel.draw,
                "wcol": wcol,
            })

    @staticmethod
    def generate_theme_area_child_panel_classes(parent_id, rna_type, theme_area, datapath):
        def generate_child_panel_classes_recurse(parent_id, rna_type, theme_area, datapath):
            props_type = {}

            for prop in rna_type.properties:
                if prop.identifier == "rna_type":
                    continue

                props_type.setdefault((prop.type, prop.subtype), []).append(prop)

            for props_type, props_ls in sorted(props_type.items()):
                if props_type[0] == 'POINTER':
                    for prop in props_ls:
                        new_datapath = datapath + "." + prop.identifier if datapath else prop.identifier
                        panel_id = parent_id + "_" + prop.identifier
                        yield type(panel_id, (PreferenceThemeSpacePanel, ThemePanel, Panel), {
                            "bl_label": rna_type.properties[prop.identifier].name,
                            "bl_parent_id": parent_id,
                            "bl_options": {'DEFAULT_CLOSED'},
                            "draw": PreferenceThemeSpacePanel.draw,
                            "theme_area": theme_area.identifier,
                            "datapath": new_datapath,
                        })

                        yield from generate_child_panel_classes_recurse(
                            panel_id,
                            prop.fixed_type,
                            theme_area,
                            new_datapath,
                        )

        yield from generate_child_panel_classes_recurse(parent_id, rna_type, theme_area, datapath)

    @staticmethod
    def generate_panel_classes_from_theme_areas():
        from bpy.types import Theme

        for theme_area in Theme.bl_rna.properties["theme_area"].enum_items_static:
            if theme_area.identifier in {'USER_INTERFACE', 'STYLE', 'BONE_COLOR_SETS'}:
                continue

            panel_id = "USERPREF_PT_theme_" + theme_area.identifier.lower()
            # Generate panel-class from theme_area
            yield type(panel_id, (PreferenceThemeSpacePanel, ThemePanel, Panel), {
                "bl_label": theme_area.name,
                "bl_options": {'DEFAULT_CLOSED'},
                "draw_header": PreferenceThemeSpacePanel.draw_header,
                "draw": PreferenceThemeSpacePanel.draw,
                "theme_area": theme_area.identifier,
                "icon": theme_area.icon,
                "datapath": theme_area.identifier.lower(),
            })

            yield from ThemeGenericClassGenerator.generate_theme_area_child_panel_classes(
                panel_id, Theme.bl_rna.properties[theme_area.identifier.lower()].fixed_type,
                theme_area, theme_area.identifier.lower())


# -----------------------------------------------------------------------------
# File Paths Panels

# Panel mix-in.
class FilePathsPanel:
    bl_space_type = 'PREFERENCES'
    bl_region_type = 'WINDOW'
    bl_context = "file_paths"


class USERPREF_PT_file_paths_data(FilePathsPanel, Panel):
    bl_label = "Data"

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        paths = context.preferences.filepaths

        col = self.layout.column()
        col.prop(paths, "font_directory", text="Fonts")
        col.prop(paths, "texture_directory", text="Textures")
        col.prop(paths, "sound_directory", text="Sounds")
        col.prop(paths, "temporary_directory", text="Temporary Files")


class USERPREF_PT_file_paths_script_directories(FilePathsPanel, Panel):
    bl_label = "Script Directories"

    def draw(self, context):
        layout = self.layout

        paths = context.preferences.filepaths

        if len(paths.script_directories) == 0:
            layout.operator("preferences.script_directory_add", text="Add", icon='ADD')
            return

        layout.use_property_split = False
        layout.use_property_decorate = False

        box = layout.box()
        split = box.split(factor=0.35)
        name_col = split.column()
        path_col = split.column()

        row = name_col.row(align=True)  # Padding
        row.separator()
        row.label(text="Name")

        row = path_col.row(align=True)  # Padding
        row.separator()
        row.label(text="Path", text_ctxt=i18n_contexts.editor_filebrowser)

        row.operator("preferences.script_directory_add", text="", icon='ADD', emboss=False)

        for i, script_directory in enumerate(paths.script_directories):
            row = name_col.row()
            row.alert = not script_directory.name
            row.prop(script_directory, "name", text="")

            row = path_col.row()
            subrow = row.row()
            subrow.alert = not script_directory.directory
            subrow.prop(script_directory, "directory", text="")
            row.operator("preferences.script_directory_remove", text="", icon='X', emboss=False).index = i


class USERPREF_PT_file_paths_render(FilePathsPanel, Panel):
    bl_label = "Render"
    bl_parent_id = "USERPREF_PT_file_paths_data"

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        paths = context.preferences.filepaths

        col = self.layout.column()
        col.prop(paths, "texture_cache_directory", text="Texture Cache")
        col.prop(paths, "render_output_directory", text="Render Output")
        col.prop(paths, "render_cache_directory", text="Render Cache")


class USERPREF_PT_text_editor_presets(PresetPanel, Panel):
    bl_label = "Text Editor Presets"
    preset_subdir = "text_editor"
    preset_operator = "script.execute_preset"
    preset_add_operator = "text_editor.preset_add"


class USERPREF_PT_file_paths_applications(FilePathsPanel, Panel):
    bl_label = "Applications"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        paths = context.preferences.filepaths

        col = layout.column()
        col.prop(paths, "image_editor", text="Image Editor")
        col.prop(paths, "animation_player_preset", text="Animation Player")
        if paths.animation_player_preset == 'CUSTOM':
            col.prop(paths, "animation_player", text="Player")


class USERPREF_PT_text_editor(FilePathsPanel, Panel):
    bl_label = "Text Editor"
    bl_parent_id = "USERPREF_PT_file_paths_applications"

    def draw_header_preset(self, _context):
        USERPREF_PT_text_editor_presets.draw_panel_header(self.layout)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        paths = context.preferences.filepaths

        col = layout.column()
        col.prop(paths, "text_editor", text="Program")
        col.prop(paths, "text_editor_args", text="Arguments")


class USERPREF_PT_file_paths_development(FilePathsPanel, Panel):
    bl_label = "Development"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        prefs = context.preferences
        return prefs.view.show_developer_ui

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        paths = context.preferences.filepaths
        layout.prop(paths, "i18n_branches_directory", text="I18n Branches")


class USERPREF_PT_saveload_autorun(FilePathsPanel, Panel):
    bl_label = "Auto Run Python Scripts"
    bl_parent_id = "USERPREF_PT_saveload_blend"

    def draw_header(self, context):
        prefs = context.preferences
        paths = prefs.filepaths

        self.layout.prop(paths, "use_scripts_auto_execute", text="")

    def draw(self, context):
        layout = self.layout
        prefs = context.preferences
        paths = prefs.filepaths

        layout.use_property_split = True
        layout.use_property_decorate = False  # No animation.

        layout.active = paths.use_scripts_auto_execute

        box = layout.box()
        row = box.row()
        row.label(text="Excluded Paths")
        row.operator("preferences.autoexec_path_add", text="", icon='ADD', emboss=False)
        for i, path_cmp in enumerate(prefs.autoexec_paths):
            row = box.row()
            row.prop(path_cmp, "path", text="")
            row.prop(path_cmp, "use_glob", text="", icon='FILTER')
            row.operator("preferences.autoexec_path_remove", text="", icon='X', emboss=False).index = i


class USERPREF_UL_extension_repos(UIList):
    def draw_item(self, _context, layout, _data, item, icon, _active_data, _active_propname, _index):
        repo = item
        icon = 'INTERNET' if repo.use_remote_url else 'DISK_DRIVE'
        layout.prop(repo, "name", text="", icon=icon, emboss=False)

        # Show an error icon if this repository has unusable settings.
        if repo.enabled:
            if (
                    (repo.use_custom_directory and repo.custom_directory == "") or
                    (repo.use_remote_url and repo.remote_url == "")
            ):
                layout.label(text="", icon='ERROR')

        layout.prop(repo, "enabled", text="", emboss=False, icon='CHECKBOX_HLT' if repo.enabled else 'CHECKBOX_DEHLT')

    def filter_items(self, _context, data, propname):
        # Repositories has no index, converting to a list.
        items = list(getattr(data, propname))

        flags = [self.bitflag_filter_item] * len(items)

        indices = [None] * len(items)
        for index, orig_index in enumerate(sorted(
            range(len(items)),
            key=lambda i: (
                # Order [Remote, User, System].
                0 if (repo := items[i]).use_remote_url else (1 if (repo.source != 'SYSTEM') else 2),
                repo.name.casefold(),
            )
        )):
            indices[orig_index] = index

        return flags, indices


# -----------------------------------------------------------------------------
# Save/Load Panels

class SaveLoadPanel:
    bl_space_type = 'PREFERENCES'
    bl_region_type = 'WINDOW'
    bl_context = "save_load"


class USERPREF_PT_saveload_blend(SaveLoadPanel, CenterAlignMixIn, Panel):
    bl_label = "Blend Files"

    def draw_centered(self, context, layout):
        prefs = context.preferences
        paths = prefs.filepaths
        view = prefs.view

        col = layout.column(heading="Save")
        col.prop(view, "use_save_prompt")

        layout.prop(paths, "save_modified_images")

        col = layout.column()
        col.prop(paths, "save_version")
        col.prop(paths, "recent_files")

        layout.separator()

        col = layout.column(heading="Auto-Save")
        row = col.row()
        row.prop(paths, "use_auto_save_temporary_files", text="")
        subrow = row.row()
        subrow.active = paths.use_auto_save_temporary_files
        subrow.prop(paths, "auto_save_time", text="Timer (Minutes)")

        layout.separator()

        layout.prop(paths, "file_preview_type")

        layout.separator()

        layout.separator()

        col = layout.column(heading="Default To")
        col.prop(paths, "use_relative_paths")
        col.prop(paths, "use_file_compression")
        col.prop(paths, "use_load_ui")

        col = layout.column(heading="Text Files")
        col.prop(paths, "use_tabs_as_spaces")


class USERPREF_PT_saveload_file_browser(SaveLoadPanel, CenterAlignMixIn, Panel):
    bl_label = "File Browser"

    def draw_centered(self, context, layout):
        prefs = context.preferences
        paths = prefs.filepaths

        col = layout.column(heading="Show Locations")
        col.prop(paths, "show_recent_locations", text="Recent")
        col.prop(paths, "show_system_bookmarks", text="System")

        col = layout.column(heading="Defaults")
        col.prop(paths, "use_filter_files")
        col.prop(paths, "show_hidden_files_datablocks")


# -----------------------------------------------------------------------------
# Input Panels

class InputPanel:
    bl_space_type = 'PREFERENCES'
    bl_region_type = 'WINDOW'
    bl_context = "input"


class USERPREF_PT_input_keyboard(InputPanel, CenterAlignMixIn, Panel):
    bl_label = "Keyboard"

    def draw_centered(self, context, layout):
        prefs = context.preferences
        inputs = prefs.inputs

        layout.prop(inputs, "use_emulate_numpad")
        layout.prop(inputs, "use_numeric_input_advanced")


class USERPREF_PT_input_mouse(InputPanel, CenterAlignMixIn, Panel):
    bl_label = "Mouse"

    def draw_centered(self, context, layout):
        import sys
        prefs = context.preferences
        inputs = prefs.inputs

        flow = layout.grid_flow(row_major=False, columns=0, even_columns=True, even_rows=False, align=False)

        if sys.platform[:3] == "win":
            flow.prop(inputs, "use_mouse_emulate_3_button")
        else:
            col = flow.column(heading="Emulate 3 Button Mouse")
            row = col.row()
            row.prop(inputs, "use_mouse_emulate_3_button", text="")
            subrow = row.row()
            subrow.prop(inputs, "mouse_emulate_3_button_modifier", text="")
            subrow.active = inputs.use_mouse_emulate_3_button

        flow.prop(inputs, "use_mouse_continuous")
        flow.prop(inputs, "use_drag_immediately")
        flow.prop(inputs, "mouse_double_click_time", text="Double Click Speed")
        flow.prop(inputs, "drag_threshold_mouse")
        flow.prop(inputs, "drag_threshold_tablet")
        flow.prop(inputs, "drag_threshold")
        flow.prop(inputs, "move_threshold")


class USERPREF_PT_input_touchpad(InputPanel, CenterAlignMixIn, Panel):
    bl_label = "Touchpad"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        import sys
        if sys.platform[:3] == "win" or sys.platform == "darwin":
            return True

        # WAYLAND supports multi-touch, X11 and SDL don't.
        from _bpy import _ghost_backend
        if _ghost_backend() == 'WAYLAND':
            return True

        return False

    def draw_centered(self, context, layout):
        prefs = context.preferences
        inputs = prefs.inputs

        col = layout.column()
        col.prop(inputs, "use_multitouch_gestures")

        from _bpy import _wm_capabilities
        capabilities = _wm_capabilities()
        if not capabilities['TRACKPAD_PHYSICAL_DIRECTION']:
            row = col.row()
            row.active = inputs.use_multitouch_gestures
            row.prop(inputs, "touchpad_scroll_direction", text="Scroll Direction")


class USERPREF_PT_input_tablet(InputPanel, CenterAlignMixIn, Panel):
    bl_label = "Tablet"

    def draw_centered(self, context, layout):
        prefs = context.preferences
        inputs = prefs.inputs

        import sys
        if sys.platform[:3] == "win":
            layout.prop(inputs, "tablet_api")
            layout.separator()

        col = layout.column()
        col.prop(inputs, "pressure_threshold_max")
        col.prop(inputs, "pressure_softness")
        use_debug = prefs.experimental.use_paint_debug and prefs.view.show_developer_ui

        if use_debug:
            col.prop(inputs, "show_tablet_debug_values")


class USERPREF_PT_input_ndof(InputPanel, CenterAlignMixIn, Panel):
    bl_label = "NDOF"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        return bpy.app.build_options.input_ndof

    def draw_centered(self, context, layout):
        prefs = context.preferences
        inputs = prefs.inputs

        USERPREF_PT_ndof_settings.draw_settings(layout, inputs)


# -----------------------------------------------------------------------------
# Navigation Panels

class NavigationPanel:
    bl_space_type = 'PREFERENCES'
    bl_region_type = 'WINDOW'
    bl_context = "navigation"


class USERPREF_PT_navigation_orbit(NavigationPanel, CenterAlignMixIn, Panel):
    bl_label = "Orbit & Pan"

    def draw_centered(self, context, layout):
        prefs = context.preferences
        inputs = prefs.inputs
        view = prefs.view

        col = layout.column()

        col.row().prop(inputs, "view_rotate_method", expand=True)
        if inputs.view_rotate_method == 'TURNTABLE':
            col.prop(inputs, "view_rotate_sensitivity_turntable")
        else:
            col.prop(inputs, "view_rotate_sensitivity_trackball")
        col.prop(inputs, "use_rotate_around_active")

        col.separator()

        col = layout.column(heading="Auto")
        col.prop(inputs, "use_auto_perspective", text="Perspective")
        col.prop(inputs, "use_mouse_depth_navigate", text="Depth")

        col = layout.column()
        col.prop(view, "smooth_view")
        col.prop(view, "rotation_angle")


class USERPREF_PT_navigation_zoom(NavigationPanel, CenterAlignMixIn, Panel):
    bl_label = "Zoom"

    def draw_centered(self, context, layout):
        prefs = context.preferences
        inputs = prefs.inputs

        col = layout.column()

        col.row().prop(inputs, "view_zoom_method", text="Zoom Method")
        if inputs.view_zoom_method in {'DOLLY', 'CONTINUE'}:
            col.row().prop(inputs, "view_zoom_axis")
            col.prop(inputs, "use_zoom_to_mouse")
            col = layout.column(heading="Invert Zoom Direction", align=True)
            col.prop(inputs, "invert_mouse_zoom", text="Mouse")
            col.prop(inputs, "invert_zoom_wheel", text="Wheel")
        else:
            col.prop(inputs, "use_zoom_to_mouse")
            col.prop(inputs, "invert_zoom_wheel", text="Invert Wheel Zoom Direction")


class USERPREF_PT_navigation_fly_walk(NavigationPanel, CenterAlignMixIn, Panel):
    bl_label = "Fly & Walk"

    def draw_centered(self, context, layout):
        prefs = context.preferences
        inputs = prefs.inputs

        layout.row().prop(inputs, "navigation_mode", expand=True)


class USERPREF_PT_navigation_fly_walk_navigation(NavigationPanel, CenterAlignMixIn, Panel):
    bl_label = "Walk"
    bl_parent_id = "USERPREF_PT_navigation_fly_walk"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        prefs = context.preferences
        return prefs.inputs.navigation_mode == 'WALK'

    def draw_centered(self, context, layout):
        prefs = context.preferences
        inputs = prefs.inputs
        walk = inputs.walk_navigation

        col = layout.column()
        col.prop(walk, "use_mouse_reverse")
        col.prop(walk, "mouse_speed")
        col.prop(walk, "teleport_time")

        col = layout.column(align=True)
        col.prop(walk, "walk_speed")
        col.prop(walk, "walk_speed_factor")


class USERPREF_PT_navigation_fly_walk_gravity(NavigationPanel, CenterAlignMixIn, Panel):
    bl_label = "Gravity"
    bl_parent_id = "USERPREF_PT_navigation_fly_walk"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        prefs = context.preferences
        return prefs.inputs.navigation_mode == 'WALK'

    def draw_header(self, context):
        prefs = context.preferences
        inputs = prefs.inputs
        walk = inputs.walk_navigation

        self.layout.prop(walk, "use_gravity", text="")

    def draw_centered(self, context, layout):
        prefs = context.preferences
        inputs = prefs.inputs
        walk = inputs.walk_navigation

        layout.active = walk.use_gravity

        col = layout.column()
        col.prop(walk, "view_height")
        col.prop(walk, "jump_height")


# Special case, this is only exposed as a popover.
class USERPREF_PT_ndof_settings(Panel):
    bl_label = "3D Mouse Settings"
    bl_space_type = 'TOPBAR'  # dummy.
    bl_region_type = 'HEADER'
    bl_ui_units_x = 12

    @staticmethod
    def draw_settings(layout, props, show_3dview_settings=True):

        # Include this setting as it impacts 2D views as well (inverting translation).
        col = layout.column()
        col.row().prop(props, "ndof_navigation_mode", text="Navigation Mode")

        if show_3dview_settings:
            colsub = col.column()
            colsub.active = props.ndof_navigation_mode in {'FLY', 'OBJECT'}
            colsub.prop(props, "ndof_lock_horizon", text="Lock Horizon")
            del colsub
            colsub = col.column()
            colsub.active = props.ndof_navigation_mode in {'FLY', 'DRONE'}
            colsub.prop(props, "ndof_fly_speed_auto", text="Auto Fly Speed")
            del colsub
            layout.separator()

        if show_3dview_settings:
            col = layout.column(heading="Orbit Center")
            col.active = props.ndof_navigation_mode == 'OBJECT'
            col.prop(props, "ndof_orbit_center_auto")
            colsub = col.column()
            colsub.active = props.ndof_orbit_center_auto
            colsub.prop(props, "ndof_orbit_center_selected")
            del colsub
            col.separator()

            col = layout.column(heading="Show")
            col.prop(props, "ndof_show_guide_orbit_axis", text="Orbit Axis")
            colsub = col.column()
            colsub.active = props.ndof_navigation_mode == 'OBJECT'
            colsub.prop(props, "ndof_show_guide_orbit_center", text="Orbit Center")
            del colsub

        layout.separator()

        layout_header, layout_advanced = layout.panel("NDOF_advanced", default_closed=True)
        layout_header.label(text="Advanced")
        if layout_advanced:
            col = layout_advanced.column()
            col.prop(props, "ndof_translation_sensitivity")
            col.prop(props, "ndof_rotation_sensitivity")
            col.prop(props, "ndof_deadzone")

            col.separator()
            col.row().prop(props, "ndof_zoom_direction", expand=True)
            col.separator()

            row = col.row(heading=("Invert Pan" if show_3dview_settings else "Invert Pan Axis"))
            for text, attr in (
                    ("X", "ndof_panx_invert_axis"),
                    ("Y", "ndof_pany_invert_axis"),
                    ("Z", "ndof_panz_invert_axis"),
            ):
                row.prop(props, attr, text=text, toggle=True)

            if show_3dview_settings:
                row = col.row(heading="Invert Rotate")
                for text, attr in (
                        ("X", "ndof_rotx_invert_axis"),
                        ("Y", "ndof_roty_invert_axis"),
                        ("Z", "ndof_rotz_invert_axis"),
                ):
                    row.prop(props, attr, text=text, toggle=True)

            if show_3dview_settings:
                col.prop(props, "ndof_lock_camera_pan_zoom")

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False  # No animation.

        input_prefs = context.preferences.inputs
        is_view3d = context.space_data.type == 'VIEW_3D'
        self.draw_settings(layout, input_prefs, is_view3d)

# -----------------------------------------------------------------------------
# Key-Map Editor Panels


class KeymapPanel:
    bl_space_type = 'PREFERENCES'
    bl_region_type = 'WINDOW'
    bl_context = "keymap"


class USERPREF_MT_keyconfigs(Menu):
    bl_label = "KeyPresets"
    preset_subdir = "keyconfig"
    preset_operator = "preferences.keyconfig_activate"

    def draw(self, context):
        Menu.draw_preset(self, context)


class USERPREF_PT_keymap(KeymapPanel, Panel):
    bl_label = "Keymap"
    bl_options = {'HIDE_HEADER'}

    def draw(self, context):
        from rna_keymap_ui import draw_keymaps

        layout = self.layout

        # import time

        # start = time.time()

        # Keymap Settings
        draw_keymaps(context, layout)

        # print("runtime", time.time() - start)


# -----------------------------------------------------------------------------
# Extension Panels


class USERPREF_MT_extensions_active_repo(Menu):
    bl_label = "Active Repository"

    def draw(self, _context):
        # Add-ons may extend.
        pass


class USERPREF_MT_extensions_active_repo_remove(Menu):
    bl_label = "Remove Extension Repository"

    def draw(self, context):
        layout = self.layout

        extensions = context.preferences.extensions
        active_repo_index = extensions.active_repo

        try:
            active_repo = None if active_repo_index < 0 else extensions.repos[active_repo_index]
        except IndexError:
            active_repo = None

        is_system_repo = (active_repo.use_remote_url is False) and (active_repo.source == 'SYSTEM')

        props = layout.operator("preferences.extension_repo_remove", text="Remove Repository")
        props.index = active_repo_index

        if not is_system_repo:
            props = layout.operator("preferences.extension_repo_remove", text="Remove Repository & Files")
            props.index = active_repo_index
            props.remove_files = True


class USERPREF_PT_extensions_repos(Panel):
    bl_label = "Repositories"
    bl_options = {'HIDE_HEADER'}

    bl_space_type = 'TOPBAR'  # dummy.
    bl_region_type = 'HEADER'

    # Show wider than most panels so the URL & directory aren't overly clipped.
    bl_ui_units_x = 16

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = False
        layout.use_property_decorate = False

        extensions = context.preferences.extensions
        active_repo_index = extensions.active_repo

        row = layout.row()

        row.template_list(
            "USERPREF_UL_extension_repos", "user_extension_repos",
            extensions, "repos",
            extensions, "active_repo",
        )

        col = row.column(align=True)
        col.operator_menu_enum("preferences.extension_repo_add", "type", text="", icon='ADD')
        col.menu("USERPREF_MT_extensions_active_repo_remove", text="", icon='REMOVE')

        col.separator()

        col.menu_contents("USERPREF_MT_extensions_active_repo")

        try:
            active_repo = None if active_repo_index < 0 else extensions.repos[active_repo_index]
        except IndexError:
            active_repo = None

        if active_repo is None:
            return

        # NOTE: changing repositories from remote to local & vice versa could be supported but is obscure enough
        # that it can be hidden entirely. If there is a some justification to show this, it can be exposed.
        # For now it can be accessed from Python if someone is.
        # `layout.prop(active_repo, "use_remote_url", text="Use Remote URL")`

        use_remote_url = active_repo.use_remote_url
        if use_remote_url:
            row = layout.row()
            split = row.split(factor=0.936)
            if active_repo.remote_url == "":
                split.alert = True
            split.prop(active_repo, "remote_url", text="", icon='INTERNET', placeholder="Repository URL")
            split = row.split()

            if active_repo.use_access_token:
                access_token_icon = 'LOCKED' if active_repo.access_token else 'UNLOCKED'
                row = layout.row()
                split = row.split(factor=0.936)
                split.prop(active_repo, "access_token", icon=access_token_icon)
                split = row.split()

            layout.prop(active_repo, "use_sync_on_startup")

        layout_header, layout_panel = layout.panel("advanced", default_closed=True)
        layout_header.label(text="Advanced")

        if layout_panel:
            layout_panel.use_property_split = True
            use_custom_directory = active_repo.use_custom_directory

            col = layout_panel.column(align=False, heading="Custom Directory")
            row = col.row(align=True)
            sub = row.row(align=True)
            sub.prop(active_repo, "use_custom_directory", text="")
            sub = sub.row(align=True)
            sub.active = use_custom_directory
            if use_custom_directory:
                if active_repo.custom_directory == "":
                    sub.alert = True
                sub.prop(active_repo, "custom_directory", text="")
            else:
                # Show the read-only directory property.
                # Apart from being consistent with the custom directory UI,
                # prefer a read-only property over a label because this is not necessarily
                # valid UTF-8 which will raise a Python exception when passed in as text.
                sub.prop(active_repo, "directory", text="")

            if use_remote_url:
                row = layout_panel.row(align=True, heading="Authentication")
                row.prop(active_repo, "use_access_token")

                layout_panel.prop(active_repo, "use_cache")
            else:
                layout_panel.prop(active_repo, "source")

            layout_panel.separator()

            layout_panel.prop(active_repo, "module")


# -----------------------------------------------------------------------------
# Extensions Panels

class ExtensionsPanel:
    bl_space_type = 'PREFERENCES'
    bl_region_type = 'WINDOW'
    bl_context = "extensions"


class USERPREF_PT_extensions(ExtensionsPanel, Panel):
    bl_label = "Extensions"
    bl_options = {'HIDE_HEADER'}

    def draw(self, context):
        pass


# -----------------------------------------------------------------------------
# Add-on Panels

# Only a popover.
class USERPREF_PT_addons_filter(Panel):
    bl_label = "Add-ons Filter"

    bl_space_type = 'TOPBAR'  # dummy.
    bl_region_type = 'HEADER'
    bl_ui_units_x = 12

    def draw(self, context):
        USERPREF_PT_addons._draw_addon_header_for_extensions_popover(self.layout, context)


class AddOnPanel:
    bl_space_type = 'PREFERENCES'
    bl_region_type = 'WINDOW'
    bl_context = "addons"


class USERPREF_PT_addons(AddOnPanel, Panel):
    bl_label = "Add-ons"
    bl_options = {'HIDE_HEADER'}

    _support_icon_mapping = {
        'OFFICIAL': 'BLENDER',
        'COMMUNITY': 'COMMUNITY',
        'TESTING': 'EXPERIMENTAL',
    }

    @staticmethod
    def is_user_addon(mod, user_addon_paths):
        import os

        if not user_addon_paths:
            for path in (
                    bpy.utils.script_path_user(),
                    *bpy.utils.script_paths_pref(),
            ):
                if path is not None:
                    user_addon_paths.append(os.path.join(path, "addons"))

        for path in user_addon_paths:
            if bpy.path.is_subdir(mod.__file__, path):
                return True
        return False

    @staticmethod
    def draw_addon_preferences(layout, context, addon_preferences):
        if (draw := getattr(addon_preferences, "draw", None)) is None:
            return

        addon_preferences_class = type(addon_preferences)
        layout.label(text=" Preferences")
        box_prefs = layout.box()
        addon_preferences_class.layout = box_prefs
        try:
            draw(context)
        except Exception:
            import traceback
            traceback.print_exc()
            box_prefs.label(text="Error (see console)", icon='ERROR')
        del addon_preferences_class.layout

    @staticmethod
    def draw_error(layout, message):
        lines = message.split("\n")
        box = layout.box()
        sub = box.row()
        sub.label(text=lines[0])
        sub.label(icon='ERROR')
        for line in lines[1:]:
            box.label(text=line)

    @staticmethod
    def _draw_addon_header(layout, prefs, wm):
        split = layout.split(factor=0.6)

        row = split.row()
        row.prop(wm, "addon_support", expand=True)

        row = split.row(align=True)
        row.operator("preferences.addon_install", icon='IMPORT', text="Install...")
        row.operator("preferences.addon_refresh", icon='FILE_REFRESH', text="Refresh")

        row = layout.row()
        row.prop(prefs.view, "show_addons_enabled_only")
        row.prop(wm, "addon_filter", text="")
        row.prop(wm, "addon_search", text="", icon='VIEWZOOM')

    @staticmethod
    def _draw_addon_header_for_extensions_popover(layout, context):

        wm = context.window_manager
        prefs = context.preferences

        row = layout.row()
        row.prop(wm, "addon_support", expand=True)

        row = layout.row()
        row.prop(prefs.view, "show_addons_enabled_only")

        # Not filter, we could expose elsewhere.
        row = layout.row()
        row.operator("preferences.addon_install", icon='IMPORT', text="Install...")
        row.operator("preferences.addon_refresh", icon='FILE_REFRESH', text="Refresh")

    def draw(self, context):
        import os
        import addon_utils

        prefs = context.preferences

        if self.is_extended():
            # Rely on the draw function being extended by the extensions add-on (`bl_pkg`).
            return

        layout = self.layout
        wm = context.window_manager

        used_addon_module_name_map = {addon.module: addon for addon in prefs.addons}

        addon_user_dirs = tuple(
            p for p in (
                *[os.path.join(pref_p, "addons") for pref_p in bpy.utils.script_paths_pref()],
                bpy.utils.user_resource('SCRIPTS', path="addons"),
            )
            if p
        )

        self._draw_addon_header(layout, prefs, wm)

        layout_topmost = layout.column()

        col = layout.column()

        # set in addon_utils.modules_refresh()
        if addon_utils.error_duplicates:
            box = col.box()
            row = box.row()
            row.label(text="Multiple add-ons with the same name found!")
            row.label(icon='ERROR')
            box.label(text="Delete one of each pair to resolve:")
            for (addon_name, addon_file, addon_path) in addon_utils.error_duplicates:
                box.separator()
                sub_col = box.column(align=True)
                sub_col.label(text=addon_name + ":")

                sub_row = sub_col.row()
                sub_row.label(text="    " + addon_file)
                sub_row.operator("wm.path_open", text="", icon='FILE_FOLDER').filepath = os.path.dirname(addon_file)

                sub_row = sub_col.row()
                sub_row.label(text="    " + addon_path)
                sub_row.operator("wm.path_open", text="", icon='FILE_FOLDER').filepath = os.path.dirname(addon_path)

        if addon_utils.error_encoding:
            self.draw_error(
                col,
                "One or more addons do not have UTF-8 encoding\n"
                "(see console for details)",
            )

        show_enabled_only = prefs.view.show_addons_enabled_only
        filter = wm.addon_filter
        search = wm.addon_search.casefold()
        support = wm.addon_support

        module_names = set()

        # initialized on demand
        user_addon_paths = []

        for mod in addon_utils.modules(refresh=False):
            module_names.add(addon_module_name := mod.__name__)
            bl_info = addon_utils.module_bl_info(mod)

            is_enabled = addon_module_name in used_addon_module_name_map

            if bl_info["support"] not in support:
                continue

            # check if addon should be visible with current filters
            is_visible = (
                (filter == "All") or
                (filter == bl_info["category"]) or
                (filter == "User" and (mod.__file__.startswith(addon_user_dirs)))
            )
            if show_enabled_only:
                is_visible = is_visible and is_enabled

            if not is_visible:
                continue

            if search and not (
                    (search in bl_info["name"].casefold() or
                     search in iface_(bl_info["name"]).casefold()) or
                    (bl_info["author"] and (search in bl_info["author"].casefold())) or
                    ((filter == "All") and (
                        search in bl_info["category"].casefold() or
                        search in iface_(bl_info["category"]).casefold()
                    ))
            ):
                continue

            # Addon UI Code
            col_box = col.column()
            box = col_box.box()
            colsub = box.column()
            row = colsub.row(align=True)

            row.operator(
                "preferences.addon_expand",
                icon='DISCLOSURE_TRI_DOWN' if bl_info["show_expanded"] else 'DISCLOSURE_TRI_RIGHT',
                emboss=False,
            ).module = addon_module_name

            row.operator(
                "preferences.addon_disable" if is_enabled else "preferences.addon_enable",
                icon='CHECKBOX_HLT' if is_enabled else 'CHECKBOX_DEHLT', text="",
                emboss=False,
            ).module = addon_module_name

            sub = row.row()
            sub.active = is_enabled
            sub.label(text="{:s}: {:s}".format(iface_(bl_info["category"]), iface_(bl_info["name"])))

            if bl_info["warning"]:
                sub.label(icon='ERROR')

            # icon showing support level.
            sub.label(icon=self._support_icon_mapping.get(bl_info["support"], 'QUESTION'))

            # Expanded UI (only if additional bl_info is available)
            if bl_info["show_expanded"]:
                if value := bl_info["description"]:
                    split = colsub.row().split(factor=0.15)
                    split.label(text="Description:")
                    split.label(text=iface_(value))
                if value := bl_info["location"]:
                    split = colsub.row().split(factor=0.15)
                    split.label(text="Location:")
                    split.label(text=iface_(value))
                if mod:
                    split = colsub.row().split(factor=0.15)
                    split.label(text="File:")
                    split.label(text=mod.__file__, translate=False)
                if value := bl_info["author"]:
                    split = colsub.row().split(factor=0.15)
                    split.label(text="Author:")
                    split.label(text=value, translate=False)
                if value := bl_info["version"]:
                    split = colsub.row().split(factor=0.15)
                    split.label(text="Version:")
                    split.label(text=".".join(str(x) for x in value), translate=False)
                if value := bl_info["warning"]:
                    split = colsub.row().split(factor=0.15)
                    split.label(text="Warning:")
                    split.label(text="  " + iface_(value), icon='ERROR')
                del value

                user_addon = USERPREF_PT_addons.is_user_addon(mod, user_addon_paths)
                if bl_info["doc_url"] or bl_info.get("tracker_url"):
                    split = colsub.row().split(factor=0.15)
                    split.label(text="Internet:")
                    sub = split.row()
                    if bl_info["doc_url"]:
                        sub.operator(
                            "wm.url_open", text="Documentation", icon='HELP',
                        ).url = bl_info["doc_url"]
                    # Only add "Report a Bug" button if tracker_url is set.
                    # None of the core add-ons are expected to have tracker info (glTF is the exception).
                    if bl_info.get("tracker_url"):
                        sub.operator(
                            "wm.url_open", text="Report a Bug", icon='URL',
                        ).url = bl_info["tracker_url"]

                if user_addon:
                    split = colsub.row().split(factor=0.15)
                    split.label(text="User:")
                    split.operator(
                        "preferences.addon_remove", text="Remove", icon='CANCEL',
                    ).module = mod.__name__

                # Show addon user preferences
                if is_enabled:
                    if (addon_preferences := used_addon_module_name_map[addon_module_name].preferences) is not None:
                        self.draw_addon_preferences(col_box, context, addon_preferences)

        if filter in {"All", "Enabled"}:
            # Append missing scripts
            # First collect scripts that are used but have no script file.
            missing_modules = {
                addon_module_name for addon_module_name in used_addon_module_name_map
                if addon_module_name not in module_names
            }

            if missing_modules:
                layout_topmost.column().separator()
                layout_topmost.column().label(text="Missing script files")

                for addon_module_name in sorted(missing_modules):
                    is_enabled = addon_module_name in used_addon_module_name_map
                    # Addon UI Code
                    box = layout_topmost.column().box()
                    colsub = box.column()
                    row = colsub.row(align=True)

                    row.label(text="", icon='ERROR')

                    if is_enabled:
                        row.operator(
                            "preferences.addon_disable", icon='CHECKBOX_HLT', text="", emboss=False,
                        ).module = addon_module_name

                    row.label(text=addon_module_name, translate=False)


# -----------------------------------------------------------------------------
# Asset Panels

class AssetsPanel:
    bl_space_type = 'PREFERENCES'
    bl_region_type = 'WINDOW'
    bl_context = "assets"


class USERPREF_PT_assets(AssetsPanel, Panel):
    bl_label = "Assets"
    bl_options = {'HIDE_HEADER'}

    def draw(self, context):
        prefs = context.preferences

        # Check if the "Welcome" panel should be displayed.

        if bpy.app.online_access or prefs.extensions.use_online_access_handled:
            # Either online access is allowed, or the warning has already been dismissed. No need to draw.
            return

        has_online_library = any(
            library.enabled and library.use_remote_url for library in prefs.filepaths.asset_libraries
        )
        if not has_online_library:
            # No online libraries, so no need to draw.
            return

        layout = self.layout
        layout_header, layout_panel = layout.panel("advanced", default_closed=False)
        layout_header.label(text="Internet Access Required", icon='INTERNET_OFFLINE')

        if layout_panel is None:
            return

        box = layout_panel.box()

        # Text wrapping isn't supported, manually wrap.
        for line in (
                rpt_("Internet access is required to browse and download online assets."),
                rpt_("You can adjust this later from \"System\" preferences."),
        ):
            box.label(text=line, translate=False)

        # TODO: Link to the manual?
        # row.operator(
        #     "wm.url_open",
        #     text="",
        #     icon='URL',
        #     emboss=False,
        # ).url = (
        #     "https://docs.blender.org/manual/"
        #     "{:s}/{:d}.{:d}/editors/preferences/extensions.html#installing-extensions"
        # ).format(
        #     bpy.utils.manual_language_code(),
        #     *bpy.app.version[:2],
        # )

        row = box.row()
        props = row.operator("wm.context_set_boolean", text="Continue Offline", icon='X')
        props.data_path = "preferences.extensions.use_online_access_handled"
        props.value = True

        # The only reason to prefer this over `screen.userpref_show`
        # is it will be disabled when `--offline-mode` is forced with a useful error for why.
        row.operator("extensions.userpref_allow_online", text="Allow Online Access", icon='CHECKMARK')


# The panel is not located in the file paths section anymore and should be renamed. The old name is only kept for
# compatibility (add-ons extend it). Planned for removal in 6.0, see #153901.
class USERPREF_PT_file_paths_asset_libraries(AssetsPanel, Panel):
    bl_label = "Asset Libraries"

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = False
        layout.use_property_decorate = False

        paths = context.preferences.filepaths
        active_library_index = paths.active_asset_library

        row = layout.row()

        row.template_list(
            "USERPREF_UL_asset_libraries", "user_asset_libraries",
            paths, "asset_libraries",
            paths, "active_asset_library",
        )

        col = row.column(align=True)
        if context.preferences.experimental.use_remote_asset_libraries:
            col.operator_menu_enum("preferences.asset_library_add", "type", text="", icon='ADD')
        else:
            col.operator("preferences.asset_library_add", text="", icon='ADD').type = 'LOCAL'
        props = col.operator("preferences.asset_library_remove", text="", icon='REMOVE')
        props.index = active_library_index

        try:
            active_library = None if active_library_index < 0 else paths.asset_libraries[active_library_index]
        except IndexError:
            active_library = None

        if active_library is None:
            return

        layout.separator()

        if active_library.use_remote_url:
            use_remote_libraries = context.preferences.experimental.use_remote_asset_libraries
            if use_remote_libraries:
                layout.prop(active_library, "remote_url")
        else:
            layout.prop(active_library, "path")
            layout.prop(active_library, "import_method", text="Import Method")
            layout.prop(active_library, "use_relative_path")


class USERPREF_UL_asset_libraries(UIList):
    def draw_item(self, context, layout, _data, item, _icon, _active_data, _active_propname, _index):
        del context
        asset_library = item

        icon = 'INTERNET' if asset_library.use_remote_url else 'DISK_DRIVE'
        row = layout.row(align=True)
        row.prop(asset_library, "name", text="", icon=icon, emboss=False)
        row.prop(asset_library, "enabled", text="", emboss=False,
                 icon='CHECKBOX_HLT' if asset_library.enabled else 'CHECKBOX_DEHLT')

    def filter_items(self, context, data, property):
        asset_libraries = getattr(data, property)

        # Determine the bitflags for remote & non-remote asset libraries.
        use_remote_libs = context.preferences.experimental.use_remote_asset_libraries
        flag_remote = self.bitflag_filter_item if use_remote_libs else self.bitflag_item_never_show
        flag_nonremote = self.bitflag_filter_item

        # Construct arrays of flags & indices.
        flags = [
            flag_remote if asset_library.use_remote_url else flag_nonremote
            for asset_library in asset_libraries]
        indices = list(range(len(asset_libraries)))

        return flags, indices


# -----------------------------------------------------------------------------
# Studio Light Panels


class StudioLightPanel:
    bl_space_type = 'PREFERENCES'
    bl_region_type = 'WINDOW'
    bl_context = "lights"


class StudioLightPanelMixin:

    def _get_lights(self, prefs):
        return [light for light in prefs.studio_lights if light.is_user_defined and light.type == self.sl_type]

    def draw(self, context):
        layout = self.layout
        prefs = context.preferences
        lights = self._get_lights(prefs)

        self.draw_light_list(layout, lights)

    def draw_light_list(self, layout, lights):
        if lights:
            flow = layout.grid_flow(row_major=False, columns=4, even_columns=True, even_rows=True, align=False)
            for studio_light in lights:
                self.draw_studio_light(flow, studio_light)
        else:
            layout.label(text=self.get_error_message())

    def get_error_message(self):
        return rpt_("No custom {:s} configured").format(self.bl_label)

    def draw_studio_light(self, layout, studio_light):
        box = layout.box()
        row = box.row()

        row.template_icon(layout.icon(studio_light), scale=3.0)
        col = row.column()
        props = col.operator("preferences.studiolight_uninstall", text="", icon='REMOVE')
        props.index = studio_light.index

        if studio_light.type == 'STUDIO':
            props = col.operator("preferences.studiolight_copy_settings", text="", icon='IMPORT')
            props.index = studio_light.index

        box.label(text=studio_light.name)


class USERPREF_PT_studiolight_matcaps(StudioLightPanel, StudioLightPanelMixin, Panel):
    bl_label = "MatCaps"
    sl_type = 'MATCAP'

    def draw_header_preset(self, _context):
        layout = self.layout
        layout.operator("preferences.studiolight_install", icon='IMPORT', text="Install...").type = 'MATCAP'
        layout.separator()

    def get_error_message(self):
        return rpt_("No custom MatCaps configured")


class USERPREF_PT_studiolight_world(StudioLightPanel, StudioLightPanelMixin, Panel):
    bl_label = "HDRIs"
    sl_type = 'WORLD'

    def draw_header_preset(self, _context):
        layout = self.layout
        layout.operator("preferences.studiolight_install", icon='IMPORT', text="Install...").type = 'WORLD'
        layout.separator()

    def get_error_message(self):
        return rpt_("No custom HDRIs configured")


class USERPREF_PT_studiolight_lights(StudioLightPanel, StudioLightPanelMixin, Panel):
    bl_label = "Studio Lights"
    sl_type = 'STUDIO'

    def draw_header_preset(self, _context):
        layout = self.layout
        props = layout.operator("preferences.studiolight_install", icon='IMPORT', text="Install...")
        props.type = 'STUDIO'
        props.filter_glob = ".sl"
        layout.separator()

    def get_error_message(self):
        return rpt_("No custom Studio Lights configured")


class USERPREF_PT_studiolight_light_editor(StudioLightPanel, Panel):
    bl_label = "Editor"
    bl_parent_id = "USERPREF_PT_studiolight_lights"
    bl_options = {'DEFAULT_CLOSED'}

    @staticmethod
    def opengl_light_buttons(layout, light):
        col = layout.column()
        box = col.box()
        box.active = light.use

        box.prop(light, "use", text="Use Light")
        box.prop(light, "diffuse_color", text="Diffuse")
        box.prop(light, "specular_color", text="Specular")
        box.prop(light, "smooth")
        box.prop(light, "direction")

        col.separator()

    def draw(self, context):
        layout = self.layout

        prefs = context.preferences
        system = prefs.system

        row = layout.row()
        row.prop(system, "use_studio_light_edit", toggle=True)
        row.operator("preferences.studiolight_new", text="Save as Studio light", icon='FILE_TICK')

        layout.separator()

        layout.use_property_split = True

        flow = layout.grid_flow(row_major=True, columns=2, even_rows=True, even_columns=True)
        flow.active = system.use_studio_light_edit

        for light in system.solid_lights:
            self.opengl_light_buttons(flow, light)

        layout.prop(system, "light_ambient")


# -----------------------------------------------------------------------------
# Experimental Panels

# Also used for "Developer Tools" which are stored in `preferences.experimental` too.
def _draw_experimental_items(layout, preferences, items, url_prefix="https://projects.blender.org/"):
    experimental = preferences.experimental

    layout.use_property_split = False
    layout.use_property_decorate = False

    for prop_keywords, reference in items:
        split = layout.split(factor=0.66)
        col = split.split()
        col.prop(experimental, **prop_keywords)

        if reference:
            if type(reference) is tuple:
                url_ext = reference[0]
                text = reference[1]
            else:
                url_ext = reference
                text = reference

            col = split.split()
            col.operator("wm.url_open", text=text, icon='URL').url = url_prefix + url_ext


class USERPREF_PT_developer_tools(Panel):
    bl_space_type = 'PREFERENCES'
    bl_region_type = 'WINDOW'
    bl_context = "developer_tools"
    bl_label = "Debug"

    @classmethod
    def poll(cls, context):
        return context.preferences.view.show_developer_ui

    def draw(self, context):
        _draw_experimental_items(
            self.layout,
            context.preferences,
            (
                ({"property": "use_undo_legacy"}, ("blender/blender/issues/60695", "#60695")),
                ({"property": "override_auto_resync"}, ("blender/blender/issues/83811", "#83811")),
                ({"property": "use_all_linked_data_direct"}, None),
                ({"property": "use_recompute_usercount_on_save_debug"}, None),
                ({"property": "use_cycles_debug"}, None),
                ({"property": "show_asset_debug_info"}, None),
                ({"property": "use_asset_indexing"}, None),
                ({"property": "use_viewport_debug"}, None),
                ({"property": "use_eevee_debug"}, None),
                ({"property": "use_paint_debug"}, None),
                ({"property": "use_extensions_debug"}, ("/blender/blender/issues/119521", "#119521")),
                ({"property": "write_legacy_blend_file_format"}, ("/blender/blender/issues/129309", "#129309")),
                ({"property": "no_data_block_packing"}, ("/blender/blender/issues/132167", "#132167")),
            ),
        )


class ExperimentalPanel:
    bl_space_type = 'PREFERENCES'
    bl_region_type = 'WINDOW'
    bl_context = "experimental"

    @classmethod
    def poll(cls, _context):
        return bpy.app.version_cycle == "alpha"


"""
# Example panel, leave it here so we always have a template to follow even
# after the features are gone from the experimental panel.

class USERPREF_PT_experimental_virtual_reality(ExperimentalPanel, Panel):
    bl_label = "Virtual Reality"

    def draw(self, context):
        _draw_experimental_items(
            self.layout,
            context.preferences,
            (
                ({"property": "use_virtual_reality_scene_inspection"}, ("blender/blender/issues/71347", "#71347")),
                ({"property": "use_virtual_reality_immersive_drawing"}, ("blender/blender/issues/71348", "#71348")),
            ),
        )
"""


class USERPREF_PT_experimental_new_features(ExperimentalPanel, Panel):
    bl_label = "New Features"

    def draw(self, context):
        _draw_experimental_items(
            self.layout,
            context.preferences,
            (
                ({"property": "use_extended_asset_browser"},
                 ("blender/blender/projects/10", "Pipeline, Assets & IO Project Page")),
                ({"property": "use_shader_node_previews"}, ("blender/blender/issues/110353", "#110353")),
                ({"property": "use_geometry_nodes_lists"}, ("blender/blender/issues/140918", "#140918")),
                ({"property": "use_geometry_bundle"}, ("blender/blender/issues/150574", "#150574")),
                ({"property": "use_remote_asset_libraries"}, ("blender/blender/issues/134495", "#134495")),
                ({"property": "use_collection_importer"}, ("blender/blender/issues/132171", "#132171")),
            ),
        )


class USERPREF_PT_experimental_prototypes(ExperimentalPanel, Panel):
    bl_label = "Prototypes"

    def draw(self, context):
        _draw_experimental_items(
            self.layout,
            context.preferences,
            (
                ({"property": "use_new_curves_tools"}, ("blender/blender/issues/68981", "#68981")),
                ({"property": "use_sculpt_texture_paint"}, ("blender/blender/issues/96225", "#96225")),
            ),
        )


# Keep this as tweaks can be useful to restore.
"""
class USERPREF_PT_experimental_tweaks(ExperimentalPanel, Panel):
    bl_label = "Tweaks"

    def draw(self, context):
        _draw_experimental_items(
            self.layout,
            context.preferences,
            (
                ({"property": "use_select_nearest_on_first_click"}, ("blender/blender/issues/96752", "#96752")),
            ),
        )

"""

# -----------------------------------------------------------------------------
# Category Tabs Settings Popup

class VIEW3D_OT_category_tabs_settings(Operator):
    """Adjust category tabs size"""
    bl_idname = "view3d.category_tabs_settings"
    bl_label = "Display Mode Settings"
    bl_description = "Adjust display mode settings for category tabs"
    bl_options = {'REGISTER', 'UNDO'}

    @staticmethod
    def _display_mode_owner(context):
        space = context.space_data
        if space and hasattr(space, "category_tabs_display_mode"):
            return space
        return context.preferences.view

    @classmethod
    def _display_mode_value(cls, context):
        owner = cls._display_mode_owner(context)
        return owner.category_tabs_display_mode

    @classmethod
    def _zoom_owner(cls, context):
        owner = cls._display_mode_owner(context)
        if hasattr(owner, "category_tabs_zoom_icon"):
            return owner
        return context.preferences.view

    def draw(self, context):
        layout = self.layout
        prefs = context.preferences
        view = prefs.view
        display_mode_owner = self._display_mode_owner(context)
        display_mode_value = self._display_mode_value(context)
        zoom_owner = self._zoom_owner(context)

        # Display mode buttons
        layout.label(text="Display Mode")
        row = layout.row(align=True)

        # Per-editor when available; fallback to global preference.
        row.prop_enum(display_mode_owner, "category_tabs_display_mode", "GLYPHS_ONLY", text="Icon")
        row.prop_enum(display_mode_owner, "category_tabs_display_mode", "GLYPHS_TEXT", text="Mixed")
        row.prop_enum(display_mode_owner, "category_tabs_display_mode", "TEXT_ONLY", text="Text")

        # Size slider - different property based on mode
        layout.separator()
        if display_mode_value == 'GLYPHS_ONLY':
            layout.prop(zoom_owner, "category_tabs_zoom_icon", text="Icon Size")
        elif display_mode_value == 'GLYPHS_TEXT':
            layout.prop(zoom_owner, "category_tabs_zoom_mixed", text="Mixed Size")
        else:  # TEXT_ONLY
            layout.prop(zoom_owner, "category_tabs_zoom_text", text="Text Size")

        # Show active tab name option - only in Icon mode
        if display_mode_value == 'GLYPHS_ONLY':
            layout.separator()
            layout.prop(view, "category_tabs_show_active_name", text="Show Active Tab Name")
            layout.prop(view, "category_tabs_show_drag_tooltips", text="Show Drag Tooltips")
            # Inactive tab behavior - only in Icon mode
            # Sticky Tab option requires Show Active Tab Name to be enabled
            layout.separator()
            layout.label(text="Inactive Tab Settings")
            row = layout.row(align=True)
            row.prop_enum(view, "category_tabs_inactive_behavior", "DEFAULT", text="Default")
            sticky_row = row.row(align=True)
            sticky_row.active = view.category_tabs_show_active_name
            sticky_row.prop_enum(view, "category_tabs_inactive_behavior", "STICKY", text="Sticky Tab")
            # Tab shape - only in Icon mode
            layout.separator()
            layout.label(text="Tab Shape")
            row = layout.row(align=True)
            row.prop_enum(view, "category_tabs_shape", "BOX", text="Box Shape")
            row.prop_enum(view, "category_tabs_shape", "CAPSULE", text="Capsule Shape")

        # --- BEGIN: MIXED_MODE_CONTENT_FLAGS (optional per-type visibility in Mixed mode) ---
        # To remove: delete this entire block
        if display_mode_value == 'GLYPHS_TEXT':
            layout.separator()
            layout.label(text="Content Display")
            row = layout.row(align=True)
            row.prop(view, "category_tabs_mixed_show_glyphs", text="Glyphs")
            row.prop(view, "category_tabs_mixed_show_first_letter", text="First Letter")
            row.prop(view, "category_tabs_mixed_show_icons", text="Icons")
        # --- END: MIXED_MODE_CONTENT_FLAGS ---

        # Show color indicator option - only in Text mode
        if display_mode_value == 'TEXT_ONLY':
            layout.separator()
            layout.prop(view, "category_tabs_text_mode_show_color_indicator", text="Show Color Indicator")

        # Show colored text option - only in Text mode
        if display_mode_value == 'TEXT_ONLY':
            layout.prop(view, "category_tabs_text_mode_show_colored_text", text="Show Colored Text")

        # Hide text for inactive reserved tabs in Mixed/Text modes
        if display_mode_value in {'GLYPHS_TEXT', 'TEXT_ONLY'}:
            layout.separator()
            layout.prop(view,
                        "category_tabs_hide_reserved_inactive_text",
                        text="Reserved Tabs: Icons Only")

        # Allow editing category data
        layout.separator()
        layout.prop(view, "category_tabs_allow_edit", text="Allow Edit Category Data")

    def execute(self, context):
        return {'FINISHED'}

    def invoke(self, context, event):
        # Ensure glyph mappings are registered when opening settings
        register_category_glyph_mappings()
        wm = context.window_manager
        return wm.invoke_popup(self, width=200)


# -----------------------------------------------------------------------------
# Category Tags UIList

class USERPREF_UL_category_tags(UIList):
    """UI List for displaying category tags with colored glyphs."""

    def draw_item(self, context, layout, _data, item, _icon, _active_data, _active_propname, _index):
        tag = item
        if self.layout_type in {'DEFAULT', 'COMPACT'}:
            # Use a split with a fixed factor to separate glyph and name
            # factor=0.15 gives enough space for the glyph even when resized
            split = layout.split(factor=0.15, align=True)
            
            # Left: Glyph (fixed relative width)
            col_glyph = split.column()
            if tag.glyph:
                glyph_char = _hex_to_glyph(tag.glyph)
                col_glyph.colored_label(
                    text=glyph_char,
                    icon='NONE',
                    color_r=tag.color[0],
                    color_g=tag.color[1],
                    color_b=tag.color[2]
                )
            else:
                col_glyph.label(text="", icon='DOT')
            
            # Right: Name (will be truncated if not enough space)
            col_name = split.column()
            col_name.label(text=tag.name, translate=False)
        elif self.layout_type == 'GRID':
            layout.alignment = 'CENTER'
            if tag.glyph:
                glyph_char = _hex_to_glyph(tag.glyph)
                layout.label(text=glyph_char, translate=False)
            else:
                layout.label(text="", icon='DOT')

    def filter_items(self, context, data, propname):
        """Filter tags by WindowManager.category_tag_filter_mode (0 = all tags)."""
        items = getattr(data, propname, None)
        if not items:
            return ([], [])

        wm = context.window_manager
        filter_mode_flag = _get_tag_filter_mode_flag_from_wm(wm)
        if filter_mode_flag == 0:
            return ([], [])

        flags = []
        hidden_flag = self.bitflag_filter_item
        for item in items:
            mode_flags = int(getattr(item, "mode_flags", 0))
            visible = (mode_flags == 0) or bool(mode_flags & filter_mode_flag)
            flags.append(hidden_flag if visible else 0)

        return (flags, [])


# -----------------------------------------------------------------------------
# Category Tags Panel

class TagsPanel:
    bl_space_type = 'PREFERENCES'
    bl_region_type = 'WINDOW'
    bl_context = "tags"


class USERPREF_OT_tag_mode_toggle(Operator):
    """Toggle a specific mode for the current tag."""
    bl_idname = "userpref.tag_mode_toggle"
    bl_label = "Toggle Mode"
    bl_options = {'REGISTER', 'INTERNAL'}

    mode: bpy.props.EnumProperty(
        items=[(mode_id, label, "") for _name, mode_id, _bit, label, _icon in _CATEGORY_TAG_MODES]
    )

    def execute(self, context):
        global _all_tags_cache

        wm = context.window_manager
        idx = wm.category_tags_active_index

        if not wm or not hasattr(wm, 'category_tags'):
            return {'CANCELLED'}

        if idx < 0 or idx >= len(wm.category_tags):
            return {'CANCELLED'}

        tag_name = wm.category_tags[idx].name
        if tag_name not in _all_tags_cache:
            return {'CANCELLED'}

        tag_data = _all_tags_cache[tag_name]
        if not isinstance(tag_data, dict):
            return {'CANCELLED'}

        mode_flags = tag_data.get("mode_flags", _CATEGORY_TAG_DEFAULT_MODE_FLAGS)
        bit = _CATEGORY_TAG_MODE_ID_TO_BIT.get(self.mode, 0)
        mode_flags ^= (1 << bit)  # Toggle bit

        tag_data["mode_flags"] = mode_flags

        # Sync to WM
        if idx < len(wm.category_tags):
            wm.category_tags[idx].mode_flags = mode_flags

        return {'FINISHED'}


class USERPREF_OT_tag_mode_select_all(Operator):
    """Select all modes for the current tag."""
    bl_idname = "userpref.tag_mode_select_all"
    bl_label = "Select All Modes"

    def execute(self, context):
        global _all_tags_cache

        wm = context.window_manager
        idx = wm.category_tags_active_index

        if not wm or not hasattr(wm, 'category_tags'):
            return {'CANCELLED'}

        if idx < 0 or idx >= len(wm.category_tags):
            return {'CANCELLED'}

        tag_name = wm.category_tags[idx].name
        if tag_name in _all_tags_cache and isinstance(_all_tags_cache[tag_name], dict):
            _all_tags_cache[tag_name]["mode_flags"] = _CATEGORY_TAG_ALL_MODE_FLAGS
            wm.category_tags[idx].mode_flags = _CATEGORY_TAG_ALL_MODE_FLAGS

        return {'FINISHED'}


class USERPREF_OT_tag_mode_select_none(Operator):
    """Deselect all modes for the current tag."""
    bl_idname = "userpref.tag_mode_select_none"
    bl_label = "Select None"

    def execute(self, context):
        global _all_tags_cache

        wm = context.window_manager
        idx = wm.category_tags_active_index

        if not wm or not hasattr(wm, 'category_tags'):
            return {'CANCELLED'}

        if idx < 0 or idx >= len(wm.category_tags):
            return {'CANCELLED'}

        tag_name = wm.category_tags[idx].name
        if tag_name in _all_tags_cache and isinstance(_all_tags_cache[tag_name], dict):
            _all_tags_cache[tag_name]["mode_flags"] = 0
            wm.category_tags[idx].mode_flags = 0

        return {'FINISHED'}


class USERPREF_PT_tag_mode_filter_popover(Panel):
    """Popover panel for selecting tag filter modes."""
    bl_label = "Filter Mode"
    bl_idname = "USERPREF_PT_tag_mode_filter_popover"
    bl_space_type = 'PREFERENCES'
    bl_region_type = 'HEADER'

    @classmethod
    def poll(cls, context):
        wm = context.window_manager
        if not wm or not hasattr(wm, 'category_tags'):
            return False
        idx = wm.category_tags_active_index
        return 0 <= idx < len(wm.category_tags)

    def draw(self, context):
        layout = self.layout
        wm = context.window_manager
        idx = wm.category_tags_active_index

        if not wm or not hasattr(wm, 'category_tags'):
            layout.label(text="No tags available")
            return

        if idx < 0 or idx >= len(wm.category_tags):
            layout.label(text="No tag selected")
            return

        tag_name = wm.category_tags[idx].name
        mode_flags = _get_mode_flags_for_tag(tag_name)

        col = layout.column(align=True)
        for _mode_name, mode_id, bit, label, mode_icon in _CATEGORY_TAG_MODES:
            is_active = bool(mode_flags & (1 << bit))
            check_icon = 'CHECKBOX_HLT' if is_active else 'CHECKBOX_DEHLT'
            
            row = col.row(align=True)
            # Left part: Checkbox icon
            op_check = row.operator("userpref.tag_mode_toggle", text="", icon=check_icon, emboss=False)
            op_check.mode = mode_id
            
            # Right part: Mode icon and label
            op_label = row.operator("userpref.tag_mode_toggle", text=label, icon=mode_icon, emboss=False)
            op_label.mode = mode_id

        # Quick buttons
        row = layout.row()
        row.operator("userpref.tag_mode_select_all", text="All")
        row.operator("userpref.tag_mode_select_none", text="None")


class USERPREF_PT_tags(TagsPanel, Panel):
    bl_label = "Category Tags"
    bl_options = {'HIDE_HEADER'}

    def draw(self, context):
        layout = self.layout
        wm = context.window_manager

        # Header with description
        row = layout.row()
        row.label(text="Manage tags for category tabs. Tags help organize and filter panels.", icon='TAG')

        layout.separator()

        # Main container box
        main_box = layout.box()

        # Two-column layout inside the box - split for proportional sizing
        # 30% for UI List (left), 70% for detail panel (right)
        split = main_box.split(factor=0.35)

        # === Left: Tag list with buttons ===
        left_container = split.row()
        left_col = left_container.column()

        mode_row = left_col.row(align=True)
        mode_row.prop(wm, "category_tag_filter_mode", text="")
        mode_row.separator()

        # template_list
        left_col.template_list(
            "USERPREF_UL_category_tags", "",
            wm, "category_tags",
            wm, "category_tags_active_index",
            rows=25, maxrows=64
        )

        # Buttons to the right of list
        col_btn = left_container.column(align=True)
        col_btn.operator("wm.category_tag_add", text="", icon='ADD')
        col_btn.operator("wm.category_tag_delete", text="", icon='REMOVE')
        col_btn.separator()
        col_btn.operator("wm.category_tag_move", text="", icon='TRIA_UP').direction = 'UP'
        col_btn.operator("wm.category_tag_move", text="", icon='TRIA_DOWN').direction = 'DOWN'

        # === Right: Detail panel ===
        col_right = split.column()

        # Get selected tag
        active_idx = wm.category_tags_active_index
        tags = wm.category_tags
        tag = tags[active_idx] if 0 <= active_idx < len(tags) else None

        if tag:
            # Preview section (first)
            preview_box = col_right.box()
            preview_box.label(text="Preview (as in tabs):")

            preview_row = preview_box.row()
            preview_row.alignment = 'LEFT'
            if tag.glyph:
                glyph_char = _hex_to_glyph(tag.glyph)
                preview_row.colored_label(
                    text=glyph_char,
                    icon='NONE',
                    color_r=tag.color[0],
                    color_g=tag.color[1],
                    color_b=tag.color[2]
                )
            preview_row.label(text=tag.name, translate=False)

            # Edit section (second)
            col_right.separator()
            col_right.label(text="Edit Tag", icon='GREASEPENCIL')

            box = col_right.box()
            box.use_property_split = True
            box.prop(tag, "name", text="Name")
            
            row = box.row(align=True)
            row.prop(tag, "glyph", text="Glyph")
            op = row.operator("wm.glyph_picker_grid", text=_hex_to_glyph("f02f"), icon='NONE')
            op.target_property = f"category_tags[{active_idx}].glyph"

            # Color presets with glyph buttons
            box.label(text="Color:")
            row = box.row()
            row.template_color_glyph_presets(tag, "color")

            # Filter Mode button
            row = box.row()
            row.label(text="Filter Mode:")
            
            # Show active mode icons at a glance
            m_flags = _get_mode_flags_for_tag(tag.name)
            m_icons = [(bit, icon) for _name, _mode_id, bit, _label, icon in _CATEGORY_TAG_MODES]
            
            icon_row = row.row(align=True)
            any_mode = False
            for bit, m_icon in m_icons:
                if m_flags & (1 << bit):
                    icon_row.label(text="", icon=m_icon)
                    any_mode = True
            
            if not any_mode:
                icon_row.label(text="", icon='RESTRICT_SELECT_ON')

            row.popover("USERPREF_PT_tag_mode_filter_popover", text="", icon='TRIA_DOWN')

            # Categories using this tag
            col_right.separator()
            col_right.label(text="Categories using this tag:", icon='FILE_PARENT')

            cats_box = col_right.box()
            categories = get_categories_for_tag(tag.name)

            if categories:
                # Use automatic columns (columns=0) but force each item to be compact
                cats_flow = cats_box.grid_flow(row_major=True, columns=0, even_columns=False, even_rows=False, align=False)

                for cat in categories:
                    # Create an aligned row inside the flow
                    item_row = cats_flow.row()
                    item_row.alignment = 'LEFT'

                    # Get all visual data for the category
                    glyph, color, display_name = get_category_glyph_data(cat)

                    # Create row layout with Tag button (returns row for adding more buttons)
                    tag_row = item_row.tag_button_pref_row(
                        tag_name=display_name,
                        glyph=glyph if glyph else "",
                        color=(color[0], color[1], color[2]) if glyph else (0.0, 0.0, 0.0),
                        width=0,  # Auto width
                        height=0,  # Auto height
                        no_background=True,
                        align=False,  # Align buttons together for seamless appearance
                        operator="",  # Optional operator for button click
                        context_menu_operator="",  # TODO: Temporarily disabled
                        operator_param_name="",  # TODO: Temporarily disabled
                        operator_param_value=""  # TODO: Temporarily disabled
                    )

                    # Add delete button (X) to the same row - seamless appearance with borders
                    op_x = tag_row.operator("wm.category_tag_remove_from_category", text="", icon='X')
                    op_x.category = cat
                    op_x.tag_name = tag.name
            else:
                cats_box.label(text="No categories using this tag", icon='INFO')

        else:
            # No tag selected or list is empty
            col_right.label(text="Select a tag to edit", icon='INFO')
            if len(tags) == 0:
                col_right.label(text="Click '+' to create a tag", icon='ADD')


# -----------------------------------------------------------------------------
# Class Registration

# Order of registration defines order in UI,
# so dynamically generated classes are "injected" in the intended order.
classes = (
    USERPREF_PT_theme_user_interface,
    *ThemeGenericClassGenerator.generate_panel_classes_for_wcols(),
    USERPREF_HT_header,
    USERPREF_PT_navigation_bar,
    USERPREF_PT_save_preferences,
    USERPREF_MT_editor_menus,
    USERPREF_MT_view,
    USERPREF_MT_save_load,
    USERPREF_OT_save_category_glyphs,
    USERPREF_OT_sync_category_glyphs,

    # Tag system classes
    CategoryTagItem,
    CategoryTagAssignment,
    USERPREF_OT_category_tag_create,
    USERPREF_OT_category_tag_add,
    USERPREF_OT_category_tag_edit,
    USERPREF_OT_category_tag_delete,
    USERPREF_OT_category_tag_move,
    USERPREF_OT_category_tag_toggle,
    USERPREF_OT_category_tag_filter_set,
    USERPREF_OT_category_tag_filter_set_mode,

    USERPREF_PT_interface_display,
    USERPREF_PT_interface_editors,
    USERPREF_PT_interface_temporary_windows,
    USERPREF_PT_interface_statusbar,
    USERPREF_PT_interface_translation,
    USERPREF_PT_interface_accessibility,
    USERPREF_PT_interface_text,
    USERPREF_PT_interface_menus,
    USERPREF_PT_interface_menus_mouse_over,
    USERPREF_PT_interface_menus_pie,

    USERPREF_PT_viewport_display,
    USERPREF_PT_viewport_quality,
    USERPREF_PT_viewport_textures,
    USERPREF_PT_viewport_subdivision,

    USERPREF_PT_edit_objects,
    USERPREF_PT_edit_objects_new,
    USERPREF_PT_edit_objects_duplicate_data,
    USERPREF_PT_edit_cursor,
    USERPREF_PT_edit_annotations,
    USERPREF_PT_edit_weight_paint,
    USERPREF_PT_edit_gpencil,
    USERPREF_PT_edit_text_editor,
    USERPREF_PT_edit_node_editor,
    USERPREF_PT_edit_sequence_editor,
    USERPREF_PT_edit_misc,

    USERPREF_PT_animation_timeline,
    USERPREF_PT_animation_keyframes,
    USERPREF_PT_animation_fcurves,

    USERPREF_PT_system_cycles_devices,
    USERPREF_PT_system_display_graphics,
    USERPREF_PT_system_os_settings,
    USERPREF_PT_system_network,
    USERPREF_PT_system_memory,
    USERPREF_PT_system_video_sequencer,
    USERPREF_PT_system_sound,

    USERPREF_MT_interface_theme_presets,
    USERPREF_PT_theme,
    USERPREF_PT_theme_interface_panel,
    USERPREF_PT_theme_interface_gizmos,
    USERPREF_PT_theme_interface_icons,
    USERPREF_PT_theme_interface_state,
    USERPREF_PT_theme_interface_styles,
    USERPREF_PT_theme_interface_transparent_checker,
    USERPREF_PT_theme_text_style,
    USERPREF_PT_theme_bone_color_sets,
    USERPREF_PT_theme_collection_colors,
    USERPREF_PT_theme_strip_colors,
    USERPREF_PT_theme_glyph_colors,

    USERPREF_PT_file_paths_data,
    USERPREF_PT_file_paths_render,
    USERPREF_PT_file_paths_script_directories,
    USERPREF_PT_file_paths_applications,
    USERPREF_PT_text_editor,
    USERPREF_PT_text_editor_presets,
    USERPREF_PT_file_paths_development,

    USERPREF_PT_saveload_blend,
    USERPREF_PT_saveload_autorun,
    USERPREF_PT_saveload_file_browser,

    USERPREF_MT_keyconfigs,

    USERPREF_PT_input_keyboard,
    USERPREF_PT_input_mouse,
    USERPREF_PT_input_tablet,
    USERPREF_PT_input_touchpad,
    USERPREF_PT_input_ndof,
    USERPREF_PT_navigation_orbit,
    USERPREF_PT_navigation_zoom,
    USERPREF_PT_navigation_fly_walk,
    USERPREF_PT_navigation_fly_walk_navigation,
    USERPREF_PT_navigation_fly_walk_gravity,

    USERPREF_PT_keymap,

    USERPREF_PT_extensions,
    USERPREF_PT_addons,

    USERPREF_PT_assets,
    USERPREF_PT_file_paths_asset_libraries,

    USERPREF_MT_extensions_active_repo,
    USERPREF_MT_extensions_active_repo_remove,
    USERPREF_PT_extensions_repos,

    USERPREF_PT_studiolight_lights,
    USERPREF_PT_studiolight_light_editor,
    USERPREF_PT_studiolight_matcaps,
    USERPREF_PT_studiolight_world,

    # Popovers.
    USERPREF_PT_ndof_settings,
    USERPREF_PT_addons_filter,
    USERPREF_PT_tag_mode_filter_popover,

    # Operators.
    VIEW3D_OT_category_tabs_settings,
    USERPREF_OT_tag_mode_toggle,
    USERPREF_OT_tag_mode_select_all,
    USERPREF_OT_tag_mode_select_none,
    USERPREF_OT_category_tag_remove_from_category,

    USERPREF_PT_experimental_new_features,
    USERPREF_PT_experimental_prototypes,
    # USERPREF_PT_experimental_tweaks,

    USERPREF_PT_developer_tools,

    USERPREF_PT_tags,

    # UI lists
    USERPREF_UL_asset_libraries,
    USERPREF_UL_extension_repos,
    USERPREF_UL_category_tags,

    # Add dynamically generated editor theme panels last,
    # so they show up last in the theme section.
    *ThemeGenericClassGenerator.generate_panel_classes_from_theme_areas(),
)

if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
