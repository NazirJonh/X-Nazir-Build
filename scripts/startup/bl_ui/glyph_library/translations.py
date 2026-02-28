# scripts/startup/bl_ui/glyph_library/translations.py

import json
import logging
from pathlib import Path
from typing import Dict, Optional

log = logging.getLogger(__name__)

TRANSLATIONS_DIR = Path(__file__).parent / "data" / "translations"

SUPPORTED_LOCALES = [
    'en_US', 'ru_RU', 'de_DE', 'fr_FR', 'es_ES',
    'ja_JP', 'zh_CN', 'zh_TW', 'ko_KR',
    'pt_PT', 'it_IT', 'uk_UA'
]


class TranslationManager:
    _instance = None
    
    def __new__(cls):
        if cls._instance is None:
            cls._instance = super().__new__(cls)
            cls._instance._initialized = False
        return cls._instance
    
    def __init__(self):
        if self._initialized:
            return
        
        self.translations: Dict[str, dict] = {}
        self.current_locale = 'en_US'
        self._initialized = True
    
    def load_locale(self, locale: str) -> bool:
        if locale in self.translations:
            return True
        
        filepath = TRANSLATIONS_DIR / f"{locale}.json"
        
        if not filepath.exists():
            log.warning("Translation file not found: %s", filepath)
            return False
        
        try:
            with open(filepath, 'r', encoding='utf-8') as f:
                data = json.load(f)
            
            self.translations[locale] = data
            log.info("Loaded translations for %s", locale)
            return True
            
        except Exception as e:
            log.error("Error loading translation %s: %s", locale, e)
            return False
    
    def set_locale(self, locale: str) -> bool:
        if locale not in SUPPORTED_LOCALES:
            log.warning("Unsupported locale: %s, falling back to en_US", locale)
            locale = 'en_US'
        
        if self.load_locale(locale):
            self.current_locale = locale
            return True
        
        if locale != 'en_US':
            self.load_locale('en_US')
            self.current_locale = 'en_US'
        
        return False
    
    def get_glyph_translation(self, glyph_name: str, locale: str = None) -> Optional[dict]:
        locale = locale or self.current_locale
        
        if locale not in self.translations:
            if not self.load_locale(locale):
                return None
        
        trans = self.translations.get(locale, {})
        glyphs = trans.get('glyphs', {})
        return glyphs.get(glyph_name)
    
    def get_glyph_name(self, glyph_name: str, locale: str = None) -> str:
        trans = self.get_glyph_translation(glyph_name, locale)
        if trans and 'name' in trans:
            return trans['name']
        return glyph_name.replace('_', ' ').title()
    
    def get_glyph_keywords(self, glyph_name: str, locale: str = None) -> list:
        trans = self.get_glyph_translation(glyph_name, locale)
        if trans and 'keywords' in trans:
            return trans['keywords']
        return []
    
    def get_category_name(self, category: str, locale: str = None) -> str:
        locale = locale or self.current_locale
        
        if locale not in self.translations:
            self.load_locale(locale)
        
        trans = self.translations.get(locale, {})
        categories = trans.get('categories', {})
        
        if category in categories:
            return categories[category]
        
        return category.title()
    
    def get_ui_label(self, key: str, locale: str = None) -> str:
        locale = locale or self.current_locale
        
        if locale not in self.translations:
            self.load_locale(locale)
        
        trans = self.translations.get(locale, {})
        ui_labels = trans.get('ui_labels', {})
        
        if key in ui_labels:
            return ui_labels[key]
        
        en_trans = self.translations.get('en_US', {})
        en_ui_labels = en_trans.get('ui_labels', {})
        return en_ui_labels.get(key, key)


_translation_manager: Optional[TranslationManager] = None


def get_translation_manager() -> TranslationManager:
    global _translation_manager
    
    if _translation_manager is None:
        _translation_manager = TranslationManager()
        _translation_manager.load_locale('en_US')
    
    return _translation_manager


def init_translations():
    """Initialize translations with Blender's current locale."""
    manager = get_translation_manager()
    
    try:
        import bpy
        locale = bpy.app.translations.locale
        if locale:
            manager.set_locale(locale)
    except Exception:
        pass


def get_current_locale() -> str:
    """Get current locale from Blender or fallback to en_US."""
    try:
        import bpy
        locale = bpy.app.translations.locale
        if locale:
            return locale
    except Exception:
        pass
    return 'en_US'
