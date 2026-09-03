# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Atomic / safe JSON persistence helpers for the Category Tabs / Glyph / Tag system.

Extracted verbatim from ``space_userpref.py`` (no behavioural change). These helpers are
stateless: they depend only on the debug flag :data:`.defaults.TAG_BACKUP_ENABLED`, the
deduplicating logger :mod:`.log`, and the standard library, so this module is bpy-free and
can be imported standalone.

The names are re-imported back into ``space_userpref`` to preserve its attribute contract.
"""

import json
import os
import shutil
import time
from contextlib import contextmanager
from datetime import datetime

from .defaults import TAG_BACKUP_ENABLED
from .log import save_debug_print, tag_log


@contextmanager
def safe_file_write(filepath):
    """Atomic file write with rollback on error and retry logic for Windows.

    Uses unique temporary filename with timestamp and PID to avoid race conditions
    when multiple threads or rapid successive calls attempt to write the same file.
    """
    import time

    # Create unique temp file in the same directory to ensure atomic rename works
    dir_name = os.path.dirname(filepath)
    base_name = os.path.basename(filepath)

    # Generate unique temp filename: {base_name}.tmp.{pid}.{timestamp}
    timestamp = int(time.time() * 1000000)  # Microsecond precision
    temp_name = f"{base_name}.tmp.{os.getpid()}.{timestamp}"
    temp_path = os.path.join(dir_name, temp_name) if dir_name else temp_name

    # CRITICAL DEBUG: Always log file write attempts (even if TAG_DEBUG=False)
    save_debug_print(f"[SAFE_FILE_WRITE] >>>>> START: filepath={filepath!r}, temp_path={temp_path!r}, dir_name={dir_name!r}")

    max_retries = 3
    retry_delay = 0.1  # 100ms

    try:
        save_debug_print(f"[SAFE_FILE_WRITE] Opening temp file for writing: {temp_path!r}")
        with open(temp_path, 'w', encoding='utf-8') as f:
            yield f
        save_debug_print(f"[SAFE_FILE_WRITE] Temp file written successfully, attempting rename...")

        # Atomic rename with retry on Windows (file may be locked or temp file missing)
        for attempt in range(max_retries):
            try:
                save_debug_print(f"[SAFE_FILE_WRITE] Attempt {attempt + 1}/{max_retries}: os.replace({temp_path!r}, {filepath!r})")
                os.replace(temp_path, filepath)
                save_debug_print(f"[SAFE_FILE_WRITE] <<<<< SUCCESS: Saved {filepath!r}")
                tag_log(f"Saved: {filepath}")
                return
            except (PermissionError, FileNotFoundError) as e:
                save_debug_print(f"[SAFE_FILE_WRITE] Attempt {attempt + 1} failed: {type(e).__name__}: {e}")
                if attempt < max_retries - 1:
                    tag_log(f"Retry {attempt + 1}/{max_retries} for {filepath}: {e}")
                    time.sleep(retry_delay)
                    retry_delay *= 2  # Exponential backoff
                else:
                    save_debug_print(f"[SAFE_FILE_WRITE] <<<<< FAILED after {max_retries} retries: {e}")
                    tag_log(f"Failed to save after {max_retries} retries: {e}", "ERROR")
                    raise e
    except Exception as e:
        # Remove temp file on error
        save_debug_print(f"[SAFE_FILE_WRITE] <<<<< EXCEPTION: {type(e).__name__}: {e}")
        if os.path.exists(temp_path):
            try:
                save_debug_print(f"[SAFE_FILE_WRITE] Cleaning up temp file: {temp_path!r}")
                os.remove(temp_path)
            except Exception as cleanup_error:
                save_debug_print(f"[SAFE_FILE_WRITE] Cleanup failed: {cleanup_error}")
        tag_log(f"Failed to save {filepath}: {e}", "ERROR")
        raise e


def load_json_safely(filepath, default_structure):
    """Load JSON with fallback to defaults on corruption."""
    if not os.path.exists(filepath):
        tag_log(f"File not found: {filepath}, creating defaults")
        return default_structure

    try:
        with open(filepath, 'r', encoding='utf-8') as f:
            data = json.load(f)
        tag_log(f"Loaded: {filepath}")
        return data
    except json.JSONDecodeError as e:
        tag_log(f"JSON corrupted: {e}", "ERROR")
        # Rename corrupted file for recovery
        os.rename(filepath, f"{filepath}.corrupted_{int(time.time())}")
        return default_structure
    except Exception as e:
        tag_log(f"Load error: {e}", "ERROR")
        return default_structure


def create_backup(filepath):
    """Create timestamped backup before overwriting."""
    if not TAG_BACKUP_ENABLED:
        return None
    if not os.path.exists(filepath):
        return None

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    backup_path = f"{filepath}.backup_{timestamp}"

    # Keep only last 5 backups
    backup_dir = os.path.dirname(filepath)
    basename = os.path.basename(filepath)
    existing_backups = sorted([
        f for f in os.listdir(backup_dir)
        if f.startswith(basename) and ".backup_" in f
    ])

    while len(existing_backups) >= 5:
        old_backup = os.path.join(backup_dir, existing_backups.pop(0))
        os.remove(old_backup)
        tag_log(f"Removed old backup: {old_backup}")

    shutil.copy(filepath, backup_path)
    tag_log(f"Created backup: {backup_path}")
    return backup_path
