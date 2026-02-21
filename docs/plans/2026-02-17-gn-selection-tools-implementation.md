# GN Selection Tools Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement toolbar-based selection tools for GN Selection Mode with proper Blender ToolDef integration and user feedback.

**Architecture:** Python ToolDef definitions for Select/Box/Lasso/Circle tools, C++ tool keymaps that activate only when tool is selected, and BKE_reportf feedback for Info Editor.

**Tech Stack:** Python (ToolDef, bpy), C++ (wmKeyMap, operators), Blender tool system

---

## Task 1: Add Tool Keymaps in C++

**Files:**
- Modify: `source/blender/editors/space_view3d/view3d_gn_selection.cc` (add new function)
- Modify: `source/blender/editors/include/ED_gn_selection.hh` (add declaration)
- Modify: `source/blender/editors/space_view3d/view3d_ops.cc` (call new function)

### Step 1: Add declaration to header file

Add to `source/blender/editors/include/ED_gn_selection.hh` after `view3d_keymap_gn_selection`:

```cpp
/**
 * Register GN Selection Tool keymaps.
 * These keymaps activate only when the corresponding tool is selected.
 */
void view3d_keymap_gn_selection_tools(wmKeyConfig *keyconf);
```

### Step 2: Add keymap function to view3d_gn_selection.cc

Add to `source/blender/editors/space_view3d/view3d_gn_selection.cc` before the closing namespace:

```cpp
/* -------------------------------------------------------------------- */
/** \name Tool Keymaps
 * \{ */

void view3d_keymap_gn_selection_tools(wmKeyConfig *keyconf)
{
  GN_DEBUG_PRINT("=== view3d_keymap_gn_selection_tools START ===\n");

  /* Tool: Select */
  wmKeyMap *km_select = WM_keymap_ensure(
      keyconf, "GN Selection Tool: Select", SPACE_EMPTY, RGN_TYPE_WINDOW);
  keymap_item_add(km_select, "GN_OT_select", LEFTMOUSE, KM_PRESS, 0, KM_ANY);
  keymap_item_add(km_select, "GN_OT_select", LEFTMOUSE, KM_PRESS, KM_SHIFT, KM_ANY);
  keymap_item_add(km_select, "GN_OT_select", LEFTMOUSE, KM_PRESS, KM_CTRL, KM_ANY);

  /* Tool: Box */
  wmKeyMap *km_box = WM_keymap_ensure(
      keyconf, "GN Selection Tool: Box", SPACE_EMPTY, RGN_TYPE_WINDOW);
  keymap_item_add(km_box, "GN_OT_select_box", EVT_TWEAK_S, KM_ANY, 0, KM_ANY);

  /* Tool: Lasso */
  wmKeyMap *km_lasso = WM_keymap_ensure(
      keyconf, "GN Selection Tool: Lasso", SPACE_EMPTY, RGN_TYPE_WINDOW);
  keymap_item_add(km_lasso, "GN_OT_select_lasso", EVT_TWEAK_S, KM_ANY, 0, KM_ANY);

  /* Tool: Circle */
  wmKeyMap *km_circle = WM_keymap_ensure(
      keyconf, "GN Selection Tool: Circle", SPACE_EMPTY, RGN_TYPE_WINDOW);
  keymap_item_add(km_circle, "GN_OT_select_circle", LEFTMOUSE, KM_PRESS, 0, KM_ANY);
  keymap_item_add(km_circle, "GN_OT_select_circle", LEFTMOUSE, KM_PRESS, KM_SHIFT, KM_ANY);
  keymap_item_add(km_circle, "GN_OT_select_circle", LEFTMOUSE, KM_PRESS, KM_CTRL, KM_ANY);

  GN_DEBUG_PRINT("=== view3d_keymap_gn_selection_tools END ===\n");
}

/** \} */
```

### Step 3: Call the new keymap function

Modify `source/blender/editors/space_view3d/view3d_ops.cc` to add the call:

```cpp
  /* GN Selection Mode keymap */
  view3d_keymap_gn_selection(keyconf);

  /* GN Selection Tool keymaps */
  view3d_keymap_gn_selection_tools(keyconf);
```

### Step 4: Build and verify keymaps are created

Run: `cmake --build build --target blender`
Expected: Build succeeds

### Step 5: Commit

```bash
git add source/blender/editors/space_view3d/view3d_gn_selection.cc
git add source/blender/editors/include/ED_gn_selection.hh
git add source/blender/editors/space_view3d/view3d_ops.cc
git commit -m "feat(gn-selection): add tool keymaps for Select/Box/Lasso/Circle

Tool keymaps activate only when the corresponding tool is selected,
unlike the mode keymap which requires poll function."
```

---

## Task 2: Add Feedback to Operators

**Files:**
- Modify: `source/blender/editors/space_view3d/view3d_gn_selection.cc`

### Step 1: Add helper function for domain name

Add after `gn_selection_apply` function in `view3d_gn_selection.cc`:

```cpp
/**
 * Get human-readable domain name for feedback messages.
 */
static const char *gn_selection_domain_name(bke::AttrDomain domain)
{
  switch (domain) {
    case bke::AttrDomain::Point:
      return "vertex";
    case bke::AttrDomain::Edge:
      return "edge";
    case bke::AttrDomain::Face:
      return "face";
    default:
      return "element";
  }
}
```

### Step 2: Add feedback to gn_select_exec

In `gn_select_exec`, after the successful selection block (around line 417), add:

```cpp
    if (gn_selection_apply(data->current_selection, hit_index, sel_op, true)) {
      GN_DEBUG_PRINT("Selection changed! New size: %zu\n", data->current_selection.size());
      data->selection_changed = true;

      /* Feedback: Report to Info Editor */
      BKE_reportf(op->reports,
                  RPT_INFO,
                  "Selected %s %d (total: %zu)",
                  gn_selection_domain_name(data->domain),
                  hit_index,
                  data->current_selection.size());
    }
```

### Step 3: Add feedback to gn_select_box_exec

In `gn_select_box_exec`, after `if (userdata.changed)` block (around line 618), add:

```cpp
  if (userdata.changed) {
    data->selection_changed = true;
    GN_DEBUG_PRINT("Selection size now: %zu\n", data->current_selection.size());

    /* Feedback: Report to Info Editor */
    BKE_reportf(op->reports,
                RPT_INFO,
                "Box selected %zu %ss (total: %zu)",
                data->current_selection.size(),
                gn_selection_domain_name(data->domain),
                data->current_selection.size());
  }
```

### Step 4: Add feedback to gn_select_lasso_exec

In `gn_select_lasso_exec`, after `if (userdata.changed)` block (around line 829), add:

```cpp
  if (userdata.changed) {
    data->selection_changed = true;
    GN_DEBUG_PRINT("Selection size now: %zu\n", data->current_selection.size());

    /* Feedback: Report to Info Editor */
    BKE_reportf(op->reports,
                RPT_INFO,
                "Lasso selected %zu %ss (total: %zu)",
                data->current_selection.size(),
                gn_selection_domain_name(data->domain),
                data->current_selection.size());
  }
```

### Step 5: Add feedback to gn_select_circle_exec

In `gn_select_circle_exec`, after `if (userdata.changed)` block (around line 1025), add:

```cpp
  if (userdata.changed) {
    data->selection_changed = true;
    GN_DEBUG_PRINT("Selection size now: %zu\n", data->current_selection.size());

    /* Feedback: Report to Info Editor */
    BKE_reportf(op->reports,
                RPT_INFO,
                "Circle selected %zu %ss (total: %zu)",
                data->current_selection.size(),
                gn_selection_domain_name(data->domain),
                data->current_selection.size());
  }
```

### Step 6: Build and verify

Run: `cmake --build build --target blender`
Expected: Build succeeds

### Step 7: Commit

```bash
git add source/blender/editors/space_view3d/view3d_gn_selection.cc
git commit -m "feat(gn-selection): add BKE_reportf feedback to selection operators

Reports selection results to Info Editor for user feedback."
```

---

## Task 3: Complete Python Tool Definitions

**Files:**
- Modify: `scripts/startup/bl_tools/gn_selection_tools.py`

### Step 1: Rewrite the entire file with complete tool definitions

Replace entire content of `scripts/startup/bl_tools/gn_selection_tools.py`:

```python
# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy
from bpy.types import ToolDef


class _defs_gn_selection:
    """GN Selection Mode tool definitions."""

    @staticmethod
    def _domain_icon(domain):
        """Get icon for selection domain."""
        icons = {
            0: 'VERTEXSEL',  # Point
            1: 'EDGESEL',    # Edge
            2: 'FACESEL',    # Face
        }
        return icons.get(domain, 'FACESEL')

    @staticmethod
    def draw_settings(context, layout, tool):
        """Draw tool settings in the toolbar."""
        obj = context.active_object
        if not obj:
            return

        # Get domain from operator properties if available
        props = tool.operator_properties("GN_OT_select_mode")
        if props:
            row = layout.row(align=True)
            row.prop_enum(props, "type", value=0, text="", icon='VERTEXSEL')
            row.prop_enum(props, "type", value=1, text="", icon='EDGESEL')
            row.prop_enum(props, "type", value=2, text="", icon='FACESEL')

    @ToolDef.from_fn
    def select():
        return dict(
            idname="gn.select",
            label="Select",
            description="Select elements in GN Selection Mode",
            icon="ops.generic.select",
            cursor='CROSSHAIR',
            keymap="GN Selection Tool: Select",
            draw_settings=draw_settings.__func__,
        )

    @ToolDef.from_fn
    def box():
        def draw_settings_box(_context, layout, tool):
            props = tool.operator_properties("GN_OT_select_box")
            row = layout.row()
            row.use_property_split = False
            row.prop(props, "mode", text="", expand=True, icon_only=True)

        return dict(
            idname="gn.select_box",
            label="Select Box",
            description="Box select elements in GN Selection Mode",
            icon="ops.generic.select_box",
            cursor='CROSSHAIR',
            keymap="GN Selection Tool: Box",
            draw_settings=draw_settings_box,
        )

    @ToolDef.from_fn
    def lasso():
        def draw_settings_lasso(_context, layout, tool):
            props = tool.operator_properties("GN_OT_select_lasso")
            row = layout.row()
            row.use_property_split = False
            row.prop(props, "mode", text="", expand=True, icon_only=True)

        return dict(
            idname="gn.select_lasso",
            label="Select Lasso",
            description="Lasso select elements in GN Selection Mode",
            icon="ops.generic.select_lasso",
            cursor='CROSSHAIR',
            keymap="GN Selection Tool: Lasso",
            draw_settings=draw_settings_lasso,
        )

    @ToolDef.from_fn
    def circle():
        def draw_settings_circle(_context, layout, tool):
            props = tool.operator_properties("GN_OT_select_circle")
            row = layout.row()
            row.use_property_split = False
            row.prop(props, "mode", text="", expand=True, icon_only=True)
            layout.prop(props, "radius")

        def draw_cursor(_context, tool, xy):
            from gpu_extras.presets import draw_circle_2d
            props = tool.operator_properties("GN_OT_select_circle")
            radius = props.radius
            draw_circle_2d(xy, (1.0,) * 4, radius, segments=32)

        return dict(
            idname="gn.select_circle",
            label="Select Circle",
            description="Circle select elements in GN Selection Mode",
            icon="ops.generic.select_circle",
            cursor='CROSSHAIR',
            keymap="GN Selection Tool: Circle",
            draw_settings=draw_settings_circle,
            draw_cursor=draw_cursor,
        )


def register():
    """Register GN Selection tools."""
    pass


def unregister():
    """Unregister GN Selection tools."""
    pass


if __name__ == "__main__":
    register()
```

### Step 2: Verify Python syntax

Run: `python -m py_compile scripts/startup/bl_tools/gn_selection_tools.py`
Expected: No errors

### Step 3: Commit

```bash
git add scripts/startup/bl_tools/gn_selection_tools.py
git commit -m "feat(gn-selection): complete Python tool definitions

Add full ToolDef definitions for Select, Box, Lasso, Circle tools
with settings drawing and cursor support."
```

---

## Task 4: Register Tools in Toolbar

**Files:**
- Modify: `scripts/startup/bl_ui/space_toolsystem_toolbar.py`

### Step 1: Import GN selection tools

Add at the top of `space_toolsystem_toolbar.py` with other imports (around line 20):

```python
from bl_tools.gn_selection_tools import _defs_gn_selection
```

### Step 2: Add tool list for GN Selection Mode

Find the line with `_tools_select = (` around line 3672 and add after the `_tools_transform` definition:

```python
    _tools_gn_selection = (
        (
            _defs_gn_selection.select,
            _defs_gn_selection.box,
            _defs_gn_selection.circle,
            _defs_gn_selection.lasso,
        ),
    )
```

### Step 3: Add tools to Object Mode with conditional visibility

Find the `'OBJECT'` entry in the `_tools` dictionary (around line 3680). The tools need to appear when GN Selection Mode is active. Since Blender's tool system doesn't have built-in conditional visibility per mode, we need to modify `tools_from_context`.

**Alternative approach**: Add a helper function that returns tools based on mode.

Find the class definition for `VIEW3D_PT_tools_active` (around line 3655) and locate the `tools_from_context` method. Add GN Selection tools handling:

Look for this pattern in the file:
```python
def tools_from_context(cls, context, mode=None):
```

Add this logic to return GN Selection tools when in GN Selection Mode:

```python
        # GN Selection Mode tools
        if context.mode == 'OBJECT':
            ob = context.active_object
            if ob and (ob.mode & OB_MODE_GN_SELECTION):
                return (cls._tools_gn_selection[0],)
```

**Note**: This requires `OB_MODE_GN_SELECTION` to be accessible from Python. If not available, we need to use `ED_gn_selection_mode_active` through a helper.

### Step 4: Alternative - Use mode property check

If `OB_MODE_GN_SELECTION` isn't directly accessible from Python, add a helper function to check mode. First verify what's available:

```python
from bpy.types import Object
# Check if OB_MODE_GN_SELECTION is available
```

Since the mode flag might not be exposed to Python, we'll need to add a context property or use the existing `ED_gn_selection_mode_active` function exposed via RNA.

For now, let's use a simpler approach - check if we're in the mode by looking at the active tool:

Actually, the cleanest approach is to register the tools in a separate mode entry. Let's check if we can add a new mode:

Looking at the `_tools` dictionary structure, modes are strings like 'OBJECT', 'EDIT_MESH', etc. We can add a custom mode or use the existing structure.

**Simpler Solution**: Just add the tools to the OBJECT mode tool list, but they'll only work when the operator poll returns true.

Add to `_tools` dictionary in the `'OBJECT'` section:

```python
        'OBJECT': [
            *_tools_select,
            None,
            _defs_view3d_add.cube_add,
            # ... existing tools ...
            None,
            *_tools_gn_selection,  # Add this line
        ],
```

Wait - this will always show the tools. Let me reconsider.

**Best approach**: The tools should be registered but only appear when the mode is active. In Blender, this is typically done through the tool's poll function or by returning different tools from `tools_from_context`.

Let me check how `tools_from_context` works more carefully...

The key insight is that `tools_from_context` can return different tool lists based on context. We need to modify it to return GN Selection tools when in GN Selection Mode.

### Step 4 (Revised): Modify tools_from_context

Find the `tools_from_context` method in `VIEW3D_PT_tools_active` class. Add before the existing mode handling:

```python
    @classmethod
    def tools_from_context(cls, context, mode=None):
        # GN Selection Mode - show only GN selection tools
        if mode is None:
            mode = context.mode
        if mode == 'OBJECT':
            ob = context.active_object
            # Check for GN Selection Mode
            # We need to check if gn_selection_mode_data is active
            # This requires a Python-accessible property or function
            try:
                if ob and hasattr(bpy.data, 'gn_selection_active') and bpy.data.gn_selection_active:
                    return list(cls._tools_gn_selection[0])
            except:
                pass
```

Actually, this is getting complicated. Let's use a different approach - store the tool references and add them conditionally.

### Step 5: Simplest working solution

For now, add the tools to the OBJECT mode. They will appear in the toolbar but will only work when GN Selection Mode is active (because the operator poll checks this).

Add after `_tools_gn_selection` definition:

```python
    # Add GN Selection tools to the OBJECT mode toolbar
    # They will only function when GN Selection Mode is active
```

Then modify the `'OBJECT'` entry in `_tools` to include them after the annotation tools:

```python
        'OBJECT': [
            *_tools_select,
            None,
            _defs_view3d_add.cube_add,
            _defs_view3d_add.cone_add,
            _defs_view3d_add.cylinder_add,
            _defs_view3d_add.uv_sphere_add,
            _defs_view3d_add.ico_sphere_add,
            None,
            _defs_view3d_add.empty_add(),
            _defs_view3d_add.image_empty_add(),
            _defs_view3d_add.light_add(),
            _defs_view3d_add.collection_instance_add(),
            _defs_view3d_add.data_instance_add(),
            None,
            _defs_view3d_generic.ruler,
            None,
            *_tools_annotate,
            *_tools_gn_selection,  # Add GN Selection tools here
        ],
```

### Step 6: Commit

```bash
git add scripts/startup/bl_ui/space_toolsystem_toolbar.py
git commit -m "feat(gn-selection): register tools in 3D View toolbar

GN Selection tools now appear in Object Mode toolbar.
They function only when GN Selection Mode is active."
```

---

## Task 5: Manual Testing

### Step 1: Build Blender

Run: `cmake --build build --target blender`
Expected: Build succeeds

### Step 2: Start Blender

Run: `./build/bin/blender.exe` (or your build output path)

### Step 3: Test GN Selection Mode entry

1. Create a mesh object (Cube)
2. Add Geometry Nodes modifier
3. Add a "3D View Selection" node to the node tree
4. From the node editor, click "Enter Selection Mode" button

Expected: Object enters GN Selection Mode (console shows debug output)

### Step 4: Verify tools appear in toolbar

1. Look at the 3D View toolbar on the left
2. Find the GN Selection tools (Select, Select Box, Select Lasso, Select Circle)

Expected: Tools are visible in the toolbar

### Step 5: Test Select tool

1. Click on the "Select" tool in the toolbar
2. Click on a face of the mesh

Expected:
- Console shows "[GN Selection] === gn_select_exec (single click) START ==="
- Info Editor shows "Selected face X (total: 1)"

### Step 6: Test Box Select tool

1. Click on "Select Box" tool
2. Drag a box over the mesh

Expected:
- Box selection is performed
- Info Editor shows "Box selected X faces (total: Y)"

### Step 7: Test confirm/cancel

1. Press Enter to confirm selection
Expected: Mode exits, selection saved to node

2. Re-enter mode and press Esc
Expected: Mode exits, selection discarded

---

## Task 6: Fix Any Issues Found During Testing

If tools don't appear or don't work, debug:

1. **Tools not appearing**: Check import path, check tool registration
2. **Clicks not working**: Check keymap name matches tool's `keymap` property exactly
3. **Reports not showing**: Ensure Info Editor is visible

---

## Summary

After completing all tasks:

1. C++ tool keymaps registered (Task 1)
2. Operator feedback via BKE_reportf (Task 2)
3. Python tool definitions complete (Task 3)
4. Tools registered in toolbar (Task 4)
5. Manual testing passed (Task 5)

The GN Selection tools should now:
- Appear in the 3D View toolbar
- Work when clicked (tool keymaps route events to operators)
- Provide feedback in Info Editor
- Work with Shift (extend) and Ctrl (deselect) modifiers
