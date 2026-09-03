# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
blender -b --factory-startup --python tests/python/bl_pbr_channels.py
"""

import os
import sys
import unittest

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
    # Channel maps are named "<Channel> TexLayer" (see BKE_paint_principled_channel_image_ensure).
    for image in bpy.data.images:
        if image.name.endswith("Metallic TexLayer"):
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
    assert len(brush.material_paint.channels) == 10


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


class TestChannelSourceImage(unittest.TestCase):
    def setUp(self):
        self.brush = bpy.data.brushes.new("SourceTest", mode='SCULPT')
        bpy.context.tool_settings.sculpt.brush = self.brush
        bpy.ops.paint.material_paint_brush_ensure()
        self.channels = {c.channel: c for c in self.brush.material_paint.channels}

    def tearDown(self):
        bpy.data.brushes.remove(self.brush)

    def test_assign_creates_image_texture(self):
        image = bpy.data.images.new("SourceImage", 4, 4)
        channel = self.channels['ROUGHNESS']
        channel.source_image = image

        self.assertIsNotNone(channel.source_texture_slot.texture)
        self.assertEqual(channel.source_texture_slot.texture.type, 'IMAGE')
        self.assertEqual(channel.source_image, image)

    def test_reassign_same_image_reuses_texture(self):
        image = bpy.data.images.new("SourceImage", 4, 4)
        channel = self.channels['ROUGHNESS']
        channel.source_image = image
        first = channel.source_texture_slot.texture
        channel.source_image = image

        self.assertIs(channel.source_texture_slot.texture, first)

    def test_same_image_in_two_channels_gets_two_textures(self):
        image = bpy.data.images.new("SourceImage", 4, 4)
        self.channels['ROUGHNESS'].source_image = image
        self.channels['METALLIC'].source_image = image

        self.assertIsNot(
            self.channels['ROUGHNESS'].source_texture_slot.texture,
            self.channels['METALLIC'].source_texture_slot.texture,
        )

    def test_clearing_removes_texture_not_just_image(self):
        image = bpy.data.images.new("SourceImage", 4, 4)
        channel = self.channels['ROUGHNESS']
        channel.source_image = image
        channel.source_image = None

        # Leaving an empty Tex behind would keep the source nominally active and make the
        # engine paint zeros instead of the slider value.
        self.assertIsNone(channel.source_texture_slot.texture)
        self.assertIsNone(channel.source_image)

    def test_assign_over_procedural_texture_replaces_it(self):
        procedural = bpy.data.textures.new("Procedural", type='CLOUDS')
        channel = self.channels['ROUGHNESS']
        channel.source_texture_slot.texture = procedural

        image = bpy.data.images.new("SourceImage", 4, 4)
        channel.source_image = image

        self.assertEqual(channel.source_texture_slot.texture.type, 'IMAGE')
        self.assertIsNot(channel.source_texture_slot.texture, procedural)
        # The user's procedural texture must not be mutated in place.
        self.assertEqual(procedural.type, 'CLOUDS')


class TestPaintLayerId(unittest.TestCase):
    NIL = "00000000-0000-0000-0000-000000000000"
    SAMPLE = "1b4e28ba-2fa1-11d2-883f-0016d3cca427"

    def _new_image(self, name):
        img = bpy.data.images.new(name, 4, 4)
        self.addCleanup(bpy.data.images.remove, img)
        return img

    def test_default_is_empty(self):
        img = self._new_image("LayerIdDefault")
        self.assertEqual(img.paint_layer_id, "")

    def test_set_valid_roundtrips(self):
        img = self._new_image("LayerIdRoundtrip")
        img.paint_layer_id = self.SAMPLE
        self.assertEqual(img.paint_layer_id, self.SAMPLE)

    def test_set_uppercase_canonicalises_to_lower(self):
        img = self._new_image("LayerIdUpper")
        img.paint_layer_id = self.SAMPLE.upper()
        self.assertEqual(img.paint_layer_id, self.SAMPLE)

    def test_set_surrounding_whitespace_tolerated(self):
        img = self._new_image("LayerIdSpaces")
        img.paint_layer_id = "  " + self.SAMPLE + "  "
        self.assertEqual(img.paint_layer_id, self.SAMPLE)

    def test_empty_clears(self):
        img = self._new_image("LayerIdClearEmpty")
        img.paint_layer_id = self.SAMPLE
        img.paint_layer_id = ""
        self.assertEqual(img.paint_layer_id, "")

    def test_nil_string_clears(self):
        img = self._new_image("LayerIdClearNil")
        img.paint_layer_id = self.SAMPLE
        img.paint_layer_id = self.NIL
        self.assertEqual(img.paint_layer_id, "")

    def test_garbage_clears_and_does_not_raise(self):
        img = self._new_image("LayerIdGarbage")
        img.paint_layer_id = self.SAMPLE
        img.paint_layer_id = "not-a-uuid"
        self.assertEqual(img.paint_layer_id, "")

    def test_ensure_is_stable_and_valid(self):
        img = self._new_image("LayerIdEnsure")
        first = img.paint_layer_id_ensure()
        self.assertRegex(first, r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")
        self.assertEqual(img.paint_layer_id_ensure(), first)
        self.assertEqual(img.paint_layer_id, first)

    def test_copy_shares_id(self):
        img = self._new_image("LayerIdCopySrc")
        img.paint_layer_id = self.SAMPLE
        dup = img.copy()
        self.addCleanup(bpy.data.images.remove, dup)
        # Documents the spec 5.7 sharp edge: no auto-fresh-id on copy.
        self.assertEqual(dup.paint_layer_id, self.SAMPLE)


class TestPaintLayerIdCreation(unittest.TestCase):
    def test_create_pbr_paint_maps_stamps_identical_ids(self):
        bpy.ops.wm.read_factory_settings(use_empty=True)
        generate_monkey(BackendType.MESH)
        ob = bpy.context.active_object

        mat = bpy.data.materials.new("LayerIdOpMat")
        mat.use_nodes = True
        ob.data.materials.append(mat)

        bpy.context.tool_settings.paint_mode.canvas_source = 'MATERIAL'

        brush = bpy.data.brushes.new("LayerIdOpBrush", mode='SCULPT')
        brush.sculpt_brush_type = 'PAINT'
        bpy.context.tool_settings.sculpt.brush = brush
        self.assertEqual(bpy.ops.paint.material_paint_brush_ensure(), {'FINISHED'})

        channels = {c.channel: c for c in brush.material_paint.channels}
        channels['BASE_COLOR'].use = True
        channels['ROUGHNESS'].use = True

        self.assertEqual(bpy.ops.paint.material_paint_images_ensure(), {'FINISHED'})

        ids = {im.paint_layer_id for im in bpy.data.images if im.is_paint_canvas}
        self.assertEqual(len(ids), 1, ids)
        self.assertNotIn("", ids)


class TestPaintLayerIdOldFile(unittest.TestCase):
    def test_old_file_paint_layer_id_absent(self):
        # TODO(PBR_PAINT_LAYER_ID): needs a .blend saved from a build at/before commit
        # 282b785e21f (before Image.paint_layer_id existed) that contains >=1 Image datablock.
        # Add it at tests/files/pbr_paint/paint_layer_id_pre_field.blend and enable this test.
        # The mandatory guarantee is already covered by
        # TestPaintLayerId.test_default_is_empty (a fresh Image reports "").
        self.skipTest("pre-field fixture not yet added")


class TestChannelSourceLifecycle(unittest.TestCase):
    def setUp(self):
        self.brush = bpy.data.brushes.new("LifecycleTest", mode='SCULPT')
        bpy.context.tool_settings.sculpt.brush = self.brush
        bpy.ops.paint.material_paint_brush_ensure()
        self.channels = {c.channel: c for c in self.brush.material_paint.channels}

    def tearDown(self):
        if self.brush.name in bpy.data.brushes:
            bpy.data.brushes.remove(self.brush)

    def test_copying_brush_shares_source_texture(self):
        image = bpy.data.images.new("SourceImage", 4, 4)
        self.channels['ROUGHNESS'].source_image = image
        texture = self.channels['ROUGHNESS'].source_texture_slot.texture
        users_before = texture.users

        copy = self.brush.copy()
        try:
            copied = {c.channel: c for c in copy.material_paint.channels}
            # A shallow DNA copy shares the Tex, so the copy must register as another user.
            self.assertIs(copied['ROUGHNESS'].source_texture_slot.texture, texture)
            self.assertEqual(texture.users, users_before + 1)
        finally:
            bpy.data.brushes.remove(copy)

        self.assertEqual(texture.users, users_before)

    def test_removing_brush_releases_source_texture(self):
        image = bpy.data.images.new("SourceImage", 4, 4)
        self.channels['ROUGHNESS'].source_image = image
        texture = self.channels['ROUGHNESS'].source_texture_slot.texture
        users_before = texture.users

        bpy.data.brushes.remove(self.brush)

        self.assertEqual(texture.users, users_before - 1)

    def test_source_survives_save_and_reload(self):
        import tempfile

        image = bpy.data.images.new("SourceImage", 4, 4)
        self.channels['ROUGHNESS'].source_image = image
        brush_name = self.brush.name

        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "source_roundtrip.blend")
            bpy.ops.wm.save_as_mainfile(filepath=path)
            bpy.ops.wm.open_mainfile(filepath=path)

            reloaded = bpy.data.brushes[brush_name]
            channels = {c.channel: c for c in reloaded.material_paint.channels}
            slot = channels['ROUGHNESS'].source_texture_slot
            self.assertIsNotNone(slot.texture)
            self.assertEqual(slot.texture.type, 'IMAGE')
            self.assertIsNotNone(channels['ROUGHNESS'].source_image)


class TestMaterialBakeAPI(unittest.TestCase):
    """Material.bake_paint_channels and the two bake operators.

    Every case bakes blocking at a tiny size: the point is the data-block plumbing -- the link,
    the layer stamping, the colorspace, staleness -- not render quality.
    """

    def _new_material(self, name):
        mat = bpy.data.materials.new(name)
        mat.use_nodes = True
        return mat

    def _maps_of_layer(self, layer_id):
        return {img.material_source_channel: img
                for img in bpy.data.images
                if img.paint_layer_id == layer_id}

    def test_constant_only_material_blocking(self):
        mat = self._new_material("const_mat")
        bsdf = mat.node_tree.nodes["Principled BSDF"]
        bsdf.inputs["Metallic"].default_value = 0.75
        # Metallic unlinked -> ChannelResolution::Constant -> flat fill, no render.
        layer_id = mat.bake_paint_channels(channels={'METALLIC'}, size=32, blocking=True)
        self.assertTrue(layer_id)
        maps = self._maps_of_layer(layer_id)
        self.assertEqual(len(maps), 1)
        img = maps['METALLIC']
        self.assertEqual(img.material_source, mat)
        self.assertAlmostEqual(img.pixels[0], 0.75, places=2)

    def test_bake_paint_channels_returns_uuid_and_collects(self):
        mat = self._new_material("fn_mat")
        layer_id = mat.bake_paint_channels(
            channels={'BASE_COLOR', 'ROUGHNESS'}, size=32, blocking=True)
        self.assertRegex(layer_id, r"^[0-9a-f-]{36}$")
        self.assertSetEqual(set(self._maps_of_layer(layer_id)), {'BASE_COLOR', 'ROUGHNESS'})

    def test_operator_creates_maps_collectible_by_layer_id(self):
        import uuid
        mat = self._new_material("op_mat")
        layer_id = str(uuid.uuid4())
        res = bpy.ops.image.bake_from_material(
            material=mat.name, channels={'BASE_COLOR', 'ROUGHNESS'},
            size=32, layer_id=layer_id, blocking=True)
        self.assertEqual(res, {'FINISHED'})
        maps = self._maps_of_layer(layer_id)
        self.assertIn('BASE_COLOR', maps)
        self.assertIn('ROUGHNESS', maps)

    def test_material_source_properties(self):
        mat = self._new_material("props_mat")
        layer_id = mat.bake_paint_channels(channels={'ROUGHNESS'}, size=32, blocking=True)
        img = self._maps_of_layer(layer_id)['ROUGHNESS']
        self.assertEqual(img.material_source, mat)
        self.assertFalse(img.material_source_is_baking)
        self.assertFalse(img.material_source_is_stale)
        bpy.data.materials.remove(mat)
        # The IDProperty machinery nulls the pointer when the material goes away.
        self.assertIsNone(img.material_source)

    def test_clear_material_source_detaches(self):
        mat = self._new_material("clr_mat")
        layer_id = mat.bake_paint_channels(channels={'ROUGHNESS'}, size=32, blocking=True)
        img = self._maps_of_layer(layer_id)['ROUGHNESS']
        img.clear_material_source()
        self.assertIsNone(img.material_source)
        self.assertEqual(img.material_source_channel, 'NONE')
        img.rebake_material_source(blocking=True)  # No-op, must not raise.

    def test_rebake_stale_updates_pixels(self):
        mat = self._new_material("rebake_mat")
        bsdf = mat.node_tree.nodes["Principled BSDF"]
        bsdf.inputs["Metallic"].default_value = 0.2
        layer_id = mat.bake_paint_channels(channels={'METALLIC'}, size=32, blocking=True)
        img = self._maps_of_layer(layer_id)['METALLIC']
        self.assertAlmostEqual(img.pixels[0], 0.2, places=2)

        bsdf.inputs["Metallic"].default_value = 0.9
        self.assertTrue(img.material_source_is_stale)
        bpy.ops.image.rebake_stale_material_sources(layer_id=layer_id, blocking=True)
        self.assertAlmostEqual(img.pixels[0], 0.9, places=2)
        self.assertFalse(img.material_source_is_stale)

    def test_colorspace_per_channel(self):
        mat = self._new_material("cs_mat")
        layer_id = mat.bake_paint_channels(
            channels={'BASE_COLOR', 'ROUGHNESS'}, size=32, blocking=True)
        maps = self._maps_of_layer(layer_id)
        self.assertEqual(maps['BASE_COLOR'].colorspace_settings.name, 'sRGB')
        self.assertEqual(maps['ROUGHNESS'].colorspace_settings.name, 'Non-Color')

    def test_image_resolution_channel_is_rendered(self):
        mat = self._new_material("ir_mat")
        node_tree = mat.node_tree
        tex = node_tree.nodes.new('ShaderNodeTexImage')
        src = bpy.data.images.new("src_rough", 4, 4)
        src.generated_color = (0.6, 0.6, 0.6, 1.0)
        tex.image = src
        node_tree.links.new(
            tex.outputs['Color'], node_tree.nodes['Principled BSDF'].inputs['Roughness'])
        layer_id = mat.bake_paint_channels(channels={'ROUGHNESS'}, size=32, blocking=True)
        img = self._maps_of_layer(layer_id)['ROUGHNESS']
        # A distinct data-block, not the sampled image aliased into the layer.
        self.assertNotEqual(img, src)
        self.assertAlmostEqual(img.pixels[0], 0.6, places=1)

    def test_copy_strips_link(self):
        mat = self._new_material("cp_mat")
        layer_id = mat.bake_paint_channels(channels={'METALLIC'}, size=32, blocking=True)
        img = self._maps_of_layer(layer_id)['METALLIC']
        dup = img.copy()
        self.assertIsNone(dup.material_source)
        # The fork stays in the layer; only the live bake target is given up.
        self.assertEqual(dup.paint_layer_id, img.paint_layer_id)

    def test_blend_roundtrip_and_remap(self):
        import os
        import tempfile
        mat = self._new_material("rt_mat")
        mat.bake_paint_channels(channels={'METALLIC'}, size=32, blocking=True)
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "pbr_bake_rt.blend")
            bpy.ops.wm.save_as_mainfile(filepath=path)
            bpy.ops.wm.open_mainfile(filepath=path)
            img = next(i for i in bpy.data.images if i.material_source is not None)
            self.assertEqual(img.material_source.name, "rt_mat")
            img.material_source.name = "renamed_mat"
            self.assertEqual(img.material_source.name, "renamed_mat")

    def test_is_stale_flips_on_sampled_image_paint(self):
        mat = self._new_material("si_mat")
        node_tree = mat.node_tree
        tex = node_tree.nodes.new('ShaderNodeTexImage')
        src = bpy.data.images.new("si_src", 4, 4)
        tex.image = src
        node_tree.links.new(
            tex.outputs['Color'], node_tree.nodes['Principled BSDF'].inputs['Base Color'])
        layer_id = mat.bake_paint_channels(channels={'BASE_COLOR'}, size=32, blocking=True)
        img = self._maps_of_layer(layer_id)['BASE_COLOR']
        self.assertFalse(img.material_source_is_stale)
        src.pixels[0] = 0.5
        src.update()
        self.assertTrue(img.material_source_is_stale)


if __name__ == "__main__":
    test_brush_material_paint_lazy_init()
    test_metallic_paint_erase_restores_default()
    test_vertex_attribute_paint_creates_and_writes_attribute()
    test_vertex_attribute_paint_undo_redo()
    test_scalar_channel_ignores_blend_mode()
    test_custom_channel_without_name_is_inert()

    loader = unittest.TestLoader()
    suite = unittest.TestSuite()
    suite.addTests(loader.loadTestsFromTestCase(TestChannelSourceImage))
    suite.addTests(loader.loadTestsFromTestCase(TestPaintLayerId))
    suite.addTests(loader.loadTestsFromTestCase(TestPaintLayerIdCreation))
    suite.addTests(loader.loadTestsFromTestCase(TestPaintLayerIdOldFile))
    suite.addTests(loader.loadTestsFromTestCase(TestChannelSourceLifecycle))
    suite.addTests(loader.loadTestsFromTestCase(TestMaterialBakeAPI))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful():
        sys.exit(1)

    print("bl_pbr_channels: OK")
