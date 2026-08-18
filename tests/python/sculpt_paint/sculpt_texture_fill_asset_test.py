# SPDX-FileCopyrightText: 2026 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""
blender -b --factory-startup --python tests/python/sculpt_paint/sculpt_texture_fill_asset_test.py
"""
import sys
import unittest
import bpy


class SculptTextureFillAssetTest(unittest.TestCase):
    def test_fill_asset_loads_with_texture_fill_type(self):
        bpy.ops.object.mode_set(mode='SCULPT')
        result = bpy.ops.brush.asset_activate(
            asset_library_type='ESSENTIALS',
            relative_asset_identifier='brushes/essentials_brushes-mesh_sculpt.blend/Brush/Fill',
        )
        self.assertEqual({'FINISHED'}, result, msg="Fill asset missing from mesh_sculpt.blend")
        brush = bpy.context.tool_settings.sculpt.brush
        self.assertEqual(brush.sculpt_brush_type, 'TEXTURE_FILL')
        self.assertEqual(brush.fill_expand, 'PIXELS')


if __name__ == "__main__":
    unittest.main(argv=sys.argv[:1] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []))
