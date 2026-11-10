# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Measurement harness for paint-layer regeneration and GLSL compilation counts.

Two modes in one file:

* Orchestrator (plain Python, no `bpy`): runs every requested (scenario, rows) pair in a fresh
  Blender process, parses the diagnostic stdout and writes `results_<label>.json`.
* Worker (inside Blender, after `--`): builds a stack of N paint rows, performs the scenario
  actions and prints `PERF_MARK_BEGIN` / `PERF_MARK_END` markers around each repetition so the
  orchestrator can attribute the counter lines to a repetition.

Usage (orchestrator):
  python run.py --exe <X-NazirSculpt.exe> --label B --out <dir> --scenarios S1,S2 --rows 1,10 --reps 10

Requires a build with `PBR_LAYERS_DIAGNOSTICS=1` (paint_layers_intern.hh) and the `PERF-TEMP`
counters in gpu_pass.cc, gpu_material.cc and node_shader_tree.cc. Run the worker with
`--debug-gpu`, otherwise `PERF compile` is not printed.
"""

import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import time
import traceback
import zlib

# Scenarios that need a live viewport (EEVEE draw, timers, undo from the UI). They are not run in
# background mode; the orchestrator reports them as NOT_RUN.
GUI_ONLY = {"S12", "S13", "S14", "S15", "S16"}
SCENARIOS = ["S1", "S2", "S3", "S4", "S5", "S6", "S7", "S8", "S9", "S10", "S11", "S12", "S13"]
# Scenarios whose final state is compared against a fresh build (section 5 of the TZ).
RENDER_CHECK = {"S2", "S3", "S6", "S7", "S8", "S10", "S12"}
CHANNELS = ["BASE_COLOR", "ROUGHNESS", "NORMAL", "HEIGHT"]
IMAGE_SIZE = 256
MATERIAL_NAME = "Mat"
REPS_DEFAULT = 10

# Counter lines printed by the instrumented build. Regex -> metric name.
LINE_METRICS = {
    "full_rebuild": re.compile(r"\[PBR-Layers regen\].*=> FULL REBUILD"),
    "kept": re.compile(r"\[PBR-Layers regen\].*=> KEPT \(in-place\)"),
    "compile": re.compile(r"^PERF compile count="),
    "extract": re.compile(r"^PERF extract count="),
    "pass_hit": re.compile(r"^PERF pass_cache hit"),
    "pass_miss": re.compile(r"^PERF pass_cache miss"),
    "queued_fallback": re.compile(r"^PERF queued fallback"),
    "queued_default": re.compile(r"^PERF queued default"),
    "prune_mix": re.compile(r"^PERF prune_mix"),
}
REGEN_TIME = re.compile(r"\[PBR-Layers regen\] time .* total=([0-9.]+) ms")

# The layered material is the scenario material with the `MA` prefix the engine puts on its
# extract diagnostics. Other extracts (default surface, world) are not counted as layered.
LAYERED_EXTRACT_NAME = "MA" + MATERIAL_NAME
EXTRACT_NAME_LINE = re.compile(r"^PERF extract count=\S+ name=(.*)$")
EXTRACT_TIME_LINE = re.compile(
    r"^PERF extract_time name=(.*?) inline=(\S+) build=(\S+) tracked=([01]) entries=(\d+)")
VALUE_PATH_REFUSED_LINE = re.compile(r"^PERF value_path name=(.*?) refused=(.*)$")
GPU_BACKENDS = ["opengl", "vulkan", "metal"]


# --------------------------------------------------------------------------------------------
# Orchestrator
# --------------------------------------------------------------------------------------------


def _to_float(text):
    try:
        return float(text)
    except ValueError:
        return None


def _median_or_none(values):
    return statistics.median(values) if values else None


def parse_log(text):
    """Split the worker stdout into repetitions and count the counter lines of each one.

    Returns (rep_metrics, checks, errors, diag). `diag` collects lines that are not tied to a
    repetition: refused value paths over the whole run and render skips.
    """
    reps = []
    checks = []
    errors = []
    refused = {}
    render_skips = []
    current = None
    for line in text.splitlines():
        m = VALUE_PATH_REFUSED_LINE.search(line)
        if m:
            reason = m.group(2).strip()
            refused[reason] = refused.get(reason, 0) + 1
        if line.startswith("PERF_RENDER_SKIPPED "):
            render_skips.append(json.loads(line[len("PERF_RENDER_SKIPPED "):]))
        if line.startswith("PERF_MARK_BEGIN"):
            current = {"lines": [], "rep": int(line.split("rep=")[1].split()[0])}
            continue
        if line.startswith("PERF_MARK_END"):
            m = re.search(r"wall_ms=([0-9.]+)", line)
            if current is not None:
                current["wall_ms"] = float(m.group(1)) if m else None
                reps.append(current)
            current = None
            continue
        if line.startswith("PERF_CHECK "):
            checks.append(json.loads(line[len("PERF_CHECK "):]))
            continue
        if line.startswith("PERF_ERROR "):
            errors.append(json.loads(line[len("PERF_ERROR "):]))
            continue
        if current is not None:
            current["lines"].append(line)

    rep_metrics = []
    for rep in reps:
        counts = {name: 0 for name in LINE_METRICS}
        counts["extract_layered"] = 0
        regen_ms = 0.0
        layered_inline = []
        layered_build = []
        layered_entries = []
        tracked = {"0": 0, "1": 0}
        for line in rep["lines"]:
            for name, rx in LINE_METRICS.items():
                if rx.search(line):
                    counts[name] += 1
            m = REGEN_TIME.search(line)
            if m:
                regen_ms += float(m.group(1))
            m = EXTRACT_NAME_LINE.search(line)
            if m and m.group(1) == LAYERED_EXTRACT_NAME:
                counts["extract_layered"] += 1
            m = EXTRACT_TIME_LINE.search(line)
            if m and m.group(1) == LAYERED_EXTRACT_NAME:
                inline = _to_float(m.group(2))
                build = _to_float(m.group(3))
                if inline is not None:
                    layered_inline.append(inline)
                if build is not None:
                    layered_build.append(build)
                layered_entries.append(int(m.group(5)))
                tracked[m.group(4)] += 1
        # The compile/extract/pass counters are cumulative; the per-rep value is the number of
        # printed events, which is what the TZ asks for (events per scenario).
        counts["regen_ms"] = regen_ms
        counts["extract_inline_ms_sum"] = sum(layered_inline) if layered_inline else None
        counts["extract_inline_ms_median"] = _median_or_none(layered_inline)
        counts["extract_build_ms_sum"] = sum(layered_build) if layered_build else None
        counts["extract_build_ms_median"] = _median_or_none(layered_build)
        counts["entries_median"] = _median_or_none(layered_entries)
        counts["tracked_1"] = tracked["1"]
        counts["tracked_0"] = tracked["0"]
        tracked_total = tracked["0"] + tracked["1"]
        counts["tracked_share"] = tracked["1"] / tracked_total if tracked_total else None
        counts["wall_ms"] = rep.get("wall_ms")
        counts["rep"] = rep["rep"]
        rep_metrics.append(counts)
    diag = {"refused": refused, "render_skips": render_skips}
    return rep_metrics, checks, errors, diag


def summarize(rep_metrics):
    summary = {}
    if not rep_metrics:
        return summary
    # Union of keys: a per-rep value can be None (no layered extract in that rep) in some reps only.
    keys = []
    for rep_counts in rep_metrics:
        for key in rep_counts:
            if key != "rep" and key not in keys:
                keys.append(key)
    for key in keys:
        values = sorted(float(r[key]) for r in rep_metrics if r.get(key) is not None)
        if not values:
            continue
        median = statistics.median(values)
        p95_index = min(len(values) - 1, max(0, int(round(0.95 * (len(values) - 1)))))
        summary[key] = {"median": median, "p95": values[p95_index], "min": values[0],
                        "max": values[-1]}
    return summary


def orchestrate(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    parser.add_argument("--label", required=True, help="A or B, used in the file names")
    parser.add_argument("--out", required=True, help="Directory for logs, PNGs and results")
    parser.add_argument("--scenarios", default=",".join(SCENARIOS))
    parser.add_argument("--rows", default="1,10,30,60")
    parser.add_argument("--reps", type=int, default=REPS_DEFAULT)
    parser.add_argument("--timeout", type=int, default=3600)
    parser.add_argument("--gpu-backend", choices=GPU_BACKENDS, default=None,
                        help="Passed to Blender as --gpu-backend; omitted when not given")
    parser.add_argument("--no-render", action="store_true",
                        help="Skip all renders (for large N). Extract counters are produced by "
                             "the draw, so with this flag they read 0 and are not meaningful.")
    args = parser.parse_args(argv)

    if args.no_render:
        print("WARNING: --no-render: extract counters need a draw, expect 0 layered extracts",
              flush=True)
    os.makedirs(args.out, exist_ok=True)
    here = os.path.abspath(__file__)
    results = []
    for scenario in args.scenarios.split(","):
        scenario = scenario.strip()
        for rows in [int(r) for r in args.rows.split(",")]:
            entry = {"scenario": scenario, "rows": rows, "reps": args.reps}
            if scenario in GUI_ONLY:
                entry["status"] = "NOT_RUN_BACKGROUND"
                entry["note"] = "Needs a viewport (EEVEE draw, timers, UI undo); see the report."
                results.append(entry)
                print(f"{scenario} N={rows}: NOT_RUN_BACKGROUND", flush=True)
                continue
            cmd = [args.exe, "--background", "--factory-startup", "--debug-gpu"]
            if args.gpu_backend:
                cmd += ["--gpu-backend", args.gpu_backend]
            cmd += ["--python-exit-code", "1", "--python", here, "--",
                    "--worker", "--scenario", scenario, "--rows", str(rows),
                    "--reps", str(args.reps), "--out", args.out, "--label", args.label]
            if args.no_render:
                cmd.append("--no-render")
            t0 = time.time()
            try:
                proc = subprocess.run(cmd, capture_output=True, timeout=args.timeout)
                text = proc.stdout.decode("utf-8", errors="replace")
                stderr = proc.stderr.decode("utf-8", errors="replace")
                returncode = proc.returncode
            except subprocess.TimeoutExpired as exc:
                text = (exc.stdout or b"").decode("utf-8", errors="replace")
                stderr = "TIMEOUT"
                returncode = -999
            log_path = os.path.join(args.out, f"log_{args.label}_{scenario}_{rows}.txt")
            with open(log_path, "w", encoding="utf-8") as fh:
                fh.write(text)
                fh.write("\n--- stderr ---\n")
                fh.write(stderr)
            rep_metrics, checks, errors, diag = parse_log(text)
            render_skips = diag["render_skips"]
            entry["returncode"] = returncode
            entry["elapsed_s"] = round(time.time() - t0, 1)
            entry["log"] = log_path
            entry["reps_done"] = len(rep_metrics)
            entry["per_rep"] = rep_metrics
            entry["summary"] = summarize(rep_metrics)
            entry["checks"] = checks
            entry["errors"] = errors
            entry["gpu_backend"] = args.gpu_backend
            entry["render_skipped"] = bool(render_skips) or args.no_render
            entry["render_skip_reason"] = (render_skips[-1].get("reason") if render_skips
                                           else None)
            entry["layered"] = {
                "name": LAYERED_EXTRACT_NAME,
                "extract_layered_total": sum(r["extract_layered"] for r in rep_metrics),
                "tracked_1_total": sum(r["tracked_1"] for r in rep_metrics),
                "tracked_0_total": sum(r["tracked_0"] for r in rep_metrics),
                "refused": diag["refused"],
            }
            if errors or returncode != 0 or len(rep_metrics) < args.reps:
                entry["status"] = "ERROR"
            else:
                entry["status"] = "OK"
            results.append(entry)
            med = entry["summary"].get("full_rebuild", {}).get("median")
            comp = entry["summary"].get("compile", {}).get("median")
            layered = entry["summary"].get("extract_layered", {}).get("median")
            print(f"{scenario} N={rows}: {entry['status']} reps={len(rep_metrics)} "
                  f"full_rebuild_med={med} compile_med={comp} extract_layered_med={layered} "
                  f"render_skipped={entry['render_skipped']} elapsed={entry['elapsed_s']}s",
                  flush=True)

    out_path = os.path.join(args.out, f"results_{args.label}.json")
    with open(out_path, "w", encoding="utf-8") as fh:
        json.dump({"label": args.label, "exe": args.exe, "results": results}, fh, indent=1,
                  ensure_ascii=False)
    print(f"WROTE {out_path}", flush=True)


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


def worker(argv):
    import bpy
    import numpy as np

    parser = argparse.ArgumentParser()
    parser.add_argument("--worker", action="store_true")
    parser.add_argument("--scenario", required=True)
    parser.add_argument("--rows", type=int, required=True)
    parser.add_argument("--reps", type=int, default=REPS_DEFAULT)
    parser.add_argument("--out", required=True)
    parser.add_argument("--label", default="X")
    parser.add_argument("--no-render", action="store_true")
    args = parser.parse_args(argv)
    os.makedirs(args.out, exist_ok=True)
    tag = f"{args.label}_{args.scenario}_{args.rows}"

    # ---- scene -------------------------------------------------------------------------------
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = 'BLENDER_EEVEE'
    scene.render.resolution_x = 64
    scene.render.resolution_y = 64
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = 'PNG'
    scene.render.image_settings.color_depth = '8'
    scene.eevee.taa_render_samples = 1

    mesh = bpy.data.meshes.new("PlaneMesh")
    mesh.from_pydata([(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)], [], [(0, 1, 2, 3)])
    uv = mesh.uv_layers.new(name="UVMap")
    for loop_index, co in enumerate([(0, 0), (1, 0), (1, 1), (0, 1)]):
        uv.data[loop_index].uv = co
    ob = bpy.data.objects.new("Plane", mesh)
    bpy.context.collection.objects.link(ob)
    cam = bpy.data.cameras.new("Cam")
    cam_ob = bpy.data.objects.new("Cam", cam)
    bpy.context.collection.objects.link(cam_ob)
    cam_ob.location = (0.0, 0.0, 3.0)
    scene.camera = cam_ob
    sun = bpy.data.lights.new("Sun", 'SUN')
    sun.energy = 3.0
    sun_ob = bpy.data.objects.new("Sun", sun)
    bpy.context.collection.objects.link(sun_ob)
    sun_ob.rotation_euler = (0.3, 0.2, 0.0)

    names = {"paint0": "Paint 0"}

    # ---- helpers -------------------------------------------------------------------------------

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

    def material():
        return bpy.data.materials[MATERIAL_NAME]

    def p0():
        return layer_named(material(), names["paint0"])

    def layer_named(ma, name):
        for layer in ma.paint_layers:
            if layer.name == name:
                return layer
        raise KeyError(f"no paint layer named {name!r}")

    def record(layer, channel):
        return next(c for c in layer.channels if c.channel == channel)

    # Once a render fails (e.g. a sampler limit at large N) or --no-render is given, all further
    # renders are skipped, the scenario still runs and the orchestrator records render_skipped.
    render_state = {"skipped": args.no_render}

    def skip_render(reason):
        render_state["skipped"] = True
        emit("PERF_RENDER_SKIPPED " + json.dumps({"scenario": args.scenario, "rows": args.rows,
                                                  "reason": reason}))

    def render_to(path=None):
        bpy.context.view_layer.update()
        if render_state["skipped"]:
            return
        try:
            bpy.ops.render.render(write_still=False)
            if path is not None:
                bpy.data.images['Render Result'].save_render(filepath=path)
        except Exception as exc:
            skip_render(str(exc)[:300])

    def tick(path=None):
        render_to(path)

    def make_state(rows, scenario):
        """Declarative stack: the same description builds the stack and is read back from it."""
        state = {"rows": [], "mask": None}
        for i in range(rows):
            row = {
                "name": f"Paint {i}",
                "source": "IMAGE",
                "opacity": 1.0,
                "blend": "MIX",
                "enabled": True,
                "channels": {
                    ch: {"image": f"img_{i}_{ch}_v0", "invert": False} for ch in CHANNELS
                },
                "mask_items": [],
                "effects": [],
            }
            state["rows"].append(row)
        if scenario == "S3":
            state["rows"].append({"name": "Fill", "source": "CONSTANT", "opacity": 1.0,
                                  "blend": "MIX", "enabled": True, "channels": {},
                                  "mask_items": [], "effects": []})
        if scenario in ("S8", "S9"):
            state["rows"].append({"name": "Grad", "source": "GENERATOR", "opacity": 1.0,
                                  "blend": "MIX", "enabled": True, "channels": {},
                                  "mask_items": [], "effects": [],
                                  "stops": [[0.0, [0.0, 0.0, 0.0, 1.0]],
                                            [1.0, [1.0, 1.0, 1.0, 1.0]]]})
        if scenario == "S7":
            state["rows"][0]["mask_items"] = [
                {"name": "", "channels": {"BASE_COLOR": {"image": "mask_v0", "invert": False}}}]
            state["rows"][0]["effects"] = [
                {"name": "", "channels": {"BASE_COLOR": {"image": "corr_v0", "invert": False}}}]
        return state

    def set_record(rec, cfg):
        # Records without a map (a Fill row's, say) carry no image in the state; they stay empty.
        if cfg["image"] is not None:
            rec.image = image_for(cfg["image"])
        if cfg["invert"]:
            rec.invert = True

    def apply_state(ma, state):
        """Build the stack from a declarative state, in list order (collection order = list)."""
        ma.paint_layers_channels = set(CHANNELS)
        for row in state["rows"]:
            if row["source"] == "IMAGE":
                layer = ma.paint_layers.new(name=row["name"], source='IMAGE',
                                            make_active=False)
            elif row["source"] == "CONSTANT":
                layer = ma.paint_layers.new(name=row["name"], source='CONSTANT',
                                            make_active=False)
            else:
                layer = ma.paint_layers.new(name=row["name"], source='GENERATOR',
                                            make_active=False)
            layer.opacity = row["opacity"]
            layer.blend_type = row["blend"]
            layer.enabled = row["enabled"]
            for ch, cfg in row["channels"].items():
                set_record(record(layer, ch), cfg)
            for item in row["mask_items"]:
                # The first mask correction also creates the base mask item, so it is not added twice.
                if item["name"] == "Mask":
                    continue
                mi = layer.mask_item_add(name=item["name"], source='IMAGE')
                for ch, cfg in item["channels"].items():
                    set_record(record(mi, ch), cfg)
            for item in row["effects"]:
                ef = layer.effect_add(name=item["name"], source='IMAGE')
                for ch, cfg in item["channels"].items():
                    set_record(record(ef, ch), cfg)
            if "stops" in row:
                set_stops(layer, row["stops"])
        return ma

    def set_stops(layer, stops):
        ramp = layer.gradient_ramp
        elements = ramp.elements
        while len(elements) > len(stops):
            elements.remove(elements[-1])
        while len(elements) < len(stops):
            elements.new(0.5)
        for el, (pos, col) in zip(elements, stops):
            el.position = pos
            el.color = col

    def observe_state(ma):
        state = {"rows": []}
        for layer in ma.paint_layers:
            row = {
                "name": layer.name,
                "source": layer.source,
                "opacity": round(float(layer.opacity), 6),
                "blend": layer.blend_type,
                "enabled": bool(layer.enabled),
                "channels": {},
                "mask_items": [],
                "effects": [],
            }
            for rec in layer.channels:
                row["channels"][rec.channel] = {
                    "image": rec.image.name if rec.image else None,
                    "invert": bool(rec.invert),
                }
            for item in layer.mask_items:
                row["mask_items"].append({"name": item.name, "channels": {
                    rec.channel: {"image": rec.image.name if rec.image else None,
                                  "invert": bool(rec.invert)} for rec in item.channels}})
            for item in layer.effects:
                row["effects"].append({"name": item.name, "channels": {
                    rec.channel: {"image": rec.image.name if rec.image else None,
                                  "invert": bool(rec.invert)} for rec in item.channels}})
            if layer.gradient_supported:
                row["stops"] = [[float(el.position), [float(c) for c in el.color]]
                                for el in layer.gradient_ramp.elements]
            state["rows"].append(row)
        return state

    def digest(ma):
        from collections import Counter
        nt = ma.node_tree
        if nt is None:
            return None
        kinds = Counter(n.bl_idname for n in nt.nodes)
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
        return {
            "nodes": sum(kinds.values()),
            "kinds": sorted(kinds.items()),
            "links": len(nt.links),
            "values_crc": zlib.crc32("|".join(values).encode("utf-8")),
        }

    def compare_png(path_a, path_b):
        a = bpy.data.images.load(path_a, check_existing=False)
        b = bpy.data.images.load(path_b, check_existing=False)
        pa = np.empty(len(a.pixels), dtype=np.float32)
        pb = np.empty(len(b.pixels), dtype=np.float32)
        a.pixels.foreach_get(pa)
        b.pixels.foreach_get(pb)
        bpy.data.images.remove(a)
        bpy.data.images.remove(b)
        if pa.shape != pb.shape:
            return {"max_diff_255": None, "reason": "size mismatch"}
        return {"max_diff_255": float(np.max(np.abs(pa - pb)) * 255.0)}

    # ---- scenario actions ----------------------------------------------------------------------

    def ops_for(scenario, ma, rep):
        """Return a list of tick callables for one repetition."""
        ticks = []
        other_row_name = "Paint 1" if args.rows > 1 else "Paint 0"
        if scenario == "S1":
            def set_op(value):
                def f():
                    p0().opacity = value
                return f
            ticks = [set_op(0.02 * (k + 1)) for k in range(50)]
        elif scenario == "S2":
            def rename_op(k):
                def f():
                    new_name = f"Renamed {rep}_{k}"
                    p0().name = new_name
                    names["paint0"] = new_name
                return f
            ticks = [rename_op(k) for k in range(10)]
        elif scenario == "S3":
            def fill_op(v):
                def f():
                    layer_named(material(), "Fill").opacity = v
                return f
            ticks = [fill_op(1.0 if k % 2 == 0 else 0.0) for k in range(10)]
        elif scenario == "S4":
            def enabled_op(v):
                def f():
                    layer_named(material(), other_row_name).enabled = v
                return f
            ticks = [enabled_op(k % 2 == 0) for k in range(10)]
        elif scenario == "S5":
            modes = ['MULTIPLY', 'OVERLAY', 'ADD', 'DARKEN', 'BURN', 'LIGHTEN', 'SCREEN',
                     'DODGE', 'SUBTRACT', 'DIVIDE']
            def blend_op(m):
                def f():
                    p0().blend_type = m
                return f
            ticks = [blend_op(m) for m in modes]
        elif scenario == "S6":
            def swap_op(v):
                def f():
                    rec = record(p0(), "BASE_COLOR")
                    rec.image = image_for(f"img_0_BASE_COLOR_v{v}")
                return f
            ticks = [swap_op(k % 2) for k in range(10)]
        elif scenario == "S7":
            def mask_corr_op(v):
                def f():
                    row = p0()
                    item = row.mask_items[-1]
                    record(item, "BASE_COLOR").image = image_for(f"mask_v{v}")
                    effect = row.effects[-1]
                    record(effect, "BASE_COLOR").image = image_for(f"corr_v{v}")
                return f
            ticks = [mask_corr_op(k % 2) for k in range(10)]
        elif scenario == "S8":
            def stop_op(k):
                def f():
                    grad = layer_named(material(), "Grad")
                    el = grad.gradient_ramp.elements[0]
                    el.position = 0.05 + 0.4 * ((k % 10) / 10.0)
                    el.color = (0.1 * (k % 10), 0.5, 1.0 - 0.05 * (k % 10), 1.0)
                return f
            ticks = [stop_op(k) for k in range(50)]
        elif scenario == "S9":
            def count_op(n):
                def f():
                    set_stops(layer_named(material(), "Grad"),
                              [[(j / max(1, n - 1)), [j / 4.0, 0.5, 0.5, 1.0]]
                               for j in range(n)])
                return f
            ticks = [count_op(1), count_op(2), count_op(5)]
        elif scenario == "S10":
            def invert_op(v):
                def f():
                    record(p0(), "BASE_COLOR").invert = v
                return f
            ticks = [invert_op(k % 2 == 1) for k in range(10)]
        elif scenario == "S11":
            def add_op(k):
                def f():
                    m = material()
                    new = m.paint_layers.new(name=f"Tmp {rep}_{k}", source='CONSTANT',
                                             make_active=False)
                    m.paint_layers.reorder(new, index=0)
                return f
            def remove_op(k):
                def f():
                    m = material()
                    m.paint_layers.remove(layer_named(m, f"Tmp {rep}_{k}"))
                return f
            for k in range(10):
                ticks.append(add_op(k))
                ticks.append(remove_op(k))
        elif scenario == "S12":
            def undo_op(k):
                def f():
                    p0().opacity = 0.2 + 0.05 * (k % 10)
                    bpy.ops.ed.undo_push(message=f"perf {k}")
                    bpy.ops.ed.undo()
                    bpy.ops.ed.redo()
                return f
            ticks = [undo_op(k) for k in range(10)]
        elif scenario == "S13":
            def struct_op(k):
                def f():
                    m = material()
                    m.paint_layers.new(name=f"Tmp {rep}_{k}", source='CONSTANT',
                                       make_active=False)
                    bpy.ops.ed.undo_push(message=f"struct {k}")
                    bpy.ops.ed.undo()
                    bpy.ops.ed.redo()
                    m = material()
                    layer_named(m, f"Tmp {rep}_{k}")
                    m.paint_layers.remove(layer_named(m, f"Tmp {rep}_{k}"))
                return f
            ticks = [struct_op(k) for k in range(10)]
        else:
            raise ValueError(scenario)
        return ticks

    # ---- run ------------------------------------------------------------------------------------

    if args.scenario in GUI_ONLY:
        emit("PERF_ERROR " + json.dumps({"scenario": args.scenario,
                                         "error": "GUI-only scenario"}))
        return

    ob.data.materials.append(bpy.data.materials.new(MATERIAL_NAME))
    ma = material()
    ma.use_nodes = True
    if args.no_render:
        skip_render("--no-render")
    try:
        state0 = make_state(args.rows, args.scenario)
        apply_state(ma, state0)
        render_to()  # Builds the stack and compiles the first shaders outside the measurements.
        init_png = os.path.join(args.out, f"{tag}_init.png")
        render_to(init_png)

        for rep in range(args.reps):
            emit(f"PERF_MARK_BEGIN rep={rep}")
            t0 = time.perf_counter()
            for index, action in enumerate(ops_for(args.scenario, ma, rep)):
                action()
                tick()
            wall_ms = (time.perf_counter() - t0) * 1000.0
            emit(f"PERF_MARK_END rep={rep} wall_ms={wall_ms:.3f}")
            ma = material()

        # Section 5: rendering after the sync path equals a fresh build of the final state.
        # Undo replaces the datablocks, so the object is looked up again.
        ob = bpy.data.objects["Plane"]
        if (args.scenario in RENDER_CHECK or args.scenario == "S13") \
                and not render_state["skipped"]:
            check = {"scenario": args.scenario, "rows": args.rows}
            mut_png = os.path.join(args.out, f"{tag}_mut.png")
            fresh_png = os.path.join(args.out, f"{tag}_fresh.png")
            reload_png = os.path.join(args.out, f"{tag}_reload.png")
            render_to(mut_png)
            final_state = observe_state(ma)
            check["digest_mut"] = digest(ma)
            fresh = bpy.data.materials.new("Fresh")
            fresh.use_nodes = True
            apply_state(fresh, final_state)
            bpy.context.view_layer.update()
            ob.data.materials[0] = fresh
            check["digest_fresh"] = digest(fresh)
            fresh_state = observe_state(fresh)
            check["state_equal_fresh"] = (fresh_state == final_state)
            if not check["state_equal_fresh"]:
                # Keep both sides so the first differing field can be read from the results file.
                check["state_mut"] = final_state
                check["state_fresh"] = fresh_state
            render_to(fresh_png)
            check["mut_vs_fresh"] = compare_png(mut_png, fresh_png)
            # Zero here would mean the check cannot tell a stale render from a correct one.
            check["mut_vs_initial"] = compare_png(init_png, mut_png)
            ob.data.materials[0] = ma

            blend_path = os.path.join(args.out, f"{tag}.blend")
            bpy.ops.wm.save_as_mainfile(filepath=blend_path, copy=True)
            bpy.ops.wm.open_mainfile(filepath=blend_path, load_ui=False)
            ma2 = bpy.data.materials.get(MATERIAL_NAME)
            if ma2 is not None:
                bpy.data.objects["Plane"].data.materials[0] = ma2
                render_to(reload_png)
                check["reload_vs_mut"] = compare_png(mut_png, reload_png)
                check["digest_reload"] = digest(ma2)
            check["status"] = "checked"
            emit("PERF_CHECK " + json.dumps(check))
    except Exception:
        emit("PERF_ERROR " + json.dumps({"scenario": args.scenario, "rows": args.rows,
                                         "error": traceback.format_exc()}))
        raise


def main():
    argv = sys.argv
    # Blender passes its own options before `--`; the harness options come after it. Plain Python
    # has no `--`, so its options are the whole argument list.
    tail = argv[argv.index("--") + 1:] if "--" in argv else argv[1:]
    if "--worker" in tail:
        worker(tail)
    else:
        orchestrate(tail)


if __name__ == "__main__":
    main()
