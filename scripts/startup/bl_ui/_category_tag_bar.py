# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Shared helpers for the per-editor category Tag Bar.

These helpers were previously duplicated verbatim across ``space_image``,
``space_node`` and ``space_view3d``. They are intentionally free of any
editor-specific state: a caller computes its editor's current tag *mode flag*
(which depends on the active editor mode) and passes it in, so the same code can
serve every editor that hosts a Tag Bar.

This is a plain helper module: it is imported directly by the editor modules
that need it and is deliberately not listed in ``bl_ui._modules`` (it defines no
classes and needs no registration).
"""


def tag_glyph_display(glyph):
    """Return the printable glyph for a tag.

    ``glyph`` may be a hexadecimal Unicode code point as authored in the glyph
    library (for example ``"e88a"``) or an already-decoded character. Values of
    up to eight hexadecimal digits are converted to their character; anything
    else is returned unchanged. A false/empty value yields an empty string.
    """
    if not glyph:
        return ""
    try:
        if all(c in "0123456789abcdefABCDEF" for c in glyph) and len(glyph) <= 8:
            return chr(int(glyph, 16))
    except Exception:
        pass
    return glyph


def visible_tags_for_current_mode(window_manager, mode_flag):
    """Return WindowManager tags that have a glyph and are visible for a mode.

    A tag is visible when it is mode-agnostic (``mode_flags == 0``) or when its
    ``mode_flags`` bitmask intersects ``mode_flag``. Editors that need extra
    entries (such as preview-mode tags that are not yet committed to the
    WindowManager) build on top of this list.
    """
    return [
        tag for tag in window_manager.category_tags
        if tag.glyph and (tag.mode_flags == 0 or (tag.mode_flags & mode_flag))
    ]
