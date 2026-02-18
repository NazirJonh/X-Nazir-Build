# scripts/startup/bl_ui/glyph_library/ui.py

import bpy
from bpy.types import Operator, PropertyGroup
from bpy.props import StringProperty, IntProperty, CollectionProperty

from .registry import get_glyph_library
from .translations import get_translation_manager, get_current_locale


class GlyphSearchResult(PropertyGroup):
    name: StringProperty()
    codepoint: StringProperty()
    unicode: StringProperty()
    category: StringProperty()
    display_name: StringProperty()


class GlyphSearchResults(PropertyGroup):
    results: CollectionProperty(type=GlyphSearchResult)
    active_index: IntProperty(default=0)


class WM_OT_glyph_search(Operator):
    bl_idname = "wm.glyph_search"
    bl_label = "Search Glyphs"
    bl_description = "Search for glyphs by name, tags or keywords"
    bl_options = {'REGISTER', 'UNDO'}
    
    query: StringProperty(
        name="Search",
        description="Search query",
        default=""
    )
    
    category: StringProperty(
        name="Category",
        description="Filter by category",
        default=""
    )
    
    max_results: IntProperty(
        name="Max Results",
        default=50,
        min=1,
        max=200
    )
    
    def execute(self, context):
        library = get_glyph_library()
        manager = get_translation_manager()
        locale = get_current_locale()
        
        results = library.search(self.query, self.category, self.max_results)
        
        storage = context.window_manager.glyph_search_results
        storage.results.clear()
        
        for glyph in results:
            item = storage.results.add()
            item.name = glyph['name']
            item.codepoint = glyph['codepoint']
            item.unicode = glyph['unicode']
            item.category = glyph.get('category', '')
            item.display_name = manager.get_glyph_name(glyph['name'], locale)
        
        return {'FINISHED'}
    
    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self, width=400)


class WM_OT_glyph_select(Operator):
    bl_idname = "wm.glyph_select"
    bl_label = "Select Glyph"
    bl_description = "Select a glyph from search results"
    bl_options = {'REGISTER', 'INTERNAL'}
    
    glyph_name: StringProperty(
        name="Glyph Name",
        default=""
    )
    
    target_property: StringProperty(
        name="Target Property",
        description="RNA path to property that will receive the glyph",
        default=""
    )
    
    def execute(self, context):
        library = get_glyph_library()
        glyph = library.get_by_name(self.glyph_name)
        
        if not glyph:
            self.report({'ERROR'}, "Glyph not found: {:s}".format(self.glyph_name))
            return {'CANCELLED'}
        
        if self.target_property:
            try:
                rna_path, prop_name = self.target_property.rsplit('.', 1)
                obj = context.path_resolve(rna_path)
                setattr(obj, prop_name, glyph['unicode'])
            except Exception as e:
                self.report({'ERROR'}, "Failed to set property: {:s}".format(str(e)))
                return {'CANCELLED'}
        
        context.window_manager.glyph_selected = glyph['unicode']
        context.window_manager.glyph_selected_name = glyph['name']
        
        return {'FINISHED'}


class WM_OT_glyph_picker(Operator):
    bl_idname = "wm.glyph_picker"
    bl_label = "Glyph Picker"
    bl_description = "Open glyph picker dialog"
    
    target_property: StringProperty(default="")
    
    def execute(self, context):
        return {'FINISHED'}
    
    def invoke(self, context, event):
        bpy.ops.wm.glyph_search('INVOKE_DEFAULT')
        return {'RUNNING_MODAL'}


classes = [
    GlyphSearchResult,
    GlyphSearchResults,
    WM_OT_glyph_search,
    WM_OT_glyph_select,
    WM_OT_glyph_picker,
]


def register():
    for cls in classes:
        bpy.utils.register_class(cls)
    
    bpy.types.WindowManager.glyph_search_results = bpy.props.PointerProperty(
        type=GlyphSearchResults
    )
    bpy.types.WindowManager.glyph_selected = bpy.props.StringProperty(default="")
    bpy.types.WindowManager.glyph_selected_name = bpy.props.StringProperty(default="")
    bpy.types.WindowManager.glyph_selected_hex = bpy.props.StringProperty(default="")


def unregister():
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
    
    del bpy.types.WindowManager.glyph_search_results
    del bpy.types.WindowManager.glyph_selected
    del bpy.types.WindowManager.glyph_selected_name
    del bpy.types.WindowManager.glyph_selected_hex
