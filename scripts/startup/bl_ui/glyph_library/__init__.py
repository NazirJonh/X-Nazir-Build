# scripts/startup/bl_ui/glyph_library/__init__.py

bl_info = {
    "name": "Glyph Library",
    "author": "Blender Team",
    "version": (1, 0, 0),
    "blender": (4, 1, 0),
    "description": "Material Symbols glyph library with search",
    "category": "UI",
}

from .registry import GlyphLibrary, get_glyph_library, search_glyphs
from .translations import TranslationManager, get_translation_manager

__all__ = [
    'GlyphLibrary', 'get_glyph_library',
    'TranslationManager', 'get_translation_manager',
    'search_glyphs'
]

# Empty classes list for bl_ui/__init__.py compatibility
classes = []

def register():
    from .registry import register as register_registry
    from .ui import register as register_ui

    register_registry()
    register_ui()

def unregister():
    from .registry import unregister as unregister_registry
    from .ui import unregister as unregister_ui

    unregister_ui()
    unregister_registry()
