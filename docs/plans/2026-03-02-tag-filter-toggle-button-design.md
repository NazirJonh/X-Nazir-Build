# Tag Filter Toggle Button Design

**Date:** 2026-03-02
**Status:** Design Approved
**Related:** Horizontal Tag Bar, Category Filtering

## Overview

Add a toggle button to the tag bar that enables/disables tag-based category filtering. When enabled, selected tags filter categories. When disabled, all categories are shown regardless of tag selection.

## Requirements

### Functional
- Toggle button to enable/disable tag filtering
- Button position: at the end of the tag bar (right side, after all tags)
- Default state: ENABLED (filtering active)
- Icon: `ICON_FILTER` (Blender standard icon #174)
- When DISABLED: all categories shown, selected tags preserved
- When ENABLED: selected tags filter categories (AND logic)

### Technical
- State stored in `active_tag_filter_tags` (empty = disabled, non-empty = enabled)
- Temporarily save selected tags when disabling filter for restoration on re-enable
- Button renders in `tag_bar_draw_in_layout()` function

## Architecture

### Data Structures

**TagBarRuntimeData** (interface_tag_bar.hh):
```cpp
struct TagBarRuntimeData {
  blender::Vector<TagButton> buttons;
  int total_width = 0;
  int visible_buttons = 0;
  int scroll_offset = 0;
  int max_scroll = 0;
  bool needs_update = true;

  // NEW: Temporary storage for tags when filter is disabled
  char saved_tags[256] = "";
};
```

### UI Components

**Filter Toggle Button:**
- Type: `ButToggle`
- Icon: `ICON_FILTER`
- Position: End of tag bar (after all tag buttons)
- Callback: `tag_filter_toggle_click()`
- Active state: `v3d->active_tag_filter_tags[0] != '\0'`

### Behavior Flow

#### Filter Enabled -> Disabled
1. User clicks filter button (currently active)
2. Callback copies `active_tag_filter_tags` to `data->saved_tags`
3. Clears `active_tag_filter_tags` (sets to empty string)
4. Sends notifier to update category display
5. All categories become visible

#### Filter Disabled -> Enabled
1. User clicks filter button (currently inactive)
2. Callback checks if `saved_tags` has content
3. If yes: copies `saved_tags` to `active_tag_filter_tags`
4. If no: selects first available tag or requires user to select
5. Sends notifier to update category display
6. Categories filtered by restored tags

## Implementation Files

### Modified Files

1. **source/blender/editors/interface/interface_tag_bar.hh**
   - Add `saved_tags[256]` field to `TagBarRuntimeData`

2. **source/blender/editors/interface/interface_tag_bar.cc**
   - Implement `tag_filter_toggle_click()` callback
   - Modify `tag_bar_draw_in_layout()` to draw filter button at end
   - Update `tag_bar_buttons_update()` to handle filter state

## State Diagram

```
                    ┌─────────────────┐
                    │  Filter ENABLED │
                    │  (active tags   │
                    │   filter cats)  │
                    └────────┬────────┘
                             │ Click button
                             │ (save tags, clear)
                             ▼
                    ┌─────────────────┐
                    │  Filter DISABLED│
                    │  (empty string, │
                    │   show all cats)│
                    └────────┬────────┘
                             │ Click button
                             │ (restore tags)
                             ▼
                    ┌─────────────────┐
                    │  Filter ENABLED │
                    └─────────────────┘
```

## Edge Cases

1. **No tags available**: Button remains but has no effect
2. **All tags deselected while enabled**: Treated as "show all" (same as disabled)
3. **Scene load**: Default to enabled state with empty string (no filter active)
4. **Window manager change**: `saved_tags` is runtime-only, resets on wm change

## Testing

1. Click filter button: verify all categories shown
2. Click again: verify previous tag selection restored
3. Select new tags while disabled: should enable filter automatically
4. Switch between editor types: filter state persists per View3D
