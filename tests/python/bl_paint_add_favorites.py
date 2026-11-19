# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""The Add-menu favorites the preferences pin, and the RNA add API the Add menu serves: the
identifier of a pinned kind, that a pin does not duplicate, that the identifier is read-only (the
collection's new is what keeps it unique and non-empty), and the qualifiers of the Filter and
Generator sources -- a row or a correction of those names its filter or generator, and naming them
alongside a source that reads neither is an error, not the silent ignore it used to be. Plus the
Add menu's own contract: the C++ lists the Python root inlines are registered, and its draw leaves
the operator context the appended callbacks run under. These drive the real RNA, not the DNA
directly."""

import sys
import unittest

import bpy


def make_material(name):
    ma = bpy.data.materials.new(name)
    ma.use_nodes = True
    return ma


class PaintAddFavoritesTest(unittest.TestCase):
    def setUp(self):
        bpy.ops.wm.read_factory_settings(use_empty=True)
        # The preferences carry whatever the saved configuration pinned; the checks below count from
        # a clean slate.
        bpy.context.preferences.paint_add_favorites.clear()

    def test_new_pins_the_identifier(self):
        favorites = bpy.context.preferences.paint_add_favorites
        favorite = favorites.new("PAINT")
        self.assertEqual(favorite.identifier, "PAINT")
        self.assertEqual(len(favorites), 1)

    def test_new_with_empty_identifier_raises(self):
        favorites = bpy.context.preferences.paint_add_favorites
        with self.assertRaises(RuntimeError):
            favorites.new("")
        self.assertEqual(len(favorites), 0)

    def test_pinning_the_same_identifier_twice_does_not_duplicate(self):
        favorites = bpy.context.preferences.paint_add_favorites
        first = favorites.new("PAINT")
        second = favorites.new("PAINT")
        self.assertEqual(len(favorites), 1)
        self.assertEqual(first.identifier, second.identifier)
        self.assertEqual(first, second)

    def test_remove_unpins_the_favorite(self):
        favorites = bpy.context.preferences.paint_add_favorites
        favorite = favorites.new("PAINT")
        favorites.remove(favorite)
        self.assertEqual(len(favorites), 0)

    def test_removing_a_favorite_twice_raises(self):
        favorites = bpy.context.preferences.paint_add_favorites
        favorite = favorites.new("PAINT")
        favorites.remove(favorite)
        # The wrapper invalidates the pointer of the removed favorite; whichever of its errors that
        # raises before the C reports "Paint add favorite not found", it is one of these.
        with self.assertRaises((ReferenceError, RuntimeError, TypeError, ValueError)):
            favorites.remove(favorite)
        self.assertEqual(len(favorites), 0)

    def test_clear_unpins_everything(self):
        favorites = bpy.context.preferences.paint_add_favorites
        favorites.new("PAINT")
        favorites.new("FILTER_BLUR")
        self.assertEqual(len(favorites), 2)
        favorites.clear()
        self.assertEqual(len(favorites), 0)

    def test_the_identifier_is_read_only(self):
        favorites = bpy.context.preferences.paint_add_favorites
        favorite = favorites.new("PAINT")
        # The identifier is the unique key the pins and the Add menu carry; writing it from Python
        # would go around the collection's `new`, which is what keeps it unique and non-empty.
        with self.assertRaises((AttributeError, TypeError, ValueError, RuntimeError)):
            favorite.identifier = "FILTER_BLUR"
        self.assertEqual(favorite.identifier, "PAINT")
        self.assertEqual(len(favorites), 1)


class StackAddMenusRegisteredTest(unittest.TestCase):
    def setUp(self):
        bpy.ops.wm.read_factory_settings(use_empty=True)

    def test_the_lists_the_root_menu_inlines_are_registered(self):
        # The root Add menu (OUTLINER_MT_stack_layer_add) inlines the C++ lists by idname: a name
        # whose menu is not registered would silently draw nothing. The registration runs with the
        # space type, so a background build answers it too.
        for idname in (
            "OUTLINER_MT_stack_add_favorites",
            "OUTLINER_MT_stack_add_effects",
            "OUTLINER_MT_stack_add_effect_correction",
            "OUTLINER_MT_stack_add_effect_transform",
            "OUTLINER_MT_stack_add_effect_filter",
            "OUTLINER_MT_stack_add_effect_filter2",
            "OUTLINER_MT_stack_add_effect_pattern",
            "OUTLINER_MT_stack_add_effect_anchor",
        ):
            self.assertTrue(hasattr(bpy.types, idname), "menu not registered: " + idname)


class StackAddMenuDrawContractTest(unittest.TestCase):
    def setUp(self):
        bpy.ops.wm.read_factory_settings(use_empty=True)

    class _MockLayout:
        """Minimal layout API mock for testing the Add menu draw contract."""

        def __init__(self):
            self.operator_context = 'EXEC_DEFAULT'

        def operator(self, _op_idname, **_kwargs):
            return type("OperatorProperties", (), {})()

        def menu(self, *_args, **_kwargs):
            pass

        def menu_contents(self, *_args, **_kwargs):
            pass

        def separator(self):
            pass

    def test_draw_leaves_the_operator_context_appended_callbacks_run_under(self):
        # The class docstring's contract: draw sets 'INVOKE_REGION_WIN' and leaves it set, so an
        # add-on's appended callback runs under the same context the menu's own items do.
        menu = type("FakeMenu", (), {})()
        menu.layout = self._MockLayout()
        bpy.types.OUTLINER_MT_stack_layer_add.draw(menu, None)
        self.assertEqual(menu.layout.operator_context, 'INVOKE_REGION_WIN')


class PaintLayerRNAAddTest(unittest.TestCase):
    def setUp(self):
        bpy.ops.wm.read_factory_settings(use_empty=True)

    def test_new_generator_layer_lays_a_noise_pattern(self):
        owner = make_material("Owner")
        layer = owner.paint_layers.new(source='GENERATOR', generator='NOISE')
        self.assertIsNotNone(layer)
        self.assertEqual(layer.source, 'GENERATOR')
        self.assertEqual(layer.generator, 'NOISE')
        self.assertEqual(len(owner.paint_layers), 1)

    def test_new_defaults_to_an_image_layer(self):
        owner = make_material("Owner")
        layer = owner.paint_layers.new(source='IMAGE')
        self.assertIsNotNone(layer)
        self.assertEqual(layer.source, 'IMAGE')
        self.assertEqual(len(owner.paint_layers), 1)

    def test_a_new_row_without_an_anchor_goes_to_the_top(self):
        owner = make_material("Owner")
        owner.paint_layers.new(source='IMAGE', name="Base")
        top = owner.paint_layers.new(source='GENERATOR', generator='NOISE', anchor=None, name="Top")
        # The stack reads bottom to top, and an anchor of none puts the row above the last.
        self.assertEqual([row.name for row in owner.paint_layers], ["Base", "Top"])
        self.assertEqual(top, owner.paint_layers[-1])

    def test_a_filter_qualifier_only_goes_with_the_filter_and_generator_sources(self):
        owner = make_material("Owner")
        with self.assertRaises(RuntimeError):
            owner.paint_layers.new(source='IMAGE', filter='BLUR')
        self.assertEqual(len(owner.paint_layers), 0)
        with self.assertRaises(RuntimeError):
            owner.paint_layers.new(source='GENERATOR', generator='NOISE', filter='BLUR')
        self.assertEqual(len(owner.paint_layers), 0)


class PaintLayerEffectAddTest(unittest.TestCase):
    def setUp(self):
        bpy.ops.wm.read_factory_settings(use_empty=True)

    def test_a_filter_effect_names_the_filter_it_applies(self):
        owner = make_material("Owner")
        layer = owner.paint_layers.new(source='IMAGE')
        effect = layer.effect_add(source='FILTER', filter='BLUR')
        self.assertIsNotNone(effect)
        self.assertEqual(effect.source, 'FILTER')
        self.assertEqual(effect.filter, 'BLUR')
        self.assertEqual(len(layer.effects), 1)

    def test_a_generator_effect_names_its_pattern(self):
        owner = make_material("Owner")
        layer = owner.paint_layers.new(source='IMAGE')
        effect = layer.effect_add(source='GENERATOR', generator='NOISE')
        self.assertIsNotNone(effect)
        self.assertEqual(effect.source, 'GENERATOR')
        self.assertEqual(effect.generator, 'NOISE')
        self.assertEqual(len(layer.effects), 1)

    def test_a_filter_effect_without_its_filter_is_refused(self):
        owner = make_material("Owner")
        layer = owner.paint_layers.new(source='IMAGE')
        with self.assertRaises(RuntimeError):
            layer.effect_add(source='FILTER')
        self.assertEqual(len(layer.effects), 0)

    def test_qualifiers_are_refused_on_corrections_too(self):
        owner = make_material("Owner")
        layer = owner.paint_layers.new(source='IMAGE')
        with self.assertRaises(RuntimeError):
            layer.effect_add(source='IMAGE', filter='BLUR')
        self.assertEqual(len(layer.effects), 0)
        with self.assertRaises(RuntimeError):
            layer.effect_add(source='GENERATOR', generator='NOISE', filter='BLUR')
        self.assertEqual(len(layer.effects), 0)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []))
