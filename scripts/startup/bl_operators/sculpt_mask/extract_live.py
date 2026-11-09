# SPDX-FileCopyrightText: 2026 Nazir
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Extract, staying in Sculpt Mode: make a new object from the masked faces and shape it live.

``sculpt.extract_live`` (Sculpt > Mask > Extract Masked Area) makes the object from the masked
faces right away, while Sculpt Mode stays on the masked mesh. A panel in the viewport shapes it
live:

- **Scale / Inflate / Move**: the draft's transform and a modifier, no rebuild needed.
- **Thickness / Thickness Loops / Smoothness**: modifiers on the draft; the loops are cut across
  the thickness on Accept and drawn as a preview.
- **Grow / Shrink**: change which faces are extracted, so they rebuild the draft mesh (throttled
  during a drag, final on release).
- **Name / Result / Collection / Accept**: what the accepted part becomes and where it goes.

With several objects in Sculpt Mode (Multi-Object Edit Scope = All Objects) every masked one gets
its own draft (a ``Part``); the panel's values are shared and Accept / Cancel act on all of them.

Values are read from ``Scene.sculpt_mask_extra`` at invoke and written back on Accept, so they
remember themselves between calls. The layering: the pure mesh math lives in
``extract_geometry``, the per-object draft build in ``extract_parts``, the panel definition in
``extract_panel`` and the preview drawing in ``extract_preview``; this module is the operator and
its modal handling.

**Accept** bakes the modifiers into a plain mesh with polygroups. **Cancel** deletes the drafts.
Ctrl+Z / Ctrl+Shift+Z step through the panel's own value history before falling back to
cancelling the whole extract. Not on Dyntopo meshes.
"""

import math
import time

import bpy
from bpy.app.translations import pgettext_rpt
from bpy.types import Operator

from . import settings
from .extract_panel import (
    FADE_TIME,
    INT_SLIDERS,
    REBUILD_SLIDERS,
    ROW_NAME,
    ROW_PLACE,
    ROW_PREVIEW,
    ROW_SPACE,
    ROW_TARGET,
    SEGMENTS,
    SLIDER,
    SLIDERS,
    SNAP,
    draw_panel,
    extract_layout,
    segment_enabled,
    set_panel_scale,
)
from .extract_parts import MAX_NAME_LEN, _NO_MASK, Part
from .extract_preview import draw_preview
from .panel_ui import hit, ui_scale

_REBUILD_KEYS = set(REBUILD_SLIDERS)
# Sliders that take whole numbers.
_INT_KEYS = set(INT_SLIDERS)


class SCULPT_OT_extract_live(Operator):
    """Make a new object from the masked area and shape it (scale, inflate, thickness, """ \
        """smoothness, grow) before keeping it"""
    bl_idname = "sculpt.extract_live"
    bl_label = "Extract Masked Area"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        ob = context.active_object
        return context.mode == 'SCULPT' and ob is not None and ob.type == 'MESH'

    # -- setup ------------------------------------------------------------------
    def invoke(self, context, event):
        active = context.active_object
        # Read-only RNA property here (backed by the sculpt session), fine for a check.
        if active.use_dynamic_topology_sculpting:
            self.report({'WARNING'}, pgettext_rpt("Turn off Dyntopo to extract"))
            return {'CANCELLED'}

        self.active_name = active.name
        self.area = context.area

        s = self._settings()
        self._threshold = self._tune(s, "mask_threshold")
        self._inflate_at_one = self._tune(s, "inflate_at_one")
        self._thickness_at_one = self._tune(s, "thickness_at_one")
        self._smooth_iterations = int(self._tune(s, "smooth_iterations"))

        # Every mesh in Sculpt Mode when the edit scope covers them all, else the active one. Objects
        # sharing a mesh with one already taken are left out: they would extract the same faces twice.
        sources = [active]
        if context.tool_settings.sculpt.multi_object_edit_scope != 'ACTIVE':
            taken = {active.data}
            for ob in context.objects_in_mode_unique_data:
                if (ob != active and ob.type == 'MESH' and ob.mode == 'SCULPT'
                        and ob.data not in taken and not ob.use_dynamic_topology_sculpting):
                    taken.add(ob.data)
                    sources.append(ob)

        # Start from the remembered values; fall back to the slider neutrals if the
        # settings module is absent.
        self.values = {slider.key: self._setting(s, slider.key, slider.neutral) for slider in SLIDERS}
        for key in _INT_KEYS:
            self.values[key] = int(round(min(max(self.values[key], SLIDER[key].min), SLIDER[key].max)))
        self.values["smoothness"] = min(max(self.values["smoothness"], 0.0), 1.0)
        self.preview = bool(self._setting(s, "preview", True))
        self.collection_mode = self._setting(s, "collection", 'SOURCE')
        self.accept_mode = self._setting(s, "accept_mode", 'KEEP')
        self.offset_space = self._setting(s, "offset_space", 'GLOBAL')
        # The collection picked for Source (remembered in the file); gone ones fall back to empty.
        self.target_collection = self._setting(s, "target_collection", "")
        if self.target_collection not in {c.name for c in self._scene_collections(context)}:
            self.target_collection = ""
        self.placement = self._setting(s, "placement", 'COLLECTION')
        # The panel's own look (size, place) lives in the user preferences, not the scene.
        prefs = getattr(bpy.context.window_manager, "sculpt_panel_prefs", None)
        self.values["ui_scale"] = prefs.ui_scale if prefs is not None else 1.0
        self.panel_offset = (prefs.offset_x, prefs.offset_y) if prefs is not None else (0.0, 0.0)
        # The first redraw already needs the panel's size, not only after the first mouse move.
        set_panel_scale(self.values["ui_scale"])
        self.panel_drag = None
        self._scale_track = None
        self._shift = False

        # One part per masked object. The active object's own problem is reported; the others are
        # only skipped.
        self.parts = []
        problem = None
        for ob in sources:
            part = Part(self, ob)
            reason = part.prepare(context, ob)
            if reason is None:
                self.parts.append(part)
            elif ob == active:
                problem = reason
        if not self.parts:
            message = problem if problem not in (None, _NO_MASK) else \
                pgettext_rpt("Mask part of the mesh first")
            self.report({'WARNING'}, message)
            return {'CANCELLED'}
        self.multi = len(self.parts) > 1
        self.large = any(part.large for part in self.parts)
        # Joining into the source is not possible from a Multires snapshot.
        if any(part.mesh_copy for part in self.parts) and self.placement == 'MESH':
            self.placement = 'COLLECTION'
        self.obj_name = self._default_name()
        self.source_collection_name = self._source_collection_label(context)

        # Panel/modal state.
        self.hover = None
        self.dragging = None
        self.editing = None
        self.edit_text = ""
        self.edit_fresh = False
        self.opened = time.perf_counter()
        self.cleaned = False
        self._cursor_default = False
        # Value history for the panel-local undo; each entry is a full state dict.
        self._history = []
        self._redo = []

        for part in self.parts:
            part.create_draft(part.source())

        self.handle = bpy.types.SpaceView3D.draw_handler_add(draw_panel, (self,), 'WINDOW', 'POST_PIXEL')
        self.preview_handle = bpy.types.SpaceView3D.draw_handler_add(
            draw_preview, (self,), 'WINDOW', 'POST_VIEW')
        self.timer = context.window_manager.event_timer_add(1.0 / 60.0, window=context.window)
        settings.set_extract_active(True)
        context.window_manager.modal_handler_add(self)
        context.area.tag_redraw()
        return {'RUNNING_MODAL'}

    def _default_name(self):
        """One part: "<source> Extract". Several: just "Extract", each part is prefixed by its
        source's name (see :meth:`part_name`)."""
        return "Extract" if self.multi else self.parts[0].source_name + " Extract"

    def part_name(self, part):
        """The name of a part's object."""
        if self.multi:
            return "{} {}".format(part.source_name, self.obj_name)[:MAX_NAME_LEN]
        return self.obj_name[:MAX_NAME_LEN]

    def _source_collection_label(self, context):
        """What "Source" resolves to when no collection is picked."""
        if self.multi:
            return pgettext_rpt("Each source's own")
        source = self.parts[0].source()
        return source.users_collection[0].name if source.users_collection \
            else context.scene.collection.name

    # -- settings ---------------------------------------------------------
    def _settings(self):
        return settings.settings(bpy.context.scene)

    def _setting(self, group, key, default):
        """Read `key` from the settings group with a `extract_<key>`/fallback, defensive when the
        group is missing (property absent)."""
        if group is None:
            return default
        return getattr(group, "extract_" + key, default)

    def _tune(self, group, key):
        """Read a tuning setting by its own name; without the group the default is the one declared
        on the property in :mod:`settings`, so the numbers live in one place."""
        if group is None:
            from .settings import SculptMaskExtraSettings
            return SculptMaskExtraSettings.bl_rna.properties[key].default
        return getattr(group, key)

    def _persist(self):
        group = self._settings()
        if group is None:
            return
        group.extract_scale = self.values["scale"]
        group.extract_inflate = self.values["inflate"]
        group.extract_thickness = self.values["thickness"]
        group.extract_smoothness = self.values["smoothness"]
        group.extract_grow = int(self.values["grow"])
        group.extract_thickness_loops = int(self.values["thickness_loops"])
        group.extract_target_collection = self.target_collection
        group.extract_preview = bool(self.preview)
        group.extract_collection = self.collection_mode
        group.extract_accept_mode = self.accept_mode
        group.extract_offset_x = self.values["offset_x"]
        group.extract_offset_y = self.values["offset_y"]
        group.extract_offset_z = self.values["offset_z"]
        group.extract_offset_space = self.offset_space
        group.extract_placement = self.placement

    # -- all parts --------------------------------------------------------------
    def _rebuild(self, context):
        for part in self.parts:
            part.rebuild()

    def _apply_values(self):
        set_panel_scale(self.values["ui_scale"])
        for part in self.parts:
            part.apply()

    # -- slider interaction -----------------------------------------------------
    def _set_from_mouse(self, key, mx):
        lay = extract_layout(self.area, self._region(), self.panel_offset)
        x0, x1, _cy = lay.sliders[key]["track"]
        if key == "ui_scale":
            # The panel resizes under the cursor while this slider is dragged, so the track of the
            # first move is kept: the value then follows the mouse without feeding back.
            if self._scale_track is None:
                self._scale_track = (x0, x1)
            x0, x1 = self._scale_track
        slider = SLIDER[key]
        t = min(max((mx - x0) / (x1 - x0), 0.0), 1.0)
        neutral_x = x0 + (x1 - x0) * (slider.neutral - slider.min) / (slider.max - slider.min)
        if abs(mx - neutral_x) <= SNAP * ui_scale():
            self.values[key] = slider.neutral
        else:
            value = slider.min + (slider.max - slider.min) * t
            self.values[key] = int(round(value)) if key in _INT_KEYS else value
        if key in _REBUILD_KEYS:
            # Throttled during a drag (a full rebuild is not free); the final one runs on release.
            now = time.perf_counter()
            if not self.large and now - getattr(self, "_last_rebuild", 0.0) >= 0.1:
                self._last_rebuild = now
                self._rebuild(bpy.context)
            if self.large:
                self._report_large_once()
        else:
            self._apply_values()

    def _commit_rebuild(self):
        """A structural rebuild at the end of a Grow change (release or typed value)."""
        self._last_rebuild = time.perf_counter()
        self._rebuild(bpy.context)

    def _report_large_once(self):
        if not getattr(self, "_large_warned", False):
            self._large_warned = True
            self.report({'INFO'}, pgettext_rpt("Large mesh: Grow rebuilds on release only"))

    # -- name / collection / preview -------------------------------------------
    def _start_name_edit(self):
        self._snapshot()
        self.editing = ROW_NAME
        self.edit_text = self.obj_name
        self.edit_fresh = True

    def _commit_name(self):
        name = self.edit_text.strip()[:MAX_NAME_LEN]
        if name and name != self.obj_name:
            self.obj_name = name
            self._rename_drafts()

    def _rename_drafts(self):
        for part in self.parts:
            ob = part.object()
            if ob is not None:
                ob.name = self.part_name(part)

    @staticmethod
    def _scene_collections(context):
        """The scene's collections, master first."""
        scene_collection = context.scene.collection
        return [scene_collection, *scene_collection.children_recursive]

    def _cycle_target(self, step):
        """Pick the next / previous collection of the scene for Source (the empty choice is the
        source object's own collection)."""
        if self.collection_mode != 'SOURCE' or self.placement != 'COLLECTION':
            return
        names = [""] + [c.name for c in self._scene_collections(bpy.context)]
        index = names.index(self.target_collection) if self.target_collection in names else 0
        self._snapshot()
        self.target_collection = names[(index + step) % len(names)]

    def _choose(self, row_key, value):
        """A click on a segmented button: set that choice."""
        attr = SEGMENTS[row_key][1]
        if getattr(self, attr) == value:
            return
        if row_key == ROW_PLACE and value == 'MESH' and any(p.mesh_copy for p in self.parts):
            self.report({'WARNING'}, pgettext_rpt(
                "Multires meshes cannot be joined; use Child or Collection"))
            return
        self._snapshot()
        setattr(self, attr, value)
        if row_key == ROW_SPACE:
            self._apply_values()

    def _toggle_preview(self):
        self._snapshot()
        self.preview = not self.preview

    def _reset_defaults(self):
        self._snapshot()
        for slider in SLIDERS:
            self.values[slider.key] = slider.neutral
        for key in _INT_KEYS:
            self.values[key] = int(round(SLIDER[key].neutral))
        self.preview = True
        self.collection_mode = 'SOURCE'
        self.accept_mode = 'KEEP'
        self.offset_space = 'GLOBAL'
        self.placement = 'COLLECTION'
        self.target_collection = ""
        self.obj_name = self._default_name()
        self._rename_drafts()
        self._rebuild(bpy.context)

    # -- value typing -----------------------------------------------------------
    def _start_editing(self, key):
        self._snapshot()
        self.editing = key
        value = self.values[key]
        self.edit_text = "{:g}".format(round(value, 4)) if key not in _INT_KEYS else str(int(value))
        self.edit_fresh = True

    def _finish_editing(self, apply):
        """Use the typed value (when it reads as a number), and stop typing."""
        key = self.editing
        self.editing = None
        if not apply or key is None:
            return
        try:
            value = float(self.edit_text.replace(",", "."))
        except ValueError:
            return
        if not math.isfinite(value):
            return
        slider = SLIDER[key]
        if key in _INT_KEYS:
            self.values[key] = int(round(min(max(value, slider.typed_min), slider.typed_max)))
            if key in _REBUILD_KEYS:
                self._commit_rebuild()
            else:
                self._apply_values()
        else:
            self.values[key] = min(max(value, slider.typed_min), slider.typed_max)
            self._apply_values()

    # -- history ----------------------------------------------------------
    def _current_state(self):
        state = dict(self.values)
        state["_preview"] = self.preview
        state["_collection"] = self.collection_mode
        state["_accept"] = self.accept_mode
        state["_space"] = self.offset_space
        state["_placement"] = self.placement
        state["_target"] = self.target_collection
        state["_name"] = self.obj_name
        return state

    def _snapshot(self):
        """Record the current state so the next change can be undone to it."""
        self._history.append(self._current_state())
        if len(self._history) > 50:
            self._history.pop(0)
        self._redo.clear()

    def _restore(self, state):
        for slider in SLIDERS:
            if slider.key in state:
                self.values[slider.key] = state[slider.key]
        self.preview = state.get("_preview", self.preview)
        self.collection_mode = state.get("_collection", self.collection_mode)
        self.accept_mode = state.get("_accept", self.accept_mode)
        self.offset_space = state.get("_space", self.offset_space)
        self.placement = state.get("_placement", self.placement)
        self.target_collection = state.get("_target", self.target_collection)
        self.obj_name = state.get("_name", self.obj_name)
        self._rename_drafts()
        self._rebuild(bpy.context)

    def _undo_step(self):
        if not self._history:
            return False
        self._redo.append(self._current_state())
        self._restore(self._history.pop())
        return True

    def _redo_step(self):
        if not self._redo:
            return False
        self._history.append(self._current_state())
        self._restore(self._redo.pop())
        return True

    # -- text field editing ----------------------------------------------
    def _type_text(self, context, event):
        """A key while typing the object name. Printable characters go to the field."""
        if event.value != 'PRESS':
            return {'RUNNING_MODAL'}
        if event.type in {'RET', 'NUMPAD_ENTER'}:
            self._commit_name()
            self.editing = None
        elif event.type == 'ESC':
            self.editing = None
        elif event.type == 'TAB':
            self._commit_name()
            self.editing = None
        elif event.type == 'BACK_SPACE':
            self.edit_text = "" if self.edit_fresh else self.edit_text[:-1]
            self.edit_fresh = False
        elif event.type == 'DEL':
            self.edit_text = ""
            self.edit_fresh = False
        elif event.type == 'V' and (event.ctrl or event.oskey):
            self.edit_text = ("" if self.edit_fresh else self.edit_text) + \
                context.window_manager.clipboard.strip()[:MAX_NAME_LEN]
            self.edit_fresh = False
        elif event.unicode and not (event.ctrl or event.oskey) and event.unicode.isprintable():
            if self.edit_fresh:
                self.edit_text = ""
            self.edit_fresh = False
            if len(self.edit_text) < MAX_NAME_LEN:
                self.edit_text += event.unicode
        self.area.tag_redraw()
        return {'RUNNING_MODAL'}

    def _type(self, context, event):
        """A key while typing a value. Every key goes to the field, none to sculpting."""
        if self.editing == ROW_NAME:
            return self._type_text(context, event)
        if event.value != 'PRESS':
            return {'RUNNING_MODAL'}
        if event.type in {'RET', 'NUMPAD_ENTER'}:
            self._finish_editing(True)
        elif event.type == 'ESC':
            self._finish_editing(False)
        elif event.type == 'TAB':
            keys = [slider.key for slider in SLIDERS]
            step = -1 if event.shift else 1
            nxt = keys[(keys.index(self.editing) + step) % len(keys)]
            self._finish_editing(True)
            self._start_editing(nxt)
        elif event.type == 'BACK_SPACE':
            self.edit_text = "" if self.edit_fresh else self.edit_text[:-1]
            self.edit_fresh = False
        elif event.type == 'DEL':
            self.edit_text = ""
            self.edit_fresh = False
        elif event.type == 'V' and (event.ctrl or event.oskey):
            self.edit_text = ("" if self.edit_fresh else self.edit_text) + \
                context.window_manager.clipboard.strip()[:16]
            self.edit_fresh = False
        elif event.unicode and event.unicode in "0123456789.,-+" and not (event.ctrl or event.oskey):
            if self.edit_fresh:
                self.edit_text = ""
            self.edit_fresh = False
            if len(self.edit_text) < 16:
                self.edit_text += event.unicode
        self.area.tag_redraw()
        return {'RUNNING_MODAL'}

    def _region(self):
        return next(r for r in self.area.regions if r.type == 'WINDOW')

    def _set_cursor_over_panel(self, context, over):
        """The standard cursor over the panel; the tool's own one everywhere else."""
        if over == self._cursor_default:
            return
        self._cursor_default = over
        if over:
            context.window.cursor_modal_set('DEFAULT')
        else:
            context.window.cursor_modal_restore()

    def _restore_cursor(self, context):
        self._set_cursor_over_panel(context, False)

    def _end(self, context):
        if self.cleaned:
            return
        self.cleaned = True
        prefs = getattr(bpy.context.window_manager, "sculpt_panel_prefs", None)
        if prefs is not None:
            prefs.ui_scale = self.values["ui_scale"]
            prefs.offset_x, prefs.offset_y = self.panel_offset
        set_panel_scale(1.0)
        self._restore_cursor(context)
        bpy.types.SpaceView3D.draw_handler_remove(self.handle, 'WINDOW')
        bpy.types.SpaceView3D.draw_handler_remove(self.preview_handle, 'WINDOW')
        context.window_manager.event_timer_remove(self.timer)
        settings.set_extract_active(False)
        self.area.tag_redraw()

    # -- accept / cancel --------------------------------------------------------
    def _accept(self, context):
        """Bake every draft into a plain mesh, ready to sculpt."""
        self._end(context)
        baked = []
        for part in self.parts:
            ob = part.bake(context)
            if ob is not None:
                baked.append((part, ob))
        if not baked:
            return self._cancel(context)
        # Remember the values for the next call.
        self._persist()
        # Undo must take the new objects away, and only steps holding the whole scene do that (the
        # sculpt steps before don't). So: an undo step of the scene without them, then (when the
        # operator finishes) the one with them. Cancel leaves no step behind this way. The relink
        # is in a `finally` -- an aborted push must not leave the drafts unlinked (they would look
        # deleted but stay alive as orphans).
        links = [(ob, list(ob.users_collection)) for _part, ob in baked]
        try:
            for ob, collections in links:
                for collection in collections:
                    collection.objects.unlink(ob)
            bpy.ops.ed.undo_push(message="Before Extract")
        finally:
            for ob, collections in links:
                for collection in collections:
                    collection.objects.link(ob)
        # Remove / fill and join edit the sources. This runs after the undo push above, so one
        # Ctrl+Z still reverts the whole operation.
        active = context.view_layer.objects.active
        names = [part.finish_source(context, ob) for part, ob in baked]
        if active is not None and active.name in bpy.data.objects:
            context.view_layer.objects.active = active
        for part in self.parts:
            part.release_mesh_copy()
        if len(names) == 1:
            self.report({'INFO'}, "Extracted \"{}\"".format(names[0]))
        else:
            self.report({'INFO'}, "Extracted {} objects".format(len(names)))
        return {'FINISHED'}

    def _cancel(self, context):
        self._end(context)
        for part in self.parts:
            part.discard()
        return {'CANCELLED'}

    # -- modal ------------------------------------------------------------------
    def cancel(self, context):
        # Called by Blender when the modal is dropped from outside (window closed, file loaded):
        # the handlers, timer, cursor and drafts must not outlive it.
        if getattr(self, "cleaned", True):
            return
        self._cancel(context)

    def modal(self, context, event):
        status = None
        try:
            status = self._modal_step(context, event)
        finally:
            if status is None:
                # Any raise out of the step would leak two draw handlers, a timer and the live
                # drafts (orphaned datablocks, a stuck viewport): tear everything down, then let
                # the exception propagate to the console.
                self._cancel(context)
        if status not in ({'RUNNING_MODAL'}, {'PASS_THROUGH'}):
            # Accept (FINISHED) must keep the objects: only the handlers go. Cancel paths have
            # already removed the drafts themselves.
            self._end(context)
            self._restore_cursor(context)
        return status

    def _still_sculpting(self, context):
        """True while the active object and every part's source are still in Sculpt Mode."""
        active = context.view_layer.objects.active
        if active is None or active.name != self.active_name:
            return False
        for part in self.parts:
            source = part.source()
            if part.object() is None or source is None or source.mode != 'SCULPT':
                return False
        return True

    def _modal_step(self, context, event):
        if not self._still_sculpting(context):
            # The masked mesh is no longer sculpted (another object, mode or file): drop it.
            return self._cancel(context)
        if event.type == 'TIMER':
            now = time.perf_counter()
            if now - self.opened < FADE_TIME * 2:
                self.area.tag_redraw()
            return {'PASS_THROUGH'}
        # Typing a value or a name: keys go to the field wherever the mouse is.
        if self.editing is not None and event.type not in {
                'MOUSEMOVE', 'INBETWEEN_MOUSEMOVE', 'LEFTMOUSE', 'MIDDLEMOUSE', 'RIGHTMOUSE',
                'WHEELUPMOUSE', 'WHEELDOWNMOUSE', 'TRACKPADPAN', 'TRACKPADZOOM'}:
            return self._type(context, event)
        if context.area != self.area:
            self._restore_cursor(context)
            return {'PASS_THROUGH'}
        region = self._region()
        mx, my = event.mouse_x - region.x, event.mouse_y - region.y
        lay = extract_layout(self.area, region, self.panel_offset)

        if event.type == 'MOUSEMOVE':
            if self.panel_drag is not None:
                # Dragging the panel by its title: follow the mouse, kept inside the region.
                start_x, start_y, off_x, off_y = self.panel_drag
                self.panel_offset = extract_layout(
                    self.area, region, (off_x + mx - start_x, off_y + my - start_y)).offset
                self.area.tag_redraw()
                return {'RUNNING_MODAL'}
            self._set_cursor_over_panel(
                context, self.dragging is not None or hit(lay.panel, mx, my))
            if self.dragging is not None:
                self._set_from_mouse(self.dragging, mx)
                self.area.tag_redraw()
                return {'RUNNING_MODAL'}
            hover = next((k for k, r in lay.buttons.items() if hit(r, mx, my)), None)
            if hover is None:
                hover = next((("SEG", key, value) for key, items in lay.segments.items()
                              for value, r in items if hit(r, mx, my)), None)
            if hover is None:
                hover = next((("FIELD", k) for k, p in lay.sliders.items()
                              if hit(p["field"], mx, my)), None)
            if hover is None:
                hover = next((k for k, p in lay.sliders.items() if hit(p["hit"], mx, my)), None)
            if hover is None:
                hover = next((k for k, r in lay.rows.items() if hit(r, mx, my)), None)
            if hover != self.hover:
                self.hover = hover
                self.area.tag_redraw()
            return {'PASS_THROUGH'}

        if event.type == 'LEFTMOUSE':
            if event.value == 'DOUBLE_CLICK':
                return self._on_double_click(context, mx, my, lay)
            if event.value == 'RELEASE' and self.panel_drag is not None:
                self.panel_drag = None
                return {'RUNNING_MODAL'}
            if event.value == 'RELEASE' and self.dragging is not None:
                key = self.dragging
                self.dragging = None
                if key in _REBUILD_KEYS:
                    self._commit_rebuild()
                self._scale_track = None
                self.area.tag_redraw()
                return {'RUNNING_MODAL'}
            if event.value == 'PRESS':
                clicked_field = next((k for k, p in lay.sliders.items()
                                      if hit(p["field"], mx, my)), None)
                if (self.editing is not None and self.editing != ROW_NAME
                        and clicked_field != self.editing):
                    self._finish_editing(True)
                if self.editing == ROW_NAME:
                    self._commit_name()
                    self.editing = None
                if clicked_field is not None:
                    if self.editing != clicked_field:
                        self._start_editing(clicked_field)
                    self.area.tag_redraw()
                    return {'RUNNING_MODAL'}
                for identifier, rect in lay.buttons.items():
                    if hit(rect, mx, my):
                        return self._accept(context) if identifier == "ACCEPT" else self._cancel(context)
                for row_key, items in lay.segments.items():
                    for value, rect in items:
                        if hit(rect, mx, my) and segment_enabled(self, row_key):
                            self._choose(row_key, value)
                            self.area.tag_redraw()
                            return {'RUNNING_MODAL'}
                self._shift = event.shift
                for row_key, rect in lay.rows.items():
                    if hit(rect, mx, my):
                        return self._on_row_click(context, row_key)
                for key, parts in lay.sliders.items():
                    if hit(parts["hit"], mx, my):
                        self._snapshot()
                        self.dragging = key
                        self._set_from_mouse(key, mx)
                        self.area.tag_redraw()
                        return {'RUNNING_MODAL'}
                if hit(lay.title_rect, mx, my):
                    self.panel_drag = (mx, my, *self.panel_offset)
                    return {'RUNNING_MODAL'}
                if hit(lay.panel, mx, my):
                    return {'RUNNING_MODAL'}
            return {'PASS_THROUGH'}

        if event.type in {'WHEELUPMOUSE', 'WHEELDOWNMOUSE'} and self.hover == ROW_TARGET:
            self._cycle_target(1 if event.type == 'WHEELDOWNMOUSE' else -1)
            self.area.tag_redraw()
            return {'RUNNING_MODAL'}

        if event.value == 'PRESS':
            if event.type in {'RET', 'NUMPAD_ENTER'}:
                return self._accept(context)
            if event.type == 'ESC':
                return self._cancel(context)
            if (event.ctrl or event.oskey) and event.type == 'Z':
                if event.shift:
                    self._redo_step()
                    self.area.tag_redraw()
                    return {'RUNNING_MODAL'}
                if self._undo_step():
                    self.area.tag_redraw()
                    return {'RUNNING_MODAL'}
                # No panel history: fall back to cancelling the extract.
                return self._cancel(context)
            if (event.ctrl or event.oskey) and event.type == 'Y':
                self._redo_step()
                self.area.tag_redraw()
                return {'RUNNING_MODAL'}
        return {'PASS_THROUGH'}

    def _on_double_click(self, context, mx, my, lay):
        # Double-click a slider row resets it to neutral; a number field starts typing; the title
        # resets everything to defaults.
        if hit(lay.title_rect, mx, my):
            self.panel_drag = None
            self._reset_defaults()
            self.area.tag_redraw()
            return {'RUNNING_MODAL'}
        clicked_field = next((k for k, p in lay.sliders.items() if hit(p["field"], mx, my)), None)
        if clicked_field is not None:
            self.dragging = None
            if self.editing != clicked_field:
                self._start_editing(clicked_field)
            self.area.tag_redraw()
            return {'RUNNING_MODAL'}
        for key, parts in lay.sliders.items():
            if hit(parts["hit"], mx, my):
                self._snapshot()
                self.dragging = None
                self.values[key] = int(round(SLIDER[key].neutral)) if key in _INT_KEYS \
                    else SLIDER[key].neutral
                self._commit_rebuild() if key in _REBUILD_KEYS else self._apply_values()
                self.area.tag_redraw()
                return {'RUNNING_MODAL'}
        return {'RUNNING_MODAL'}

    def _on_row_click(self, context, row_key):
        if row_key == ROW_NAME:
            self._start_name_edit()
        elif row_key == ROW_TARGET:
            self._cycle_target(-1 if self._shift else 1)
        elif row_key == ROW_PREVIEW:
            self._toggle_preview()
        self.area.tag_redraw()
        return {'RUNNING_MODAL'}


classes = (
    SCULPT_OT_extract_live,
)
