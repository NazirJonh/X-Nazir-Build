# SPDX-FileCopyrightText: 2026 Nazir
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Shared UI palette for the Extract viewport overlays.

Every consumer uses :func:`get_theme`, which pulls from the active Blender theme and falls back
to the built-in tones below when a value cannot be read (factory theme, headless).

The ``HINT_*`` entries are the Add / Erase colors of the hint: Add is the accent, Erase a warning
tone.
"""

import bpy

PANEL_COLOR = (0.105, 0.105, 0.125, 0.94)
OUTLINE_COLOR = (0.55, 0.55, 1.0, 0.35)
# The value boxes show they can be clicked to type in.
FIELD_COLOR = (0.0, 0.0, 0.0, 0.35)
FIELD_OUTLINE_COLOR = (0.55, 0.55, 1.0, 0.8)
FIELD_IDLE_COLOR = (0.0, 0.0, 0.0, 0.18)
FIELD_IDLE_OUTLINE_COLOR = (1.0, 1.0, 1.0, 0.22)
FIELD_HOVER_OUTLINE_COLOR = (0.55, 0.55, 1.0, 0.65)
TRACK_COLOR = (1.0, 1.0, 1.0, 0.14)
FILL_COLOR = (0.55, 0.55, 1.0, 0.85)
KNOB_COLOR = (0.92, 0.92, 0.95, 1.0)
TEXT_COLOR = (0.92, 0.92, 0.95, 1.0)
DIM_TEXT_COLOR = (0.92, 0.92, 0.95, 0.6)
ACCEPT_COLOR = (1.0, 0.62, 0.34, 0.45)
ACCEPT_HOVER_COLOR = (1.0, 0.62, 0.34, 0.65)
CANCEL_COLOR = (1.0, 1.0, 1.0, 0.08)
CANCEL_HOVER_COLOR = (1.0, 1.0, 1.0, 0.16)
# Drag hint: Add uses the accent (overridden from the theme), Erase a warning tone.
HINT_ADD_COLOR = (0.30, 0.72, 1.0, 0.9)
# No theme warning color is exposed to Python, so Erase keeps a fixed fallback tone.
HINT_ERASE_COLOR = (1.0, 0.45, 0.22, 0.95)

_THEME_FALLBACK = {
    "PANEL": PANEL_COLOR, "OUTLINE": OUTLINE_COLOR, "FIELD": FIELD_COLOR,
    "FIELD_OUTLINE": FIELD_OUTLINE_COLOR, "FIELD_IDLE": FIELD_IDLE_COLOR,
    "FIELD_IDLE_OUTLINE": FIELD_IDLE_OUTLINE_COLOR, "FIELD_HOVER_OUTLINE": FIELD_HOVER_OUTLINE_COLOR,
    "TRACK": TRACK_COLOR, "FILL": FILL_COLOR, "KNOB": KNOB_COLOR, "TEXT": TEXT_COLOR,
    "DIM_TEXT": DIM_TEXT_COLOR, "ACCEPT": ACCEPT_COLOR, "ACCEPT_HOVER": ACCEPT_HOVER_COLOR,
    "CANCEL": CANCEL_COLOR, "CANCEL_HOVER": CANCEL_HOVER_COLOR,
    "HINT_ADD": HINT_ADD_COLOR, "HINT_ERASE": HINT_ERASE_COLOR,
}


def get_theme():
    """The overlay colors, taken from the active Blender theme (the built-in ones when they
    cannot be read)."""
    colors = dict(_THEME_FALLBACK)
    colors["SELECTION"] = (0.55, 0.55, 1.0, 0.45)
    colors["TEXT_SEL"] = TEXT_COLOR
    try:
        ui = bpy.context.preferences.themes[0].user_interface
        back, regular, slider = ui.wcol_menu_back, ui.wcol_regular, ui.wcol_numslider
        text_field, tool = ui.wcol_text, ui.wcol_tool

        def rgb(color, alpha):
            return (*tuple(color)[:3], alpha)

        def lift(color, alpha):
            # A lighter shade of a theme color, for hover states.
            return (*(min(c + 0.12, 1.0) for c in tuple(color)[:3]), alpha)

        accent = tuple(regular.inner_sel)
        text = tuple(regular.text)
        colors.update({
            "PANEL": rgb(back.inner, 0.94),
            "OUTLINE": rgb(back.outline, 0.8),
            "FIELD": rgb(text_field.inner_sel, 0.95),
            "FIELD_OUTLINE": rgb(text_field.item, 1.0),
            "FIELD_IDLE": rgb(text_field.inner, 0.9),
            "FIELD_IDLE_OUTLINE": rgb(text_field.outline, 0.8),
            "FIELD_HOVER_OUTLINE": rgb(accent, 0.9),
            "TRACK": rgb(slider.inner, 0.9),
            "FILL": rgb(slider.item, 0.95),
            "KNOB": rgb(text, 1.0),
            "TEXT": rgb(text, 1.0),
            "DIM_TEXT": rgb(text, 0.6),
            "ACCEPT": rgb(tool.inner_sel, 0.85),
            "ACCEPT_HOVER": lift(tool.inner_sel, 1.0),
            "CANCEL": rgb(tool.inner, 0.9),
            "CANCEL_HOVER": lift(tool.inner, 1.0),
            "SELECTION": rgb(text_field.item, 0.45),
            "TEXT_SEL": rgb(regular.text_sel, 1.0),
            # Add is the accent; the theme has no warning color for Erase (see the fallback).
            "HINT_ADD": rgb(accent, 0.9),
        })
    except (AttributeError, IndexError, TypeError):
        pass
    return colors
