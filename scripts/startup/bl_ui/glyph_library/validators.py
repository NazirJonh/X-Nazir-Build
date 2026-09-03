# scripts/startup/bl_ui/glyph_library/validators.py

import re
from typing import Tuple, List

MATERIAL_SYMBOLS_REQUIRED_FIELDS = ['codepoint', 'name', 'unicode', 'category']

VALID_CATEGORIES = [
    'action', 'alert', 'av', 'communication', 'content', 'device',
    'editor', 'file', 'hardware', 'image', 'maps', 'navigation',
    'places', 'search', 'social', 'toggle'
]

FALLBACK_GLYPH_CODEPOINT = "e574"
FALLBACK_GLYPH_UNICODE = "\ue574"
FALLBACK_GLYPH_NAME = "category"


def validate_glyph_data(glyph: dict) -> Tuple[bool, List[str]]:
    errors = []
    
    for field in MATERIAL_SYMBOLS_REQUIRED_FIELDS:
        if field not in glyph:
            errors.append(f"Missing required field: {field}")
    
    if errors:
        return False, errors
    
    if not re.match(r'^[0-9a-fA-F]{4,5}$', glyph['codepoint']):
        errors.append(f"Invalid codepoint format: {glyph['codepoint']}")
    
    try:
        glyph['unicode'].encode('utf-8').decode('utf-8')
    except (UnicodeError, AttributeError):
        errors.append(f"Invalid Unicode: {glyph.get('unicode')}")
    
    if not glyph.get('name'):
        errors.append("Glyph name cannot be empty")
    elif not re.match(r'^[a-z0-9_]+$', glyph['name']):
        errors.append(f"Invalid glyph name: {glyph['name']}")
    
    if glyph['category'] not in VALID_CATEGORIES:
        errors.append(f"Unknown category: {glyph['category']}")
    
    return len(errors) == 0, errors


def validate_material_symbols_file(data: dict) -> Tuple[bool, List[str]]:
    errors = []
    
    if 'version' not in data:
        errors.append("Missing 'version' field")
    
    if 'glyphs' not in data:
        errors.append("Missing 'glyphs' field")
        return False, errors
    
    if not isinstance(data['glyphs'], list):
        errors.append("'glyphs' must be an array")
        return False, errors
    
    if len(data['glyphs']) == 0:
        errors.append("'glyphs' array is empty")
    
    duplicate_names = set()
    duplicate_codepoints = set()
    seen_names = set()
    seen_codepoints = set()
    
    for i, glyph in enumerate(data['glyphs']):
        is_valid, glyph_errors = validate_glyph_data(glyph)
        
        if not is_valid:
            for err in glyph_errors:
                errors.append(f"Glyph [{i}]: {err}")
        
        name = glyph.get('name', '')
        if name in seen_names:
            duplicate_names.add(name)
        seen_names.add(name)
        
        codepoint = glyph.get('codepoint', '')
        if codepoint and codepoint in seen_codepoints:
            duplicate_codepoints.add(codepoint)
        seen_codepoints.add(codepoint)
    
    if duplicate_names:
        errors.append(f"Duplicate names: {', '.join(duplicate_names)}")
    
    if duplicate_codepoints:
        errors.append(f"Duplicate codepoints: {', '.join(duplicate_codepoints)}")
    
    return len(errors) == 0, errors


def validate_categories_file(data: dict) -> Tuple[bool, List[str]]:
    errors = []
    
    if 'version' not in data:
        errors.append("Missing 'version' field")
    
    if 'categories' not in data:
        errors.append("Missing 'categories' field")
        return False, errors
    
    if not isinstance(data['categories'], dict):
        errors.append("'categories' must be an object")
        return False, errors
    
    for cat_name, cat_data in data['categories'].items():
        if not isinstance(cat_data, dict):
            errors.append(f"Category '{cat_name}': must be an object")
            continue
        
        if 'name' not in cat_data:
            errors.append(f"Category '{cat_name}': missing 'name'")
    
    return len(errors) == 0, errors
