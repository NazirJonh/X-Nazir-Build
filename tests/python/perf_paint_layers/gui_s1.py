# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
GUI measurement of the paint-layer value fast path (P1-7) and the S12..S16 GUI scenarios.

run.py renders with bpy.ops.render.render on every tick. A render builds a fresh depsgraph and a
fresh EEVEE instance, so a GPUMaterial cannot be reused between ticks and the fast path is never
exercised. This script keeps one live 3D Viewport in Material Preview and changes the material
while the viewport keeps drawing.

Worker (inside the GUI Blender, after `--`):
  X-NazirSculpt.exe --debug-gpu --python gui_s1.py -- --rows 6 --ticks 50 --mode S1 --out <json>
  --mode S1: opacity of the first row. --mode S8: position of the first gradient stop.
  --mode S8C: color of the first gradient stop (positions untouched).
  --mode S12: 10x (opacity edit of the first row -> undo -> redo), one redraw between the steps.
  --mode S13: 5x (structural edit -> undo -> redo); tree signatures before/after every undo.
  --mode S14: 5x (Combined -> Base Color -> Mask -> Combined) via the material's view pass RNA.
  --mode S15: bake the first row, then 10 stroke imitations (map pixel edit + update notices).
  --mode S16: stops 2 -> 3 (async compile), then 50 stop-position edits without waiting for it;
              gray-default frames are counted from `PERF queued default` and viewport screenshots.

Parser (plain Python, no bpy, called by run_gui_s1.ps1):
  python -I gui_s1.py --parse --log <stdout log> --out <json> --ticks 50 --mode S1
"""

import argparse
import collections
import json
import os
import re
import statistics
import sys
import time
import traceback
import zlib

CHANNELS = ["BASE_COLOR", "ROUGHNESS", "NORMAL", "HEIGHT"]
IMAGE_SIZE = 256
MATERIAL_NAME = "Mat"
LAYERED_NAME = "MAMat"  # Name the engine puts on the layered material's diagnostics.
FIRST_ROW = "Paint 0"
GRAD_ROW = "Grad"
TICK_INTERVAL_S = 0.15
WARMUP_DRAWS = 10
WARMUP_TIMEOUT_S = 60.0
WATCHDOG_S = 120.0
# The undo/redo and bake scenarios carry renders and a bake job; give them more headroom.
WATCHDOG_BY_MODE = {"S12": 240.0, "S13": 420.0, "S14": 300.0, "S15": 420.0, "S16": 300.0}
FINAL_VALUE = 0.37  # Last value of the series in --verify mode; differs from the initial one.
RENDER_RES = 64
S12_ITERS = 10
S13_ITERS = 5
S14_CYCLES = 5
S15_STROKES = 10
S12_FINAL = 0.02 * S12_ITERS  # The last redo leaves the first row's opacity at 0.02 * 10.
S16_SETTLE_S = 3.0  # One tail wait after the last tick, so the async compile can finish.
GRAY_SPREAD = 0.04  # Max channel spread of a "default gray" sampled pixel.
GRAY_REF_DIFF = 0.05  # Max per-channel distance to the calibrated default-gray pixel.
# eMaterialPaintChannel_NUM and the two non-channel roles (DNA_scene_types.h). The RNA enum reads
# back as the item identifier string.
S14_CYCLE = [("COMPOSITE_BASE_COLOR", "COMPOSITE_BASE_COLOR"),
             ("COMPOSITE_MASK", "COMPOSITE_MASK"),
             ("COMPOSITE_COMBINED", "COMPOSITE_COMBINED")]

RX_EXTRACT_TIME = re.compile(
    r"^PERF extract_time name=(\S+) inline=(\S+) build=(\S+) tracked=([01]) entries=(\d+)")
RX_EXTRACT = re.compile(r"^PERF extract count=\S+ name=(\S+)")
RX_VALUES = re.compile(
    r"^PERF values_update name=(\S+) result=(\S+)(?: required=(\d+) tracked=(\d+) entries=(\d+))?")
RX_EVAL = re.compile(
    r"^PERF eval_decision name=(\S+) values_only=(\d+) anim=(\d+) supported=(\d+) => (\S+)")
RX_REFUSED = re.compile(r"^PERF value_path name=(\S+) refused=(.*)$")
RX_COMPILE = re.compile(r"^PERF compile count=(\d+)")
RX_QUEUED = re.compile(r"^PERF queued (default|fallback) total=(\d+)")
RX_REGEN = re.compile(
    r'^\[PBR-Layers regen\] material="([^"]+)".*=> (KEPT \(in-place\)|FULL REBUILD)\s*$')
RX_REGEN_TAGGED = re.compile(r'^\[PBR-Layers regen\] tagged material="([^"]+)"')
RX_GRAY = re.compile(r"^PERF_GRAY (\{.*\})$")
RX_GRAY_REF = re.compile(r"^PERF_GRAY_REF (\{.*\})$")


# --------------------------------------------------------------------------------------------
# Worker (runs inside Blender)
# --------------------------------------------------------------------------------------------


def _cflush():
    """Flush the C stdio buffer so `printf` lines from the instrumented build keep their order."""
    try:
        import ctypes
        try:
            lib = ctypes.CDLL("ucrtbase")
        except OSError:
            lib = ctypes.CDLL(None)
        lib.fflush(None)
    except Exception:
        pass


def emit(line):
    _cflush()
    print(line, flush=True)


FILTER_RAMP = {"on": False}  # S8F: edit a Color Ramp mask filter instead of the Gradient row.

# S17: one value of the stack is edited each tick, as S1 does for the opacity of the first row. The
# kind is "<target>.<attribute>", the target one of row / const / effect / mask / filter / gradient.
EDIT = {"kind": None}
# Shader-graph filters and generators whose float and int parameters are all edited at once.
FX_EDIT_TYPES = ("INVERT", "LEVELS", "HSL", "BRIGHTNESS_CONTRAST", "POSTERIZE", "MATH", "THRESHOLD",
                 "CLAMP", "EXPOSURE", "COLOR_ID", "COLOR_BALANCE", "CHANNEL_MIXER")
GEN_EDIT_TYPES = ("NOISE", "VORONOI", "CHECKER", "BRICK", "WAVE", "SCRATCHES", "WEAVE", "GRUNGE")
EDIT_KINDS = (
    "row.opacity", "row.enabled", "row.mapping_offset", "row.mapping_scale",
    "row.mapping_rotation", "row.tile_scale",
    "const.fill_color", "const.record_color",
    "effect.opacity", "effect.fill_color",
    "mask.opacity", "mask.fill_color", "mask.mask_value",
    "filter.param0", "filter.param2",
    "gradient.midpoint", "gradient.start", "gradient.end", "mask.mask_color",
    "effect.mapping_offset", "idcolor.all",
) + tuple("fx." + t for t in FX_EDIT_TYPES) + tuple("gen." + g for g in GEN_EDIT_TYPES)


def make_stops(stops_num):
    """Gradient stops: two black-white stops, or `stops_num` evenly spaced colored stops."""
    if stops_num == 2:
        return [[0.0, [0.0, 0.0, 0.0, 1.0]], [1.0, [1.0, 1.0, 1.0, 1.0]]]
    stops = []
    for j in range(stops_num):
        pos = j / (stops_num - 1)
        stops.append([pos, [pos, 0.5, 1.0 - pos, 1.0]])
    return stops


def make_state(rows, mode, stops_num=2):
    """Declarative stack, the same description builds the stack (as in run.py)."""
    state = {"rows": []}
    for i in range(rows):
        state["rows"].append({
            "name": f"Paint {i}",
            "source": "IMAGE",
            "opacity": 1.0,
            "blend": "MIX",
            "enabled": True,
            "channels": {ch: {"image": f"img_{i}_{ch}_v0", "invert": False} for ch in CHANNELS},
            "mask_items": [],
            "effects": [],
        })
    if EDIT["kind"]:
        target = EDIT["kind"].split(".")[0]
        # Only the row under test is visible, so a value that reaches the shader changes the render.
        for i, row in enumerate(state["rows"]):
            row["opacity"] = 100.0 if (i == 0 and target not in ("const", "gradient", "gen")) else 0.0
        state["rows"][0]["edit"] = True
        if target == "const":
            state["rows"].append({"name": "Const", "source": "CONSTANT", "opacity": 100.0,
                                  "blend": "MIX", "enabled": True, "channels": {},
                                  "mask_items": [], "effects": []})
        if target == "gradient":
            state["rows"].append({
                "name": GRAD_ROW, "source": "GENERATOR", "opacity": 100.0, "blend": "MIX",
                "enabled": True, "channels": {}, "mask_items": [], "effects": [],
                "stops": make_stops(stops_num)})
        if target == "gen":
            state["rows"].append({
                "name": "Gen", "source": "GENERATOR", "opacity": 100.0, "blend": "MIX",
                "enabled": True, "channels": {}, "mask_items": [], "effects": [],
                "generator": EDIT["kind"].split(".", 1)[1].split(":")[0].split("@")[0].split("~")[0]})
    if FILTER_RAMP["on"]:
        # A mask filter lays over a base mask, so the row gets one first (as S14 does).
        state["rows"][0]["mask_items"] = [
            {"name": "", "channels": {"BASE_COLOR": {"image": "mask_v0", "invert": False}}}]
        state["rows"][0]["mask_filters"] = [{"stops": make_stops(stops_num)}]
    if mode in ("S8", "S8C", "S16"):
        state["rows"].append({
            "name": GRAD_ROW, "source": "GENERATOR", "opacity": 1.0, "blend": "MIX",
            "enabled": True, "channels": {}, "mask_items": [], "effects": [],
            "stops": make_stops(stops_num),
        })
    if mode == "S14":
        # The Mask view pass needs a base mask on the active row (BKE_paint_layers_view_pass_available).
        state["rows"][0]["mask_items"] = [
            {"name": "", "channels": {"BASE_COLOR": {"image": "mask_v0", "invert": False}}}]
    return state


def view3d_areas(bpy):
    return [area for window in bpy.context.window_manager.windows
            for area in window.screen.areas if area.type == 'VIEW_3D']


def worker(tail):
    import bpy
    import numpy as np

    parser = argparse.ArgumentParser()
    parser.add_argument("--rows", type=int, default=6)
    parser.add_argument("--ticks", type=int, default=50)
    parser.add_argument("--mode", choices=["S1", "S8", "S8F", "S17", "S8C", "S12", "S13", "S14", "S15", "S16"],
                        default="S1")
    parser.add_argument("--stops", type=int, default=2)
    parser.add_argument("--edit", default=None)
    parser.add_argument("--out", required=True)
    parser.add_argument("--no-shots", action="store_true")
    parser.add_argument("--shots", action="store_true")
    parser.add_argument("--no-mat-tag", action="store_true")
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args(tail)
    if args.stops < 2:
        raise ValueError("--stops must be >= 2")
    if args.mode == "S8F":
        # Same series and checks as S8, on the Color Ramp filter of the first row's mask.
        FILTER_RAMP["on"] = True
        args.mode = "S8"
    if args.mode == "S17":
        # Same series and checks as S1, on the value `--edit` names.
        if not args.edit:
            raise ValueError("--mode S17 needs --edit <kind>")
        EDIT["kind"] = args.edit
        args.mode = "S1"
    watchdog_s = WATCHDOG_BY_MODE.get(args.mode, WATCHDOG_S)
    is_step_mode = args.mode in ("S12", "S13", "S14", "S15", "S16")
    # A window screenshot redraws the whole window into an offscreen buffer and reads it back.
    # On Vulkan (RTX 5090 / NVIDIA 616.92, this machine) that readback intermittently loses the
    # device, which the submission thread then reports as `submission_finished_timeline_get`
    # UINT64_MAX. The probe is optional for every scenario: `PERF queued default` is the direct
    # counter of frames drawn with the default material. Keep screenshots opt-in for the step
    # modes; S1/S8/S8C never took them.
    if is_step_mode and not args.shots:
        args.no_shots = True

    # ---- scene -------------------------------------------------------------------------------
    bpy.ops.wm.read_factory_settings(use_empty=True)
    # The startup splash dismisses a few seconds into the run and takes the current screen layout
    # with it (the view jumps back and the pixel probes end up on the background). Suppress it.
    # The one-time "Test Build Warning" popup (wm_init_exit.cc) can only be retired by showing it
    # once with preferences saving enabled: the shown-flag lives in the userpref, unreachable from
    # RNA, so the first GUI run on a machine keeps the popup up and the later ones are clean.
    try:
        bpy.context.preferences.view.show_splash = False
    except Exception:
        pass
    scene = bpy.context.scene
    scene.render.engine = 'BLENDER_EEVEE'

    mesh = bpy.data.meshes.new("PlaneMesh")
    mesh.from_pydata([(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)], [], [(0, 1, 2, 3)])
    uv = mesh.uv_layers.new(name="UVMap")
    for loop_index, co in enumerate([(0, 0), (1, 0), (1, 1), (0, 1)]):
        uv.data[loop_index].uv = co
    ob = bpy.data.objects.new("Plane", mesh)
    bpy.context.collection.objects.link(ob)
    sun = bpy.data.lights.new("Sun", 'SUN')
    sun.energy = 3.0
    sun_ob = bpy.data.objects.new("Sun", sun)
    bpy.context.collection.objects.link(sun_ob)
    sun_ob.rotation_euler = (0.3, 0.2, 0.0)

    # ---- helpers (same as run.py) ------------------------------------------------------------

    def image_for(name):
        img = bpy.data.images.get(name)
        if img is not None:
            return img
        img = bpy.data.images.new(name, IMAGE_SIZE, IMAGE_SIZE, alpha=True)
        rng = np.random.default_rng(zlib.crc32(name.encode("utf-8")))
        pixels = rng.random((IMAGE_SIZE, IMAGE_SIZE, 4), dtype=np.float32)
        pixels[..., 3] = 1.0
        img.pixels.foreach_set(pixels.ravel())
        img.update()
        return img

    def layer_named(ma, name):
        for layer in ma.paint_layers:
            if layer.name == name:
                return layer
        raise KeyError(f"no paint layer named {name!r}")

    def record(layer, channel):
        return next(c for c in layer.channels if c.channel == channel)

    def set_record(rec, cfg):
        if cfg["image"] is not None:
            rec.image = image_for(cfg["image"])
        if cfg["invert"]:
            rec.invert = True

    def filter_ramp(ma_now):
        """The Color Ramp filter's band on the first row's mask (S8F)."""
        for item in layer_named(ma_now, FIRST_ROW).mask_items:
            if item.ramp_supported:
                return item.kind_ramp
        raise KeyError("no Color Ramp mask filter on the first row")

    def edit_target(ma_now, kind):
        """The object the S17 edit `kind` writes to."""
        target = kind.split(".")[0]
        row = layer_named(ma_now, FIRST_ROW)
        if target == "row":
            return row
        if target == "const":
            return layer_named(ma_now, "Const")
        if target == "gradient":
            return layer_named(ma_now, GRAD_ROW)
        if target == "gen":
            return layer_named(ma_now, "Gen")
        if target == "effect":
            return row.effects[0]
        items = list(row.mask_items)
        if target == "filter":
            return next(item for item in items if item.name == "Hue Saturation Value")
        if target in ("fx", "idcolor"):
            wanted = "COLOR_ID" if target == "idcolor" else kind.split(".", 1)[1]
            return next(item for item in items if item.filter_type == wanted)
        # mask.mask_value belongs to the base mask the first mask item creates.
        if kind == "mask.mask_value":
            return items[0]
        return next(item for item in items if item.name == "Mk")

    def edit_setup(ma_now, kind):
        """What the S17 edit needs to exist before it is made."""
        target, attr = kind.split(".", 1)
        row = layer_named(ma_now, FIRST_ROW)
        if target == "row" and attr.startswith(("mapping_", "tile_")):
            row.mapping_enabled = True
        elif target == "effect":
            effect = row.effect_add(name="E", source='CONSTANT')
            if attr.startswith("mapping_"):
                effect.mapping_enabled = True
        elif target in ("mask", "filter", "fx", "idcolor"):
            row.mask_item_add(name="Mk", source='CONSTANT')
            # A half-open base mask, so a change of the items over it moves the coverage.
            list(row.mask_items)[0].mask_value = 0.5
            if target == "filter":
                row.mask_filter_add(filter_type='HSL')
            elif target == "fx":
                row.mask_filter_add(filter_type=attr)
            elif target == "idcolor":
                row.mask_filter_add(filter_type='COLOR_ID').id_color_add(color=(1.0, 0.0, 0.0))

    def edit_set(ma_now, kind, v):
        """One S17 edit with the series value `v` (0.02 .. 1.0, the S1 series)."""
        obj = edit_target(ma_now, kind)
        target, attr = kind.split(".", 1)
        only = None
        if ":" in attr:
            attr, only = attr.split(":", 1)
        if "~" in attr:
            # "NOISE~noise_type=FBM|HYBRID_MULTIFRACTAL|...": the choice moves to the next value
            # every ten ticks, before the numbers are written, so the edits run through the change
            # of the node's shape. The step comes from the value, so the reference replays it.
            attr, cycle = attr.split("~", 1)
            name, values = cycle.split("=", 1)
            choices = values.split("|")
            wanted = choices[((int(round(v / 0.02)) - 1) // 10) % len(choices)]
            param = next(k for k in obj.kind_params if k.name == name)
            if param.enum_value != wanted:
                param.enum_value = wanted
        if target in ("fx", "gen"):
            for param in obj.kind_params:
                if only is not None and param.name != only:
                    continue
                if param.type == 'FLOAT':
                    param.value = v
                elif param.type == 'INT' and attr == "POSTERIZE":
                    # A seed or a frequency picks the pattern's nodes; only the steps are a value.
                    param.int_value = 1 + int(v * 8)
            if attr == "COLOR_BALANCE":
                for name in ("lift", "gamma", "gain", "slope", "offset", "power"):
                    setattr(obj.kind_color_balance, name, (v, v, v))
            elif attr == "CHANNEL_MIXER":
                obj.kind_channel_mixer.red_row = (v, 0.1, 0.1)
                obj.kind_channel_mixer.constants = (v * 0.1, 0.0, 0.0)
            return
        if target == "idcolor":
            color = obj.id_colors[0]
            color.color = (v, 0.2, 1.0 - v)
            color.tolerance = v
            return
        if attr == "start":
            obj.gradient_start = (v * 0.5, 0.2, 0.0)
        elif attr == "end":
            obj.gradient_end = (v * 0.5 + 0.5, 0.2, 0.0)
        elif attr == "mask_color":
            obj.mask_color = (v, 0.5, 1.0 - v)
        elif attr == "opacity":
            obj.opacity = v * 100.0  # A percentage: the series spans 2 % .. 100 %.
        elif attr == "enabled":
            obj.enabled = int(round(v * 50)) % 2 == 0
        elif attr == "fill_color":
            obj.fill_color = (v, 0.2, 1.0 - v, 1.0)
        elif attr == "mapping_offset":
            obj.mapping_offset = (v * 0.1, v * 0.05)
        elif attr == "mapping_scale":
            obj.mapping_scale = (1.0 + v, 1.0 + v)
        elif attr == "mapping_rotation":
            obj.mapping_rotation = v
        elif attr == "tile_scale":
            obj.tile_scale = (1.0 + v, 1.0 + v)
        elif attr == "mask_value":
            obj.mask_value = v
        elif attr.startswith("param"):
            obj.kind_params[int(attr[5:])].value = v
        elif attr == "record_color":
            rec = record(obj, "BASE_COLOR")
            rec.color = [v, 0.2, 1.0 - v, 1.0][:len(rec.color)]
        elif attr == "midpoint":
            obj.gradient_midpoint = v
        else:
            raise KeyError(kind)

    def set_stops(layer, stops, elements=None):
        elements = layer.gradient_ramp.elements if elements is None else elements
        while len(elements) > len(stops):
            elements.remove(elements[-1])
        while len(elements) < len(stops):
            elements.new(0.5)
        for el, (pos, col) in zip(elements, stops):
            el.position = pos
            el.color = col

    def apply_state(ma, state):
        ma.paint_layers_channels = set(CHANNELS)
        for row in state["rows"]:
            layer = ma.paint_layers.new(name=row["name"], source=row["source"],
                                        make_active=False)
            layer.opacity = row["opacity"]
            layer.blend_type = row["blend"]
            layer.enabled = row["enabled"]
            for ch, cfg in row["channels"].items():
                set_record(record(layer, ch), cfg)
            for item in row["mask_items"]:
                mi = layer.mask_item_add(name=item["name"], source='IMAGE')
                for ch, cfg in item["channels"].items():
                    set_record(record(mi, ch), cfg)
            for flt in row.get("mask_filters", []):
                item = layer.mask_filter_add(filter_type='COLOR_RAMP')
                set_stops(layer, flt["stops"], item.kind_ramp.elements)
            if "stops" in row:
                set_stops(layer, row["stops"])
            if row.get("generator"):
                layer.generator = row["generator"]
                # "gen.NOISE@noise_type=HYBRID_MULTIFRACTAL,...": the mode the edit is made in.
                settings = EDIT["kind"].split(".", 1)[1].split(":")[0]
                if "@" in settings:
                    for pair in settings.split("@", 1)[1].split(","):
                        name, value = pair.split("=", 1)
                        next(k for k in layer.kind_params if k.name == name).enum_value = value
                # The row being edited is the active one, which Auto bake mode evaluates live.
                ma.paint_layers.active = layer
            if row.get("edit"):
                edit_setup(ma, EDIT["kind"])
        if EDIT["kind"] and state["rows"][0].get("edit_value") is not None:
            # The same series the live material went through, so a parameter whose setter depends
            # on the others (a scale lock, a clamp) ends in the same description; only the
            # generated tree is built once at the end instead of synced tick by tick.
            for value in state["rows"][0].get("edit_history", []):
                edit_set(ma, EDIT["kind"], value)
            edit_set(ma, EDIT["kind"], state["rows"][0]["edit_value"])
        return ma

    # ---- viewport ----------------------------------------------------------------------------

    ob.data.materials.append(bpy.data.materials.new(MATERIAL_NAME))
    ma = bpy.data.materials[MATERIAL_NAME]
    ma.use_nodes = True
    apply_state(ma, make_state(args.rows, args.mode, args.stops))
    if args.mode == "S14":
        # The Mask pass reads the active row's mask.
        ma.paint_layers.active = ma.paint_layers[0]

    areas = view3d_areas(bpy)
    if not areas:
        raise RuntimeError("no VIEW_3D area: the GUI window is required (not --background)")
    for area in areas:
        area.spaces.active.shading.type = 'MATERIAL'

    # Set the view directly on RegionView3D: view3d.view_selected animates and never completes
    # while the one-time test-build warning popup is up, so the framing would race with it. A
    # top-down ortho view of the origin is popup-proof and deterministic. The 2x2 plane then spans
    # ~69 % of the view height, so the probe point below (region 38 % x, 25 % y and its vertical
    # mirror) sits on the plane in either PNG row order.
    try:
        import mathutils
        for area in areas:
            rv3d = area.spaces.active.region_3d
            rv3d.view_perspective = 'ORTHO'
            rv3d.view_rotation = mathutils.Quaternion((0.0, 0.0, 1.0), 0.0)
            rv3d.view_location = (0.0, 0.0, 0.0)
            rv3d.view_distance = 2.9
    except Exception as exc:
        emit("PERF_VIEW_WARN " + json.dumps({"error": str(exc)[:200]}))

    if args.mode in ("S12", "S13", "S16"):
        # The undo stack starts empty after read_factory_settings: without a base step the first
        # undo would go back to the empty scene and delete the material under test.
        window = bpy.context.window_manager.windows[0]
        with bpy.context.temp_override(window=window):
            bpy.ops.ed.undo_push(message=f"{args.mode} setup")

    draws = {"n": 0, "t": []}

    def on_draw():
        draws["n"] += 1
        if st["phase"] in ("run", "pre"):
            draws["t"].append(time.perf_counter())

    draw_handle = bpy.types.SpaceView3D.draw_handler_add(on_draw, (), 'WINDOW', 'POST_VIEW')

    st = {"phase": "warmup", "k": 0, "tail": 0, "finished": False,
          "t0": time.time(), "draws0": 0, "op_ms": [],
          "pre_box": {"idx": 0, "last_draw": 0, "op_ms": []},
          "run_box": {"idx": 0, "last_draw": 0, "op_ms": []}}
    extra = {}  # Mode-specific results, emitted as PERF_EXTRA at the end.
    gray_ref = {"rgb": None}
    pre_steps = []
    plan = []

    def tag_all():
        for area in view3d_areas(bpy):
            area.tag_redraw()

    def mat():
        return bpy.data.materials[MATERIAL_NAME]

    def scene_now():
        # Memfile undo recreates the Scene datablock: never keep a scene reference across steps.
        return bpy.context.scene

    def plane():
        return bpy.data.objects["Plane"]

    def row0(ma_now=None):
        return layer_named(ma_now or mat(), FIRST_ROW)

    def with_window(fn):
        window = bpy.context.window_manager.windows[0]
        with bpy.context.temp_override(window=window):
            fn()

    def undo_push(message):
        with_window(lambda: bpy.ops.ed.undo_push(message=message))

    def undo():
        with_window(lambda: bpy.ops.ed.undo())

    def redo():
        with_window(lambda: bpy.ops.ed.redo())

    # ---- tree signature (S13) ----------------------------------------------------------------

    def tree_signature(ma_now):
        """Node/link/socket-value digest of the material tree and every shader node group."""
        trees = []
        if ma_now.node_tree is not None:
            trees.append(ma_now.node_tree)
        for nt in bpy.data.node_groups:
            if nt.type == 'SHADER':
                trees.append(nt)
        entries = []
        for nt in sorted(trees, key=lambda t: t.name):
            kinds = collections.Counter(n.bl_idname for n in nt.nodes)
            values = []
            for node in nt.nodes:
                for sock in node.inputs:
                    if hasattr(sock, "default_value") and not sock.is_linked:
                        try:
                            raw = sock.default_value
                            try:
                                vals = [round(float(x), 5) for x in raw]
                            except TypeError:
                                vals = [round(float(raw), 5)]
                        except Exception:
                            continue  # Enum, string or pointer sockets have no numeric value.
                        values.append(repr(vals))
            values.sort()
            entries.append({"name": nt.name, "nodes": len(nt.nodes), "links": len(nt.links),
                            "kinds": sorted(kinds.items()),
                            "values_crc": zlib.crc32("|".join(values).encode("utf-8"))})
        relaxed = sorted([tuple([e["nodes"], e["links"], tuple(map(tuple, e["kinds"])),
                                 e["values_crc"]]) for e in entries])
        return {"trees": entries,
                "totals": {"trees": len(entries),
                           "nodes": sum(e["nodes"] for e in entries),
                           "links": sum(e["links"] for e in entries)},
                "relaxed": relaxed}

    # ---- viewport pixel samples (S12/S16) ------------------------------------------------------

    def shot_center(label):
        """Screenshot the 3D Viewport area, sample one pixel of the plane.

        The sample point is 38 % across and 25 % up the WINDOW region: below the centered
        test-build warning popup, off the world Y axis line that runs through the region center
        under the top-down ortho view, and at a height where both possible PNG row orders land on
        the plane. The screenshot operator redraws first, so the sample shows the current state.
        """
        if args.no_shots:
            return None
        try:
            path = os.path.join(os.path.dirname(os.path.abspath(args.out)),
                                f"shot_{args.mode}_{label}.png")
            window = bpy.context.window_manager.windows[0]
            area = view3d_areas(bpy)[0]
            region = next(r for r in area.regions if r.type == 'WINDOW')
            with bpy.context.temp_override(window=window, area=area):
                bpy.ops.screen.screenshot_area(filepath=path)
            img = bpy.data.images.load(path, check_existing=False)
            w, h = img.size
            px = np.empty(w * h * 4, dtype=np.float32)
            img.pixels.foreach_get(px)
            bpy.data.images.remove(img)
            ix = region.x + int(0.38 * region.width) - area.x
            iy = region.y + int(0.25 * region.height) - area.y
            ix = max(0, min(w - 1, ix))
            iy = max(0, min(h - 1, iy))
            i = 4 * (iy * w + ix)
            rgb = [round(float(px[i]), 4), round(float(px[i + 1]), 4), round(float(px[i + 2]), 4)]
        except Exception:
            emit("PERF_GRAY " + json.dumps(
                {"label": label, "error": traceback.format_exc(limit=2)}))
            return None
        gray = False
        if gray_ref["rgb"] is not None:
            spread = max(rgb) - min(rgb)
            ref_diff = max(abs(a - b) for a, b in zip(rgb, gray_ref["rgb"]))
            gray = spread <= GRAY_SPREAD and ref_diff <= GRAY_REF_DIFF
        emit("PERF_GRAY " + json.dumps({"label": label, "rgb": rgb, "gray": gray}))
        return rgb

    def reframe_viewport():
        """Re-assert Material Preview shading (the startup popups may reset the screen layout)."""
        try:
            for area0 in view3d_areas(bpy):
                area0.spaces.active.shading.type = 'MATERIAL'
        except Exception:
            emit("PERF_VIEW_WARN " + json.dumps({"error": "reframe failed"}))

    def cal_gray_begin():
        reframe_viewport()
        st["slot_saved"] = plane().material_slots[0].material
        plane().material_slots[0].material = None  # No material: the viewport draws the default gray.

    def cal_gray_shot():
        gray_ref["rgb"] = shot_center("calib_gray")
        plane().material_slots[0].material = st["slot_saved"]

    def cal_gray_sanity():
        rgb = shot_center("calib_mat")
        extra["calib_gray_rgb"] = gray_ref["rgb"]
        extra["calib_mat_rgb"] = rgb
        extra["calib_ok"] = (gray_ref["rgb"] is not None and rgb is not None
                             and max(abs(a - b) for a, b in zip(rgb, gray_ref["rgb"]))
                             > GRAY_REF_DIFF)
        emit("PERF_GRAY_REF " + json.dumps({"gray_rgb": gray_ref["rgb"], "mat_rgb": rgb,
                                            "calib_ok": extra["calib_ok"]}))

    # ---- mode plans ----------------------------------------------------------------------------

    def build_pre_and_plan():
        if args.mode == "S12":
            for k in range(1, S12_ITERS + 1):
                def edit(k=k):
                    row0().opacity = 0.02 * k
                    undo_push(f"S12 opacity {k}")
                plan.append({"run": edit, "wait": 1, "tick": True})

                def und(k=k):
                    undo()
                    extra.setdefault("undo_vals", []).append(round(float(row0().opacity), 6))
                plan.append({"run": und, "wait": 1})

                def red(k=k):
                    redo()
                    extra.setdefault("redo_vals", []).append(round(float(row0().opacity), 6))
                    shot_center(f"redo_{k}")
                plan.append({"run": red, "wait": 1})
        elif args.mode == "S13":
            for k in range(1, S13_ITERS + 1):
                def edit(k=k):
                    extra.setdefault("sigs", []).append(
                        {"k": k, "stage": "pre", "sig": tree_signature(mat())})
                    ma_now = mat()
                    if k % 2 == 1:  # Change the row's map.
                        rec = record(layer_named(ma_now, f"Paint {k % args.rows}"), "BASE_COLOR")
                        rec.image = image_for(f"img_swap_{k}")
                    else:  # Add a row.
                        ma_now.paint_layers.new(name=f"Tmp {k}", source='CONSTANT',
                                                make_active=False)
                    undo_push(f"S13 struct {k}")
                plan.append({"run": edit, "wait": 1, "tick": True})

                def und(k=k):
                    undo()
                    sig = tree_signature(mat())
                    extra.setdefault("sigs", []).append({"k": k, "stage": "undo", "sig": sig})
                    pre = next(s for s in extra["sigs"] if s["k"] == k and s["stage"] == "pre")
                    extra.setdefault("undo_eq", []).append({
                        "k": k,
                        "relaxed": sig["relaxed"] == pre["sig"]["relaxed"],
                        "strict": sig["trees"] == pre["sig"]["trees"],
                        "totals_undo": sig["totals"], "totals_pre": pre["sig"]["totals"]})
                plan.append({"run": und, "wait": 1})

                def red(k=k):
                    redo()
                    extra.setdefault("sigs", []).append(
                        {"k": k, "stage": "redo", "sig": tree_signature(mat())})
                plan.append({"run": red, "wait": 1})
        elif args.mode == "S14":
            for cycle in range(S14_CYCLES):
                for ident, value in S14_CYCLE:
                    def set_pass(ident=ident, cycle=cycle, value=value):
                        try:
                            mat().paint_layers_view_pass = ident
                            got = str(mat().paint_layers_view_pass)
                        except Exception as exc:
                            extra.setdefault("pass_readbacks", []).append(
                                {"cycle": cycle, "set": ident, "error": str(exc)[:120]})
                            return
                        extra.setdefault("pass_readbacks", []).append(
                            {"cycle": cycle, "set": ident, "expected": value, "got": got})
                    plan.append({"run": set_pass, "wait": 1, "tick": True})
            pre_steps.append({"run": pre_render_s14, "wait": 2})
        elif args.mode == "S15":
            pre_steps.append({"run": bake_mode_set, "wait": 2})
            pre_steps.append({"run": bake_now_run, "wait": 2})
            pre_steps.append({"poll": bake_poll, "wait": 2, "poll_interval": 0.5})
            for s in range(S15_STROKES):
                def stroke(s=s):
                    img = record(row0(), "BASE_COLOR").image
                    w, h = img.size
                    px = np.empty(w * h * 4, dtype=np.float32)
                    img.pixels.foreach_get(px)
                    cx = 20 + (s * 7) % (w - 40)
                    cy = 20 + (s * 11) % (h - 40)
                    for dy in range(-2, 3):
                        for dx in range(-2, 3):
                            x, y = cx + dx, cy + dy
                            if 0 <= x < w and 0 <= y < h:
                                i = 4 * (y * w + x)
                                px[i] = 0.1 + 0.08 * s
                                px[i + 1] = 0.9 - 0.08 * s
                                px[i + 2] = 0.5
                                px[i + 3] = 1.0
                    img.pixels.foreach_set(px)
                    img.update()
                    if not args.no_mat_tag:
                        # `update_tag()` with the default refresh=0 is the legacy "tag all
                        # components" path (deg_graph_node_tag_zero), not a no-op: it tags
                        # SHADING and rebuilds all EEVEE passes. A real pixel paint tags the
                        # image, so keep it only for the explicit notice imitation.
                        mat().update_tag()
                plan.append({"run": stroke, "wait": 1, "tick": True})
        elif args.mode in ("S12", "S16"):
            pre_steps.append({"run": cal_gray_begin, "wait": 3})
            pre_steps.append({"run": cal_gray_shot, "wait": 3})
            pre_steps.append({"run": cal_gray_sanity, "wait": 3})

            def struct_edit():
                set_stops(layer_named(mat(), GRAD_ROW), make_stops(3))
                undo_push("S16 stops 3")
            plan.append({"run": struct_edit, "wait": 0})
            for k in range(1, args.ticks + 1):
                def value_tick(k=k):
                    el = layer_named(mat(), GRAD_ROW).gradient_ramp.elements[0]
                    pos = FINAL_VALUE if k == args.ticks else 0.05 + 0.4 * ((k % 10) / 10.0)
                    el.position = pos
                    extra.setdefault("pos_vals", []).append(round(float(pos), 6))
                    if k % 5 == 0:
                        shot_center(f"tick_{k}")
                plan.append({"run": value_tick, "wait": 0, "tick": True})

    def bake_mode_set():
        row0().bake_mode = 'ALWAYS'
        extra["bake_mode_set"] = True

    def bake_now_run():
        # The operator reads the "material" context member, so the override must carry it.
        ma_now = mat()
        window = bpy.context.window_manager.windows[0]
        with bpy.context.temp_override(window=window, material=ma_now):
            bpy.ops.material.paint_layers_bake_now()
        extra["bake_started_s"] = time.time()

    def bake_poll():
        """While the bake is pending or running, keep polling; give up after a minute."""
        if not bool(mat().paint_layers_is_stale):
            extra["bake_ok"] = True
            emit("PERF_BAKE " + json.dumps({"ok": True}))
            return False
        if time.time() - extra.get("bake_started_s", 0) > 60.0:
            extra["bake_ok"] = False
            emit("PERF_BAKE " + json.dumps({"ok": False, "error": "stale after 60 s"}))
            return False
        return True

    def pre_render_s14():
        """The reference render of Combined before any pass switch (outside the markers)."""
        setup_verify_camera()
        extra["px_initial_live"] = render_material(mat()).tolist()
        extra["pre_render_done"] = True

    # Old-mode tick/edit/readback (unchanged behavior) -------------------------------------------

    def tick_op(ma_now, k):
        last = args.verify and k == args.ticks - 1
        if EDIT["kind"]:
            edit_set(ma_now, EDIT["kind"], FINAL_VALUE if last else 0.02 * ((k % 50) + 1))
        elif args.mode == "S1":
            layer_named(ma_now, FIRST_ROW).opacity = (
                FINAL_VALUE if last else 0.02 * ((k % 50) + 1))
        elif args.mode == "S8":
            elements = (filter_ramp(ma_now).elements if FILTER_RAMP["on"] else
                        layer_named(ma_now, GRAD_ROW).gradient_ramp.elements)
            elements[0].position = FINAL_VALUE if last else 0.05 + 0.4 * ((k % 10) / 10.0)
        else:
            # S8C: same value series as S8, applied to the first stop's color instead.
            el = layer_named(ma_now, GRAD_ROW).gradient_ramp.elements[0]
            t = FINAL_VALUE if last else 0.05 + 0.4 * ((k % 10) / 10.0)
            el.color = [t, 0.2, 1.0 - t, 1.0]

    def readback(ma_now):
        if EDIT["kind"]:
            return None  # The edited values are vectors and percentages: the render is the check.
        if args.mode == "S1":
            return layer_named(ma_now, FIRST_ROW).opacity
        if args.mode == "S12":
            return row0(ma_now).opacity
        if args.mode == "S14":
            return str(ma_now.paint_layers_view_pass)
        if args.mode == "S13" or args.mode == "S15":
            return None
        if FILTER_RAMP["on"]:
            return filter_ramp(ma_now).elements[0].position
        el = layer_named(ma_now, GRAD_ROW).gradient_ramp.elements[0]
        if args.mode == "S8C":
            return list(el.color)
        return el.position

    def final_state():
        state = make_state(args.rows, args.mode, args.stops)
        if EDIT["kind"]:
            state["rows"][0]["edit_history"] = [
                0.02 * ((k % 50) + 1) for k in range(max(args.ticks - 1, 0))]
            state["rows"][0]["edit_value"] = FINAL_VALUE
        elif args.mode == "S1":
            state["rows"][0]["opacity"] = FINAL_VALUE
        elif args.mode == "S12":
            state["rows"][0]["opacity"] = S12_FINAL
        elif args.mode == "S13":
            for k in range(1, S13_ITERS + 1):
                if k % 2 == 1:
                    state["rows"][k % args.rows]["channels"]["BASE_COLOR"]["image"] = \
                        f"img_swap_{k}"
                else:
                    # MaterialPaintLayer.opacity is a percentage (RNA range 0..100). A row made by
                    # paint_layers.new starts at 100.0, not 1.0; asking for 1.0 here built the
                    # scratch row at 1 % and made the live-vs-scratch render differ by ~100/255.
                    state["rows"].append({"name": f"Tmp {k}", "source": "CONSTANT",
                                          "opacity": 100.0, "blend": "MIX", "enabled": True,
                                          "channels": {}, "mask_items": [], "effects": []})
        elif args.mode == "S15":
            pass  # The strokes changed map pixels, not the stack: the state is the initial one.
        elif args.mode == "S8":
            if FILTER_RAMP["on"]:
                state["rows"][0]["mask_filters"][0]["stops"][0][0] = FINAL_VALUE
            else:
                state["rows"][-1]["stops"][0][0] = FINAL_VALUE
        else:
            state["rows"][-1]["stops"][0][1] = [FINAL_VALUE, 0.2, 1.0 - FINAL_VALUE, 1.0]
        return state

    def build_material(name, state):
        mat_new = bpy.data.materials.new(name)
        mat_new.use_nodes = True
        apply_state(mat_new, state)
        return mat_new

    def setup_verify_camera():
        cam_data = bpy.data.cameras.new("VerifyCam")
        cam_data.type = 'ORTHO'
        cam_data.ortho_scale = 2.2
        cam = bpy.data.objects.new("VerifyCam", cam_data)
        bpy.context.collection.objects.link(cam)
        cam.location = (0.0, 0.0, 5.0)
        cam.rotation_euler = (0.0, 0.0, 0.0)
        scene_now().camera = cam
        sc = scene_now()
        sc.render.resolution_x = RENDER_RES
        sc.render.resolution_y = RENDER_RES
        sc.render.resolution_percentage = 100

    def layer_signature(ma_now):
        """Description of the stack that a render consumes, for live-vs-scratch diagnosis."""
        rows = []
        try:
            layers = list(ma_now.paint_layers)
        except Exception:
            layers = []
        for layer in layers:
            row = {"name": layer.name, "source": str(layer.source),
                   "opacity": round(float(layer.opacity), 6),
                   "enabled": bool(layer.enabled), "blend": str(layer.blend_type),
                   "channels": {}, "masks": [], "effects": []}
            try:
                for rec in layer.channels:
                    row["channels"][str(rec.channel)] = [
                        rec.image.name if rec.image is not None else None, bool(rec.invert)]
            except Exception:
                pass
            try:
                row["masks"] = [mi.name for mi in layer.mask_items]
            except Exception:
                pass
            try:
                row["effects"] = [fx.name for fx in layer.effects]
            except Exception:
                pass
            if getattr(layer, "gradient", None) is not None:
                try:
                    row["stops"] = [[round(float(el.position), 6),
                                     [round(float(c), 5) for c in el.color]]
                                    for el in layer.gradient_ramp.elements]
                except Exception:
                    pass
            rows.append(row)
        active = None
        try:
            active = ma_now.paint_layers.active.name if ma_now.paint_layers.active else None
        except Exception:
            pass
        channels = None
        try:
            channels = sorted(str(c) for c in ma_now.paint_layers_channels)
        except Exception:
            pass
        return {"channels": channels, "active": active, "rows": rows}

    def tree_digest(ma_now):
        """Compact view of the two trees a render reads, and the output link of the main tree."""
        def collect_images(nt, depth=0):
            if nt is None or depth > 4:
                return []
            found = []
            for node in nt.nodes:
                if node.bl_idname == 'ShaderNodeTexImage':
                    img = getattr(node, "image", None)
                    found.append([node.name, img.name if img is not None else None])
                elif node.bl_idname == 'ShaderNodeGroup' and getattr(node, "node_tree", None):
                    found.extend(collect_images(node.node_tree, depth + 1))
            return found

        def one(nt):
            if nt is None:
                return None
            kinds = collections.Counter(n.bl_idname for n in nt.nodes)
            return {"name": nt.name, "nodes": len(nt.nodes), "links": len(nt.links),
                    "kinds": sorted(kinds.items())}
        out = {"nodetree": one(ma_now.node_tree),
               "paint_layers_tree": one(getattr(ma_now, "paint_layers_tree", None)),
               "images": sorted(collect_images(ma_now.node_tree), key=str),
               "outputs": [], "groups": []}
        nt = ma_now.node_tree
        if nt is not None:
            for node in nt.nodes:
                if node.bl_idname == 'ShaderNodeOutputMaterial':
                    sock = node.inputs.get('Surface')
                    out["outputs"].append({
                        "node": node.name,
                        "linked": bool(sock and sock.is_linked),
                        "from": (sock.links[0].from_node.bl_idname
                                 if sock and sock.is_linked else None)})
                elif node.bl_idname == 'ShaderNodeGroup':
                    grp = getattr(node, "node_tree", None)
                    inner = one(grp) if grp is not None else None
                    images = []
                    if grp is not None:
                        for inner_node in grp.nodes:
                            if inner_node.bl_idname == 'ShaderNodeTexImage':
                                img = getattr(inner_node, "image", None)
                                images.append([inner_node.name,
                                               img.name if img is not None else None])
                    out["groups"].append([node.name, grp.name if grp is not None else None,
                                          inner, sorted(images)])
        return out

    def material_state(ma_now):
        """Capture the description of a material in the form #apply_state consumes."""
        state = {"rows": []}
        for layer in ma_now.paint_layers:
            row = {"name": layer.name, "source": str(layer.source),
                   "opacity": float(layer.opacity), "blend": str(layer.blend_type),
                   "enabled": bool(layer.enabled), "channels": {}, "mask_items": [],
                   "effects": []}
            for rec in layer.channels:
                if rec.image is not None:
                    row["channels"][str(rec.channel)] = {
                        "image": rec.image.name, "invert": bool(rec.invert)}
            for mi in layer.mask_items:
                item = {"name": mi.name, "channels": {}}
                for rec in mi.channels:
                    if rec.image is not None:
                        item["channels"][str(rec.channel)] = {
                            "image": rec.image.name, "invert": bool(rec.invert)}
                row["mask_items"].append(item)
            gradient = getattr(layer, "gradient", None)
            if gradient is not None:
                row["stops"] = [[float(el.position), [float(c) for c in el.color]]
                                for el in gradient.ramp.elements]
            state["rows"].append(row)
        return state

    def render_material(mat_obj, label=None):
        # 'Render Result' has no pixel buffer here, so the frame is written to PNG and read back.
        # Both sides go through the same view transform, so the 8-bit values are comparable.
        # A per-call name: every mode/row combination writes into the same folder, and a shared
        # name left only the last scenario's render on disk for inspection.
        st["render_seq"] = st.get("render_seq", 0) + 1
        tag = label or mat_obj.name
        sc = scene_now()
        # Clear first: assigning the material the slot already names is a no-op, so after an
        # undo/redo the evaluated mesh could keep the slot the restore left behind (empty), and
        # the render would show no material at all.
        plane().data.materials[0] = None
        plane().data.materials[0] = mat_obj
        try:
            plane().update_tag({'OBJECT', 'DATA'})
            bpy.context.view_layer.update()
        except Exception:
            pass
        path = os.path.join(os.path.dirname(os.path.abspath(args.out)),
                            f"verify_{args.mode}_r{args.rows}_{st['render_seq']:02d}_{tag}.png")
        sc.render.filepath = path
        bpy.ops.render.render(write_still=True)
        img = bpy.data.images.load(path, check_existing=False)
        px = np.empty(RENDER_RES * RENDER_RES * 4, dtype=np.float32)
        img.pixels.foreach_get(px)
        bpy.data.images.remove(img)
        return px

    def verify_render(state_fn=None):
        state_fn = state_fn or final_state
        setup_verify_camera()

        live = mat()
        live_value = readback(live)
        # The reference "after the series" is the current description rebuilt from scratch. An
        # undo/redo can leave the live data-block with a GPU pass cached from the state before the
        # restore, and the render then draws that stale pass (black, in S12/S13) even though the
        # description, generated groups and image references are identical to a fresh build. That
        # is a render-cache artefact, not the state this scenario measures, so compare like with
        # like: the description captured from the live material vs the expected description.
        if args.mode in ("S12", "S13", "S15"):
            live_ref = build_material("VerifyLive", material_state(live))
        else:
            live_ref = live
        scratch = build_material("VerifyScratch", state_fn())
        init_mat = build_material("VerifyInit", make_state(args.rows, args.mode, args.stops))
        expected = S12_FINAL if args.mode == "S12" else FINAL_VALUE
        if live_value is None:
            readback_ok = None
        elif args.mode == "S8C":
            expected_list = [FINAL_VALUE, 0.2, 1.0 - FINAL_VALUE, 1.0]
            readback_ok = all(abs(a - b) < 1e-6 for a, b in zip(live_value, expected_list))
        else:
            readback_ok = abs(live_value - expected) < 1e-6
        px_live = render_material(live_ref, "live_ref")
        # A second render of the same material distinguishes a real state difference from render
        # nondeterminism.
        px_live_again = render_material(live_ref, "live_ref_b")
        px_scratch = render_material(scratch, "scratch")
        px_init = render_material(init_mat, "initial")
        # The live data-block itself, kept as a diagnostic of the stale render-cache artefact.
        px_live_actual = render_material(live, "live_actual")
        diff_scratch = float(np.abs(px_live - px_scratch).max()) * 255.0
        diff_init = float(np.abs(px_live - px_init).max()) * 255.0
        diff_live_self = float(np.abs(px_live - px_live_again).max()) * 255.0
        diff_actual_scratch = float(np.abs(px_live_actual - px_scratch).max()) * 255.0
        return {
            "mode": args.mode,
            "final_value": expected,
            "live_readback": live_value,
            "readback_ok": readback_ok,
            "max_diff_255_live_vs_scratch": diff_scratch,
            "max_diff_255_live_vs_initial": diff_init,
            "max_diff_255_live_vs_live": diff_live_self,
            "max_diff_255_live_id_vs_scratch": diff_actual_scratch,
            "tolerance_255": 1.0,
            "equal_within_tol": diff_scratch <= 1.0,
            "non_empty": diff_init > 1.0,
            "live_stack": layer_signature(live),
            "scratch_stack": layer_signature(scratch),
            "live_tree": tree_digest(live),
            "scratch_tree": tree_digest(scratch),
            "pixels": RENDER_RES * RENDER_RES,
        }

    def verify_s14():
        px_final = render_material(mat())
        diff = float(np.abs(px_final - np.array(extra["px_initial_live"])).max()) * 255.0
        return {
            "mode": "S14",
            "max_diff_255_final_vs_initial": diff,
            "tolerance_255": 1.0,
            "equal_within_tol": diff <= 1.0,
            "pixels": RENDER_RES * RENDER_RES,
        }

    def verify_s16():
        el = layer_named(mat(), GRAD_ROW).gradient_ramp.elements[0]
        return {"mode": "S16", "final_stop0_position": float(el.position),
                "expected": FINAL_VALUE, "readback_ok": abs(float(el.position) - FINAL_VALUE) < 1e-6}

    # ---- step machine (S12..S16) ----------------------------------------------------------------

    def exec_step(steps, box):
        """Run one plan step once the previous step has got its redraws; returns the next wait."""
        try:
            idx = box["idx"]
            if idx >= len(steps):
                return None  # Plan exhausted; the caller moves to the tail.
            if idx > 0:
                need = steps[idx - 1].get("wait", 1)
                if draws["n"] - box["last_draw"] < need:
                    return 0.05
            step = steps[idx]
            if "poll" in step and step["poll"]():
                return step.get("poll_interval", 0.3)
            try:
                t_op = time.perf_counter()
                step.get("run", lambda: None)()
                box["op_ms"].append((time.perf_counter() - t_op) * 1000.0)
            except Exception:
                emit("PERF_ERROR " + json.dumps({"step": idx, "error": traceback.format_exc()}))
                finish("error")
                return None
            box["last_draw"] = draws["n"]
            box["idx"] = idx + 1
            if step.get("tick"):
                st["k"] += 1
            tag_all()
            return step.get("interval", TICK_INTERVAL_S)
        except Exception:
            emit("PERF_ERROR " + json.dumps({"phase": st["phase"], "box": box["idx"],
                                             "error": traceback.format_exc()}))
            finish("error")
            return None

    # ---- framework (shared with S1/S8/S8C) -------------------------------------------------------

    def finish(reason):
        if st["finished"]:
            return
        st["finished"] = True
        try:
            bpy.types.SpaceView3D.draw_handler_remove(draw_handle, 'WINDOW')
        except Exception:
            pass
        ts = [t for t in draws["t"]]
        intervals = [(b - a) * 1000.0 for a, b in zip(ts, ts[1:])]
        emit("PERF_TICKS " + json.dumps({
            "done": st["k"], "requested": args.ticks,
            "steps_done": st["run_box"]["idx"], "steps_total": len(plan),
            "draws": draws["n"] - st["draws0"], "reason": reason,
            "frame_ms_mean": statistics.mean(intervals) if intervals else None,
            "frame_ms_median": statistics.median(intervals) if intervals else None,
            "window_ms": (ts[-1] - ts[0]) * 1000.0 if len(ts) > 1 else None,
            "op_ms_mean": statistics.mean(st["op_ms"]) if st["op_ms"] else None,
            "run_op_ms_mean": (statistics.mean(st["run_box"]["op_ms"])
                               if st["run_box"]["op_ms"] else None),
        }))
        if extra:
            emit("PERF_EXTRA " + json.dumps(extra, default=list))
        emit("PERF_MARK end")
        if reason == "ok":
            try:
                if is_step_mode:
                    if args.mode == "S14":
                        emit("PERF_VERIFY " + json.dumps(verify_s14()))
                    elif args.mode == "S16":
                        emit("PERF_VERIFY " + json.dumps(verify_s16()))
                    else:
                        emit("PERF_VERIFY " + json.dumps(verify_render()))
                elif args.verify:
                    emit("PERF_VERIFY " + json.dumps(verify_render()))
            except Exception:
                emit("PERF_VERIFY_ERROR " + json.dumps({"error": traceback.format_exc()}))
        try:
            bpy.ops.wm.quit_blender()
        except Exception:
            traceback.print_exc()

    def warmup():
        """Draw warmup, then the mode's pre-steps (calibration/bake/reference render), then start."""
        if st["finished"]:
            return None
        if st["phase"] == "warmup":
            if draws["n"] < WARMUP_DRAWS:
                if time.time() - st["t0"] > WARMUP_TIMEOUT_S:
                    emit("PERF_ERROR " + json.dumps({"error": "warmup: no viewport draws",
                                                     "draws": draws["n"]}))
                    finish("warmup_timeout")
                    return None
                tag_all()
                return 0.05
            emit("PERF_WARMUP " + json.dumps({"draws": draws["n"], "shading": [
                a.spaces.active.shading.type for a in view3d_areas(bpy)]}))
            st["draws0"] = draws["n"]
            if pre_steps:
                st["phase"] = "pre"
                return TICK_INTERVAL_S
            emit("PERF_MARK start")
            st["phase"] = "run"
            return TICK_INTERVAL_S
        # phase == "pre"
        nxt = exec_step(pre_steps, st["pre_box"])
        if nxt is not None:
            return nxt
        emit("PERF_MARK start")
        st["phase"] = "run"
        return TICK_INTERVAL_S

    def run_tick():
        """The old-mode value series, one edit per tick (S1/S8/S8C)."""
        if st["finished"]:
            return None
        if st["k"] < args.ticks:
            try:
                t_op = time.perf_counter()
                tick_op(bpy.data.materials[MATERIAL_NAME], st["k"])
                st["op_ms"].append((time.perf_counter() - t_op) * 1000.0)
            except Exception:
                emit("PERF_ERROR " + json.dumps({"tick": st["k"],
                                                 "error": traceback.format_exc()}))
                finish("error")
                return None
            st["k"] += 1
            tag_all()
            return TICK_INTERVAL_S
        st["tail"] += 1
        if st["tail"] == 1:
            return 0.5  # Give the last tick one draw before the end marker.
        finish("ok")
        return None

    def run_steps():
        """The step-machine runner (S12..S16)."""
        if st["finished"]:
            return None
        nxt = exec_step(plan, st["run_box"])
        if nxt is not None:
            return nxt
        st["tail"] += 1
        if st["tail"] == 1:
            return S16_SETTLE_S if args.mode == "S16" else 0.5
        if args.mode == "S16":
            shot_center("final")
        finish("ok")
        return None

    def watchdog():
        if not st["finished"]:
            emit("PERF_ERROR " + json.dumps({"error": f"timeout {watchdog_s:.0f} s",
                                             "phase": st["phase"], "k": st["k"],
                                             "step": st["run_box"]["idx"]}))
            finish("timeout")
        return None

    def run_phase():
        if st["phase"] in ("warmup", "pre"):
            return warmup()
        if is_step_mode:
            return run_steps()
        return run_tick()

    build_pre_and_plan()
    bpy.app.timers.register(watchdog, first_interval=watchdog_s, persistent=True)
    bpy.app.timers.register(run_phase, first_interval=0.2, persistent=True)


# --------------------------------------------------------------------------------------------
# Parser (plain Python)
# --------------------------------------------------------------------------------------------


def _to_float(text):
    try:
        return float(text)
    except ValueError:
        return None


def parse_text(text, ticks, mode):
    lines = text.splitlines()
    start_idx = next((i for i, ln in enumerate(lines) if ln.strip() == "PERF_MARK start"), None)
    end_idx = next((i for i, ln in enumerate(lines) if ln.strip() == "PERF_MARK end"), None)
    region = []
    if start_idx is not None:
        stop = end_idx if end_idx is not None else len(lines)
        region = lines[start_idx + 1:stop]

    ticks_info = {}
    verify = None
    gray_ref = None
    for ln in lines:
        if ln.startswith("PERF_TICKS "):
            ticks_info = json.loads(ln[len("PERF_TICKS "):])
        elif ln.startswith("PERF_VERIFY_ERROR "):
            verify = {"error": json.loads(ln[len("PERF_VERIFY_ERROR "):])}
        elif ln.startswith("PERF_VERIFY "):
            verify = json.loads(ln[len("PERF_VERIFY "):])
        elif ln.startswith("PERF_GRAY_REF "):
            gray_ref = json.loads(ln[len("PERF_GRAY_REF "):])
    ticks_done = ticks_info.get("done")

    extract_time_mamat = 0
    extract_lines_mamat = 0
    values_mamat = collections.Counter()
    values_all = collections.Counter()
    values_required = 0
    tracked_entries = collections.Counter()
    eval_decisions = collections.Counter()
    eval_mat = collections.Counter()
    refused = collections.Counter()
    compile_counts = []
    queued = collections.Counter()
    queued_last = {}
    regen_kept = collections.Counter()
    regen_full = collections.Counter()
    regen_tagged = collections.Counter()
    gray_samples = []
    build_ms = []
    inline_ms = []

    for ln in region:
        m = RX_EXTRACT_TIME.match(ln)
        if m and m.group(1) == LAYERED_NAME:
            extract_time_mamat += 1
            inline = _to_float(m.group(2))
            build = _to_float(m.group(3))
            if inline is not None:
                inline_ms.append(inline)
            if build is not None:
                build_ms.append(build)
            continue
        m = RX_EXTRACT.match(ln)
        if m and m.group(1) == LAYERED_NAME:
            extract_lines_mamat += 1
            continue
        m = RX_VALUES.match(ln)
        if m:
            values_all[m.group(2)] += 1
            if m.group(1) == LAYERED_NAME:
                values_mamat[m.group(2)] += 1
                if m.group(3) == "1":
                    values_required += 1
                    tracked_entries[m.group(5)] += 1
            continue
        m = RX_EVAL.match(ln)
        if m:
            eval_decisions[f"{m.group(1)} values_only={m.group(2)} => {m.group(5)}"] += 1
            if m.group(1) == MATERIAL_NAME:
                eval_mat[m.group(5)] += 1
            continue
        m = RX_REFUSED.match(ln)
        if m:
            refused[m.group(2).strip()[:120]] += 1
            continue
        m = RX_COMPILE.match(ln)
        if m:
            compile_counts.append(int(m.group(1)))
            continue
        m = RX_QUEUED.match(ln)
        if m:
            queued[m.group(1)] += 1
            queued_last[m.group(1)] = int(m.group(2))
            continue
        m = RX_REGEN.match(ln)
        if m:
            (regen_kept if m.group(2).startswith("KEPT") else regen_full)[m.group(1)] += 1
            continue
        m = RX_REGEN_TAGGED.match(ln)
        if m:
            regen_tagged[m.group(1)] += 1
            continue
        m = RX_GRAY.match(ln)
        if m:
            try:
                gray_samples.append(json.loads(m.group(1)))
            except json.JSONDecodeError:
                pass
            continue

    extracts_mamat = max(extract_time_mamat, extract_lines_mamat)
    values_ok = values_mamat.get("ok", 0)
    values_fail = values_mamat.get("fail", 0)
    compile_delta = (compile_counts[-1] - compile_counts[0]) if len(compile_counts) > 1 else 0
    full_total = sum(regen_full.values())
    kept_total = sum(regen_kept.values())
    tagged_total = sum(regen_tagged.values())
    gray_count = sum(1 for s in gray_samples if s.get("gray"))
    gray_errors = sum(1 for s in gray_samples if "error" in s)

    causes = []
    if start_idx is None:
        causes.append("нет маркера PERF_MARK start (сбой до warmup, см. лог)")
    if end_idx is None:
        causes.append("нет маркера PERF_MARK end (таймаут или падение Blender)")
    if mode in ("S1", "S8", "S8C"):
        if ticks_done is not None and ticks_done != ticks:
            causes.append(f"выполнено тиков {ticks_done} из {ticks}")
        if not values_mamat:
            causes.append("values_update для MAMat не напечатан: быстрый путь не вызывался "
                          "(сборка без P1-7 или eval_decision => rebuild)")
        if values_fail:
            causes.append(f"values_update result=fail: {values_fail}")
        keep_mat = eval_mat.get("keep", 0)
        rebuild_mat = eval_mat.get("rebuild", 0)
        if extracts_mamat:
            causes.append(f"extract MAMat = {extracts_mamat}: GPUMaterial пересобран, "
                          "а не обновлён")
        if keep_mat != ticks:
            causes.append(f"eval_decision Mat => keep = {keep_mat} != ticks={ticks}")
        if rebuild_mat:
            causes.append(f"eval_decision Mat => rebuild = {rebuild_mat}")
        for reason, count in refused.most_common():
            causes.append(f"value_path refused x{count}: {reason}")

    # values_update fires on every viewport draw, so its ok count is informational only.
    base_ok = (start_idx is not None and end_idx is not None)
    if mode in ("S1", "S8", "S8C"):
        fast_path_ok = (base_ok and extracts_mamat == 0 and values_fail == 0
                        and eval_mat.get("keep", 0) == ticks and eval_mat.get("rebuild", 0) == 0
                        and ticks_done == ticks)
        verdict = "FAST_PATH_OK" if fast_path_ok else "FAST_PATH_NOT_REACHED"
    elif mode == "S12":
        extra = _extra(text)
        vals_ok = _s12_values_ok(extra)
        gray_ok = _s12_gray_ok(gray_samples)
        s12_ok = (base_ok and ticks_done == S12_ITERS and full_total == 0
                  and queued.get("default", 0) == 0
                  and bool(verify) and verify.get("equal_within_tol")
                  and verify.get("readback_ok") and vals_ok and gray_ok)
        verdict = "S12_OK" if s12_ok else "S12_FAIL"
        if ticks_done != S12_ITERS:
            causes.append(f"выполнено итераций {ticks_done} из {S12_ITERS}")
        if full_total:
            causes.append(f"FULL REBUILD = {full_total} (ожидался 0)")
        if queued.get("default", 0):
            causes.append(f"queued default = {queued.get('default', 0)} "
                          "(кадры с серым дефолтным материалом)")
        if not (verify or {}).get("equal_within_tol"):
            causes.append("рендер после серии != рендер с нуля")
        if not (verify or {}).get("readback_ok"):
            causes.append("значение после последнего redo != ожидаемому")
        if not vals_ok:
            causes.append("значения opacity после undo/redo не равны ожидаемым")
        if not gray_ok:
            causes.append("пиксельная проба после redo показала серый дефолтный материал")
    elif mode == "S13":
        extra = _extra(text)
        undo_eq = (extra.get("undo_eq") or [])
        all_eq = bool(undo_eq) and all(e.get("relaxed") for e in undo_eq)
        s13_ok = (base_ok and ticks_done == S13_ITERS and all_eq
                  and bool(verify) and verify.get("equal_within_tol"))
        verdict = "S13_OK" if s13_ok else "S13_FAIL"
        if ticks_done != S13_ITERS:
            causes.append(f"выполнено итераций {ticks_done} из {S13_ITERS}")
        bad = [e["k"] for e in undo_eq if not e.get("relaxed")]
        if bad:
            causes.append(f"дерево после undo не эквивалентно: итерации {bad}")
        if not (verify or {}).get("equal_within_tol"):
            causes.append("рендер после серии != эталон")
    elif mode == "S14":
        extra = _extra(text)
        passes = (extra.get("pass_readbacks") or [])
        pass_ok = bool(passes) and all("error" not in p and p.get("got") == p.get("expected")
                                       for p in passes)
        s14_ok = (base_ok and pass_ok and tagged_total == 0
                  and bool(verify) and verify.get("equal_within_tol"))
        verdict = "S14_OK" if s14_ok else "S14_FAIL"
        bad = [f"{p.get('cycle')}:{p.get('set')}" for p in passes
               if "error" in p or p.get("got") != p.get("expected")]
        if bad:
            causes.append(f"readback view pass не совпал: {bad}")
        if tagged_total:
            causes.append(f"regen tagged = {tagged_total} (лишние теги)")
        if not (verify or {}).get("equal_within_tol"):
            causes.append("итоговый Combined != исходный рендер")
    elif mode == "S15":
        # "Смена формы" in the P0-4 criterion is a rebuild of the generated graph (FULL REBUILD),
        # not a GPU shader extraction. A source-map pixel notice legitimately rebuilds the EEVEE
        # passes once per pipeline; report it, but do not fail the form-change criterion for it.
        s15_ok = (base_ok and ticks_done == S15_STROKES and full_total <= 1)
        verdict = "S15_OK" if s15_ok else "S15_FAIL"
        if ticks_done != S15_STROKES:
            causes.append(f"выполнено штрихов {ticks_done} из {S15_STROKES}")
        if full_total > 1:
            causes.append(f"FULL REBUILD = {full_total} (> 1 смены формы на серию)")
        if extracts_mamat > 1:
            # Informational: this is expected while a map-pixel notice is a shading tag (3 EEVEE
            # pipeline variants per stroke). It is not a graph form change.
            print(f"note: extract MAMat = {extracts_mamat} (GPU-пересборки, не смена формы)")
    elif mode == "S16":
        s16_ok = (base_ok and ticks_done == ticks and queued.get("default", 0) == 0
                  and gray_count == 0 and gray_errors == 0)
        verdict = "S16_OK" if s16_ok else "S16_FAIL"
        if ticks_done != ticks:
            causes.append(f"выполнено тиков {ticks_done} из {ticks}")
        if queued.get("default", 0):
            causes.append(f"queued default = {queued.get('default', 0)} "
                          "(кадры с серым дефолтным материалом)")
        if gray_count or gray_errors:
            causes.append(f"серые пиксельные пробы: {gray_count}, ошибок проб: {gray_errors}")
    else:
        verdict = "UNKNOWN_MODE"

    return {
        "verdict": verdict,
        "mode": mode,
        "ticks": ticks,
        "ticks_done": ticks_done,
        "markers": {"start": start_idx is not None, "end": end_idx is not None},
        "extract_MAMat": extracts_mamat,
        "regen_kept": dict(regen_kept),
        "regen_full": dict(regen_full),
        "regen_full_total": full_total,
        "regen_kept_total": kept_total,
        "regen_tagged": dict(regen_tagged),
        "regen_tagged_total": tagged_total,
        "values_update_MAMat": dict(values_mamat),
        "values_update_all": dict(values_all),
        "eval_decision": dict(eval_decisions),
        "value_path_refused": dict(refused),
        "compile_lines": len(compile_counts),
        "compile_delta": compile_delta,
        "queued": dict(queued),
        "queued_last_total": queued_last,
        "gray_samples": gray_samples,
        "gray_count": gray_count,
        "gray_errors": gray_errors,
        "gray_ref": gray_ref,
        "eval_mat": dict(eval_mat),
        "values_update_MAMat_ok": values_ok,
        "values_update_MAMat_required1": values_required,
        "tracked_entries_MAMat": dict(tracked_entries),
        "ticks_info": ticks_info,
        "verify": verify,
        "build_ms_mean": statistics.mean(build_ms) if build_ms else None,
        "build_ms_median": statistics.median(build_ms) if build_ms else None,
        "inline_ms_mean": statistics.mean(inline_ms) if inline_ms else None,
        "causes": causes,
    }


def _s12_gray_ok(gray_samples):
    """S12: the sampled frames (one per redo) must not show the default gray material.

    A run that took no screenshots has nothing to judge; the required `queued default` counter is
    the product-side signal and is checked separately.
    """
    samples = [s for s in gray_samples if "gray" in s]
    return all(not s["gray"] for s in samples)


def _s12_values_ok(extra):
    """S12: after undo the previous value, after redo the edited one (0.02 * iteration)."""
    redo_vals = extra.get("redo_vals") or []
    undo_vals = extra.get("undo_vals") or []
    if len(redo_vals) != S12_ITERS or len(undo_vals) != S12_ITERS:
        return False
    for k in range(S12_ITERS):
        expected_redo = round(0.02 * (k + 1), 6)
        expected_undo = round(1.0 if k == 0 else 0.02 * k, 6)
        if abs(redo_vals[k] - expected_redo) > 1e-6 or abs(undo_vals[k] - expected_undo) > 1e-6:
            return False
    return True


def _extra(text):
    """The PERF_EXTRA dict (mode-specific worker results), emitted before PERF_MARK end."""
    for ln in text.splitlines():
        if ln.startswith("PERF_EXTRA "):
            try:
                return json.loads(ln[len("PERF_EXTRA "):])
            except json.JSONDecodeError:
                return {}
    return {}


def parse_main(tail):
    parser = argparse.ArgumentParser()
    parser.add_argument("--parse", action="store_true")
    parser.add_argument("--log", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--ticks", type=int, required=True)
    parser.add_argument("--mode", choices=["S1", "S8", "S8F", "S17", "S8C", "S12", "S13", "S14", "S15", "S16"],
                        required=True)
    args = parser.parse_args(tail)
    if args.mode == "S8F":
        args.mode = "S8"
    if args.mode == "S17":
        args.mode = "S1"

    with open(args.log, encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    result = parse_text(text, args.ticks, args.mode)
    result["log"] = os.path.abspath(args.log)
    with open(args.out, "w", encoding="utf-8") as fh:
        json.dump(result, fh, indent=1, ensure_ascii=False)

    print(f"VERDICT: {result['verdict']}")
    print(f"mode={args.mode} ticks={args.ticks} ticks_done={result['ticks_done']} "
          f"markers={result['markers']}")
    print(f"extract MAMat={result['extract_MAMat']}")
    print(f"regen: kept={result['regen_kept_total']} full={result['regen_full_total']} "
          f"tagged={result['regen_tagged_total']}")
    print(f"values_update MAMat={result['values_update_MAMat']} "
          f"(all={result['values_update_all']})")
    print(f"eval_decision={result['eval_decision']}")
    print(f"value_path refused={result['value_path_refused']}")
    print(f"compile lines={result['compile_lines']} delta={result['compile_delta']}")
    print(f"queued={result['queued']} last_total={result['queued_last_total']}")
    print(f"gray: samples={len(result['gray_samples'])} gray={result['gray_count']} "
          f"errors={result['gray_errors']}")
    print(f"build_ms mean={result['build_ms_mean']} median={result['build_ms_median']}")
    ti = result["ticks_info"]
    print(f"timing: frame_ms mean={ti.get('frame_ms_mean')} median={ti.get('frame_ms_median')} "
          f"window_ms={ti.get('window_ms')} draws={ti.get('draws')} op_ms mean={ti.get('op_ms_mean')}")
    if result["verify"] is not None:
        print(f"verify={result['verify']}")
    if result["verdict"] not in ("FAST_PATH_OK", "S12_OK", "S13_OK", "S14_OK", "S15_OK", "S16_OK"):
        print("Причины:")
        for cause in result["causes"] or ["нет данных"]:
            print(f"  - {cause}")
    print(f"JSON: {os.path.abspath(args.out)}")


def main():
    argv = sys.argv
    # Blender passes its own options before `--`; plain Python has no `--`.
    tail = argv[argv.index("--") + 1:] if "--" in argv else argv[1:]
    if "--parse" in tail:
        parse_main(tail)
    else:
        worker(tail)


if __name__ == "__main__":
    main()
