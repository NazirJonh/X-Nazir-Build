# scripts/startup/bl_ui/glyph_library/registry.py

import json
import logging
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from .validators import (
    validate_material_symbols_file,
    validate_categories_file,
    FALLBACK_GLYPH_UNICODE,
    FALLBACK_GLYPH_NAME,
)

log = logging.getLogger(__name__)

# =============================================================================
# Global debug flag for enabling/disabling glyph library debug logging
# Set to False to disable all [GLYPH LIBRARY] print statements
# =============================================================================
GLYPH_LIBRARY_DEBUG_ENABLED = False

GLYPH_LIBRARY_DIR = Path(__file__).parent / "data"

# Import DEFAULT_CATEGORY_GLYPHS from space_userpref.py
try:
    from ..space_userpref import DEFAULT_CATEGORY_GLYPHS
except ImportError:
    DEFAULT_CATEGORY_GLYPHS = {}
    if GLYPH_LIBRARY_DEBUG_ENABLED:
        log.warning("DEFAULT_CATEGORY_GLYPHS not imported from space_userpref.py")


class GlyphLibrary:
    _instance = None
    
    def __new__(cls):
        if cls._instance is None:
            cls._instance = super().__new__(cls)
            cls._instance._initialized = False
        return cls._instance
    
    def __init__(self):
        if self._initialized:
            return
        
        self.glyphs: Dict[str, dict] = {}
        self.categories: Dict[str, dict] = {}
        self.blender_mappings: Dict[str, dict] = {}
        self.codepoint_index: Dict[str, str] = {}
        self.category_index: Dict[str, List[str]] = {}
        
        self.is_loaded = False
        self.load_errors: List[str] = []
        
        self._initialized = True
    
    def load_all(self) -> bool:
        if GLYPH_LIBRARY_DEBUG_ENABLED:
            print("[GLYPH LIBRARY] ===== LOAD_ALL START =====")
        self.load_errors = []

        success = True

        if GLYPH_LIBRARY_DEBUG_ENABLED:
            print("[GLYPH LIBRARY] Loading material_symbols.json...")
        if not self._load_material_symbols():
            if GLYPH_LIBRARY_DEBUG_ENABLED:
                print("[GLYPH LIBRARY] FAILED to load material_symbols.json")
            success = False
        else:
            if GLYPH_LIBRARY_DEBUG_ENABLED:
                print(f"[GLYPH LIBRARY] Loaded {len(self.glyphs)} glyphs")

        if GLYPH_LIBRARY_DEBUG_ENABLED:
            print("[GLYPH LIBRARY] Loading categories.json...")
        if not self._load_categories():
            if GLYPH_LIBRARY_DEBUG_ENABLED:
                print("[GLYPH LIBRARY] No categories loaded (non-critical)")
            pass  # Non-critical
        else:
            if GLYPH_LIBRARY_DEBUG_ENABLED:
                print(f"[GLYPH LIBRARY] Loaded {len(self.categories)} categories")

        if success:
            if GLYPH_LIBRARY_DEBUG_ENABLED:
                print("[GLYPH LIBRARY] Building indices...")
            self._build_indices()
            if GLYPH_LIBRARY_DEBUG_ENABLED:
                print("[GLYPH LIBRARY] Integrating default category glyphs...")
            self._integrate_default_category_glyphs()
            self.is_loaded = True
            log.info("Glyph library loaded: %d glyphs, %d categories",
                     len(self.glyphs), len(self.categories))
            if GLYPH_LIBRARY_DEBUG_ENABLED:
                print(f"[GLYPH LIBRARY] ===== LOAD_ALL SUCCESS: {len(self.glyphs)} glyphs =====")
        else:
            if GLYPH_LIBRARY_DEBUG_ENABLED:
                print(f"[GLYPH LIBRARY] ===== LOAD_ALL FAILED: {self.load_errors} =====")

        return success
    
    def _load_material_symbols(self) -> bool:
        filepath = GLYPH_LIBRARY_DIR / "material_symbols.json"
        if GLYPH_LIBRARY_DEBUG_ENABLED:
            print(f"[GLYPH LIBRARY] Loading from: {filepath}")
            print(f"[GLYPH LIBRARY] File exists: {filepath.exists()}")

        if not filepath.exists():
            msg = f"Critical: material_symbols.json not found at {filepath}"
            log.critical(msg)
            self.load_errors.append(msg)
            return False

        try:
            with open(filepath, 'r', encoding='utf-8') as f:
                data = json.load(f)

            if GLYPH_LIBRARY_DEBUG_ENABLED:
                print(f"[GLYPH LIBRARY] JSON loaded, keys: {list(data.keys())}")
            glyphs_list = data.get('glyphs', [])
            if GLYPH_LIBRARY_DEBUG_ENABLED:
                print(f"[GLYPH LIBRARY] Glyphs in file: {len(glyphs_list)}")

            is_valid, errors = validate_material_symbols_file(data)

            if not is_valid:
                if GLYPH_LIBRARY_DEBUG_ENABLED:
                    print(f"[GLYPH LIBRARY] Validation failed with {len(errors)} errors:")
                    for i, err in enumerate(errors[:5]):
                        print(f"[GLYPH LIBRARY]   Error {i+1}: {err}")
                for err in errors:
                    log.error("Validation: %s", err)
                    self.load_errors.append(err)

                if len(errors) > 10:
                    return False

            self.glyphs = {g['name']: g for g in glyphs_list}
            self.blender_mappings = data.get('blender_default_mappings', {})
            if GLYPH_LIBRARY_DEBUG_ENABLED:
                print(f"[GLYPH LIBRARY] Loaded {len(self.glyphs)} glyphs")

            # Print first few glyphs for verification
            if GLYPH_LIBRARY_DEBUG_ENABLED:
                for i, (name, glyph) in enumerate(list(self.glyphs.items())[:3]):
                    print(f"[GLYPH LIBRARY]   Glyph {i+1}: {name} = {glyph.get('unicode', 'N/A')}")

            return True

        except json.JSONDecodeError as e:
            msg = f"Invalid JSON in material_symbols.json: {e}"
            log.error(msg)
            if GLYPH_LIBRARY_DEBUG_ENABLED:
                print(f"[GLYPH LIBRARY] JSON decode error: {e}")
            self.load_errors.append(msg)
            return False
        except Exception as e:
            msg = f"Error loading material_symbols.json: {e}"
            log.exception(msg)
            self.load_errors.append(msg)
            return False
    
    def _load_categories(self) -> bool:
        filepath = GLYPH_LIBRARY_DIR / "categories.json"
        
        if not filepath.exists():
            log.warning("categories.json not found, using defaults")
            return False
        
        try:
            with open(filepath, 'r', encoding='utf-8') as f:
                data = json.load(f)
            
            is_valid, errors = validate_categories_file(data)
            
            if not is_valid:
                for err in errors:
                    log.warning("Categories validation: %s", err)
            
            self.categories = data.get('categories', {})
            
            if 'blender_category_mappings' in data:
                for cat, mapping in data['blender_category_mappings'].items():
                    if cat not in self.blender_mappings:
                        self.blender_mappings[cat] = mapping
            
            return True
            
        except Exception as e:
            log.warning("Error loading categories.json: %s", e)
            return False
    
    def _integrate_default_category_glyphs(self):
        """Integrate DEFAULT_CATEGORY_GLYPHS from space_userpref.py as default mappings."""
        if DEFAULT_CATEGORY_GLYPHS:
            log.info("Integrating DEFAULT_CATEGORY_GLYPHS with %d mappings", 
                     len(DEFAULT_CATEGORY_GLYPHS))
            
            for category, glyph_unicode in DEFAULT_CATEGORY_GLYPHS.items():
                if category not in self.blender_mappings:
                    self.blender_mappings[category] = {
                        'default_glyph': glyph_unicode,
                        'source': 'space_userpref'
                    }
                
                # Try to find glyph name by unicode
                for glyph_name, glyph_data in self.glyphs.items():
                    if glyph_data.get('unicode') == glyph_unicode:
                        self.blender_mappings[category]['default_glyph_name'] = glyph_name
                        break
    
    def _build_indices(self):
        self.codepoint_index = {
            g['codepoint']: name 
            for name, g in self.glyphs.items()
        }
        
        self.category_index = {}
        for name, g in self.glyphs.items():
            cat = g.get('category', 'action')
            if cat not in self.category_index:
                self.category_index[cat] = []
            self.category_index[cat].append(name)
    
    def get_by_name(self, name: str) -> Optional[dict]:
        return self.glyphs.get(name)
    
    def get_by_codepoint(self, codepoint: str) -> Optional[dict]:
        name = self.codepoint_index.get(codepoint)
        if name:
            return self.glyphs.get(name)
        return None
    
    def get_glyph_for_category(self, category: str) -> str:
        if not self.is_loaded:
            return FALLBACK_GLYPH_UNICODE
        
        mapping = self.blender_mappings.get(category, {})
        glyph_name = mapping.get('default_glyph_name', '')
        
        if glyph_name and glyph_name in self.glyphs:
            return self.glyphs[glyph_name]['unicode']
        
        return mapping.get('default_glyph', FALLBACK_GLYPH_UNICODE)
    
    def get_categories(self) -> Dict[str, dict]:
        return self.categories
    
    def get_glyphs_by_category(self, category: str) -> List[dict]:
        names = self.category_index.get(category, [])
        return [self.glyphs[n] for n in names if n in self.glyphs]
    
    def search(self, query: str, category: str = "", max_results: int = 50) -> List[dict]:
        if GLYPH_LIBRARY_DEBUG_ENABLED:
            print(f"[GLYPH LIBRARY] search() called: query='{query}', category='{category}', max={max_results}")
            print(f"[GLYPH LIBRARY] is_loaded={self.is_loaded}, total_glyphs={len(self.glyphs)}")

        if not self.is_loaded:
            if GLYPH_LIBRARY_DEBUG_ENABLED:
                print("[GLYPH LIBRARY] ERROR: Library not loaded!")
            return []

        query = query.lower().strip()
        if GLYPH_LIBRARY_DEBUG_ENABLED:
            print(f"[GLYPH LIBRARY] Normalized query: '{query}'")

        if not query:
            if GLYPH_LIBRARY_DEBUG_ENABLED:
                print("[GLYPH LIBRARY] Empty query, returning by category or all")
            if category:
                results = self.get_glyphs_by_category(category)[:max_results]
                if GLYPH_LIBRARY_DEBUG_ENABLED:
                    print(f"[GLYPH LIBRARY] Returning {len(results)} glyphs for category '{category}'")
                return results
            results = list(self.glyphs.values())[:max_results]
            if GLYPH_LIBRARY_DEBUG_ENABLED:
                print(f"[GLYPH LIBRARY] Returning {len(results)} glyphs (no category filter)")
            return results

        results_with_scores: List[Tuple[float, dict]] = []

        if GLYPH_LIBRARY_DEBUG_ENABLED:
            print(f"[GLYPH LIBRARY] Searching through {len(self.glyphs)} glyphs...")
        for name, glyph in self.glyphs.items():
            if category and glyph.get('category') != category:
                continue

            score = self._calculate_score(query, glyph)

            if score > 0:
                if GLYPH_LIBRARY_DEBUG_ENABLED:
                    print(f"[GLYPH LIBRARY] Match found: '{name}' score={score}")
                results_with_scores.append((score, glyph))

        results_with_scores.sort(key=lambda x: (-x[0], -x[1].get('popularity', 0)))

        final_results = [g for _, g in results_with_scores[:max_results]]
        if GLYPH_LIBRARY_DEBUG_ENABLED:
            print(f"[GLYPH LIBRARY] Returning {len(final_results)} results")
        return final_results
    
    def _get_fuzzy_match_errors(self, query: str, text: str) -> int:
        """
        Get fuzzy match errors using C++ BLI_string_search.
        Returns -1 if no match, otherwise returns error count.
        """
        try:
            import bpy
            # Try to call C++ function through RNA
            # This will be implemented when RNA bindings are added
            # For now, use a simple fallback
            return self._fuzzy_match_fallback(query, text)
        except Exception:
            return self._fuzzy_match_fallback(query, text)
    
    def _fuzzy_match_fallback(self, query: str, text: str) -> int:
        """Fallback fuzzy match implementation."""
        if not query or not text:
            return -1
        
        query = query.lower()
        text = text.lower()
        
        # Perfect match
        if query == text:
            return 0
        
        # Partial match
        if query in text:
            return 0
        
        # Simple distance calculation
        distance = self._levenshtein_distance_simple(query, text)
        max_len = max(len(query), len(text))
        
        if max_len == 0:
            return -1
        
        # Allow more errors for longer strings
        max_errors = max(1, len(query) // 8 + 1)
        
        if distance <= max_errors:
            return distance
        
        return -1
    
    def _levenshtein_distance_simple(self, s1: str, s2: str) -> int:
        """Simple Levenshtein distance calculation."""
        if len(s1) < len(s2):
            return self._levenshtein_distance_simple(s2, s1)
        
        if len(s2) == 0:
            return len(s1)
        
        previous_row = range(len(s2) + 1)
        
        for i, c1 in enumerate(s1):
            current_row = [i + 1]
            for j, c2 in enumerate(s2):
                insertions = previous_row[j + 1] + 1
                deletions = current_row[j] + 1
                substitutions = previous_row[j] + (c1 != c2)
                current_row.append(min(insertions, deletions, substitutions))
            previous_row = current_row
        
        return previous_row[-1]
    
    def _calculate_score(self, query: str, glyph: dict) -> float:
        name = glyph.get('name', '').lower()
        
        if query == name:
            return 1.0
        
        if name.startswith(query):
            return 0.95
        
        if query in name:
            return 0.9
        
        tags = [t.lower() for t in glyph.get('tags', [])]
        for tag in tags:
            if query == tag:
                return 0.9
            if query in tag:
                return 0.8
        
        aliases = [a.lower() for a in glyph.get('aliases', [])]
        for alias in aliases:
            if query == alias:
                return 0.85
            if query in alias:
                return 0.75
        
        # Fuzzy matching on name using C++ BLI_string_search
        error_count = self._get_fuzzy_match_errors(query, name)
        if error_count >= 0 and error_count <= 2:  # Allow up to 2 errors
            similarity = 1.0 - (error_count / max(len(query), len(name)))
            if similarity > 0.6:
                return similarity * 0.5
        
        # Fuzzy matching on tags
        for tag in tags:
            error_count = self._get_fuzzy_match_errors(query, tag)
            if error_count >= 0 and error_count <= 2:
                similarity = 1.0 - (error_count / max(len(query), len(tag)))
                if similarity > 0.7:
                    return similarity * 0.4
        
        blender_context = [c.lower() for c in glyph.get('blender_context', [])]
        for ctx in blender_context:
            if query == ctx:
                return 0.7
            if query in ctx:
                return 0.6
        
        return 0.0


_library_instance: Optional[GlyphLibrary] = None


def get_glyph_library() -> GlyphLibrary:
    global _library_instance
    
    if _library_instance is None:
        _library_instance = GlyphLibrary()
        _library_instance.load_all()
    
    return _library_instance


def register():
    if GLYPH_LIBRARY_DEBUG_ENABLED:
        print("[GLYPH LIBRARY] ===== REGISTER START =====")
    library = get_glyph_library()
    if GLYPH_LIBRARY_DEBUG_ENABLED:
        print(f"[GLYPH LIBRARY] After get_glyph_library: is_loaded={library.is_loaded}")
        print(f"[GLYPH LIBRARY] Glyphs count: {len(library.glyphs)}")
        print(f"[GLYPH LIBRARY] Load errors: {library.load_errors}")

    if not library.is_loaded:
        log.warning("Glyph library failed to load, using fallback")
        if GLYPH_LIBRARY_DEBUG_ENABLED:
            print("[GLYPH LIBRARY] WARNING: Library not loaded, using fallback!")
    else:
        if GLYPH_LIBRARY_DEBUG_ENABLED:
            print(f"[GLYPH LIBRARY] Successfully loaded {len(library.glyphs)} glyphs")
    if GLYPH_LIBRARY_DEBUG_ENABLED:
        print("[GLYPH LIBRARY] ===== REGISTER END =====")


def search_glyphs(query: str, category: str = "", max_results: int = 50) -> List[dict]:
    """Public API function for searching glyphs."""
    if GLYPH_LIBRARY_DEBUG_ENABLED:
        print(f"[GLYPH LIBRARY API] search_glyphs() called: query='{query}', category='{category}', max={max_results}")
    library = get_glyph_library()
    if GLYPH_LIBRARY_DEBUG_ENABLED:
        print(f"[GLYPH LIBRARY API] Library state: is_loaded={library.is_loaded}, glyphs={len(library.glyphs)}")
    results = library.search(query, category, max_results)
    if GLYPH_LIBRARY_DEBUG_ENABLED:
        print(f"[GLYPH LIBRARY API] Returning {len(results)} results")
    return results


def search_with_translation(query: str, category: str = "", max_results: int = 50, locale: str = None) -> List[dict]:
    """Search glyphs with localized results."""
    from .translations import get_translation_manager, get_current_locale
    
    library = get_glyph_library()
    manager = get_translation_manager()
    
    if locale is None:
        locale = get_current_locale()
    
    manager.set_locale(locale)
    
    results = library.search(query, category, max_results)
    
    # Add localized information to results
    for glyph in results:
        glyph['display_name'] = manager.get_glyph_name(glyph['name'], locale)
        glyph['localized_keywords'] = manager.get_glyph_keywords(glyph['name'], locale)
    
    return results


def unregister():
    global _library_instance
    _library_instance = None
