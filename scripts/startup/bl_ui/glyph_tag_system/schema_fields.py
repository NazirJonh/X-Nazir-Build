# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Single source of truth for the composition of a category entry.

:data:`CATEGORY_FIELDS` is a table describing every field a category entry has: its key, its
kind (which fixes the sanitizer), its default and, for the icon block, where it lives on disk.
All three entry-building paths are derived from that one table:

- :func:`new_entry` — an entry of pure defaults.
- :func:`coerce_entry` — raw (file or cache) data to a well-typed entry.
- :func:`entry_to_disk` — an entry to the on-disk shape.

Adding a field therefore means adding one row here and nothing else; a field can no longer be
present on one path and missing on another. That drift is not hypothetical: ``install_mode_flag``
was once absent from the save path, which silently disabled mode-aware filtering for panels
without ``bl_context`` because the DNA field stayed 0.

Two deliberate shape differences between the entry and the file are encoded in the table rather
than open-coded:

- Icon fields are flat in the entry (``icon_source``) and nested on disk (``icon: {"source"}``).
  :func:`coerce_entry` accepts both, because files written before the block was nested are flat.
- Every field is written unless its row says ``persist=False``. Omitting a field because its
  value happens to be empty would save a few bytes per category at the cost of reintroducing the
  "field is silently not persisted" class of bug; absent-means-default is kept on read only, for
  backward compatibility. A field is kept out of the file only when storing it would be *wrong*,
  never merely redundant, and the row has to say so out loud.

Cross-field derivation (``base_type`` inference, the ``glyph_only`` invariants, reserved-category
restoration) is not expressible per field and lives in :mod:`migrations` instead.

This module is bpy-free and free of module-level mutable state; it depends only on the lower pure
layers of the package (schema_keys / conversions).
"""

from .schema_keys import (
    KEY_BASE_TYPE,
    KEY_COLOR,
    KEY_DEFAULT_DISPLAY_NAME,
    KEY_DEFAULT_GLYPH,
    KEY_DISCOVERED_IN_MODES,
    KEY_DISCOVERED_IN_SPACES,
    KEY_DISPLAY_NAME,
    KEY_FIRST_LETTER,
    KEY_GLYPH,
    KEY_GLYPH_MODE,
    KEY_ICON,
    KEY_ICON_KEY,
    KEY_ICON_PATH,
    KEY_ICON_PROVIDER,
    KEY_ICON_SOURCE,
    KEY_INSTALL_MODE_FLAG,
    KEY_PENDING_TAG_ASSIGNMENT,
    KEY_SOURCE_EXTENSION,
    KEY_TAGS,
    KEY_WITHOUT_TAG_PREVIEW,
    ICON_BLOCK_KEY,
    ICON_BLOCK_PATH,
    ICON_BLOCK_PROVIDER,
    ICON_BLOCK_SOURCE,
)
from .conversions import (
    _unicode_escape_to_glyph,
)

# ----------------------------------------------------------------------------
# Field kinds. The kind fixes the sanitizer, so a row cannot declare a type without also
# declaring how a corrupt value of that type is made safe.

KIND_GLYPH = 'GLYPH'
KIND_STR = 'STR'
KIND_STR_COERCE = 'STR_COERCE'
KIND_ENUM = 'ENUM'
KIND_BOOL = 'BOOL'
KIND_U32 = 'U32'
KIND_STR_LIST = 'STR_LIST'
KIND_COLOR = 'COLOR'

# Enum domains.
BASE_TYPES = ("text_only", "glyph_text", "glyph_only")
GLYPH_MODES = ("auto", "first_letter")
ICON_SOURCES = ("auto", "manual", "off")


class Field:
    """One field of a category entry.

    Args:
        key: The entry key (and the on-disk key, unless ``icon_key`` is given).
        kind: One of the ``KIND_*`` constants; selects the sanitizer.
        default: The value used when the field is absent or unusable. Lists are copied per
            entry, never shared.
        choices: Allowed values, for ``KIND_ENUM`` only. An out-of-domain value falls back
            to ``default``.
        icon_key: For icon fields, the key inside the nested on-disk ``icon`` block.
        persist: Whether the field is written to the file. False only for transient state that
            would be actively harmful to restore; such a field still belongs in the table,
            because a field left out of it is dropped by :func:`coerce_entry` and its readers
            then see the default forever.
    """

    __slots__ = ("key", "kind", "default", "choices", "icon_key", "persist")

    def __init__(self, key, kind, default, choices=None, icon_key=None, persist=True):
        self.key = key
        self.kind = kind
        self.default = default
        self.choices = choices
        self.icon_key = icon_key
        self.persist = persist

    def make_default(self):
        """Return a fresh default, copying mutable ones so entries never share a list."""
        if isinstance(self.default, list):
            return list(self.default)
        return self.default


# ----------------------------------------------------------------------------
# The table. Order is the on-disk key order; the nested icon block takes the position of the
# first icon field, which keeps the file byte-comparable with what the previous writer emitted.

CATEGORY_FIELDS = (
    Field(KEY_GLYPH, KIND_GLYPH, ""),
    Field(KEY_DISPLAY_NAME, KIND_STR, ""),
    Field(KEY_FIRST_LETTER, KIND_STR, ""),
    Field(KEY_COLOR, KIND_COLOR, [0.0, 0.0, 0.0]),
    Field(KEY_DEFAULT_GLYPH, KIND_GLYPH, ""),
    Field(KEY_DEFAULT_DISPLAY_NAME, KIND_STR, ""),
    Field(KEY_BASE_TYPE, KIND_ENUM, "text_only", choices=BASE_TYPES),
    Field(KEY_TAGS, KIND_STR_LIST, []),
    Field(KEY_GLYPH_MODE, KIND_ENUM, "auto", choices=GLYPH_MODES),
    Field(KEY_ICON_SOURCE, KIND_ENUM, "auto", choices=ICON_SOURCES, icon_key=ICON_BLOCK_SOURCE),
    Field(KEY_ICON_KEY, KIND_STR_COERCE, "", icon_key=ICON_BLOCK_KEY),
    Field(KEY_ICON_PATH, KIND_STR_COERCE, "", icon_key=ICON_BLOCK_PATH),
    Field(KEY_ICON_PROVIDER, KIND_STR_COERCE, "", icon_key=ICON_BLOCK_PROVIDER),
    # Extension-related fields for the "New Add-ons!" feature.
    Field(KEY_SOURCE_EXTENSION, KIND_STR_COERCE, ""),
    Field(KEY_PENDING_TAG_ASSIGNMENT, KIND_BOOL, False),
    Field(KEY_DISCOVERED_IN_SPACES, KIND_STR_LIST, []),
    Field(KEY_DISCOVERED_IN_MODES, KIND_STR_LIST, []),
    # Mode flag captured when the extension was installed. It is the fallback the C++ side uses
    # for panels that declare no ``bl_context`` (see #interface_panel.cc), so dropping it here
    # would silently disable mode-aware filtering for those panels. DNA stores it in a
    # ``uint32_t``: a corrupt or negative value must never reach the RNA assignment.
    Field(KEY_INSTALL_MODE_FLAG, KIND_U32, 0),
    # Set when the user picks "Without Tag" in the dialog and consumed by the Save handler in
    # #wm_sync_to_wm, which turns it into pending_tag_assignment=False. Never persisted: a flag
    # restored from disk would finalize a choice the user never confirmed.
    Field(KEY_WITHOUT_TAG_PREVIEW, KIND_BOOL, False, persist=False),
)

FIELDS_BY_KEY = {field.key: field for field in CATEGORY_FIELDS}


def _normalize_color(color):
    """Coerce an RGB value to a list of three floats clamped to ``[0, 1]``.

    Guards against legacy string-encoded channels, missing/extra channels and out-of-range
    values that would otherwise reach the UI swatches unchanged.
    """
    result = [0.0, 0.0, 0.0]
    if isinstance(color, (list, tuple)):
        for i in range(min(3, len(color))):
            try:
                c = float(color[i])
            except (TypeError, ValueError):
                c = 0.0
            result[i] = min(1.0, max(0.0, c))
    return result


def _coerce_glyph(value):
    # Corrupt/hand-edited files may store a non-string glyph (e.g. a list); coerce it away so
    # downstream single-glyph checks cannot crash on it. Legacy files escape glyphs as \uXXXX.
    if not isinstance(value, str):
        return ""
    if value and '\\u' in value:
        return _unicode_escape_to_glyph(value)
    return value


def _coerce_u32(value):
    try:
        number = int(value)
    except (TypeError, ValueError):
        return 0
    return min(max(number, 0), 0xFFFFFFFF)


def _coerce_value(field, value):
    """Sanitize one raw value according to its field's kind."""
    kind = field.kind
    if kind == KIND_GLYPH:
        return _coerce_glyph(value)
    if kind == KIND_STR:
        return value if isinstance(value, str) else field.make_default()
    if kind == KIND_STR_COERCE:
        return str(value) if value is not None else ""
    if kind == KIND_ENUM:
        if not isinstance(value, str):
            return field.default
        lowered = value.lower()
        return lowered if lowered in field.choices else field.default
    if kind == KIND_BOOL:
        return bool(value)
    if kind == KIND_U32:
        return _coerce_u32(value)
    if kind == KIND_STR_LIST:
        return [str(item) for item in value] if isinstance(value, list) else field.make_default()
    if kind == KIND_COLOR:
        return _normalize_color(value)
    raise AssertionError("unknown field kind {0}".format(kind))


def new_entry():
    """Return an entry holding nothing but defaults."""
    return {field.key: field.make_default() for field in CATEGORY_FIELDS}


def _raw_value(raw, field):
    """Return the raw value for a field, or a sentinel when it is absent.

    Icon fields are read from the nested ``icon`` block first and from the legacy flat key
    second, so both on-disk shapes load.
    """
    if field.icon_key is not None:
        icon_block = raw.get(KEY_ICON)
        if isinstance(icon_block, dict) and field.icon_key in icon_block:
            return icon_block[field.icon_key], True
    if field.key in raw:
        return raw[field.key], True
    return None, False


def coerce_entry(raw):
    """Build a well-typed entry from raw file or cache data.

    Unknown keys are dropped and every table field is present in the result, so consumers can
    index the entry without ``get`` fallbacks. Cross-field derivation is *not* applied here;
    see :func:`migrations._derive_entry_invariants`.
    """
    entry = {}
    for field in CATEGORY_FIELDS:
        value, present = _raw_value(raw, field)
        entry[field.key] = _coerce_value(field, value) if present else field.make_default()
    return entry


def field_present(raw, key):
    """Return whether a field was actually supplied by ``raw``.

    The derivation pass needs to tell "absent" from "present but default": an explicitly stored
    empty ``default_glyph`` is meaningful for text_only categories, an absent one is not.
    """
    field = FIELDS_BY_KEY.get(key)
    if field is None:
        return key in raw
    return _raw_value(raw, field)[1]


def entry_to_disk(entry):
    """Convert an entry to its on-disk shape.

    Every field the table marks ``persist`` is written, so a value that reached the entry cannot
    be lost on the way to the file. The flat icon fields are folded into the nested ``icon``
    block.
    """
    out = {}
    icon_block = None
    for field in CATEGORY_FIELDS:
        if not field.persist:
            continue
        value = entry.get(field.key, field.make_default())
        if field.icon_key is None:
            out[field.key] = value
            continue
        if icon_block is None:
            icon_block = {}
            out[KEY_ICON] = icon_block
        icon_block[field.icon_key] = value
    return out
