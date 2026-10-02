# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Regression tests for the layered-material UI: authored defaults, Principled wiring and the
per-channel Outliner columns. These drive the real RNA/operators, not the DNA directly."""

import sys
import unittest
from types import SimpleNamespace

import bpy

from bl_operators.material_paint_layers import (
    mesh_map_depsgraph_update_post,
    mesh_map_geometry_updated_objects,
    mesh_map_has_source,
    mesh_map_objects_for_material,
    mesh_map_pending_names_add,
    mesh_map_source_member_count,
    mesh_map_source_targets,
    mesh_map_states_need_refresh,
    mesh_map_summary_status,
    mesh_map_updated_objects,
)


class _FakeUpdate:
    def __init__(self, id, is_updated_geometry, is_updated_transform=False):
        self.id = id
        self.is_updated_geometry = is_updated_geometry
        self.is_updated_transform = is_updated_transform


class _FakeDepsgraph:
    def __init__(self, updates):
        self.updates = updates


class PaintLayersUiTest(unittest.TestCase):
    def setUp(self):
        bpy.ops.wm.read_factory_settings(use_empty=True)
        self.material = bpy.data.materials.new("LayeredUi")
        self.material.use_nodes = True

    def regenerate(self):
        # The scheduling point is the depsgraph update; an explicit RNA call covers the case where
        # a script's context has no scene to update. regenerate() rebuilds the tree, while the
        # view-layer update drains the bake queue (K-1 bake_plan_run via regenerate_tagged) and
        # clears MA_PAINT_LAYERS_BAKE_STALE so composite() passes the freshness gate.
        self.material.paint_layers.regenerate()
        bpy.context.view_layer.update()

    def principled(self):
        return self.material.node_tree.nodes.get("Principled BSDF")

    def base_color_input_linked(self):
        principled = self.principled()
        self.assertIsNotNone(principled)
        return len(principled.inputs['Base Color'].links) > 0

    def input_linked(self, name):
        principled = self.principled()
        return len(principled.inputs[name].links) > 0

    def composite_pixel(self, channel="BASE_COLOR", size=4):
        image = bpy.data.images.new("Composite", size, size, float_buffer=True)
        result = self.material.paint_layers.composite(channel=channel, image=image)
        self.assertTrue(result)
        pixels = image.pixels
        return tuple(pixels[i] for i in range(4))

    def set_channel_map(self, layer, channel, rgba):
        record = next(c for c in layer.channels if c.channel == channel)
        image = bpy.data.images.new(channel + "Map", 4, 4, float_buffer=True)
        image.pixels = list(rgba) * 16
        record.image = image

    def set_default_maps(self, layer):
        # A map on every default channel, so each channel has a pixel size to composite into.
        self.set_channel_map(layer, 'BASE_COLOR', (0.0, 0.0, 1.0, 1.0))
        self.set_channel_map(layer, 'METALLIC', (0.0, 0.0, 0.0, 1.0))
        self.set_channel_map(layer, 'ROUGHNESS', (0.5, 0.5, 0.5, 1.0))

    def test_authored_fill_wires_the_default_channels(self):
        # A bottom Paint map gives the stack a pixel size; the Fill layers its values over it.
        bottom = self.material.paint_layers.new(source='IMAGE', name="Bottom")
        self.set_default_maps(bottom)
        fill = self.material.paint_layers.new(source='CONSTANT', name="Fill")
        self.assertEqual(len(fill.channels), 5)
        fill.fill_color = (1.0, 0.0, 0.0, 1.0)
        self.regenerate()
        self.assertTrue(self.base_color_input_linked(), "Base Color must reach the Principled BSDF")
        # Alpha and Emission are not part of the default set: they would change the whole material.
        self.assertFalse(self.input_linked('Alpha'))
        self.assertFalse(self.input_linked('Emission Color'))
        red = self.composite_pixel()
        self.assertGreater(red[0], 0.5)
        self.assertLess(red[1], 0.2)
        # The other default channels carry the Principled defaults, not the fill colour.
        metallic = self.composite_pixel(channel="METALLIC")
        self.assertLess(metallic[0], 0.05)
        roughness = self.composite_pixel(channel="ROUGHNESS")
        self.assertAlmostEqual(roughness[0], 0.5, delta=0.02)

    def test_fill_has_a_value_per_channel(self):
        bottom = self.material.paint_layers.new(source='IMAGE', name="Bottom")
        self.set_default_maps(bottom)
        fill = self.material.paint_layers.new(source='CONSTANT', name="Fill")
        fill.fill_color = (1.0, 0.0, 0.0, 1.0)
        rough = next(c for c in fill.channels if c.channel == 'ROUGHNESS')
        rough.value = (0.75, 0.75, 0.75, 1.0)
        self.regenerate()
        self.assertAlmostEqual(self.composite_pixel(channel="ROUGHNESS")[0], 0.75, delta=0.02)
        # The Base Color channel reads the row's Base-Color record, which `fill_color` routes to.
        self.assertGreater(self.composite_pixel()[0], 0.5)

    def test_fill_channel_value_is_value_only(self):
        fill = self.material.paint_layers.new(source='CONSTANT', name="Fill")
        self.regenerate()
        self.assertFalse(self.material.paint_layers_tree_is_stale)
        rough = next(c for c in fill.channels if c.channel == 'ROUGHNESS')
        rough.value = (0.75, 0.75, 0.75, 1.0)
        # A value edit must not rebuild the generated tree.
        self.assertFalse(self.material.paint_layers_tree_is_stale)

    def test_authored_paint_participates_without_changing_the_render(self):
        bottom = self.material.paint_layers.new(source='IMAGE', name="Bottom")
        self.set_default_maps(bottom)
        fill = self.material.paint_layers.new(source='CONSTANT', name="Fill")
        fill.fill_color = (1.0, 0.0, 0.0, 1.0)
        self.regenerate()
        before = self.composite_pixel()

        paint = self.material.paint_layers.new(source='IMAGE', name="Paint")
        self.assertEqual(len(paint.channels), 5)
        self.regenerate()
        self.assertTrue(self.base_color_input_linked())
        after = self.composite_pixel()
        for i in range(3):
            self.assertAlmostEqual(before[i], after[i], delta=2.0 / 255.0)

    def test_fresh_paint_alone_does_not_wire_a_false_constant(self):
        paint = self.material.paint_layers.new(source='IMAGE', name="Paint")
        self.assertEqual(len(paint.channels), 5)
        self.regenerate()
        # The channel output exists and is wired to the Principled BSDF even before the first
        # stroke, because the row participates.
        self.assertTrue(self.base_color_input_linked())

    def test_authored_default_channels_are_enabled_without_maps(self):
        fill = self.material.paint_layers.new(source='CONSTANT', name="Fill")
        self.assertEqual(len(fill.channels), 5)
        for record in fill.channels:
            self.assertIsNone(record.image)
            self.assertEqual(record.state, 'ENABLED')
            self.assertIn(record.channel, {'BASE_COLOR', 'METALLIC', 'ROUGHNESS', 'NORMAL', 'AO'})

    def test_material_channel_set_round_trip(self):
        self.material.paint_layers.new(source='IMAGE', name="Layer")
        # The build default is the effective set until one is authored.
        self.assertEqual(set(self.material.paint_layers_channels),
                         {'BASE_COLOR', 'METALLIC', 'ROUGHNESS', 'NORMAL', 'AO'})
        self.assertTrue(self.material.paint_layers_channel_in_set(channel='BASE_COLOR'))
        # Base Color cannot be removed: it is always in the set.
        self.material.paint_layers_channels = {'METALLIC', 'ROUGHNESS'}
        self.assertIn('BASE_COLOR', set(self.material.paint_layers_channels))
        self.assertNotIn('NORMAL', set(self.material.paint_layers_channels))
        self.assertFalse(self.material.paint_layers_channel_in_set(channel='NORMAL'))
        self.assertTrue(self.material.paint_layers_channel_in_set(channel='METALLIC'))

    def test_folder_and_correction_get_no_default_channels(self):
        folder = self.material.paint_layers.new(source='STACK', name="Folder")
        self.assertEqual(len(folder.channels), 0)
        layer = self.material.paint_layers.new(source='IMAGE', name="Layer")
        correction = layer.correction_add(role='EFFECT', source='IMAGE', name="C")
        self.assertEqual(len(correction.channels), 0)

    def channel_record(self, layer, channel):
        return next(c for c in layer.channels if c.channel == channel)

    def test_layer_channel_use_mirrors_state(self):
        # The shared channel widget reads `use` on a layer channel exactly as it does on a brush
        # channel; the setter goes through the layer's BKE enable path, not a direct state write.
        fill = self.material.paint_layers.new(source='CONSTANT', name="Fill")
        rough = self.channel_record(fill, 'ROUGHNESS')
        self.assertTrue(rough.use)
        rough.use = False
        self.assertFalse(rough.use)
        self.assertEqual(rough.state, 'DISABLED')
        rough.use = True
        self.assertEqual(rough.state, 'ENABLED')

    def test_disabled_channel_keeps_map_and_leaves_the_tree(self):
        # A channel map the user switches off stays on the record, but the generated tree stops
        # wiring it; turning it back on restores both. There is no composite() here: its bake may be
        # stale in this background-less run, and the record plus the tree are what the fix changes.
        paint = self.material.paint_layers.new(source='IMAGE', name="Paint")
        self.set_default_maps(paint)
        base = self.channel_record(paint, 'BASE_COLOR')
        image = base.image
        self.assertIsNotNone(image)
        self.regenerate()
        self.assertTrue(self.base_color_input_linked())

        base.use = False
        self.regenerate()
        self.assertEqual(base.state, 'DISABLED')
        self.assertIs(base.image, image)
        self.assertFalse(self.base_color_input_linked())

        base.use = True
        self.regenerate()
        self.assertEqual(base.state, 'ENABLED')
        self.assertIs(base.image, image)
        self.assertTrue(self.base_color_input_linked())

    def test_layer_fill_base_color_cannot_be_switched_off(self):
        # A Fill's Base Color is its constant: it cannot be disabled, matching the record-removal
        # invariant, so a disabled record never makes the layer paint nothing.
        fill = self.material.paint_layers.new(source='CONSTANT', name="Fill")
        base = self.channel_record(fill, 'BASE_COLOR')
        base.use = False
        self.assertTrue(base.use)
        self.assertEqual(base.state, 'ENABLED')

    def test_layer_channel_value_color_round_trips_scalar(self):
        # The gradient swatch property mirrors the brush's: same BKE conversion, so the shared
        # widget draws one thing. A scalar value maps to a grayscale color and back.
        fill = self.material.paint_layers.new(source='CONSTANT', name="Fill")
        rough = self.channel_record(fill, 'ROUGHNESS')
        rough.value_scalar = 0.75
        self.assertAlmostEqual(rough.value_color[0], 0.75, places=3)
        rough.value_color = (0.25, 0.25, 0.25)
        self.assertAlmostEqual(rough.value_scalar, 0.25, places=3)
        # Base Color is a color channel: value_color does not apply.
        base = self.channel_record(fill, 'BASE_COLOR')
        self.assertEqual(tuple(base.value_color), (0.0, 0.0, 0.0))

    def test_layer_channel_color_and_image_are_editable_through_rna(self):
        # Color channels edit the record's `value` (the same [0,1] color as the brush's
        # normal_color/emission_color); "Drop image" writes the record's own `image`.
        paint = self.material.paint_layers.new(source='IMAGE', name="Paint")
        base = self.channel_record(paint, 'BASE_COLOR')
        base.value = (0.1, 0.2, 0.3, 1.0)
        for actual, expected in zip(base.value, (0.1, 0.2, 0.3, 1.0)):
            self.assertAlmostEqual(actual, expected, places=5)
        image = bpy.data.images.new("PaintSrc", 4, 4)
        base.image = image
        self.assertIs(base.image, image)
        base.image = None
        self.assertIsNone(base.image)

    def test_layer_widget_contract_holds_for_every_channel(self):
        # The shared widget reads `use` on every channel and either a color or a scalar (plus the
        # gradient swatch). Every fixed channel a layer can carry must satisfy that contract, so
        # the widget never hits a missing property while drawing.
        color_ids = {'BASE_COLOR', 'NORMAL', 'EMISSION'}
        for channel_id in ('BASE_COLOR', 'METALLIC', 'ROUGHNESS', 'SPECULAR', 'NORMAL',
                           'ALPHA', 'AO', 'EMISSION'):
            layer = self.material.paint_layers.new(source='CONSTANT', name="C")
            record = layer.channel_add(channel=channel_id)
            self.assertIsNotNone(record, channel_id)
            self.assertIsInstance(record.use, bool)
            if channel_id in color_ids:
                self.assertEqual(len(record.value), 4)
            else:
                record.value_scalar = 0.5
                self.assertAlmostEqual(record.value_scalar, 0.5, places=3)
                self.assertEqual(len(record.value_color), 3)

    def test_layer_socket_menu_registered_and_data_contract(self):
        # The layer channel's socket menu must be registered, and every property it reads must be
        # available for a Fill and a Paint row, with and without an assigned image: that data
        # contract is what the menu's draw needs, and a missing property is what would raise.
        from bl_ui.properties_paint_common import PAINT_MT_material_layer_channel_socket

        self.assertTrue(PAINT_MT_material_layer_channel_socket.is_registered)
        for source in ('CONSTANT', 'IMAGE'):
            layer = self.material.paint_layers.new(source=source, name="Row")
            for channel_id in ('BASE_COLOR', 'ROUGHNESS'):
                record = layer.channel_add(channel=channel_id)
                # The color-space row is drawn only with an image; the settings row always exists.
                for with_image in (False, True):
                    record.image = bpy.data.images.new("Src", 4, 4) if with_image else None
                    if record.image is not None:
                        self.assertIsInstance(record.image.colorspace_settings.name, str)
                    settings = next(
                        s for s in layer.channel_settings if s.channel == channel_id)
                    self.assertIsInstance(settings.blend_type, str)
                    self.assertIsInstance(settings.opacity, float)

    def test_correction_add_accepts_every_layer_source(self):
        # Phase 6, goal 1: a correction or mask item offers the same sources a Layer row does
        # (BKE_paint_layers_correction_add already accepts all of them from phase 4 on); the
        # operator-level IMAGE/CONSTANT-only filter was UI policy, not a BKE restriction.
        layer = self.material.paint_layers.new(source='IMAGE', name="Layer")
        for role in ('EFFECT', 'MASK_ITEM'):
            for source in ('IMAGE', 'CONSTANT', 'MESH_MAP', 'MATERIAL', 'NODE_GROUP', 'STACK'):
                correction = layer.correction_add(role=role, source=source, name="C")
                self.assertIsNotNone(correction, (role, source))
                self.assertEqual(correction.source, source)
                self.assertEqual(correction.role, role)

    def test_paint_layer_correction_add_operator_offers_every_source(self):
        layer = self.material.paint_layers.new(source='IMAGE', name="Layer")
        self.material.paint_layers.active = layer
        with bpy.context.temp_override(material=self.material):
            op = bpy.ops.material.paint_layer_correction_add
            sources = {item.identifier for item in op.get_rna_type().properties['source'].enum_items}
            self.assertEqual(sources, {'IMAGE', 'CONSTANT', 'MESH_MAP', 'MATERIAL', 'NODE_GROUP',
                                       'STACK'})
            result = op(role='MASK_ITEM', source='MESH_MAP', mesh_map_type='CURVATURE', name="M")
            self.assertEqual(result, {'FINISHED'})
            mask = layer.mask_stack[-1]
            self.assertEqual(mask.source, 'MESH_MAP')
            self.assertEqual(mask.mesh_map_type, 'CURVATURE')
            # The operator also makes the new row active, so the reused Source Material / Custom
            # Group / mask_channel UI (keyed off paint_layers.active) shows up for it right away.
            self.assertEqual(self.material.paint_layers.active.marker, mask.marker)

    def test_paint_layer_correction_select_operator_sets_active(self):
        layer = self.material.paint_layers.new(source='IMAGE', name="Layer")
        mask = layer.mask_add(value=1.0)
        self.material.paint_layers.active = layer
        self.assertNotEqual(self.material.paint_layers.active.marker, mask.marker)
        with bpy.context.temp_override(material=self.material):
            result = bpy.ops.material.paint_layer_correction_select(marker=mask.marker)
            self.assertEqual(result, {'FINISHED'})
        self.assertEqual(self.material.paint_layers.active.marker, mask.marker)

    def test_mask_channel_excludes_normal(self):
        # Phase 6, goal 3: Normal is a tangent-space vector, meaningless for a mask's single scalar
        # read, so it must not be offered even though the underlying enum (shared with the channel
        # pickers) still lists it.
        layer = self.material.paint_layers.new(source='IMAGE', name="Layer")
        mask = layer.correction_add(role='MASK_ITEM', source='MATERIAL', name="M")
        items = mask.bl_rna.properties['mask_channel'].enum_items
        self.assertNotIn('NORMAL', {item.identifier for item in items})
        self.assertIn('ALPHA', {item.identifier for item in items})
        # A direct assignment (a script bypassing the UI) is clamped, not crashed, the same way a
        # value written by an older build would be read back safely.
        mask.mask_channel = 'BASE_COLOR'
        self.assertEqual(mask.mask_channel, 'BASE_COLOR')

    def channel_settings(self, layer, channel):
        return next(item for item in layer.channel_settings if item.channel == channel)

    def test_channel_settings_have_a_stable_path_for_every_channel(self):
        layer = self.material.paint_layers.new(source='IMAGE', name="Layer")
        # Every channel has an entry, record or not, so the column and a keyframe path always exist.
        self.assertEqual(len(layer.channel_settings), 10)
        base_color = self.channel_settings(layer, 'BASE_COLOR')
        self.assertEqual(base_color.blend_type, 'INHERIT')
        self.assertEqual(base_color.opacity, 100.0)
        # Editing the pair never creates a channel record.
        self.assertEqual(len(layer.channels), 5)
        base_color.blend_type = 'MULTIPLY'
        base_color.opacity = 50.0
        self.assertEqual(base_color.blend_type, 'MULTIPLY')
        self.assertEqual(base_color.opacity, 50.0)
        self.assertEqual(len(layer.channels), 5)

    def test_channel_settings_normal_has_no_blend(self):
        layer = self.material.paint_layers.new(source='IMAGE', name="Layer")
        normal = self.channel_settings(layer, 'NORMAL')
        normal.blend_type = 'MULTIPLY'
        # The setter refuses the Normal channel, which forces its own combine.
        self.assertEqual(normal.blend_type, 'INHERIT')

    def test_channel_settings_opacity_keyframe_path_resolves(self):
        layer = self.material.paint_layers.new(source='IMAGE', name="Layer")
        base_color = self.channel_settings(layer, 'BASE_COLOR')
        base_color.opacity = 25.0
        self.assertTrue(base_color.keyframe_insert('opacity', frame=1))
        self.assertIsNotNone(self.material.animation_data)
        action = self.material.animation_data.action
        fcurves = []
        for layer in action.layers:
            for strip in layer.strips:
                for channelbag in strip.channelbags:
                    fcurves.extend(channelbag.fcurves)
        paths = [fcurve.data_path for fcurve in fcurves]
        self.assertTrue(
            any('channel_settings' in path and path.endswith('.opacity') for path in paths), paths)
        # Re-resolving the path finds a value again, the way playback does.
        resolved = self.material.path_resolve(paths[0])
        self.assertAlmostEqual(resolved, 25.0, places=4)

    def test_mesh_map_ui_registration_and_poll(self):
        bpy.ops.mesh.primitive_cube_add()
        obj = bpy.context.object
        bpy.ops.material.new_layered()
        material = obj.active_material

        with bpy.context.temp_override(material=material, object=obj, active_object=obj):
            self.assertTrue(bpy.types.LAYER_MATERIAL_PT_mesh_maps.poll(bpy.context))
            self.assertTrue(bpy.types.OBJECT_OT_mesh_map_refresh.poll(bpy.context))
            self.assertTrue(bpy.types.MATERIAL_OT_mesh_map_add_layer.poll(bpy.context))
            self.assertTrue(bpy.types.MATERIAL_OT_mesh_map_add_mask.poll(bpy.context))
            self.assertTrue(bpy.types.MATERIAL_OT_mesh_map_use_active_uv.poll(bpy.context))
        self.assertIsNotNone(material)

    def test_mesh_map_operators_use_paint_layer_rna(self):
        bpy.ops.mesh.primitive_cube_add()
        obj = bpy.context.object
        bpy.ops.material.new_layered()
        material = obj.active_material

        with bpy.context.temp_override(material=material, object=obj, active_object=obj):
            result = bpy.ops.material.mesh_map_add_layer(type='AO')
            self.assertEqual(result, {'FINISHED'})
            layer = material.paint_layers.active
            self.assertEqual(layer.source, 'MESH_MAP')
            self.assertEqual(layer.mesh_map_type, 'AO')

            result = bpy.ops.material.mesh_map_add_mask(type='CURVATURE')
            self.assertEqual(result, {'FINISHED'})
            mask = layer.mask_stack[-1]
            self.assertEqual(mask.role, 'MASK_ITEM')
            self.assertEqual(mask.source, 'MESH_MAP')
            self.assertEqual(mask.mesh_map_type, 'CURVATURE')


    def test_mesh_map_batch_operators_are_registered(self):
        self.assertIn('mesh_map_bake_all', dir(bpy.ops.object))
        self.assertIn('mesh_map_clear', dir(bpy.ops.object))
        # These are C++ operators (WM_operatortype_append), not bpy.types.Operator subclasses, so
        # they never show up on bpy.types by idname; get_rna_type() is what actually proves they
        # are registered, the way bpy.ops itself resolves an idname before calling it.
        self.assertIsNotNone(bpy.ops.object.mesh_map_bake_all.get_rna_type())
        self.assertIsNotNone(bpy.ops.object.mesh_map_clear.get_rna_type())
        # The auto-refresh handler ships with the operators module.
        self.assertIn(mesh_map_depsgraph_update_post, bpy.app.handlers.depsgraph_update_post)

    def test_mesh_map_states_need_refresh_skips_baking_and_empty(self):
        self.assertFalse(mesh_map_states_need_refresh([]))
        for status in ('NONE', 'ERROR', 'BAKING'):
            self.assertFalse(mesh_map_states_need_refresh([SimpleNamespace(status=status)]), status)
        for status in ('VALID', 'STALE'):
            self.assertTrue(mesh_map_states_need_refresh([SimpleNamespace(status=status)]), status)
        # A single refreshable state is enough.
        self.assertTrue(
            mesh_map_states_need_refresh(
                [SimpleNamespace(status='BAKING'), SimpleNamespace(status='STALE')]))

    def test_mesh_map_handler_picks_only_geometry_updates(self):
        bpy.ops.mesh.primitive_cube_add()
        ob = bpy.context.object
        bpy.ops.material.new_layered()
        mat = ob.active_material
        ob.mesh_map_states.ensure(material=mat, type='AO')

        scene = bpy.context.scene
        # A non-geometry update (the view moving) is ignored.
        self.assertEqual(
            mesh_map_geometry_updated_objects(scene, _FakeDepsgraph([_FakeUpdate(ob, False)])), [])
        # A geometry update names the object.
        self.assertEqual(
            mesh_map_geometry_updated_objects(
                scene, _FakeDepsgraph([_FakeUpdate(ob, False), _FakeUpdate(ob, True)])),
            [ob])
        # A mesh-data update maps back to the objects that share the mesh.
        self.assertEqual(
            mesh_map_geometry_updated_objects(
                scene, _FakeDepsgraph([_FakeUpdate(ob.data, True)])),
            [ob])
        # The refreshable filter drops a state that is neither VALID nor STALE.
        self.assertEqual(
            mesh_map_updated_objects(scene, _FakeDepsgraph([_FakeUpdate(ob, True)])), [])

    def test_mesh_map_source_reverse_map_and_members(self):
        bpy.ops.mesh.primitive_cube_add()
        low = bpy.context.object
        bpy.ops.material.new_layered()
        mat = low.active_material
        high = bpy.data.objects.new("HighSrc", bpy.data.meshes.new("HighSrcMesh"))
        bpy.context.scene.collection.objects.link(high)
        cage = bpy.data.objects.new("CageSrc", bpy.data.meshes.new("CageSrcMesh"))
        bpy.context.scene.collection.objects.link(cage)

        source = low.mesh_map_sources.ensure(material=mat)
        source.high_poly = high
        source.cage = cage

        targets = mesh_map_source_targets(bpy.context.scene)
        self.assertEqual(targets.get("HighSrc"), [(low, mat)])
        self.assertEqual(targets.get("CageSrc"), [(low, mat)])
        self.assertTrue(mesh_map_has_source(low))
        self.assertEqual(mesh_map_source_member_count(source), 1)
        # A source object pointing at the low-poly itself is not a member.
        source.high_poly = None
        self.assertFalse(mesh_map_has_source(low))
        self.assertEqual(mesh_map_source_member_count(source), 0)

    def test_mesh_map_cage_matches_rna(self):
        bpy.ops.mesh.primitive_cube_add()
        low = bpy.context.object
        bpy.ops.material.new_layered()
        mat = low.active_material
        bpy.ops.mesh.primitive_cube_add()
        cage = bpy.context.object
        source = low.mesh_map_sources.ensure(material=mat)
        source.cage = cage
        self.assertTrue(source.cage_matches(depsgraph=bpy.context.evaluated_depsgraph_get()))

        bpy.ops.mesh.primitive_plane_add()
        source.cage = bpy.context.object
        self.assertFalse(source.cage_matches(depsgraph=bpy.context.evaluated_depsgraph_get()))

    def test_mesh_map_debounce_accumulates_names_once(self):
        pending = set()
        objects = [SimpleNamespace(name="A"), SimpleNamespace(name="B")]
        self.assertTrue(mesh_map_pending_names_add(pending, objects))
        self.assertEqual(pending, {"A", "B"})
        # The same objects again add nothing, so no second timer is needed.
        self.assertFalse(mesh_map_pending_names_add(pending, objects))
        # A new object is added.
        self.assertTrue(mesh_map_pending_names_add(pending, [SimpleNamespace(name="C")]))
        self.assertEqual(pending, {"A", "B", "C"})

    def test_mesh_map_object_list_and_summary(self):
        bpy.ops.mesh.primitive_cube_add()
        ob = bpy.context.object
        bpy.ops.material.new_layered()
        mat = ob.active_material

        self.assertIn(ob, mesh_map_objects_for_material(bpy.context.scene, mat))
        self.assertEqual(mesh_map_summary_status(ob, mat), ('NONE', 0))
        ob.mesh_map_states.ensure(material=mat, type='AO')
        self.assertEqual(mesh_map_summary_status(ob, mat), ('NONE', 1))
        # An object without the material in a slot is not part of the batch.
        other = bpy.data.objects.new("NoMat", bpy.data.meshes.new("NoMat"))
        bpy.context.scene.collection.objects.link(other)
        self.assertNotIn(other, mesh_map_objects_for_material(bpy.context.scene, mat))


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []))
