# Multi-Object Infrastructure

Infrastructure for multi-object operations in Blender curves sculpt mode.

## Components

### 1. Undo Coordinator (`multi_object_undo.hh/cc`)

Tracks which curves objects participate in a multi-object sculpt stroke.

Curves sculpt uses operator-level undo (memfile), not mesh PBVH undo. This coordinator
prepares for future specialized multi-object undo steps.

**Usage:**
```cpp
#include "multi_object/multi_object_undo.hh"

multi_object::undo::Manager undo_mgr;

undo_mgr.push_begin(scene, objects, "Operation Name");
// ... modify geometry ...
undo_mgr.push_end(objects);

// Or cancel on error:
undo_mgr.cancel();
```

### 2. PBVH Manager (`multi_object_pbvh.hh/cc`)

Finds affected curves across multiple objects using world-to-local brush transforms.

**Usage:**
```cpp
#include "multi_object/multi_object_pbvh.hh"

multi_object::pbvh::Manager pbvh_mgr;

pbvh_mgr.initialize(objects);
pbvh_mgr.find_affected_nodes(brush_pos_world, radius);
pbvh_mgr.update_all_bounds();
pbvh_mgr.clear();
```

### 3. Brush Integration

Curves brushes iterate sculpt targets via `foreach_curves_sculpt_target()` in
`sculpt_brush.cc`, which maps selected editable curves objects in sculpt mode.

## Testing

Run tests with a full build:
```bash
ctest -R multi_object -V
```

Tests live in `multi_object/tests/`:
- Unit tests for undo coordinator
- Unit tests for PBVH manager
- Integration tests
- Performance tests

## Files

- `multi_object_undo.hh/cc` - Undo coordinator
- `multi_object_pbvh.hh/cc` - PBVH manager
- `tests/multi_object_*_test.cc` - Unit, integration, and performance tests
