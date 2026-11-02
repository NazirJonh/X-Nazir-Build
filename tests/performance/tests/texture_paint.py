# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

import api
import enum
import pathlib


class MeshType(enum.IntEnum):
    CUBE = 0
    MONKEY = 1
    SUBDIV_3_MONKEY = 2


class DataType(enum.IntEnum):
    BYTE = 0
    FLOAT = 1


DIMENSIONS = [1024, 4096]


def set_view3d_context_override(context_override):
    """
    Set context override to become the first viewport in the active workspace

    The ``context_override`` is expected to be a copy of an actual current context
    obtained by `context.copy()`
    """

    for area in context_override["screen"].areas:
        if area.type != 'VIEW_3D':
            continue
        for space in area.spaces:
            if space.type != 'VIEW_3D':
                continue
            for region in area.regions:
                if region.type != 'WINDOW':
                    continue
                context_override["area"] = area
                context_override["region"] = region


def prepare_scene(context: any, object: MeshType, image_dimension: int, data_type: DataType):
    """
    Prepare a clean state of the scene suitable for benchmarking
    """
    import bpy

    # Ensure the current mode is object, as it might not be always the case
    # if the benchmark script is run from a non-clean state of the .blend file.
    if context.object:
        bpy.ops.object.mode_set(mode='OBJECT')

    # Delete all current objects from the scene.
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False)
    bpy.ops.outliner.orphans_purge()

    if object == MeshType.MONKEY:
        bpy.ops.mesh.primitive_monkey_add(size=2, align='WORLD', location=(0, 0, 0), scale=(1, 1, 1))
    elif object == MeshType.CUBE:
        bpy.ops.mesh.primitive_cube_add(size=2, align='WORLD', location=(0, 0, 0), scale=(1, 1, 1))
    elif object == MeshType.SUBDIV_3_MONKEY:
        bpy.ops.mesh.primitive_monkey_add(size=2, align='WORLD', location=(0, 0, 0), scale=(1, 1, 1))
        bpy.ops.object.subdivision_set(level=3, relative=False, ensure_modifier=True)
        bpy.ops.object.modifier_apply(modifier="Subdivision")
    else:
        raise NotImplementedError

    context_override = context.copy()
    set_view3d_context_override(context_override)
    with context.temp_override(**context_override):
        bpy.ops.view3d.view_axis(type='FRONT')
        bpy.ops.view3d.view_selected()
    bpy.ops.object.mode_set(mode='SCULPT')

    is_float_image = data_type == DataType.FLOAT

    bpy.ops.paint.add_texture_paint_slot(
        type='BASE_COLOR',
        slot_type='IMAGE',
        name="Untitled",
        color=(
            1.0,
            1.0,
            1.0,
            1.0),
        width=image_dimension,
        height=image_dimension,
        alpha=True,
        generated_type='BLANK',
        float=is_float_image)


def prepare_brush():
    import bpy
    bpy.ops.brush.asset_activate(
        asset_library_type='ESSENTIALS',
        relative_asset_identifier="brushes/essentials_brushes-mesh_sculpt.blend/Brush/Paint Hard")


def generate_stroke(context):
    """
    Generate stroke for the bpy.ops.sculpt.brush_stroke operator

    The generated stroke coves the full plane diagonal.
    """
    import bpy
    from mathutils import Vector

    template = {
        "name": "stroke",
        "mouse": (0.0, 0.0),
        "mouse_event": (0, 0),
        "is_start": True,
        "location": (0, 0, 0),
        "pressure": 1.0,
        "time": 1.0,
        "size": 1.0,
        "x_tilt": 0,
        "y_tilt": 0
    }

    version = bpy.app.version
    if version[0] <= 4 and version[1] <= 3:
        template["pen_flip"] = False

    num_steps = 100
    start = Vector((context["area"].width, context["area"].height))
    end = Vector((0, 0))
    delta = (end - start) / (num_steps - 1)

    stroke = []
    for i in range(num_steps):
        step = template.copy()
        step["mouse_event"] = start + delta * i
        stroke.append(step)

    return stroke


def _run_brush_test(args: dict):
    import bpy
    import time

    # This test can only run in alpha, for now, due to the texture paint mode being an experimental feature
    if bpy.app.version_cycle != 'alpha':
        return {"time": 0.0}

    context = bpy.context

    timeout = 10
    total_time_start = time.time()

    # Create an undo stack explicitly. This isn't created by default in background mode.
    bpy.ops.ed.undo_push()

    prepare_brush()

    min_measurements = 5
    max_measurements = 100
    measurements = []
    while True:
        prepare_scene(context, args["object_type"], args["dimension"], args["data_type"])
        context_override = context.copy()
        set_view3d_context_override(context_override)
        with context.temp_override(**context_override):
            start = time.time()
            bpy.ops.sculpt.brush_stroke(stroke=generate_stroke(context_override), override_location=True)
            bpy.ops.ed.undo_push()
            measurements.append(time.time() - start)
        if len(measurements) >= min_measurements and (time.time() - total_time_start) > timeout:
            break
        if len(measurements) >= max_measurements:
            break

    return {"time": sum(measurements) / len(measurements)}


# ---------------------------------------------------------------------------
# PBR material-channel stroke (TZ "PERF dab pipeline", phase E0)
# ---------------------------------------------------------------------------

# The four channels a Principled stroke writes: PAINT_MATERIAL_CHANNEL_BASE_COLOR,
# _METALLIC, _ROUGHNESS and _NORMAL of brush.material_paint.channels (10 entries, index order
# fixed by eMaterialPaintChannel in DNA_scene_types.h).
PBR_CHANNEL_INDICES = (0, 1, 2, 4)

# Scenario matrix: channel count (first N of PBR_CHANNEL_INDICES) and brush spacing in percent of the
# radius. Spacing 2 is the stress case (many dabs per event); 10 is closer to what a user paints with.
PBR_CHANNEL_COUNTS = [1, 4]
PBR_SPACINGS = [2, 10]

# One mouse event per stroke step: this is the unit the merged-dab work is measured in, so the
# scenario fixes the event count and reports milliseconds per event.
PBR_EVENT_COUNT = 40

PBR_DIMENSIONS = [1024, 4096]

# Material.paint_layers_image_size is an enum of the sizes a new channel map can be created with.
PBR_IMAGE_SIZE_IDENTIFIERS = {1024: 'SIZE_1K', 4096: 'SIZE_4K'}


def prepare_pbr_scene(context, image_dimension: int, channel_num: int, spacing: int):
    """
    A monkey with one Principled material, painted through the Material canvas with four channels.

    The channel maps are created when the object enters Sculpt mode with
    ``paint_mode.canvas_source == 'MATERIAL'`` (see BKE_paint_principled_channel_image_ensure), so
    the mode is set before the mode switch here.
    """
    import bpy

    if context.object:
        bpy.ops.object.mode_set(mode='OBJECT')

    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False)
    bpy.ops.outliner.orphans_purge()

    bpy.ops.mesh.primitive_monkey_add(size=2, align='WORLD', location=(0, 0, 0), scale=(1, 1, 1))
    object = context.active_object

    material = bpy.data.materials.new("PerfPBRMat")
    material.use_nodes = True
    material.paint_layers_image_size = PBR_IMAGE_SIZE_IDENTIFIERS[image_dimension]
    object.data.materials.append(material)

    bpy.context.tool_settings.paint_mode.canvas_source = 'MATERIAL'

    brush = bpy.data.brushes.new("PerfPBRBrush", mode='SCULPT')
    brush.sculpt_brush_type = 'PAINT'
    brush.strength = 0.5
    brush.spacing = spacing  # PROP_INT percentage of the radius between dabs.
    # A small radius keeps the texel count of a dab near the size the TODO numbers quote (~80k
    # texels over four channels on one PBVH node) instead of covering the whole model.
    brush.size = 0.25
    bpy.context.tool_settings.sculpt.brush = brush

    bpy.ops.object.mode_set(mode='SCULPT')

    result = bpy.ops.paint.material_paint_brush_ensure()
    assert result == {'FINISHED'}, f"material paint ensure failed: {result}"

    for channel_index in PBR_CHANNEL_INDICES[:channel_num]:
        channel = brush.material_paint.channels[channel_index]
        channel.use = True
        channel.value = (1.0, 0.0, 0.0, 1.0)

    bpy.ops.object.mode_set(mode='OBJECT')
    bpy.ops.object.mode_set(mode='SCULPT')
    return brush


def generate_pbr_stroke(context, num_events: int):
    """
    A diagonal stroke of exactly ``num_events`` mouse events.

    ``bpy.ops.sculpt.brush_stroke`` feeds one entry per event, so with spacing enabled each entry
    becomes the run of dabs one real mouse move emits - the unit the merged write batches.
    """
    from mathutils import Vector

    template = {
        "name": "stroke",
        "mouse": (0.0, 0.0),
        "mouse_event": (0, 0),
        "is_start": True,
        "location": (0, 0, 0),
        "pressure": 1.0,
        "time": 1.0,
        "size": 1.0,
        "x_tilt": 0,
        "y_tilt": 0,
    }

    start = Vector((context["area"].width * 0.2, context["area"].height * 0.2))
    end = Vector((context["area"].width * 0.8, context["area"].height * 0.8))
    delta = (end - start) / max(num_events - 1, 1)

    stroke = []
    for i in range(num_events):
        step = template.copy()
        step["is_start"] = i == 0
        step["mouse_event"] = start + delta * i
        step["mouse"] = start + delta * i
        stroke.append(step)

    return stroke


def pbr_canvas_checksum():
    """
    SHA-1 over the pixels of every paint canvas image, ordered by name.

    The optimization phases must not change the painted result (float maps: bit for bit; 8-bit maps:
    within one LSB), so each run reports this to compare against the baseline. It is printed rather
    than returned because the result dict feeds the graphs and only takes numbers.
    """
    import bpy
    import array
    import hashlib

    digest = hashlib.sha1()
    for image in sorted((im for im in bpy.data.images if im.is_paint_canvas), key=lambda im: im.name):
        pixels = array.array('f', [0.0]) * len(image.pixels)
        image.pixels.foreach_get(pixels)
        digest.update(image.name.encode())
        digest.update(pixels.tobytes())
    return digest.hexdigest()


def _run_pbr_channels_test(args: dict):
    import bpy
    import time

    context = bpy.context
    num_events = args["events"]

    timeout = 10
    total_time_start = time.time()

    # Create an undo stack explicitly. This isn't created by default in background mode.
    bpy.ops.ed.undo_push()

    min_measurements = 3
    max_measurements = 60
    measurements = []
    while True:
        prepare_pbr_scene(context, args["dimension"], args["channels"], args["spacing"])
        context_override = context.copy()
        set_view3d_context_override(context_override)
        with context.temp_override(**context_override):
            start = time.time()
            bpy.ops.sculpt.brush_stroke(
                stroke=generate_pbr_stroke(context_override, num_events),
                override_location=True,
            )
            bpy.ops.ed.undo_push()
            # Seconds per mouse event, the number the phases of the dab-pipeline work move.
            measurements.append((time.time() - start) / num_events)
            # Outside the timed region. Identical every run, so the last one is reported.
            checksum = pbr_canvas_checksum()
        if len(measurements) >= min_measurements and (time.time() - total_time_start) > timeout:
            break
        if len(measurements) >= max_measurements:
            break

    # Measurement 0 pays for the one-time setup (map creation, shader and cache warm-up), so the
    # median of the rest is the number to compare; the plain mean stays as "time" for the graphs.
    steady = sorted(measurements[1:] or measurements)
    print("PBR_PERF_RESULT dimension={} channels={} spacing={} events={} runs={} "
          "median_s_per_event={:.6f} checksum={}".format(
              args["dimension"], args["channels"], args["spacing"], num_events,
              len(measurements), steady[len(steady) // 2], checksum))
    return {"time": sum(measurements) / len(measurements)}


class PbrChannelsPaintTest(api.Test):
    """Four-channel Principled stroke over a baked UV layout, timed per mouse event."""

    def __init__(self, filepath: pathlib.Path, dimension: int, channels: int, spacing: int):
        self.filepath = filepath
        self.dimension = dimension
        self.channels = channels
        self.spacing = spacing

    def name(self):
        return "pbr_channels_{}_{}_sp{}".format(self.channels, self.dimension, self.spacing)

    def category(self):
        return "texture_paint"

    def run(self, env, _device_id, _gpu_backend):
        args = {
            'dimension': self.dimension,
            'channels': self.channels,
            'spacing': self.spacing,
            'events': PBR_EVENT_COUNT,
        }

        result, _ = env.run_in_blender(_run_pbr_channels_test, args, [self.filepath])

        return result


class TexturePaintBrushTest(api.Test):
    def __init__(self, filepath: pathlib.Path, object_type: MeshType, dimension: int, data_type: DataType):
        self.filepath = filepath
        self.object_type = object_type
        self.dimension = dimension
        self.data_type = data_type

    def name(self):
        return "{}_{}_{}".format(self.object_type.name.lower(), self.data_type.name.lower(), self.dimension)

    def category(self):
        return "texture_paint"

    def run(self, env, _device_id, _gpu_backend):
        args = {
            'object_type': self.object_type,
            'dimension': self.dimension,
            'data_type': self.data_type
        }

        result, _ = env.run_in_blender(_run_brush_test, args, [self.filepath])

        return result


def generate(env):
    filepaths = env.find_blend_files('texture_paint/*')
    # For now, we only expect there to ever be a single file to use as the basis for generating other brush tests
    assert len(filepaths) == 1

    brush_tests = [TexturePaintBrushTest(filepaths[0], object_type, dimension, data_type)
                   for object_type in MeshType for dimension in DIMENSIONS for data_type in DataType]

    # The four-channel Principled stroke (TZ "PERF dab pipeline"). Its `time` is seconds per mouse
    # event, which is the unit the merged-dab, row-cache and source-sample phases move.
    brush_tests += [PbrChannelsPaintTest(filepaths[0], dimension, channels, spacing)
                    for dimension in PBR_DIMENSIONS
                    for channels in PBR_CHANNEL_COUNTS
                    for spacing in PBR_SPACINGS]

    return brush_tests
