# SPDX-License-Identifier: GPL-2.0-or-later
"""Add or update Fill texture-fill brush in mesh_sculpt essentials library.

Run from repo root (after build with TEXTURE_FILL RNA):
  blender --background assets/brushes/essentials_brushes-mesh_sculpt.blend \
      --python tools/brushes/add_sculpt_texture_fill.py
"""
import bpy

BRUSH_NAME = "Fill"


def main():
    br = bpy.data.brushes.get(BRUSH_NAME)
    if br is None:
        br = bpy.data.brushes.new(BRUSH_NAME, 'SCULPT')
    br.use_paint_sculpt = True
    br.sculpt_brush_type = 'TEXTURE_FILL'
    br.fill_expand = 'PIXELS'
    br.color = (1.0, 1.0, 1.0)
    br.strength = 1.0

    if not br.asset_data:
        br.asset_mark()
    br.asset_data.description = "Texture fill for sculpt mode paint canvas"

    assert br.sculpt_brush_type == 'TEXTURE_FILL', br.sculpt_brush_type
    assert br.name == BRUSH_NAME
    print("OK:", BRUSH_NAME, "sculpt_brush_type=", br.sculpt_brush_type)

    bpy.ops.wm.save_mainfile()


if __name__ == "__main__":
    main()
