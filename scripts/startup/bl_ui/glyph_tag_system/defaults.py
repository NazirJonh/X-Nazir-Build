# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Pure-data constants for the Category Tabs / Glyph / Tag system.

Extracted verbatim from ``space_userpref.py`` (no behavioural change). These names
are re-imported back into ``space_userpref`` so the existing module-attribute
contract (including the C++ bridge) is preserved. This module has no ``bpy`` or
package dependencies and can be imported standalone.
"""

# ----------------------------------------------------------------------------
# Debug / feature flags

TAG_DEBUG = False  # Set to True to enable tag/glyph discovery debug output
TAG_BACKUP_ENABLED = False  # Отключено временно для отладки
SAVE_DEBUG = False  # Set to True to enable verbose save/load logging (printf-style)

# [POPULAR ADDONS DB] - Temporary: fallback icon lookup from Popular Addons Database.
# When extensions start bundling their own icons, this functionality will no longer be needed.
# Set to False to disable all Popular Addons Database integration code.
POPULAR_ADDONS_DB_ENABLED = True

# ----------------------------------------------------------------------------
# Category tag modes (single source of truth, mirrors CategoryTagMode in DNA/RNA)

# Единый источник режимов тегов (должен соответствовать CategoryTagMode в DNA/RNA).
_CATEGORY_TAG_MODES = (
    ("OBJECT_MODE", "OBJECT", 0, "Object Mode", 'OBJECT_DATAMODE'),
    ("EDIT_MODE", "EDIT", 1, "Edit Mode", 'EDITMODE_HLT'),
    ("SCULPT_MODE", "SCULPT", 2, "Sculpt Mode", 'SCULPTMODE_HLT'),
    ("VERTEX_PAINT", "VERTEX_PAINT", 3, "Vertex Paint", 'VPAINT_HLT'),
    ("WEIGHT_PAINT", "WEIGHT_PAINT", 4, "Weight Paint", 'WPAINT_HLT'),
    ("TEXTURE_PAINT", "TEXTURE_PAINT", 5, "Texture Paint", 'TPAINT_HLT'),
    ("UV_EDIT", "UV_EDIT", 6, "UV Edit", 'UV'),
    ("POSE_MODE", "POSE", 7, "Pose Mode", 'POSE_HLT'),
    ("GEOMETRY_NODES", "GEOMETRY_NODES", 8, "Geometry Nodes", 'NODETREE'),
    ("SHADER_EDITOR", "SHADER_EDITOR", 9, "Shader Editor", 'MATERIAL'),
    ("IMAGE_PAINT", "IMAGE_PAINT", 10, "Image Paint", 'TPAINT_HLT'),
    # Detailed edit modes for bl_context matching
    ("MESH_EDIT", "MESH_EDIT", 11, "Mesh Edit", 'MESH_DATA'),
    ("CURVE_EDIT", "CURVE_EDIT", 12, "Curve Edit", 'CURVE_DATA'),
    ("SURFACE_EDIT", "SURFACE_EDIT", 13, "Surface Edit", 'SURFACE_DATA'),
    ("ARMATURE_EDIT", "ARMATURE_EDIT", 14, "Armature Edit", 'ARMATURE_DATA'),
    # ("LATTICE_EDIT", "LATTICE_EDIT", 15, "Lattice Edit", 'LATTICE_DATA'),  # Temporarily hidden
    # ("META_EDIT", "META_EDIT", 16, "Metaball Edit", 'META_DATA'),  # Temporarily hidden
    # ("FONT_EDIT", "FONT_EDIT", 17, "Text Edit", 'FONT_DATA'),  # Temporarily hidden
    ("GREASE_PENCIL_EDIT", "GREASE_PENCIL_EDIT", 18, "Grease Pencil Edit", 'GREASEPENCIL'),
    # ("POINTCLOUD_EDIT", "POINTCLOUD_EDIT", 19, "Point Cloud Edit", 'POINTCLOUD_DATA'),  # Temporarily hidden
    # ("VOLUME_EDIT", "VOLUME_EDIT", 20, "Volume Edit", 'VOLUME_DATA'),  # Temporarily hidden
)
_CATEGORY_TAG_MODE_NAME_TO_FLAG = {name: (1 << bit) for name, _id, bit, _label, _icon in _CATEGORY_TAG_MODES}
_CATEGORY_TAG_MODE_FLAG_TO_NAME = {(1 << bit): name for name, _id, bit, _label, _icon in _CATEGORY_TAG_MODES}
_CATEGORY_TAG_MODE_ID_TO_BIT = {mode_id: bit for _name, mode_id, bit, _label, _icon in _CATEGORY_TAG_MODES}
_CATEGORY_TAG_ALL_MODE_FLAGS = sum((1 << bit) for _name, _id, bit, _label, _icon in _CATEGORY_TAG_MODES)
_CATEGORY_TAG_DEFAULT_MODE_FLAGS = (1 << 0) | (1 << 1) | (1 << 2)
# Bitmask of every edit-style mode: the generic EDIT plus the per-object-type *_EDIT
# modes. A tag restricted to EDIT_MODE should stay visible in any detailed edit mode.
# Currently-hidden detailed modes resolve to 0, so this stays in sync with the table
# above automatically. Mirrors the C++ EDIT_MODE_MASK in interface_tag_bar.cc.
_CATEGORY_TAG_EDIT_MODE_MASK = (
    _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("EDIT_MODE", 0)
    | _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("MESH_EDIT", 0)
    | _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("CURVE_EDIT", 0)
    | _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("SURFACE_EDIT", 0)
    | _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("ARMATURE_EDIT", 0)
    | _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("LATTICE_EDIT", 0)
    | _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("META_EDIT", 0)
    | _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("FONT_EDIT", 0)
    | _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("GREASE_PENCIL_EDIT", 0)
    | _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("POINTCLOUD_EDIT", 0)
    | _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("VOLUME_EDIT", 0)
)
_CATEGORY_TAG_FILTER_ENUM_TO_FLAG = {
    0: 0,
    "ALL": 0,
    1: (1 << 0),
    "OBJECT_MODE": (1 << 0),
    "OBJECT": (1 << 0),
    2: (1 << 1),
    "EDIT_MODE": (1 << 1),
    "EDIT": (1 << 1),
    3: (1 << 2),
    "SCULPT_MODE": (1 << 2),
    "SCULPT": (1 << 2),
    4: (1 << 3),
    "VERTEX_PAINT": (1 << 3),
    5: (1 << 4),
    "WEIGHT_PAINT": (1 << 4),
    6: (1 << 5),
    "TEXTURE_PAINT": (1 << 5),
    7: (1 << 6),
    "UV_EDIT": (1 << 6),
    8: (1 << 7),
    "POSE_MODE": (1 << 7),
    "POSE": (1 << 7),
    9: (1 << 8),
    "GEOMETRY_NODES": (1 << 8),
    10: (1 << 9),
    "SHADER_EDITOR": (1 << 9),
    11: (1 << 10),
    "IMAGE_PAINT": (1 << 10),
    # Detailed edit modes
    12: (1 << 11),
    "MESH_EDIT": (1 << 11),
    13: (1 << 12),
    "CURVE_EDIT": (1 << 12),
    14: (1 << 13),
    "SURFACE_EDIT": (1 << 13),
    15: (1 << 14),
    "ARMATURE_EDIT": (1 << 14),
    # 16: (1 << 15),  # Temporarily hidden
    # "LATTICE_EDIT": (1 << 15),  # Temporarily hidden
    # 17: (1 << 16),  # Temporarily hidden
    # "META_EDIT": (1 << 16),  # Temporarily hidden
    # 18: (1 << 17),  # Temporarily hidden
    # "FONT_EDIT": (1 << 17),  # Temporarily hidden
    19: (1 << 18),
    "GREASE_PENCIL_EDIT": (1 << 18),
    # 20: (1 << 19),  # Temporarily hidden
    # "POINTCLOUD_EDIT": (1 << 19),  # Temporarily hidden
    # 21: (1 << 20),  # Temporarily hidden
    # "VOLUME_EDIT": (1 << 20),  # Temporarily hidden
}

# ----------------------------------------------------------------------------
# Default category glyph mappings (Material Symbols)

# Default category glyph mappings (Material Symbols)
# Structure: {category: {"glyph": str, "display_name": str, "color": [r, g, b],
#                        "default_glyph": str, "default_display_name": str}}
DEFAULT_CATEGORY_GLYPHS = {
    # Common categories
    "Item": {"glyph": "\ueb75", "display_name": "", "color": [0.0, 0.0, 0.0],
             "default_glyph": "\ueb75", "default_display_name": ""},       # visibility
    "View": {"glyph": "\uf4c9", "display_name": "", "color": [0.0, 0.0, 0.0],
             "default_glyph": "\uf4c9", "default_display_name": ""},       # visibility
    "Edit": {"glyph": "\ue3c9", "display_name": "", "color": [0.0, 0.0, 0.0],
             "default_glyph": "\ue3c9", "default_display_name": ""},       # edit
    "Tool": {"glyph": "\uea3b", "display_name": "", "color": [0.0, 0.0, 0.0],
             "default_glyph": "\uea3b", "default_display_name": ""},       # construction
    "Asset": {"glyph": "\ue2c7", "display_name": "", "color": [0.0, 0.0, 0.0],
              "default_glyph": "\ue2c7", "default_display_name": ""},      # folder
    "Options": {"glyph": "\uf835", "display_name": "", "color": [0.0, 0.0, 0.0],
                 "default_glyph": "\uf835", "default_display_name": ""},    # options
    "Cache": {"glyph": "\uf720", "display_name": "", "color": [0.0, 0.0, 0.0],
              "default_glyph": "\uf720", "default_display_name": ""},      # cache
    "Proxy": {"glyph": "\ue335", "display_name": "", "color": [0.0, 0.0, 0.0],
              "default_glyph": "\ue335", "default_display_name": ""},      # proxy
    "Metadata": {"glyph": "", "display_name": "", "color": [0.0, 0.0, 0.0],
                 "default_glyph": "", "default_display_name": ""},             # metadata (text-only)

    # Editor-specific
    "Animation": {"glyph": "\uf8f0", "display_name": "", "color": [0.0, 0.0, 0.0],
                  "default_glyph": "\uf8f0", "default_display_name": ""},  # motion_photos_on
    "Texture": {"glyph": "\ue40a", "display_name": "", "color": [0.0, 0.0, 0.0],
                "default_glyph": "\ue40a", "default_display_name": ""},    # texture
    "Image": {"glyph": "\ue410", "display_name": "", "color": [0.0, 0.0, 0.0],
             "default_glyph": "\ue410", "default_display_name": ""},       # image
    "Mesh": {"glyph": "\ue3e3", "display_name": "", "color": [0.0, 0.0, 0.0],
             "default_glyph": "\ue3e3", "default_display_name": ""},       # category
    "Object": {"glyph": "\ue8d4", "display_name": "", "color": [0.0, 0.0, 0.0],
               "default_glyph": "\ue8d4", "default_display_name": ""},     # select_all
    "Scene": {"glyph": "\ue8f9", "display_name": "", "color": [0.0, 0.0, 0.0],
              "default_glyph": "\ue8f9", "default_display_name": ""},      # dashboard
    "Render": {"glyph": "\ue439", "display_name": "", "color": [0.0, 0.0, 0.0],
               "default_glyph": "\ue439", "default_display_name": ""},     # photo_camera
    "Node": {"glyph": "\uf20e", "display_name": "", "color": [0.0, 0.0, 0.0],
             "default_glyph": "\uf20e", "default_display_name": ""},       # account_tree
    "Group": {"glyph": "\ue574", "display_name": "", "color": [0.0, 0.0, 0.0],
             "default_glyph": "\ue574", "default_display_name": ""},       # account_tree
    "Scopes": {"glyph": "\ue762", "display_name": "", "color": [0.0, 0.0, 0.0],
             "default_glyph": "\ue762", "default_display_name": ""},       # account_tree
}

# ----------------------------------------------------------------------------
# Space / mode / bl_context bit-flag maps

# Maps space type string identifiers to single-bit flags for discovered_in_spaces.
# Uses the same IDs as _space_type_str_to_id but stored as power-of-two bitmask values.
# NOTE: Both SPACE_* format (internal) and bl_space_type format (from panels) are supported.
SPACE_TO_FLAG = {
    # Internal SPACE_* format
    'SPACE_VIEW3D':     (1 << 0),
    'SPACE_GRAPH':      (1 << 1),
    'SPACE_OUTLINER':   (1 << 2),
    'SPACE_PROPERTIES': (1 << 3),
    'SPACE_FILE':       (1 << 4),
    'SPACE_IMAGE':      (1 << 5),
    'SPACE_INFO':       (1 << 6),
    'SPACE_SEQ':        (1 << 7),
    'SPACE_TEXT':       (1 << 8),
    'SPACE_ACTION':     (1 << 9),
    'SPACE_NLA':        (1 << 10),
    'SPACE_NODE':       (1 << 11),
    'SPACE_CONSOLE':    (1 << 12),
    'SPACE_USERPREF':   (1 << 13),
    'SPACE_CLIP':       (1 << 14),
    'SPACE_TOPBAR':     (1 << 15),
    'SPACE_STATUSBAR':  (1 << 16),
    'SPACE_SPREADSHEET':(1 << 17),
    # bl_space_type format (from Panel.bl_space_type attribute)
    'VIEW_3D':          (1 << 0),
    'GRAPH_EDITOR':     (1 << 1),
    'OUTLINER':         (1 << 2),
    'PROPERTIES':       (1 << 3),
    'FILE_BROWSER':     (1 << 4),
    'IMAGE_EDITOR':     (1 << 5),
    'INFO':             (1 << 6),
    'SEQUENCE_EDITOR':  (1 << 7),
    'TEXT_EDITOR':      (1 << 8),
    'DOPESHEET_EDITOR': (1 << 9),
    'NLA_EDITOR':       (1 << 10),
    'NODE_EDITOR':      (1 << 11),
    'CONSOLE':          (1 << 12),
    'USER_PREFERENCES': (1 << 13),
    'CLIP_EDITOR':      (1 << 14),
    'TOPBAR':           (1 << 15),
    'STATUSBAR':        (1 << 16),
    'SPREADSHEET':      (1 << 17),
}
_FLAG_TO_SPACE = {v: k for k, v in SPACE_TO_FLAG.items()}

# Maps mode name strings to single-bit flags for discovered_in_modes.
# Reuses the same bit positions as _CATEGORY_TAG_MODE_NAME_TO_FLAG.
MODE_TO_FLAG = {name: (1 << bit) for name, _id, bit, _label, _icon in _CATEGORY_TAG_MODES}
_FLAG_TO_MODE = {v: k for k, v in MODE_TO_FLAG.items()}

# Maps Panel.bl_context values to mode flags.
# bl_context is used by panels to show only in specific modes.
# IMPORTANT: For edit modes, use the detailed flags (MESH_EDIT, CURVE_EDIT, etc.)
# so that categories only show when editing the correct object type.
BL_CONTEXT_TO_MODE_FLAG = {
    # Object mode contexts
    'objectmode': MODE_TO_FLAG.get('OBJECT_MODE', 0),
    # Edit mode contexts - now use detailed edit flags
    'mesh_edit': MODE_TO_FLAG.get('MESH_EDIT', 0) or (1 << 11),  # bit 11
    'curve_edit': MODE_TO_FLAG.get('CURVE_EDIT', 0) or (1 << 12),  # bit 12
    'surface_edit': MODE_TO_FLAG.get('SURFACE_EDIT', 0) or (1 << 13),  # bit 13
    'armature_edit': MODE_TO_FLAG.get('ARMATURE_EDIT', 0) or (1 << 14),  # bit 14
    'lattice_edit': MODE_TO_FLAG.get('LATTICE_EDIT', 0) or (1 << 15),  # bit 15
    'metaball_edit': MODE_TO_FLAG.get('META_EDIT', 0) or (1 << 16),  # bit 16
    'text_edit': MODE_TO_FLAG.get('FONT_EDIT', 0) or (1 << 17),  # bit 17
    'gpencil_edit': MODE_TO_FLAG.get('GREASE_PENCIL_EDIT', 0) or (1 << 18),  # bit 18
    'greasepencil_edit': MODE_TO_FLAG.get('GREASE_PENCIL_EDIT', 0) or (1 << 18),  # bit 18 (alternate name)
    'curves_edit': MODE_TO_FLAG.get('CURVE_EDIT', 0) or (1 << 12),  # bit 12 (Curves use same as Curve)
    'pointcloud_edit': MODE_TO_FLAG.get('POINTCLOUD_EDIT', 0) or (1 << 19),  # bit 19
    'volume_edit': MODE_TO_FLAG.get('VOLUME_EDIT', 0) or (1 << 20),  # bit 20
    # Paint modes
    'sculptmode': MODE_TO_FLAG.get('SCULPT_MODE', 0),
    'vertexpaint': MODE_TO_FLAG.get('VERTEX_PAINT', 0),
    'weightpaint': MODE_TO_FLAG.get('WEIGHT_PAINT', 0),
    'texturepaint': MODE_TO_FLAG.get('TEXTURE_PAINT', 0),
    'imagepaint': MODE_TO_FLAG.get('IMAGE_PAINT', 0),
    # UV edit
    'uv_edit': MODE_TO_FLAG.get('UV_EDIT', 0),
    'uv_sculpt': MODE_TO_FLAG.get('UV_EDIT', 0),
    # Pose mode
    'posemode': MODE_TO_FLAG.get('POSE_MODE', 0),
    # Grease pencil modes
    'greasepencil_paint': MODE_TO_FLAG.get('GREASE_PENCIL_EDIT', 0) or (1 << 18),  # Use GREASE_PENCIL_EDIT
    'greasepencil_sculpt': MODE_TO_FLAG.get('GREASE_PENCIL_EDIT', 0) or (1 << 18),
    'greasepencil_weight': MODE_TO_FLAG.get('GREASE_PENCIL_EDIT', 0) or (1 << 18),
    'greasepencil_vertex': MODE_TO_FLAG.get('VERTEX_PAINT', 0),
}

# ----------------------------------------------------------------------------
# JSON storage constants

# Current JSON format version. The schema starts at version 1 (baseline); there is
# no cross-version migration chain.
CURRENT_JSON_VERSION = 1

# JSON file name in config directory
GLYPHS_FILENAME = "category_glyphs.json"

# ----------------------------------------------------------------------------
# Reserved category priority per space type

# Priority order for reserved categories per space type.
# Categories not in this list are sorted alphabetically at the end of the reserved block.
# IMPORTANT: Names must correspond to reserved categories in DEFAULT_CATEGORY_GLYPHS.
RESERVED_CATEGORY_PRIORITY = {
    'VIEW_3D': [
        "Item", "Tool", "View", "Animation", "Edit", "Asset", "Options",
        "Modifiers", "Physics", "Material", "World", "Scene",
        "Render", "Cache", "Proxy", "Metadata"
    ],
    'PROPERTIES': [
        "Item", "Tool", "View", "Physics", "Material", "World", "Scene",
        "Render", "Options", "Texture", "Output", "Cache", "Proxy", "Metadata"
    ],
    'NODE_EDITOR': [
        "Item", "Tool", "View", "Options", "Node", "Group", "Cache", "Proxy", "Metadata"
    ],
    'IMAGE_EDITOR': [
        "Item", "Tool", "View", "Image", "Mask", "Scopes", "Cache", "Proxy", "Metadata"
    ],
    'SEQUENCE_EDITOR': [
        "Item", "Tool", "View", "Strip", "Cache", "Proxy", "Metadata"
    ],
    'CLIP_EDITOR': [
        "Item", "Tool", "View", "Mask", "Tracking", "Cache", "Proxy", "Metadata"
    ],
    'TEXT_EDITOR': [
        "Tool", "View", "Options", "Text", "Cache", "Proxy", "Metadata"
    ],
    'DOPESHEET_EDITOR': [
        "Item", "Tool", "View", "Animation", "Cache", "Proxy", "Metadata"
    ],
    'GRAPH_EDITOR': [
        "Item", "Tool", "View", "Animation", "Cache", "Proxy", "Metadata"
    ],
    'NLA_EDITOR': [
        "Item", "Tool", "View", "Animation", "Cache", "Proxy", "Metadata"
    ],
    # Default fallback for unknown space types
    'DEFAULT': [
        "Item", "Tool", "View", "Edit", "Asset", "Options", "Cache", "Proxy", "Metadata"
    ]
}

# ----------------------------------------------------------------------------
# Default tag glyph

# Default glyph hex for tags when none is specified (FontAwesome tag icon)
DEFAULT_TAG_GLYPH_HEX = "e866"
