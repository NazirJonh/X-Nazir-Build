#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Check module boundaries for the Paint Layers / Stack Layers refactoring.

Run manually from the repository root (no build required):

    python3 tools/check_paint_layers_boundaries.py

Checks (phase 0.3 of the refactoring plan):
  1. Paint/material files of `blenkernel` do not include or call `ED_*`,
     `WM_*`, `UI_*`. (Upstream Blender already has a few bad-level calls in
     unrelated BKE files, so the check is scoped to the domain this
     refactoring owns.)
  2. `windowmanager/**`, `gpu/**`, `imbuf/**`, `draw/**` do not reference
     `BKE_paint_layers*`.
  3. `WM_types.hh` does not contain Outliner stack types (`ed::outliner`,
     `StackItemIdentity`, `wmDragStackLayer`).
  4. Generic Outliner stack files do not mention paint-layer domain words
     (`Material`, `BKE_paint_layers`, `"CHANNELS"`, `"MASK"`).
  5. No leftover debug printing (`[STACK_DBG]`, `[PL-DIAG]`, `PL_TIMING`,
     `PL_DEBUG_PRINTF`, `PL_HASH_CALLER`, `PAINT_LAYERS_DEBUG_LOG`, ...).

Violations that exist today are the expected backlog of the refactoring plan,
not a failure of the script. Exit code is 1 while any violation is reported.
"""

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SOURCE_ROOT = REPO_ROOT / "source" / "blender"

SCAN_SUFFIXES = {".cc", ".hh", ".h", ".c", ".cpp", ".hpp", ".inl"}

# (label, root directory relative to source/blender, regex)
CHECK_BKE_FORBIDDEN = re.compile(r"\b(?:ED|WM|UI)_[a-z]")
# blenkernel files in the scope of this refactoring (upstream already has
# bad-level calls in unrelated BKE files: compositor.cc, main_invariants.cc,
# wm_runtime.cc, screen.cc, ...).
CHECK_BKE_SCOPE = re.compile(r"(paint|material|mesh_maps)", re.IGNORECASE)
CHECK_NO_PAINT_LAYERS_ROOTS = ("windowmanager", "gpu", "imbuf", "draw")
CHECK_NO_PAINT_LAYERS = re.compile(r"BKE_paint_layers")

WM_TYPES_REL = Path("windowmanager/WM_types.hh")
WM_TYPES_FORBIDDEN = re.compile(r"ed::outliner|StackItemIdentity|wmDragStackLayer")

GENERIC_OUTLINER_FILES = (
    Path("editors/space_outliner/outliner_draw.cc"),
    Path("editors/space_outliner/outliner_select.cc"),
    Path("editors/space_outliner/outliner_dragdrop.cc"),
    Path("editors/space_outliner/outliner_stack_layers.cc"),
    Path("editors/space_outliner/tree/tree_display_stack_layers.cc"),
    Path("editors/space_outliner/outliner_stack_source.hh"),
)
GENERIC_OUTLINER_FORBIDDEN = re.compile(
    r"\bMaterial\w*|BKE_paint_layers|\"CHANNELS\"|\"MASK\""
)

DEBUG_TOKENS = re.compile(
    r"\[STACK_DBG\]|\[PL-DIAG\]|PL_TIMING|PL_DEBUG_PRINTF|PL_DEBUG_ENABLED"
    r"|PAINT_LAYERS_DEBUG_LOG|PL_HASH_CALLER|PL_HASH_TRACE_NODES"
    r"|BKE_paint_layers_debug"
)


def iter_source_files(rel_root: str):
    root = SOURCE_ROOT / rel_root
    if not root.exists():
        return
    for path in sorted(root.rglob("*")):
        if path.suffix in SCAN_SUFFIXES and path.is_file():
            yield path


def is_comment_line(line: str) -> bool:
    stripped = line.lstrip()
    return stripped.startswith(("*", "//", "/*"))


def scan(
    regex: re.Pattern,
    rel_root: str,
    label: str,
    violations: list,
    skip_comments: bool = True,
    name_filter: re.Pattern | None = None,
):
    for path in iter_source_files(rel_root):
        rel = path.relative_to(REPO_ROOT)
        if name_filter is not None and not name_filter.search(path.name):
            continue
        try:
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError as ex:
            violations.append(f"{label}: {rel}: unreadable ({ex})")
            continue
        for lineno, line in enumerate(lines, 1):
            if skip_comments and is_comment_line(line):
                continue
            if regex.search(line):
                violations.append(f"{label}: {rel}:{lineno}: {line.strip()[:160]}")


def scan_exact_files(files, regex: re.Pattern, label: str, violations: list):
    for rel in files:
        path = SOURCE_ROOT / rel
        if not path.exists():
            violations.append(f"{label}: {rel.as_posix()}: file not found")
            continue
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        for lineno, line in enumerate(lines, 1):
            if regex.search(line):
                violations.append(f"{label}: {rel}:{lineno}: {line.strip()[:160]}")


def main() -> int:
    violations: list = []

    scan(
        CHECK_BKE_FORBIDDEN,
        "blenkernel",
        "1. BKE uses editor/WM/UI",
        violations,
        name_filter=CHECK_BKE_SCOPE,
    )
    for rel_root in CHECK_NO_PAINT_LAYERS_ROOTS:
        scan(
            CHECK_NO_PAINT_LAYERS,
            rel_root,
            f"2. {rel_root} references BKE_paint_layers",
            violations,
        )
    scan_exact_files(
        (WM_TYPES_REL,), WM_TYPES_FORBIDDEN, "3. WM_types.hh has outliner stack types", violations
    )
    scan_exact_files(
        GENERIC_OUTLINER_FILES,
        GENERIC_OUTLINER_FORBIDDEN,
        "4. generic outliner file has domain words",
        violations,
    )
    scan(DEBUG_TOKENS, ".", "5. leftover debug code", violations, skip_comments=False)

    if violations:
        print(f"Paint Layers boundary violations: {len(violations)}\n")
        for line in violations:
            print(f"  {line}")
        print(
            "\nThese are the known backlog of the refactoring plan"
            " (expected red at the start)."
        )
        return 1

    print("Paint Layers boundaries: OK, no violations.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
