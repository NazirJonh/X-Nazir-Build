# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Pure conversion / encoding helpers for the Category Tabs / Glyph / Tag system.

Extracted verbatim from ``space_userpref.py`` (no behavioural change). These functions
are stateless and depend only on the constants in :mod:`.defaults` and the standard
library, so this module has no ``bpy`` dependency and can be imported standalone.
The names are re-imported back into ``space_userpref`` to preserve its attribute contract.
"""

import re

from .defaults import (
    RESERVED_CATEGORY_PRIORITY,
    MODE_TO_FLAG,
    SPACE_TO_FLAG,
    _CATEGORY_TAG_MODE_FLAG_TO_NAME,
    _CATEGORY_TAG_MODE_NAME_TO_FLAG,
    _FLAG_TO_MODE,
    _FLAG_TO_SPACE,
)

from .log import tag_log


# Space type conversion helpers
def _space_type_str_to_id(space_type_str):
    """Convert space type string to ID."""
    space_type_map = {
        'SPACE_VIEW3D': 1, 'SPACE_GRAPH': 2, 'SPACE_OUTLINER': 3, 'SPACE_PROPERTIES': 4,
        'SPACE_FILE': 5, 'SPACE_IMAGE': 6, 'SPACE_INFO': 7, 'SPACE_SEQ': 8,
        'SPACE_TEXT': 9, 'SPACE_ACTION': 12, 'SPACE_NLA': 13, 'SPACE_NODE': 16,
        'SPACE_CONSOLE': 18, 'SPACE_USERPREF': 19, 'SPACE_CLIP': 20,
        'SPACE_TOPBAR': 21, 'SPACE_STATUSBAR': 22, 'SPACE_SPREADSHEET': 23
    }
    return space_type_map.get(space_type_str, -1)


def _space_type_id_to_str(space_type_id):
    """Convert space type ID to string."""
    id_to_str_map = {
        1: 'SPACE_VIEW3D', 2: 'SPACE_GRAPH', 3: 'SPACE_OUTLINER', 4: 'SPACE_PROPERTIES',
        5: 'SPACE_FILE', 6: 'SPACE_IMAGE', 7: 'SPACE_INFO', 8: 'SPACE_SEQ',
        9: 'SPACE_TEXT', 12: 'SPACE_ACTION', 13: 'SPACE_NLA', 16: 'SPACE_NODE',
        18: 'SPACE_CONSOLE', 19: 'SPACE_USERPREF', 20: 'SPACE_CLIP',
        21: 'SPACE_TOPBAR', 22: 'SPACE_STATUSBAR', 23: 'SPACE_SPREADSHEET'
    }
    return id_to_str_map.get(space_type_id, 'GLOBAL')


# Helper functions for cache key management
def _make_cache_key(space_type, category):
    """Create cache key for GLOBAL-only category lookup (Global-First architecture).

    Always returns (-1, category) regardless of input space_type to ensure
    a single source of truth for all category data across all editor types.
    """
    return (-1, category)


def spaces_to_flags(spaces_list):
    """Convert a list of space type strings to a combined bitmask (uint32)."""
    flags = 0
    for s in spaces_list:
        flags |= SPACE_TO_FLAG.get(s, 0)
    return flags


def flags_to_spaces(flags):
    """Convert a bitmask back to a list of space type strings."""
    result = []
    for bit in range(18):
        mask = 1 << bit
        if flags & mask:
            space_str = _FLAG_TO_SPACE.get(mask)
            if space_str:
                result.append(space_str)
    return result


def modes_to_flags(modes_list):
    """Convert a list of mode name strings to a combined bitmask (uint32)."""
    flags = 0
    for m in modes_list:
        flags |= MODE_TO_FLAG.get(m, 0)
    return flags


def flags_to_modes(flags):
    """Convert a bitmask back to a list of mode name strings."""
    result = []
    for bit in range(21):  # Extended range to include new detailed edit modes
        mask = 1 << bit
        if flags & mask:
            mode_str = _FLAG_TO_MODE.get(mask)
            if mode_str:
                result.append(mode_str)
    return result


def _normalize_category_key(value: str) -> str:
    """Normalize category/package name for robust matching (e.g. OpenVAT <-> openvat)."""
    return re.sub(r"[^a-z0-9]+", "", str(value).lower())


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


# ----------------------------------------------------------------------------
# Glyph representation map (single source of truth).
#
# A "glyph" is a Unicode codepoint, almost always in a Private Use Area (Material Symbols
# font icons). It appears in several encodings, each tied to a specific storage backend.
# Convert ONLY at these boundaries, and ONLY through the helpers below — never build an
# escape sequence or a hex string ad-hoc elsewhere.
#
#   Backend                         On-disk / stored form        Encode / decode helpers
#   ------------------------------  ---------------------------  ----------------------------------
#   In-memory cache (canonical)     raw Unicode char             (none — this is the working form)
#   JSON file: category glyph /     raw UTF-8                     write: as-is (ensure_ascii=False)
#     default_glyph, category                                    read:  _unicode_escape_to_glyph
#     names, category_orders                                            (handles legacy \uXXXX too)
#   DNA: CategoryTagDef.glyph       bare hex, e.g. "f3c1"        _glyph_to_hex / _hex_to_glyph
#     (char[8], tags)                                            C++: utf8_to_hex_codepoint /
#                                                                     tag_glyph_hex_to_utf8
#   Glyph library                   raw UTF-8 "unicode" field    read-only asset (nlohmann decodes)
#     (material_symbols.json)         + numeric "codepoint"
#
# History: JSON category glyphs used to be written as \uXXXX escapes. They are now written as
# raw UTF-8 to match the in-memory and library forms (one fewer encoding). Reading stays
# backward-compatible: every JSON reader passes values through _unicode_escape_to_glyph /
# _category_order_decode, which decode legacy \uXXXX and leave raw UTF-8 untouched. Tags keep the
# hex form because DNA stores them in a fixed char[8] field, not a Python string.
# ----------------------------------------------------------------------------


def _glyph_to_unicode_escape(glyph):
    """Convert a glyph character to \\uXXXX format.

    Retained for the public API facade and legacy call sites; the JSON writer no longer uses it
    (category glyphs are stored as raw UTF-8). See the glyph representation map above.
    """
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
    Maximum return length is 7 characters to fit in DNA char[8] field.
    """
    if not glyph:
        return ""
    if len(glyph) == 1:
        hex_str = format(ord(glyph), 'x')
    else:
        # For multi-character strings, return first char's hex
        tag_log(f"Multi-character glyph '{repr(glyph)}', using first char only", "WARN")
        hex_str = format(ord(glyph[0]), 'x')

    # Truncate to 7 characters to fit in DNA char glyph[8]
    if len(hex_str) > 7:
        tag_log(f"Glyph hex '{hex_str}' too long, truncating to 7 chars", "WARN")
        hex_str = hex_str[:7]

    return hex_str


# Reserved-category priority lookup (pure; data from defaults.RESERVED_CATEGORY_PRIORITY).
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


# Tag display-mode / icon-source converters (pure; primitive in, enum string out).
def _tag_icon_source_from_display_mode(display_mode_ui):
    """Convert display_mode_ui enum to icon_source enum.

    Handles both string ('ICON') and integer (1) values from C++,
    where C++ sets display_mode_ui as integer (0=GLYPH, 1=ICON).

    Args:
        display_mode_ui: 'GLYPH'/'ICON' string or 0/1 integer.

    Returns:
        'GLYPH' or 'BLENDER_ICON'
    """
    if display_mode_ui == 'ICON' or display_mode_ui == 1:
        return 'BLENDER_ICON'
    return 'GLYPH'

def _tag_display_mode_from_data(tag_data):
    """Determine display_mode_ui from tag data.
    
    Args:
        tag_data: Dictionary with icon_key, icon_source, etc.
    
    Returns:
        'GLYPH' or 'ICON'
    """
    if not isinstance(tag_data, dict):
        return 'GLYPH'
    
    icon_source = tag_data.get("icon_source", "auto")
    icon_key = tag_data.get("icon_key", "")
    
    # If icon_source is manual/BLENDER_ICON or has icon_key, use ICON mode
    if icon_source in ("manual", "BLENDER_ICON") or icon_key:
        return 'ICON'
    return 'GLYPH'
