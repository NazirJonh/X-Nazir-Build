# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Debug / deduplicating log helpers for the Category Tabs / Glyph / Tag system.

Extracted verbatim from ``space_userpref.py`` (no behavioural change). Depends only on
the debug flags in :mod:`.defaults` and the standard library (no ``bpy``), so it can be
imported standalone. The per-session dedup caches live here and are not accessed outside
these functions; the names are re-imported into ``space_userpref`` to preserve its
module-attribute contract.
"""

from .defaults import (
    SAVE_DEBUG,
    TAG_DEBUG,
)


def category_debug_print(message):
    """Print debug message only when TAG_DEBUG is enabled."""
    if TAG_DEBUG:
        print(message)


def save_debug_print(message):
    """Print verbose save/load debug message only when SAVE_DEBUG is enabled."""
    if SAVE_DEBUG:
        print(message)


# Log deduplication for repetitive debug messages
_pref_logged_messages = set()
_tag_logged_messages = set()
_package_match_logged = set()
_sync_miss_logged = set()

def _pref_log_once(message):
    """Print a log message only once per session to avoid log flooding."""
    if not TAG_DEBUG:
        return
    msg_hash = hash(message)
    if msg_hash not in _pref_logged_messages:
        if len(_pref_logged_messages) > 500:
            _pref_logged_messages.clear()
        _pref_logged_messages.add(msg_hash)
        print(message)


def _package_match_log_once(category, repo_name, pkg_name):
    """Print package match message only once per category+repo combination."""
    if not TAG_DEBUG:
        return
    key = (category, repo_name)
    if key not in _package_match_logged:
        if len(_package_match_logged) > 200:
            _package_match_logged.clear()
        _package_match_logged.add(key)
        category_debug_print(f"[] package match: category={category!r}, repo={repo_name!r}, pkg={pkg_name!r}")


def _sync_miss_log_once(category):
    """Print sync miss message only once per category."""
    if not TAG_DEBUG:
        return
    if category not in _sync_miss_logged:
        if len(_sync_miss_logged) > 200:
            _sync_miss_logged.clear()
        _sync_miss_logged.add(category)
        category_debug_print(f"[] sync miss: category={category!r}")



def tag_log(message, level="INFO", *, dedup=True):
    """Logging for tag system operations with deduplication to avoid log spam."""
    if not (TAG_DEBUG or level == "ERROR"):
        return

    if dedup and level != "ERROR":
        msg_hash = hash((level, message))
        if msg_hash in _tag_logged_messages:
            return
        if len(_tag_logged_messages) > 1000:
            _tag_logged_messages.clear()
        _tag_logged_messages.add(msg_hash)

    category_debug_print(f"[TAGS][{level}] {message}")


# Log deduplication: track already-logged messages to avoid flooding
_logged_messages_cache = set()
_MAX_LOG_CACHE_SIZE = 1000

def _log_once(message):
    """Print a log message only once per session to avoid log flooding."""
    if not TAG_DEBUG:
        return
    global _logged_messages_cache
    # Use hash of message to save memory
    msg_hash = hash(message)
    if msg_hash not in _logged_messages_cache:
        if len(_logged_messages_cache) > _MAX_LOG_CACHE_SIZE:
            _logged_messages_cache.clear()
        _logged_messages_cache.add(msg_hash)
        print(message)
