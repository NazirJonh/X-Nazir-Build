# GN Selection Tools Design

**Date**: 2026-02-17
**Status**: Approved
**Approach**: Toolbar-based tools with proper Blender ToolDef integration

## Problem Statement

Current keymap-based approach ("GN Selection Mode" keymap) is not working - events don't reach operators despite poll returning true. Users have no visual indication of selection tools and no feedback when selecting elements.

## Solution Overview

Implement proper Blender toolbar tools using the ToolDef system. Each selection method (Select, Box, Lasso, Circle) becomes a separate tool with its own keymap that activates only when the tool is selected.

## Architecture

```
+-------------------------------------------------------------+
|                    3D View Toolbar                          |
|  +---------+ +---------+ +---------+ +---------+           |
|  | GN Select| | GN Box | | GN Lasso| | GN Circle|          |
|  +----+----+ +----+----+ +----+----+ +----+----+           |
|       |           |           |           |                 |
|       v           v           v           v                 |
|  +-----------------------------------------------------+   |
|  |              Tool Keymaps (active only when          |   |
|  |              tool is selected)                       |   |
|  +-----------------------------------------------------+   |
|                          |                                  |
|                          v                                  |
|  +-----------------------------------------------------+   |
|  |              C++ Operators                           |   |
|  |  GN_OT_select, GN_OT_select_box,                     |   |
|  |  GN_OT_select_lasso, GN_OT_select_circle             |   |
|  +-----------------------------------------------------+   |
|                          |                                  |
|                          v                                  |
|  +-----------------------------------------------------+   |
|  |              GNSelectionModeData                      |   |
|  |  (runtime data: selection set, domain, node ref)    |   |
|  +-----------------------------------------------------+   |
+-------------------------------------------------------------+
```

## Components

### 1. Python Tool Definitions

**File**: `scripts/startup/bl_tools/gn_selection_tools.py`

```python
from bpy.types import ToolDef

class _defs_gn_selection:
    @ToolDef.from_fn
    def select():
        return dict(
            idname="gn.select",
            label="Select",
            description="Select elements in GN Selection Mode",
            icon="ops.generic.select",
            cursor='CROSSHAIR',
            keymap="GN Selection Tool: Select",
        )

    @ToolDef.from_fn
    def box():
        return dict(
            idname="gn.select_box",
            label="Select Box",
            icon="ops.generic.select_box",
            cursor='CROSSHAIR',
            keymap="GN Selection Tool: Box",
        )

    @ToolDef.from_fn
    def lasso():
        return dict(
            idname="gn.select_lasso",
            label="Select Lasso",
            icon="ops.generic.select_lasso",
            cursor='CROSSHAIR',
            keymap="GN Selection Tool: Lasso",
        )

    @ToolDef.from_fn
    def circle():
        return dict(
            idname="gn.select_circle",
            label="Select Circle",
            icon="ops.generic.select_circle",
            cursor='CROSSHAIR',
            keymap="GN Selection Tool: Circle",
        )
```

### 2. Tool Registration

**File**: `scripts/startup/bl_ui/space_toolsystem_toolbar.py`

Tools should appear dynamically only when GN Selection Mode is active. This is achieved through:
- A separate tool group with poll function
- Or filtering in `tools_from_context`

### 3. C++ Tool Keymaps

**File**: `source/blender/editors/space_view3d/view3d_gn_selection.cc`

```cpp
void view3d_keymap_gn_selection_tools(wmKeyConfig *keyconf)
{
    // Tool: Select
    wmKeyMap *km_select = WM_keymap_ensure(
        keyconf, "GN Selection Tool: Select", SPACE_EMPTY, RGN_TYPE_WINDOW);
    // NO poll - keymap active only when tool is selected
    keymap_item_add(km_select, "GN_OT_select", LEFTMOUSE, KM_PRESS, 0, KM_ANY);
    keymap_item_add(km_select, "GN_OT_select", LEFTMOUSE, KM_PRESS, KM_SHIFT, KM_ANY);
    keymap_item_add(km_select, "GN_OT_select", LEFTMOUSE, KM_PRESS, KM_CTRL, KM_ANY);

    // Tool: Box
    wmKeyMap *km_box = WM_keymap_ensure(
        keyconf, "GN Selection Tool: Box", SPACE_EMPTY, RGN_TYPE_WINDOW);
    keymap_item_add(km_box, "GN_OT_select_box", EVT_TWEAK_S, KM_ANY, 0, KM_ANY);

    // Tool: Lasso
    wmKeyMap *km_lasso = WM_keymap_ensure(
        keyconf, "GN Selection Tool: Lasso", SPACE_EMPTY, RGN_TYPE_WINDOW);
    keymap_item_add(km_lasso, "GN_OT_select_lasso", EVT_TWEAK_S, KM_ANY, KM_CTRL, KM_ANY);

    // Tool: Circle
    wmKeyMap *km_circle = WM_keymap_ensure(
        keyconf, "GN Selection Tool: Circle", SPACE_EMPTY, RGN_TYPE_WINDOW);
    keymap_item_add(km_circle, "GN_OT_select_circle", LEFTMOUSE, KM_PRESS, 0, KM_ANY);
}
```

### 4. Operator Feedback

Add to existing operators in `view3d_gn_selection.cc`:

```cpp
// In gn_select_exec after successful hit:
if (hit && hit_object == ob && hit_index >= 0) {
    // ... existing selection logic ...

    // Feedback: Report (Info Editor)
    const char *domain_name = "";
    switch (data->domain) {
        case bke::AttrDomain::Point: domain_name = "vertex"; break;
        case bke::AttrDomain::Edge:  domain_name = "edge"; break;
        case bke::AttrDomain::Face:  domain_name = "face"; break;
        default: domain_name = "element"; break;
    }

    BKE_reportf(op->reports, RPT_INFO,
                "Selected %s %d (total: %zu)",
                domain_name, hit_index, data->current_selection.size());
}
```

Similar feedback should be added to box, lasso, and circle operators.

## Data Flow

### Mode Entry

```
1. User clicks "Enter GN Selection Mode" in Node Editor
   |
   v
2. OBJECT_OT_gn_selection_mode_set operator
   |
   v
3. ED_gn_selection_mode_enter()
   - Finds 3D View Selection node
   - Creates GNSelectionModeData
   - Sets ob->mode |= OB_MODE_GN_SELECTION
   |
   v
4. Toolbar updates (GN Selection tools become visible)
```

### Element Selection

```
1. User selects GN Select tool in toolbar
   |
   v
2. Tool keymap "GN Selection Tool: Select" activates
   |
   v
3. User clicks on mesh
   |
   v
4. GN_OT_select operator:
   a. invoke() - gets mouse coordinates from event
   b. exec() - performs raycast via snap_object_project_ray_ex
   c. On hit:
      - Updates GNSelectionModeData::current_selection
      - Sends BKE_reportf() to Info Editor
      - GN_DEBUG_PRINT() to console
   |
   v
5. WM_event_add_notifier(NC_GEOM | ND_SELECT)
```

### Mode Exit

```
1. User presses Enter/Esc
   |
   v
2. GN_OT_selection_confirm or GN_OT_selection_cancel
   |
   v
3. ED_gn_selection_mode_exit(confirm)
   - If confirm: saves selection to NodeGeometry3DViewSelection
   - Frees GNSelectionModeData
   - Restores previous_mode
   |
   v
4. WM_event_add_notifier(NC_NODE | NA_EDITED)
   - Geometry Nodes re-evaluates with new selection
```

## File Changes

| File | Changes |
|------|---------|
| `scripts/startup/bl_tools/gn_selection_tools.py` | Complete tool definitions for Select, Box, Lasso, Circle |
| `scripts/startup/bl_ui/space_toolsystem_toolbar.py` | Integrate GN Selection tools with conditional visibility |
| `source/blender/editors/space_view3d/view3d_gn_selection.cc` | 1) Add `view3d_keymap_gn_selection_tools()`<br>2) Add BKE_reportf() feedback to operators |
| `source/blender/editors/space_view3d/view3d_ops.cc` | Call `view3d_keymap_gn_selection_tools(keyconf)` |
| `source/blender/editors/include/ED_gn_selection.hh` | Declare `view3d_keymap_gn_selection_tools()` |

## Testing

| # | Test | Expected Result |
|---|------|-----------------|
| 1 | Enter GN Selection Mode | Toolbar shows GN Select tools |
| 2 | Select GN Select tool | Tool active, crosshair cursor |
| 3 | Click on face | Face added to selection, Info Editor shows "Selected face X" |
| 4 | Shift+click (extend) | Adds to existing selection |
| 5 | Ctrl+click (deselect) | Removes from selection |
| 6 | Box select | All elements in box added |
| 7 | Lasso select | Elements inside lasso added |
| 8 | Circle select (drag) | Elements under brush added/removed |
| 9 | Press Enter | Selection saved to node, mode exits |
| 10 | Press Esc | Selection cancelled, mode exits |
| 11 | Exit mode | GN Select tools disappear from toolbar |

## Debug Checklist

1. **Tools don't appear in toolbar**
   - Check registration in `space_toolsystem_toolbar.py`
   - Check poll/visibility logic

2. **Clicks don't work**
   - Verify tool keymap is registered
   - Verify tool name matches keymap name
   - Check console for debug output

3. **Reports don't appear**
   - Ensure Info Editor is open
   - Check `op->reports` is not null

## Rationale

This approach follows the standard Blender pattern for tools, ensuring:
- Reliable keymap handling (tool keymaps activate only when tool selected)
- Familiar UI for users
- Extensibility (easy to add hotkeys, settings)
- Proper integration with Blender's tool system
