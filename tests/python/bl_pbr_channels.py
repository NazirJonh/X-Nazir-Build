# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
blender -b --factory-startup --python tests/python/bl_pbr_channels.py
"""

import os
import sys

import bpy

sys.path.append(os.path.join(os.path.dirname(os.path.realpath(__file__)), "sculpt_paint"))
from modules.test_helpers import BackendType, generate_monkey, generate_stroke, set_view3d_context_override

# PAINT_MATERIAL_CHANNEL_METALLIC
METALLIC_CHANNEL_INDEX = 1
METALLIC_DEFAULT = 0.0
# PAINT_MATERIAL_CHANNEL_CUSTOM
CUSTOM_CHANNEL_INDEX = 5
# Fixed attribute name of the metallic channel, from the descriptor table in paint.cc.
METALLIC_ATTRIBUTE = "material_metallic"


def _ensure_active_mesh_object():
    """Return an active mesh object suitable for entering Sculpt mode."""
    for ob in bpy.context.view_layer.objects:
        if ob.type == 'MESH':
            bpy.context.view_layer.objects.active = ob
            ob.select_set(True)
            return ob

    mesh = bpy.data.meshes.new("PBRChannelsTestMesh")
    mesh.from_pydata(
        [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (1.0, 1.0, 0.0)],
        [],
        [(0, 1, 2)],
    )
    ob = bpy.data.objects.new("PBRChannelsTestObject", mesh)
    bpy.context.collection.objects.link(ob)
    bpy.context.view_layer.objects.active = ob
    ob.select_set(True)
    return ob


def _find_metallic_image():
    for image in bpy.data.images:
        if image.name.endswith("Metallic"):
            return image
    return None


def _image_center_grayscale(image):
    width, height = image.size
    index = 4 * ((height // 2) * width + (width // 2))
    return image.pixels[index]


def _max_attribute_value(mesh, name):
    """Largest value of point float attribute ``name``, or None when it does not exist."""
    attribute = mesh.attributes.get(name)
    if attribute is None:
        return None
    return max((element.value for element in attribute.data), default=None)


def _setup_vertex_paint_brush(name):
    """Enter Sculpt mode on a monkey with a Material Paint canvas and return the brush."""
    bpy.ops.wm.read_factory_settings(use_empty=True)
    generate_monkey(BackendType.MESH)

    bpy.context.tool_settings.paint_mode.canvas_source = 'MATERIAL_PAINT'

    brush = bpy.data.brushes.new(name, mode='SCULPT')
    brush.sculpt_brush_type = 'PAINT'
    brush.strength = 1.0
    bpy.context.tool_settings.sculpt.brush = brush

    if bpy.context.object.mode != 'SCULPT':
        bpy.ops.object.mode_set(mode='SCULPT')

    result = bpy.ops.paint.material_paint_brush_ensure()
    assert result == {'FINISHED'}, f"ensure operator failed: {result}"
    return brush


def _paint_stroke(mode='NORMAL'):
    context_override = bpy.context.copy()
    set_view3d_context_override(context_override)
    stroke = generate_stroke(context_override, (0.4, 0.4), (0.6, 0.6))

    with bpy.context.temp_override(**context_override):
        result = bpy.ops.sculpt.brush_stroke(
            stroke=stroke,
            override_location=True,
            mode=mode,
        )
    assert result == {'FINISHED'}, f"{mode} stroke failed: {result}"


def test_vertex_attribute_paint_creates_and_writes_attribute():
    """Material Paint canvas writes the metallic channel into a float point attribute."""
    brush = _setup_vertex_paint_brush("pbr_vertex_metallic_test")

    metallic = brush.material_paint.channels[METALLIC_CHANNEL_INDEX]
    metallic.use = True
    metallic.value = (1.0, 0.0, 0.0)

    ob = bpy.context.active_object
    _paint_stroke()

    painted = _max_attribute_value(ob.data, METALLIC_ATTRIBUTE)
    assert painted is not None, f"{METALLIC_ATTRIBUTE} was not created by the stroke"
    assert painted > 0.5, f"expected painted metallic near 1.0, got {painted}"


def test_vertex_attribute_paint_undo_redo():
    """Undo restores the pre-stroke attribute values; redo paints them again."""
    brush = _setup_vertex_paint_brush("pbr_vertex_undo_test")

    metallic = brush.material_paint.channels[METALLIC_CHANNEL_INDEX]
    metallic.use = True
    metallic.value = (1.0, 0.0, 0.0)

    ob = bpy.context.active_object
    _paint_stroke()

    painted = _max_attribute_value(ob.data, METALLIC_ATTRIBUTE)
    assert painted is not None and painted > 0.5, f"stroke did not paint, got {painted}"

    bpy.ops.ed.undo()
    # The attribute did not exist before the stroke, so the stroke's undo step created it;
    # undoing the stroke removes it again instead of leaving a zero-valued attribute behind.
    undone = _max_attribute_value(bpy.context.active_object.data, METALLIC_ATTRIBUTE)
    assert undone is None, f"undo left the stroke-created {METALLIC_ATTRIBUTE} attribute behind: {undone}"

    bpy.ops.ed.redo()
    redone = _max_attribute_value(bpy.context.active_object.data, METALLIC_ATTRIBUTE)
    assert redone is not None and redone > 0.5, f"redo did not restore the stroke, got {redone}"


def test_scalar_channel_ignores_blend_mode():
    """Scalar channels always interpolate with Mix, whatever blend mode is set on them.

    The blend modes are colour operations; applying Multiply to Metallic would compute a
    photometric result for a quantity that has none. Setting Multiply here (which over the 0.0
    default would pin the value at 0.0 if it were honoured) must still paint the value.
    """
    brush = _setup_vertex_paint_brush("pbr_scalar_blend_ignored_test")
    # A brush-level mode too, to prove neither of them reaches the scalar path.
    brush.blend = 'MUL'

    metallic = brush.material_paint.channels[METALLIC_CHANNEL_INDEX]
    metallic.use = True
    metallic.value = (1.0, 0.0, 0.0)
    metallic.blend = 'MUL'

    ob = bpy.context.active_object
    _paint_stroke()

    painted = _max_attribute_value(ob.data, METALLIC_ATTRIBUTE)
    assert painted is not None, f"{METALLIC_ATTRIBUTE} was not created by the stroke"
    assert painted > 0.5, (
        f"scalar channels must blend with Mix regardless of their blend mode, got {painted}"
    )


def test_custom_channel_without_name_is_inert():
    """An enabled Custom channel with no attribute name must paint nothing, not crash."""
    brush = _setup_vertex_paint_brush("pbr_custom_unnamed_test")

    custom = brush.material_paint.channels[CUSTOM_CHANNEL_INDEX]
    custom.use = True
    custom.value = (1.0, 0.0, 0.0)

    paint_mode = bpy.context.tool_settings.paint_mode
    paint_mode.material_paint_custom_attr = ""

    ob = bpy.context.active_object
    attributes_before = {attribute.name for attribute in ob.data.attributes}

    _paint_stroke()

    attributes_after = {attribute.name for attribute in ob.data.attributes}
    assert attributes_after == attributes_before, (
        f"unnamed Custom channel created attributes: {attributes_after - attributes_before}"
    )


def test_brush_material_paint_lazy_init():
    # PAINT_OT_material_paint_brush_ensure uses BKE_paint_get_active_from_context,
    # which only returns sculpt paint when the active object is in SCULPT mode.
    # In Object mode it falls back to image paint, so ensure would miss our brush.
    _ensure_active_mesh_object()
    if bpy.context.object.mode != 'SCULPT':
        bpy.ops.object.mode_set(mode='SCULPT')

    brush = bpy.data.brushes.new("test_brush_pbr_channels", mode='SCULPT')
    assert brush.material_paint is None

    bpy.context.tool_settings.sculpt.brush = brush
    assert bpy.context.tool_settings.sculpt.brush == brush

    result = bpy.ops.paint.material_paint_brush_ensure()
    assert result == {'FINISHED'}, f"ensure operator failed: {result}"

    assert brush.material_paint is not None
    assert len(brush.material_paint.channels) == 6


def test_metallic_paint_erase_restores_default():
    """Sculpt material-canvas paint then invert stroke restores metallic default."""
    bpy.ops.wm.read_factory_settings(use_empty=True)
    generate_monkey(BackendType.MESH)
    ob = bpy.context.active_object

    mat = bpy.data.materials.new("PBRTestMat")
    mat.use_nodes = True
    ob.data.materials.append(mat)

    paint_mode = bpy.context.tool_settings.paint_mode
    paint_mode.canvas_source = 'MATERIAL'

    brush = bpy.data.brushes.new("pbr_metallic_erase_test", mode='SCULPT')
    brush.sculpt_brush_type = 'PAINT'
    brush.strength = 1.0
    bpy.context.tool_settings.sculpt.brush = brush

    result = bpy.ops.paint.material_paint_brush_ensure()
    assert result == {'FINISHED'}, f"ensure operator failed: {result}"

    metallic = brush.material_paint.channels[METALLIC_CHANNEL_INDEX]
    metallic.use = True
    metallic.value = (1.0, 0.0, 0.0)

    bpy.ops.object.mode_set(mode='OBJECT')
    bpy.ops.object.mode_set(mode='SCULPT')

    metallic_image = _find_metallic_image()
    assert metallic_image is not None, "metallic map was not auto-created on sculpt enter"

    default_value = _image_center_grayscale(metallic_image)
    assert abs(default_value - METALLIC_DEFAULT) < 1e-4, (
        f"expected metallic default {METALLIC_DEFAULT}, got {default_value}"
    )

    context_override = bpy.context.copy()
    set_view3d_context_override(context_override)
    stroke = generate_stroke(context_override, (0.4, 0.4), (0.6, 0.6))

    with bpy.context.temp_override(**context_override):
        result = bpy.ops.sculpt.brush_stroke(
            stroke=stroke,
            override_location=True,
            mode='NORMAL',
        )
    assert result == {'FINISHED'}, f"paint stroke failed: {result}"

    painted_value = _image_center_grayscale(metallic_image)
    assert painted_value > 0.5, f"expected painted metallic near 1.0, got {painted_value}"

    with bpy.context.temp_override(**context_override):
        result = bpy.ops.sculpt.brush_stroke(
            stroke=stroke,
            override_location=True,
            mode='INVERT',
        )
    assert result == {'FINISHED'}, f"erase stroke failed: {result}"

    erased_value = _image_center_grayscale(metallic_image)
    assert abs(erased_value - METALLIC_DEFAULT) < 0.05, (
        f"erase should restore default {METALLIC_DEFAULT}, got {erased_value}"
    )


if __name__ == "__main__":
    test_brush_material_paint_lazy_init()
    test_metallic_paint_erase_restores_default()
    test_vertex_attribute_paint_creates_and_writes_attribute()
    test_vertex_attribute_paint_undo_redo()
    test_scalar_channel_ignores_blend_mode()
    test_custom_channel_without_name_is_inert()
    print("bl_pbr_channels: OK")
