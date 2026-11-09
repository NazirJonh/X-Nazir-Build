# SPDX-FileCopyrightText: 2026 Nazir
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
A small toolkit for the viewport panels of the sculpt mask / Extract tools: a multi-column layout,
drawing helpers and the widgets (sliders, text and picker fields, segmented buttons, toggles, a
header with Accept / Cancel), plus the saved size and place of a panel.

It knows nothing about any one tool. A tool describes its panel as data -- ``COLUMNS`` (the rows of
each column) and ``SEGMENTS`` (the choices of each segmented row) -- builds a :class:`PanelLayout`
from it, and draws with the ``draw_*`` functions, passing the state to show. Hit-testing against
the layout and the modal handling stay with the tool's operator.

Columns
    A list of columns, each a list of ``(kind, key)`` rows. ``kind`` is one of:

    - ``"slider"``: ``key`` names a :class:`Slider`; laid out in ``layout.sliders``.
    - ``"heading"``: ``key`` is the text; drawn by :func:`draw_headings`.
    - ``"seg"``: ``key`` names an entry of ``SEGMENTS``; the buttons are in ``layout.segments``.
    - anything else (``"name"``, ``"target"``, ``"toggle"`` ...): a plain full-column row in
      ``layout.rows[key]`` for the tool to fill with a field or a toggle.

Segments
    ``{key: (label, attribute, ((value, text), ...))}``. A ``None`` label lets the buttons span the
    whole column, for a row set under its own heading.

The panel's own size (the UI Scale slider) is kept here as a multiplier over Blender's Resolution
Scale; :func:`ui_scale` is what every pixel size is multiplied by. A panel's size and place are
saved in the user preferences (#PanelPlacementPrefs, attached to WindowManager by the tool).
"""

import math
from collections import namedtuple

import blf
import bpy
import gpu
from bpy.props import FloatProperty
from bpy.app.translations import pgettext_iface
from gpu_extras.batch import batch_for_shader

# Unscaled pixels.
COL_W = 280
PAD = 12
ROW_H = 22
LABEL_W = 92
VALUE_W = 44
TRACK_H = 4
KNOB_R = 6
# A drag this close to the middle snaps to "nothing applied".
SNAP = 5
BUTTON_H = 26
TITLE_SIZE = 12
TEXT_SIZE = 11
CORNER = 4
# Room left below for the mask actions bar.
BAR_ROOM = 56
FADE_TIME = 0.14

# `neutral` is "nothing applied": where each slider starts, and what it fills from. Typed values
# may go from `typed_min` to `typed_max`. `fmt` drives display and typing.
Slider = namedtuple("Slider", "key label min max neutral fmt typed_min typed_max")


# -- panel size and place ---------------------------------------------------------------------

# The panel's own scale on top of Blender's Resolution Scale (the UI Scale slider).
_panel_scale = 1.0


def set_panel_scale(value):
    global _panel_scale
    _panel_scale = min(max(float(value), 0.5), 2.0)


def ui_scale():
    return bpy.context.preferences.system.ui_scale * _panel_scale


# -- panel place in the user preferences ---

class PanelPlacementPrefs(bpy.types.PropertyGroup):
    """A saved size and place of a tool's viewport panel, kept in the user preferences (attached
    to #bpy.types.WindowManager by the tool's package, not in a side file). One shared group
    serves the tools that exist; a second panel keys its own values inside its operator instead.
    """

    ui_scale: FloatProperty(name="UI Scale", default=1.0, min=0.5, max=2.0)
    offset_x: FloatProperty(name="Offset X", default=0.0)
    offset_y: FloatProperty(name="Offset Y", default=0.0)


# -- layout -----------------------------------------------------------------------------------

class PanelLayout:
    """Where a panel's parts are, in region pixels. ``offset`` is how far the user dragged the
    panel from its home place; the clamped value is kept in ``self.offset``."""

    def __init__(self, area, region, columns, segments, offset=(0.0, 0.0)):
        s = ui_scale()
        pad = PAD * s
        row = ROW_H * s
        count = len(columns)
        col_w = min(COL_W * s, max((region.width - (count + 1) * pad) / count, 120 * s))
        w = count * col_w + (count + 1) * pad
        # A narrow panel wraps the key hints on two lines.
        self.help_h = row if w >= 560 * s else 2 * row
        button_h = BUTTON_H * s
        # The header holds the grip, the title and, on the right, Cancel and Accept.
        head_h = button_h + pad
        rows = max(len(column) for column in columns)
        h = head_h + pad / 2 + rows * row + pad / 2 + self.help_h + pad
        home_x = (region.width - w) / 2
        home_y = BAR_ROOM * s
        x = min(max(home_x + offset[0], 0.0), max(region.width - w, 0.0))
        y = min(max(home_y + offset[1], 0.0), max(region.height - h, 0.0))
        self.offset = (x - home_x, y - home_y)
        self.panel = (x, y, w, h)
        # The grip (a block of dots) marks the strip that drags the panel.
        self.grip = (x + pad, y + h - head_h / 2)
        self.title = (x + pad + 20 * s, y + h - head_h / 2)
        top = y + h - head_h - pad / 2
        # Cancel and Accept, flush right in the header.
        bw = 84 * s
        by = y + h - head_h + pad / 2
        self.buttons = {
            "ACCEPT": (x + w - pad - bw, by, bw, button_h),
            "CANCEL": (x + w - pad - 2 * bw - pad / 2, by, bw, button_h),
        }
        # The header up to the buttons drags the panel (and resets the values on a double click).
        self.title_rect = (x, y + h - head_h, self.buttons["CANCEL"][0] - x - pad / 2, head_h)

        self.sliders = {}
        self.rows = {}
        self.segments = {}
        self.seg_labels = {}
        self.headings = []
        for ci, column in enumerate(columns):
            cx = x + pad + ci * (col_w + pad)
            for ri, (kind, key) in enumerate(column):
                cy = top - row * (ri + 0.5)
                if kind == "slider":
                    track_x0 = cx + LABEL_W * s + KNOB_R * s
                    track_x1 = cx + col_w - VALUE_W * s - KNOB_R * s - 4 * s
                    self.sliders[key] = {
                        "label": (cx, cy),
                        "track": (track_x0, track_x1, cy),
                        "value": (cx + col_w - 4 * s, cy),
                        # Clicking the number types a value; the rest of the row drags.
                        "field": (cx + col_w - VALUE_W * s, cy - row / 2 + 2 * s,
                                  VALUE_W * s, row - 4 * s),
                        "hit": (cx, cy - row / 2, col_w - VALUE_W * s, row),
                    }
                elif kind == "heading":
                    self.headings.append((key, (cx, cy)))
                elif kind == "seg":
                    items = segments[key][2]
                    side = LABEL_W * s if segments[key][0] else 0.0
                    sx = cx + side
                    sw = (col_w - side) / len(items)
                    self.seg_labels[key] = (cx, cy)
                    self.segments[key] = [
                        (value, (sx + i * sw, cy - row / 2 + 2 * s, sw, row - 4 * s))
                        for i, (value, _text) in enumerate(items)]
                else:
                    self.rows[key] = (cx, cy - row / 2, col_w, row)
        self.col_w = col_w
        # (center_x, bottom_baseline_y) of the help text; it is drawn upward from here.
        self.help_anchor = (x + w / 2, y + pad + row / 2)
        self.pad = pad
        self.row = row


def hit(rect, mx, my):
    x, y, w, h = rect
    return x <= mx <= x + w and y <= my <= y + h


# -- drawing primitives -----------------------------------------------------------------------

def rounded_rect(x, y, w, h, r, segments=6):
    r = min(r, w / 2, h / 2)
    corners = ((x + w - r, y + h - r, 0.0), (x + r, y + h - r, 0.5 * math.pi),
               (x + r, y + r, math.pi), (x + w - r, y + r, 1.5 * math.pi))
    outline = []
    for cx, cy, start in corners:
        for i in range(segments + 1):
            a = start + 0.5 * math.pi * i / segments
            outline.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    center = (x + w / 2, y + h / 2)
    tris = []
    for i in range(len(outline)):
        tris += (center, outline[i], outline[(i + 1) % len(outline)])
    return tris, outline


def rect(x, y, w, h):
    return [(x, y), (x + w, y), (x + w, y + h), (x, y), (x + w, y + h), (x, y + h)]


def shape(shader, kind, coords, color, alpha):
    if kind == 'LINE_LOOP':
        # Closed by hand: the GPU module doesn't draw a loop's closing segment.
        kind, coords = 'LINE_STRIP', list(coords) + [coords[0]]
    gpu.state.blend_set('ALPHA')
    batch = batch_for_shader(shader, kind, {"pos": coords})
    shader.uniform_float("color", (*color[:3], color[3] * alpha))
    batch.draw(shader)


def text(string, x, y, align, size, color, alpha):
    blf.size(0, size)
    w, _h = blf.dimensions(0, string)
    cap = blf.dimensions(0, "H")[1]
    x -= {'LEFT': 0, 'CENTER': w / 2, 'RIGHT': w}[align]
    blf.position(0, round(x), round(y - cap / 2), 0)
    blf.color(0, *color[:3], color[3] * alpha)
    blf.draw(0, string)


def fit(string, max_w, size):
    """`string` cut with an ellipsis to fit `max_w` pixels."""
    blf.size(0, size)
    if blf.dimensions(0, string)[0] <= max_w:
        return string
    while len(string) > 1 and blf.dimensions(0, string + "…")[0] > max_w:
        string = string[:-1]
    return string + "…"


def wrap_lines(string, max_w, size):
    """Split `string` into lines no wider than `max_w` (word wrap), for the help band. Falls back
    to a single truncated line if even one word is too wide, so nothing leaves the panel."""
    blf.size(0, size)
    words = string.split()
    lines, line = [], ""
    for word in words:
        candidate = (line + " " + word).strip()
        if line and blf.dimensions(0, candidate)[0] > max_w:
            lines.append(line)
            line = word
        else:
            line = candidate
    if line:
        lines.append(line)
    return lines or [string]


# -- widgets ----------------------------------------------------------------------------------

def draw_frame(shader, c, lay, s, alpha, title, dragging):
    """The panel background and outline, the grip and the title. The grip is a block of dots, like
    Blender's GRIP icon (an icon cannot be drawn from a draw handler); brighter while dragging."""
    tris, outline = rounded_rect(*lay.panel, CORNER * s)
    shape(shader, 'TRIS', tris, c["PANEL"], alpha)
    shape(shader, 'LINE_LOOP', outline, c["OUTLINE"], alpha)
    gx, gy = lay.grip
    step = 5 * s
    dot = 1.3 * s
    dots = []
    for i in range(2):
        for j in range(3):
            cx = gx + i * step + dot
            cy = gy + (1 - j) * step
            dots += rect(cx - dot, cy - dot, 2 * dot, 2 * dot)
    shape(shader, 'TRIS', dots, c["TEXT"] if dragging else c["DIM_TEXT"], alpha)
    text(title, *lay.title, 'LEFT', TITLE_SIZE * s, c["TEXT"], alpha)


def draw_headings(c, lay, s, alpha):
    for string, (hx, hy) in lay.headings:
        text(pgettext_iface(string), hx, hy, 'LEFT', TEXT_SIZE * s, c["TEXT"], alpha)


def draw_header_buttons(shader, c, lay, s, alpha, hover):
    """Accept and Cancel, flush right in the header; `hover` is the hovered button key."""
    for identifier, label, color, hover_color in (
            ("ACCEPT", "Accept", c["ACCEPT"], c["ACCEPT_HOVER"]),
            ("CANCEL", "Cancel", c["CANCEL"], c["CANCEL_HOVER"])):
        x, y, w, h = lay.buttons[identifier]
        shape(shader, 'TRIS', rounded_rect(x, y, w, h, 4 * s)[0],
              hover_color if hover == identifier else color, alpha)
        text(pgettext_iface(label), x + w / 2, y + h / 2, 'CENTER', TEXT_SIZE * s, c["TEXT"], alpha)


def draw_help(c, lay, string, s, alpha):
    """The key-hint line at the bottom, wrapped to the panel."""
    size = (TEXT_SIZE - 1) * s
    lines = wrap_lines(string, lay.panel[2] - 2 * lay.pad, size)
    cx, base_y = lay.help_anchor
    for i, line in enumerate(reversed(lines)):
        text(line, cx, base_y + i * lay.row, 'CENTER', size, c["DIM_TEXT"], alpha)


def draw_slider(shader, c, lay, slider, value, s, alpha, *, active, editing, edit_text,
                edit_fresh, field_hovered):
    """A slider row: label, track with a fill from its neutral point, knob and the number box.
    While `editing`, the box shows `edit_text` (all selected when `edit_fresh`)."""
    lo, hi = slider.min, slider.max
    parts = lay.sliders[slider.key]
    text(pgettext_iface(slider.label), *parts["label"], 'LEFT', TEXT_SIZE * s,
         c["TEXT"] if active else c["DIM_TEXT"], alpha)
    x0, x1, cy = parts["track"]
    # Typed values can be past the ends: the knob stops there.
    t = min(max((value - lo) / (hi - lo), 0.0), 1.0)
    th = TRACK_H * s
    shape(shader, 'TRIS', rounded_rect(x0, cy - th / 2, x1 - x0, th, th / 2)[0], c["TRACK"], alpha)
    # Filled from "nothing applied", so each shows which way it goes.
    neutral = (slider.neutral - lo) / (hi - lo)
    a, b = sorted((x0 + (x1 - x0) * neutral, x0 + (x1 - x0) * t))
    if b - a > 0.5:
        shape(shader, 'TRIS', rounded_rect(a, cy - th / 2, b - a, th, th / 2)[0], c["FILL"], alpha)
    kx = x0 + (x1 - x0) * t
    knob = [(kx + KNOB_R * s * math.cos(2 * math.pi * i / 16),
             cy + KNOB_R * s * math.sin(2 * math.pi * i / 16)) for i in range(16)]
    fan = []
    for i in range(16):
        fan += ((kx, cy), knob[i], knob[(i + 1) % 16])
    shape(shader, 'TRIS', fan, c["KNOB"], alpha)
    fx, fy, fw, fh = parts["field"]
    tris, outline = rounded_rect(fx, fy, fw, fh, 3 * s)
    if editing:
        shape(shader, 'TRIS', tris, c["FIELD"], alpha)
        shape(shader, 'LINE_LOOP', outline, c["FIELD_OUTLINE"], alpha)
        string = edit_text + ("" if edit_fresh else "|")
        if edit_fresh:
            # All selected: typing replaces it.
            blf.size(0, TEXT_SIZE * s)
            tw = blf.dimensions(0, string)[0]
            vx, _vy = parts["value"]
            shape(shader, 'TRIS', rect(vx - tw - 1 * s, fy + 3 * s, tw + 2 * s, fh - 6 * s),
                  c["SELECTION"], alpha)
        text(string, *parts["value"], 'RIGHT', TEXT_SIZE * s, c["TEXT"], alpha)
    else:
        shape(shader, 'TRIS', tris, c["FIELD_IDLE"], alpha)
        shape(shader, 'LINE_LOOP', outline,
              c["FIELD_HOVER_OUTLINE"] if field_hovered else c["FIELD_IDLE_OUTLINE"], alpha)
        text(slider.fmt.format(value), *parts["value"], 'RIGHT', TEXT_SIZE * s,
             c["TEXT"] if field_hovered or abs(value - slider.neutral) >= 1e-9 else c["DIM_TEXT"],
             alpha)


def draw_text_field(shader, c, lay, key, label, value, s, alpha, *, active, editing, edit_text,
                    edit_fresh):
    """A row with a label and an editable text field filling the rest (the `rows[key]` rect)."""
    x, y, w, h = lay.rows[key]
    text(pgettext_iface(label), x, y + h / 2, 'LEFT', TEXT_SIZE * s,
         c["TEXT"] if active else c["DIM_TEXT"], alpha)
    fx = x + LABEL_W * s
    fw = w - LABEL_W * s
    tris, outline = rounded_rect(fx, y + 2 * s, fw, h - 4 * s, 3 * s)
    shape(shader, 'TRIS', tris, c["FIELD"] if editing else c["FIELD_IDLE"], alpha)
    shape(shader, 'LINE_LOOP', outline,
          c["FIELD_OUTLINE"] if editing else c["FIELD_IDLE_OUTLINE"], alpha)
    if editing:
        if edit_fresh:
            # All selected: typing replaces it -- highlight the current text.
            blf.size(0, TEXT_SIZE * s)
            tw = min(blf.dimensions(0, edit_text)[0], fw - 8 * s)
            shape(shader, 'TRIS', rect(fx + 4 * s, y + 4 * s, tw + 2 * s, h - 8 * s),
                  c["SELECTION"], alpha)
        string = edit_text + ("" if edit_fresh else "|")
    else:
        string = value
    text(fit(string, fw - 12 * s, TEXT_SIZE * s), fx + 6 * s, y + h / 2, 'LEFT', TEXT_SIZE * s,
         c["TEXT"], alpha)


def draw_picker(shader, c, lay, key, label, value, s, alpha, *, pick, hovered, enabled=True):
    """A row with a label and a read-only field showing `value`; with `pick` it is clickable and
    carries a drop arrow. `enabled` False dims the whole row."""
    x, y, w, h = lay.rows[key]
    row_alpha = alpha if enabled else alpha * 0.45
    text(pgettext_iface(label), x, y + h / 2, 'LEFT', TEXT_SIZE * s,
         c["TEXT"] if hovered and pick else c["DIM_TEXT"], row_alpha)
    fx = x + LABEL_W * s
    fw = w - LABEL_W * s
    tris, outline = rounded_rect(fx, y + 2 * s, fw, h - 4 * s, 3 * s)
    shape(shader, 'TRIS', tris, c["FIELD_IDLE"], row_alpha)
    shape(shader, 'LINE_LOOP', outline,
          c["FIELD_HOVER_OUTLINE"] if hovered and pick else c["FIELD_IDLE_OUTLINE"], row_alpha)
    room = fw - (24 if pick else 12) * s
    text(fit(value, room, TEXT_SIZE * s), fx + 6 * s, y + h / 2, 'LEFT', TEXT_SIZE * s,
         c["TEXT"], row_alpha)
    if pick:
        text("▾", fx + fw - 10 * s, y + h / 2, 'CENTER', TEXT_SIZE * s, c["DIM_TEXT"],
             row_alpha)


def draw_segments(shader, c, lay, key, segments, current, s, alpha, *, hover, enabled=True,
                  labels=None):
    """A row of buttons, one per choice, all visible; the `current` choice is filled. `hover` is
    the hovered ("SEG", key, value) or anything else; `labels` replaces the text of some values."""
    label, _attr, items = segments[key]
    names = dict(items)
    lx, ly = lay.seg_labels[key]
    hovered_row = isinstance(hover, tuple) and hover[0] == "SEG" and hover[1] == key
    if label:
        text(pgettext_iface(label), lx, ly, 'LEFT', TEXT_SIZE * s,
             c["TEXT"] if hovered_row and enabled else c["DIM_TEXT"], alpha)
    row_alpha = alpha if enabled else alpha * 0.45
    for value, (x, y, w, h) in lay.segments[key]:
        string = (labels or {}).get(value) or pgettext_iface(names[value])
        selected = value == current
        if selected:
            fill = c["FIELD_HOVER_OUTLINE"]
        elif hovered_row and hover[2] == value:
            fill = c["CANCEL_HOVER"]
        else:
            fill = c["FIELD_IDLE"]
        tris, outline = rounded_rect(x + 1 * s, y, w - 2 * s, h, 3 * s)
        shape(shader, 'TRIS', tris, fill, row_alpha)
        shape(shader, 'LINE_LOOP', outline, c["FIELD_IDLE_OUTLINE"], row_alpha)
        text(fit(string, w - 8 * s, TEXT_SIZE * s), x + w / 2, y + h / 2, 'CENTER',
             TEXT_SIZE * s, c["TEXT_SEL"] if selected else c["TEXT"], row_alpha)


def draw_toggle(shader, c, lay, key, label, checked, note, s, alpha, *, hovered):
    """A row with a label, a checkbox and a short dim note after it."""
    x, y, w, h = lay.rows[key]
    text(pgettext_iface(label), x, y + h / 2, 'LEFT', TEXT_SIZE * s,
         c["TEXT"] if hovered else c["DIM_TEXT"], alpha)
    box = h - 8 * s
    bx = x + LABEL_W * s
    by = y + (h - box) / 2
    tris, outline = rounded_rect(bx, by, box, box, 2 * s)
    shape(shader, 'TRIS', tris, c["FIELD_IDLE"], alpha)
    shape(shader, 'LINE_LOOP', outline, c["FIELD_IDLE_OUTLINE"], alpha)
    if checked:
        text("✔", bx + box / 2, by + box / 2, 'CENTER', box * 0.9, c["HINT_ADD"], alpha)
    if note:
        text(pgettext_iface(note), bx + box + 8 * s, y + h / 2, 'LEFT', (TEXT_SIZE - 1) * s,
             c["DIM_TEXT"], alpha)
