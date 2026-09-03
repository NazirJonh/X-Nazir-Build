# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

r"""Runner for the Stack Layers Outliner suite (`tests/python/bl_outliner_stack_layers.py`).

The suite needs a genuinely drawn Outliner -- the tree is only built by a real draw, and
`bpy.context.screen` is None in a script or timer context -- so it cannot run under the plain
background `--python` ctest harness (`add_blender_test` registers windowless runs; the UI-test
registration, `add_blender_test_ui`, needs WITH_UI_TESTS and a running X server / weston, which
this suite does not use either). Run it against a built binary instead; a window opens for a
couple of minutes and the full unittest report lands in a file:

    <binary> --factory-startup --no-native-pixels --no-window-frame -p 0 0 800 600 \
        --python tests/utils/bl_stack_layers_test_runner.py \
        -- --report <report.txt>

With `--` and no `--report`, the report goes next to the binary as
`stack_layers_tests_report.txt`. The exit status reflects the suite result only after the report
is written; Blender quits itself.
"""

import os
import sys
import unittest

import bpy

DEFAULT_REPORT_NAME = "stack_layers_tests_report.txt"


def _arguments():
    argv = sys.argv
    if "--" in argv:
        extra = argv[argv.index("--") + 1:]
    else:
        extra = []
    report = DEFAULT_REPORT_NAME
    if "--report" in extra:
        report = extra[extra.index("--report") + 1]
    return report


def _poll_and_run():
    wm = bpy.context.window_manager
    win = wm.windows[0] if (wm is not None and len(wm.windows)) else None
    if win is None or win.screen is None:
        return 0.5  # window not up yet; poll again

    report_path = _arguments()
    with open(report_path, "w", encoding="utf-8") as stream:
        suite_dir = os.path.normpath(
            os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "python"))
        sys.path.insert(0, suite_dir)
        import bl_outliner_stack_layers as mod

        suite = unittest.defaultTestLoader.loadTestsFromModule(mod)
        runner = unittest.TextTestRunner(verbosity=2, stream=stream)
        result = runner.run(suite)
        stream.write("\nSTACK_LAYERS_TEST_RESULT: %s\n" %
                     ("OK" if result.wasSuccessful() else "FAILED"))
    bpy.ops.wm.quit_blender()
    return None


bpy.app.timers.register(_poll_and_run, first_interval=0.5)
