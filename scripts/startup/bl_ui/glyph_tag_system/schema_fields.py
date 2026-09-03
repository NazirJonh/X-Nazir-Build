# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Single source of truth for the composition of a category entry and of a tag entry.

:data:`CATEGORY_FIELDS` and :data:`TAG_FIELDS` are tables describing every field their entry
has: its key, its kind (which fixes the sanitizer), its default and, for fields whose disk shape
differs from their entry shape, how to get there. Each table drives its own matching trio:

- :func:`new_entry` / :func:`new_tag_entry` — an entry of pure defaults.
- :func:`coerce_entry` / :func:`coerce_tag_entry` — raw (file or cache) data to a well-typed
  entry.
- :func:`entry_to_disk` / :func:`tag_entry_to_disk` — an entry to the on-disk shape.

Adding a field therefore means adding one row to the relevant table and nothing else; a field can
no longer be present on one path and missing on another. That drift is not hypothetical:
``install_mode_flag`` was once absent from the category save path, which silently disabled
mode-aware filtering for panels without ``bl_context`` because the DNA field stayed 0.

Deliberate shape differences between an entry and its file are encoded in the tables rather than
open-coded:

- Category icon fields are flat in the entry (``icon_source``) and nested on disk
  (``icon: {"source"}``). :func:`coerce_entry` accepts both, because files written before the
  block was nested are flat. ``icon_key`` on a :class:`Field` controls this remap; it is unused
  by tags, whose icon fields are flat on both sides.
- A tag's glyph is a raw Unicode character in the entry/cache (matching the category glyph and
  the glyph library) but a bare hex codepoint on disk, because DNA stores it in a fixed
  ``char[8]``, not a Python string — see the glyph representation map in :mod:`conversions`.
  ``to_disk`` on a :class:`Field` controls this: it runs only in the entry-to-disk direction,
  where the input is always a canonical entry, never disk data, so it cannot double-encode. The
  read direction instead tells the two shapes apart by value (:func:`_coerce_tag_glyph`): a disk
  hex string is always ASCII, a cache glyph character never is, so shape alone decides.
- Every persisted field is written unless its row says ``persist=False``. Omitting a field
  because its value happens to be empty would save a few bytes at the cost of reintroducing the
  "field is silently not persisted" class of bug; absent-means-default is kept on read only, for
  backward compatibility. A field is kept out of the file only when storing it would be *wrong*,
  never merely redundant, and the row has to say so out loud.

Cross-field derivation (category ``base_type`` inference, the ``glyph_only`` invariants,
reserved-category restoration) is not expressible per field and lives in :mod:`migrations`
instead, alongside the top-level entry points (:func:`migrations._normalize_category_data`,
:func:`migrations._normalize_tag_data`) that callers should use to turn raw data into a
trustworthy entry — tags have no derivation pass today, but the entry point still lives there so
callers never need to know that.

This module is bpy-free and free of module-level mutable state; it depends only on the lower pure
layers of the package (schema_keys / conversions / defaults).
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
    KEY_MODE_FLAGS,
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
    _glyph_to_hex,
    _hex_to_glyph,
    _unicode_escape_to_glyph,
)
from .defaults import (
    _CATEGORY_TAG_DEFAULT_MODE_FLAGS,
)

# ----------------------------------------------------------------------------
# Field kinds. The kind fixes the sanitizer, so a row cannot declare a type without also
# declaring how a corrupt value of that type is made safe.

KIND_GLYPH = 'GLYPH'
KIND_GLYPH_HEX = 'GLYPH_HEX'
# The two plain-string kinds differ in what a corrupt (wrong-type) value becomes, and the split
# tracks what the field is downstream, not a historical accident:
# - KIND_STR (display_name, first_letter, default_display_name): user-visible label text. A
#   corrupt value is dropped to "" so it degrades through the C++ label fallback chain
#   (display_name -> default_display_name -> category, interface_tab_categories.cc) instead of
#   surfacing a stringified garbage value (e.g. a stray int rendered as "5") as a UI label —
#   first_letter is drawn directly as a glyph, where that would be worse still.
# - KIND_STR_COERCE (icon_key, icon_path, icon_provider, source_extension): opaque identifiers
#   that are only ever matched or looked up on the C++ side (RNA enum resolution, STREQ/
#   STRPREFIX comparisons, file-exists checks), never rendered as prose. Coercing a corrupt
#   value to its string form is harmless there: it just fails to match anything.
KIND_STR = 'STR'
KIND_STR_COERCE = 'STR_COERCE'
KIND_ENUM = 'ENUM'
KIND_INT_ENUM = 'INT_ENUM'
KIND_BOOL = 'BOOL'
KIND_U32 = 'U32'
KIND_STR_LIST = 'STR_LIST'
KIND_COLOR = 'COLOR'

# Enum domains.
BASE_TYPES = ("text_only", "glyph_text", "glyph_only")
GLYPH_MODES = ("auto", "first_letter")
ICON_SOURCES = ("auto", "manual", "off")

# Tag icon_source domain. Unlike the category icon_source (a string enum), this is stored as a
# plain int because DNA's CategoryTagDef.icon_source is a C ``int``, not a string.
TAG_ICON_SOURCE_GLYPH = 0
TAG_ICON_SOURCE_BLENDER_ICON = 1
TAG_ICON_SOURCE_CUSTOM = 2
TAG_ICON_SOURCES = (TAG_ICON_SOURCE_GLYPH, TAG_ICON_SOURCE_BLENDER_ICON, TAG_ICON_SOURCE_CUSTOM)


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
        to_disk: Optional entry-value -> disk-value transform, for a field whose disk
            representation is not its entry representation (e.g. a tag glyph, hex on disk).
            Runs only when writing (:func:`entry_to_disk`); reading tells the shapes apart by
            value instead, in the field's own coercer, because a decode step run on both an
            already-decoded entry and on fresh disk data cannot stay correct for every kind.
    """

    __slots__ = ("key", "kind", "default", "choices", "icon_key", "persist", "to_disk")

    def __init__(self, key, kind, default, choices=None, icon_key=None, persist=True, to_disk=None):
        self.key = key
        self.kind = kind
        self.default = default
        self.choices = choices
        self.icon_key = icon_key
        self.persist = persist
        self.to_disk = to_disk

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


def _coerce_tag_glyph(value):
    """Sanitize a tag glyph, accepting either of its two valid shapes.

    A tag glyph is a raw Unicode character in the cache/entry (the ``KIND_GLYPH`` shape used by
    categories) but a bare hex codepoint on disk (DNA stores it in a fixed ``char[8]``). The two
    are told apart by value, not by which caller passed them: the disk form is always ASCII hex
    text, the entry form is always a single character outside the ASCII range (Material Symbols
    glyphs live in the Private Use Area, well above 0x7f), so a single non-ASCII character is
    passed through untouched and everything else is decoded as hex.
    """
    if not isinstance(value, str) or not value:
        return ""
    if len(value) == 1 and ord(value) > 0x7F:
        return value
    return _hex_to_glyph(value)


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
    if kind == KIND_GLYPH_HEX:
        return _coerce_tag_glyph(value)
    if kind == KIND_STR:
        return value if isinstance(value, str) else field.make_default()
    if kind == KIND_STR_COERCE:
        return str(value) if value is not None else ""
    if kind == KIND_ENUM:
        if not isinstance(value, str):
            return field.default
        lowered = value.lower()
        return lowered if lowered in field.choices else field.default
    if kind == KIND_INT_ENUM:
        try:
            number = int(value)
        except (TypeError, ValueError):
            return field.default
        return number if number in field.choices else field.default
    if kind == KIND_BOOL:
        return bool(value)
    if kind == KIND_U32:
        return _coerce_u32(value)
    if kind == KIND_STR_LIST:
        return [str(item) for item in value] if isinstance(value, list) else field.make_default()
    if kind == KIND_COLOR:
        return _normalize_color(value)
    raise AssertionError("unknown field kind {0}".format(kind))


def _new_entry(fields):
    return {field.key: field.make_default() for field in fields}


def new_entry():
    """Return a category entry holding nothing but defaults."""
    return _new_entry(CATEGORY_FIELDS)


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


def _coerce_entry(fields, raw):
    entry = {}
    for field in fields:
        value, present = _raw_value(raw, field)
        entry[field.key] = _coerce_value(field, value) if present else field.make_default()
    return entry


def coerce_entry(raw):
    """Build a well-typed category entry from raw file or cache data.

    Unknown keys are dropped and every table field is present in the result, so consumers can
    index the entry without ``get`` fallbacks. Cross-field derivation is *not* applied here;
    see :func:`migrations._derive_entry_invariants`.
    """
    return _coerce_entry(CATEGORY_FIELDS, raw)


def field_present(raw, key):
    """Return whether a field was actually supplied by ``raw``.

    The derivation pass needs to tell "absent" from "present but default": an explicitly stored
    empty ``default_glyph`` is meaningful for text_only categories, an absent one is not.
    """
    field = FIELDS_BY_KEY.get(key)
    if field is None:
        return key in raw
    return _raw_value(raw, field)[1]


def _entry_to_disk(fields, entry):
    out = {}
    icon_block = None
    for field in fields:
        if not field.persist:
            continue
        value = entry.get(field.key, field.make_default())
        if field.to_disk is not None:
            value = field.to_disk(value)
        if field.icon_key is None:
            out[field.key] = value
            continue
        if icon_block is None:
            icon_block = {}
            out[KEY_ICON] = icon_block
        icon_block[field.icon_key] = value
    return out


def entry_to_disk(entry):
    """Convert a category entry to its on-disk shape.

    Every field the table marks ``persist`` is written, so a value that reached the entry cannot
    be lost on the way to the file. The flat icon fields are folded into the nested ``icon``
    block.
    """
    return _entry_to_disk(CATEGORY_FIELDS, entry)


# ----------------------------------------------------------------------------
# Tag entry fields. A tag has no nested disk block (all fields are flat both in the entry and on
# disk) and no cross-field derivation, but its glyph needs the hex transform above and its
# icon_source is an int domain, not the string enum categories use — see the module docstring.

TAG_FIELDS = (
    Field(KEY_GLYPH, KIND_GLYPH_HEX, "", to_disk=_glyph_to_hex),
    Field(KEY_COLOR, KIND_COLOR, [0.0, 0.0, 0.0]),
    Field(KEY_MODE_FLAGS, KIND_U32, _CATEGORY_TAG_DEFAULT_MODE_FLAGS),
    Field(KEY_ICON_KEY, KIND_STR_COERCE, ""),
    Field(KEY_ICON_PATH, KIND_STR_COERCE, ""),
    Field(KEY_ICON_SOURCE, KIND_INT_ENUM, TAG_ICON_SOURCE_GLYPH, choices=TAG_ICON_SOURCES),
)

TAG_FIELDS_BY_KEY = {field.key: field for field in TAG_FIELDS}


def new_tag_entry():
    """Return a tag entry holding nothing but defaults."""
    return _new_entry(TAG_FIELDS)


def coerce_tag_entry(raw):
    """Build a well-typed tag entry from raw file or cache data. See :func:`coerce_entry`."""
    return _coerce_entry(TAG_FIELDS, raw)


def tag_entry_to_disk(entry):
    """Convert a tag entry to its on-disk shape (glyph re-encoded to hex). See
    :func:`entry_to_disk`.
    """
    return _entry_to_disk(TAG_FIELDS, entry)


def coerce_tag_field_value(key, value):
    """Sanitize a single raw value against its row in :data:`TAG_FIELDS`.

    For point updates (:func:`tags_cache.update_tag`) that change one field of an
    already-normalized entry rather than rebuilding it from raw data — the same guarantee
    :func:`coerce_tag_entry` gives a full entry, applied to a single field. Keys outside the
    table (there are none today, but a caller error should not be silently accepted as a no-op)
    are returned unchanged.
    """
    field = TAG_FIELDS_BY_KEY.get(key)
    if field is None:
        return value
    return _coerce_value(field, value)
