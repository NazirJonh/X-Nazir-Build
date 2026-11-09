# SPDX-FileCopyrightText: 2026 Nazir
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Viewport panel of the live Extract feature: what its rows are and how they are shown.

The reusable part -- the multi-column layout, the drawing helpers, the widgets and the saved panel
size and place -- lives in ``panel_ui``. This module is only Extract's description of its
panel (the sliders, the columns, the segmented choices) and :func:`draw_panel`, which feeds the operator's
state into those widgets. The operator is passed in as the object whose attributes (``values``,
``hover``, ``editing``, ``obj_name``, ``placement``, ``collection_mode``, ``accept_mode``,
``offset_space``, ``preview`` ...) are read.

The panel has three columns (see ``COLUMNS``) under a header with Cancel and Accept. A row is a
slider (``SLIDERS``), a text field (the object name), a segmented button (``SEGMENTS``: every
choice visible at once) or a toggle (the thickness loops preview). The header drags the panel.
"""

import time

import bpy
import gpu
from bpy.app.translations import pgettext_iface

from .panel_ui import (
    FADE_TIME,
    SNAP,
    Slider,
    PanelLayout,
    draw_frame,
    draw_header_buttons,
    draw_headings,
    draw_help,
    draw_picker,
    draw_segments,
    draw_slider,
    draw_text_field,
    draw_toggle,
    set_panel_scale,
    ui_scale,
)
from .theme import get_theme

# The row keys, slider set and choices the operator drives; `draw_panel` and `extract_layout`
# feed the operator's state into the shared widgets.
__all__ = (
    "ACCEPT_MODES", "COLLECTION_MODES", "COLUMNS", "FADE_TIME", "INT_SLIDERS", "OFFSET_SPACES",
    "PLACEMENTS", "REBUILD_SLIDERS", "ROW_ACCEPT", "ROW_COLLECTION", "ROW_NAME", "ROW_PLACE",
    "ROW_PREVIEW", "ROW_SPACE", "ROW_TARGET", "SEGMENTS", "SLIDER", "SLIDERS", "SNAP",
    "draw_panel", "extract_layout", "segment_enabled", "set_panel_scale",
)

# Keys of the non-slider rows (kept distinct from the slider keys).
ROW_NAME = "NAME"
ROW_PREVIEW = "PREVIEW"
ROW_TARGET = "TARGET"
ROW_SPACE = "OFFSET_SPACE"
ROW_PLACE = "PLACEMENT"
ROW_COLLECTION = "COLLECTION"
ROW_ACCEPT = "ACCEPT_MODE"

# Segmented buttons: key -> (label, operator attribute, ((value, text), ...)).
SEGMENTS = {
    ROW_SPACE: ("Axes", "offset_space",
                (('GLOBAL', "Global"), ('LOCAL', "Local"), ('CURSOR', "Cursor"))),
    ROW_PLACE: ("Result", "placement",
                (('MESH', "Mesh"), ('CHILD', "Child"), ('COLLECTION', "Collection"))),
    ROW_COLLECTION: ("Collection", "collection_mode",
                     (('SOURCE', "Source"), ('ACTIVE', "Active"), ('NEW', "New"))),
    # No side label: it sits under its own heading and spans the whole column.
    ROW_ACCEPT: (None, "accept_mode",
                 (('KEEP', "Keep Area"), ('REMOVE', "Remove Area"),
                  ('FILL', "Remove + Fill"))),
}
# The value orders, for the operator's cycling and persistence.
COLLECTION_MODES = tuple(v for v, _t in SEGMENTS[ROW_COLLECTION][2])
ACCEPT_MODES = tuple(v for v, _t in SEGMENTS[ROW_ACCEPT][2])
PLACEMENTS = tuple(v for v, _t in SEGMENTS[ROW_PLACE][2])
OFFSET_SPACES = tuple(v for v, _t in SEGMENTS[ROW_SPACE][2])

# Sliders that change the extracted geometry (need a rebuild) versus modifier-only sliders.
REBUILD_SLIDERS = ('grow',)
# Sliders that take whole numbers.
INT_SLIDERS = ('grow', 'thickness_loops')

SLIDERS = (
    Slider("scale", "Scale", 0.25, 1.75, 1.0, "{:.2f}", 0.01, 10.0),
    Slider("inflate", "Inflate", -1.0, 1.0, 0.0, "{:+.2f}", -10.0, 10.0),
    Slider("offset_x", "Move X", -1.0, 1.0, 0.0, "{:+.2f}", -100.0, 100.0),
    Slider("offset_y", "Move Y", -1.0, 1.0, 0.0, "{:+.2f}", -100.0, 100.0),
    Slider("offset_z", "Move Z", -1.0, 1.0, 0.0, "{:+.2f}", -100.0, 100.0),
    Slider("thickness", "Thickness", -1.0, 1.0, 0.0, "{:+.2f}", -10.0, 10.0),
    Slider("smoothness", "Smoothness", 0.0, 1.0, 0.0, "{:.2f}", 0.0, 1.0),
    Slider("grow", "Grow", -5, 5, 0, "{:.0f}", -5, 5),
    Slider("thickness_loops", "Thickness Loops", 0, 8, 0, "{:.0f}", 0, 8),
    # The panel's own size; kept in the user preferences (see `panel_ui.PanelPlacementPrefs`).
    Slider("ui_scale", "UI Scale", 0.6, 1.6, 1.0, "{:.2f}", 0.5, 2.0),
)
SLIDER = {slider.key: slider for slider in SLIDERS}

# The columns, top to bottom: (kind, key) with kind one of slider / heading / seg / name / toggle.
COLUMNS = (
    (("slider", "scale"), ("slider", "inflate"), ("heading", "Transform"), ("seg", ROW_SPACE),
     ("slider", "offset_x"), ("slider", "offset_y"), ("slider", "offset_z")),
    (("slider", "thickness"), ("slider", "thickness_loops"), ("slider", "smoothness"),
     ("slider", "grow")),
    (("name", ROW_NAME), ("seg", ROW_PLACE), ("seg", ROW_COLLECTION), ("target", ROW_TARGET),
     ("heading", "Accept Mask Area"), ("seg", ROW_ACCEPT), ("toggle", ROW_PREVIEW),
     ("slider", "ui_scale")),
)

def extract_layout(area, region, offset=(0.0, 0.0)):
    """The Extract panel's layout (see :class:`panel_ui.PanelLayout`)."""
    return PanelLayout(area, region, COLUMNS, SEGMENTS, offset)


def _active_collection_name():
    try:
        return bpy.context.view_layer.active_layer_collection.collection.name
    except AttributeError:
        return "?"


def segment_enabled(op, key):
    """The Collection buttons only matter for a result that is a separate object."""
    return key != ROW_COLLECTION or op.placement == 'COLLECTION'


def target_collection_text(op):
    """What the Collection row resolves to: the name of the collection the extract will go to."""
    if op.collection_mode == 'ACTIVE':
        return _active_collection_name()
    if op.collection_mode == 'NEW':
        return (op.obj_name + " Collection")[:63]
    return op.target_collection or op.source_collection_name


def draw_panel(op):
    context = bpy.context
    if context.area != op.area or context.region is None or context.region.type != 'WINDOW':
        return
    s = ui_scale()
    alpha = min(1.0, (time.perf_counter() - op.opened) / FADE_TIME)
    lay = extract_layout(context.area, context.region, op.panel_offset)
    c = get_theme()
    shader = gpu.shader.from_builtin('UNIFORM_COLOR')

    draw_frame(shader, c, lay, s, alpha, pgettext_iface("Extract"), op.panel_drag is not None)
    draw_headings(c, lay, s, alpha)
    for slider in SLIDERS:
        draw_slider(
            shader, c, lay, slider, op.values[slider.key], s, alpha,
            active=slider.key in {op.hover, op.dragging, op.editing},
            editing=op.editing == slider.key, edit_text=op.edit_text, edit_fresh=op.edit_fresh,
            field_hovered=op.hover == ("FIELD", slider.key))
    draw_text_field(
        shader, c, lay, ROW_NAME, "Name", op.obj_name, s, alpha,
        active=ROW_NAME in {op.hover, op.editing}, editing=op.editing == ROW_NAME,
        edit_text=op.edit_text, edit_fresh=op.edit_fresh)
    enabled = segment_enabled(op, ROW_COLLECTION)
    draw_picker(
        shader, c, lay, ROW_TARGET, "Goes to", target_collection_text(op), s, alpha,
        pick=enabled and op.collection_mode == 'SOURCE', hovered=op.hover == ROW_TARGET,
        enabled=enabled)
    for key, (_label, attr, _items) in SEGMENTS.items():
        current = getattr(op, attr)
        labels = None
        if key == ROW_COLLECTION and current == 'ACTIVE':
            # The collection the extract will go to, instead of a bare "Active".
            labels = {'ACTIVE': _active_collection_name()}
        draw_segments(shader, c, lay, key, SEGMENTS, current, s, alpha, hover=op.hover,
                      enabled=segment_enabled(op, key), labels=labels)
    draw_toggle(shader, c, lay, ROW_PREVIEW, "Preview", op.preview,
                "Thickness loops on the extract", s, alpha, hovered=op.hover == ROW_PREVIEW)
    draw_header_buttons(shader, c, lay, s, alpha, op.hover)

    # The key-hint line at the bottom; contextual while a value is being typed.
    if op.editing is not None:
        help_text = pgettext_iface("Enter — apply · Esc — keep")
    else:
        help_text = pgettext_iface(
            "Enter — accept · Esc — cancel · Double-click — reset · "
            "Drag the title — move the panel")
    draw_help(c, lay, help_text, s, alpha)
    gpu.state.blend_set('NONE')
