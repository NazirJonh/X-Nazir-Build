/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 *
 * Focus of the Stack Layers display: which object's stack is shown, the owner and read context
 * that go with it, the Back and Focus operators, and the sub-selection (material slot) choice.
 */

#include <algorithm>
#include <climits>
#include <functional>
#include <set>
#include <string>

#include "DNA_ID.h"
#include "DNA_defs.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_context.hh"
#include "BKE_icons.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_report.hh"

#include "BLI_listbase_wrapper.hh"
#include "BLI_map.hh"
#include "BLI_math_color.h"
#include "BLI_mempool.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_uuid.h"

#include "BLT_translation.hh"

#include "ED_object.hh"
#include "ED_outliner_stack.hh"
#include "ED_screen.hh"
#include "ED_undo.hh"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "UI_interface_c.hh"
#include "UI_interface_icons.hh"
#include "UI_interface_layout.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "../outliner_intern.hh"
#include "../tree/tree_iterator.hh"
#include "outliner_stack_source.hh"
#include "outliner_stack_layers_intern.hh"

namespace blender::ed::outliner {

wmOperatorStatus stack_focus_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner == nullptr) {
    return OPERATOR_CANCELLED;
  }
  Main *bmain = CTX_data_main(C);
  char object_name[MAX_ID_NAME - 2];
  char library_name[MAX_ID_NAME - 2];
  RNA_string_get(op->ptr, "object", object_name);
  RNA_string_get(op->ptr, "library", library_name);
  /* The name alone is ambiguous between linked data of different libraries; an empty library
   * names a local object. */
  Object *object = id_cast<Object *>(
      BKE_libblock_find_name_and_library(bmain, ID_OB, object_name, library_name));
  if (object == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "Object not found");
    return OPERATOR_CANCELLED;
  }
  const int sub_index = RNA_int_get(op->ptr, "sub_index");
  const bool enter_edit_mode = RNA_boolean_get(op->ptr, "enter_edit_mode");
  return outliner_stack_focus_set(C, *space_outliner, *object, sub_index, enter_edit_mode) ?
             OPERATOR_FINISHED :
             OPERATOR_CANCELLED;
}

wmOperatorStatus stack_back_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner == nullptr || space_outliner->runtime == nullptr) {
    return OPERATOR_CANCELLED;
  }
  /* Leaving the stack by hand is the user's own choice of display: the follow that would otherwise
   * bring a stack back on the next object change is off until one is entered again, and a View
   * Layer it opened is the user's now. */
  space_outliner->runtime->stack_follow_state = SpaceOutliner_Runtime::StackFollowState::
      ReleasedByUser;
  /* Without `keep_view` the mode is left for the regular View Layer browsing; with it the mode
   * stays and only steps back to the objects list. */
  if (!RNA_boolean_get(op->ptr, "keep_view")) {
    space_outliner->outlinevis = SO_VIEW_LAYER;
    space_outliner->stack_layers_view = SO_SL_VIEW_OBJECTS;
    /* Leaving the mode through an operator skips #rna_SpaceOutliner_display_mode_update, so the
     * tool header that carries only the Stack Layers controls is synced here through the same
     * helper the RNA update calls. */
    if (ScrArea *area = CTX_wm_area(C)) {
      outliner_tool_header_visibility_sync(C, area, *space_outliner);
      ED_area_tag_redraw(area);
    }
  }
  else {
    space_outliner->stack_layers_view = SO_SL_VIEW_OBJECTS;
  }
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_OUTLINER, nullptr);
  return OPERATOR_FINISHED;
}

wmOperatorStatus stack_back_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  /* The modifier is read once, at invoke: an exec reached from a script or from Repeat Last has
   * no event to read, and an operator whose behavior is not in its properties is not
   * reproducible. */
  RNA_boolean_set(op->ptr, "keep_view", (event->modifier & KM_ALT) != 0);
  return stack_back_exec(C, op);
}

StackReadContext outliner_stack_read_context(const bContext &C)
{
  bContext &context = const_cast<bContext &>(C);
  StackReadContext ctx;
  ctx.bmain = CTX_data_main(&context);
  ctx.scene = CTX_data_scene(&context);
  ctx.view_layer = CTX_data_view_layer(&context);
  return ctx;
}

Object *outliner_stack_focus_object_get(const StackReadContext &ctx, const StackFocus &focus)
{
  if (focus.object_uid == 0) {
    /* Resolved at use rather than stored: the active object can be freed or replaced under a
     * stored pointer by an undo step, a file load or a delete. */
    if (ctx.bmain == nullptr || ctx.view_layer == nullptr) {
      return nullptr;
    }
    BKE_view_layer_synced_ensure(*ctx.bmain, ctx.scene, ctx.view_layer);
    return BKE_view_layer_active_object_get(ctx.view_layer);
  }
  if (ctx.bmain == nullptr) {
    return nullptr;
  }
  return id_cast<Object *>(BKE_libblock_find_session_uid(ctx.bmain, ID_OB, focus.object_uid));
}

Object *outliner_stack_focus_object_resolve(const StackReadContext &ctx,
                                            SpaceOutliner &space_outliner)
{
  Object *object = outliner_stack_focus_object_get(ctx, space_outliner.runtime->stack_focus);
  if (object == nullptr && space_outliner.runtime->stack_focus.object_uid != 0) {
    /* A focus that names a gone object pins nothing: drop it and the rows cached under it, and
     * fall back to the object the view layer has active now. */
    space_outliner.runtime->stack_focus = {};
    outliner_stack_rows_invalidate(space_outliner);
    object = outliner_stack_focus_object_get(ctx, space_outliner.runtime->stack_focus);
  }
  return object;
}

bool outliner_stack_view_follow_active(SpaceOutliner &space_outliner,
                                       const StackReadContext &ctx)
{
  if (space_outliner.runtime == nullptr) {
    return false;
  }
  /* The space's own switch turns the follow off for good, and a display the user left by hand stays
   * the way they left it. */
  using FollowState = SpaceOutliner_Runtime::StackFollowState;
  FollowState &follow_state = space_outliner.runtime->stack_follow_state;
  if ((space_outliner.stack_layers_flag & SO_SL_NO_FOLLOW_OBJECT) != 0 ||
      follow_state == FollowState::ReleasedByUser)
  {
    return false;
  }
  /* Pin Stack is the user having said "keep this stack whatever the active object is", and a pinned
   * stack is only ever shown inside the mode. */
  if ((space_outliner.stack_layers_flag & SO_SL_PINNED) != 0) {
    return false;
  }
  /* A caller without a view layer -- a script's context, say -- has no object to follow and must not
   * be read as one that has none. */
  if (ctx.bmain == nullptr || ctx.view_layer == nullptr) {
    return false;
  }
  const bool in_mode = space_outliner.outlinevis == SO_STACK_LAYERS;
  /* Only a View Layer the follow itself opened is left again on its own; a mode the user chose is
   * theirs, however the active object changes. */
  const bool on_the_way_back = space_outliner.outlinevis == SO_VIEW_LAYER &&
                               follow_state == FollowState::LeftByFollow;
  if (!in_mode && !on_the_way_back) {
    return false;
  }
  Object *object = outliner_stack_focus_object_get(ctx, StackFocus{});
  const bool has_stack =
      object != nullptr && stack_source_for_space(space_outliner)->object_has_stack(ctx, *object);
  if (in_mode) {
    if (has_stack) {
      /* The mode is already the one the stack wants: only a leftover object list inside it has to
       * give way to the stack itself. */
      if (space_outliner.stack_layers_view == SO_SL_VIEW_STACK) {
        return false;
      }
      space_outliner.stack_layers_view = SO_SL_VIEW_STACK;
      return true;
    }
    /* No stack to show: the ordinary View Layer is what the active object has. */
    space_outliner.outlinevis = SO_VIEW_LAYER;
    /* The focus names the stack that is no longer shown, and this space now follows the active
     * object like the View Layer always has: dropping it is what makes coming back open the stack
     * of whatever is active then. */
    space_outliner.runtime->stack_focus = {};
    outliner_stack_rows_invalidate(space_outliner);
    follow_state = FollowState::LeftByFollow;
    return true;
  }
  if (!has_stack) {
    return false;
  }
  space_outliner.outlinevis = SO_STACK_LAYERS;
  space_outliner.stack_layers_view = SO_SL_VIEW_STACK;
  follow_state = FollowState::Armed;
  return true;
}

bool outliner_stack_focus_object_carry(bContext &C, SpaceOutliner &space_outliner)
{
  if (space_outliner.runtime == nullptr) {
    return true;
  }
  /* An unpinned stack follows the active object already: there is nothing to carry. */
  if ((space_outliner.stack_layers_flag & SO_SL_PINNED) == 0) {
    return true;
  }
  const StackReadContext ctx = outliner_stack_read_context(C);
  ViewLayer *view_layer = ctx.view_layer;
  Object *object = outliner_stack_focus_object_resolve(ctx, space_outliner);
  if (object == nullptr || ctx.bmain == nullptr || view_layer == nullptr || ctx.scene == nullptr) {
    return true;
  }
  BKE_view_layer_synced_ensure(*ctx.bmain, ctx.scene, view_layer);
  if (BKE_view_layer_active_object_get(view_layer) == object) {
    return true;
  }
  Base *base = BKE_view_layer_base_find(view_layer, object);
  if (base == nullptr) {
    /* An object that is not in this view layer cannot be made active. */
    return true;
  }
  /* The selection is left alone: carrying the context must not change what the user selected. The
   * object-mode helper leaves Edit Mode of another object, defers to a live Paint Shape session and
   * sends the active-object notifier. The focus survives it: a carried stack is a pinned one, and
   * the listener only drops a focus whose pin is off. */
  object::base_activate_with_mode_exit_if_needed(&C, base);
  /* A deferred change leaves the old object active; the caller then must not act on the row. */
  BKE_view_layer_synced_ensure(*ctx.bmain, ctx.scene, view_layer);
  return BKE_view_layer_active_object_get(view_layer) == object;
}

void outliner_stack_follow_release_mode(SpaceOutliner &space_outliner)
{
  if (space_outliner.runtime == nullptr) {
    return;
  }
  /* A display the user picks is theirs: the follow owns neither this mode nor an earlier departure,
   * so picking Stack Layers again is what arms it. */
  space_outliner.runtime->stack_follow_state = SpaceOutliner_Runtime::StackFollowState::Armed;
}

bool outliner_stack_view_follow_resume(bContext &C, SpaceOutliner &space_outliner)
{
  if (space_outliner.runtime == nullptr) {
    return false;
  }
  /* Asking for the follow again answers the display the user left by hand. */
  space_outliner.runtime->stack_follow_state = SpaceOutliner_Runtime::StackFollowState::Armed;
  return outliner_stack_view_follow_active(space_outliner, outliner_stack_read_context(C));
}

const char *outliner_stack_focus_name_get(const SpaceOutliner &space_outliner)
{
  return (space_outliner.runtime != nullptr) ? space_outliner.runtime->stack_focus_name.c_str() :
                                               "";
}

void outliner_stack_focus_normalize(const StackReadContext &ctx, SpaceOutliner &space_outliner)
{
  if (space_outliner.runtime == nullptr) {
    return;
  }
  SpaceOutliner_Runtime &runtime = *space_outliner.runtime;
  /* A focus that names a gone object drops itself first, so what follows works on a live one. */
  Object *object = outliner_stack_focus_object_resolve(ctx, space_outliner);
  if (object == nullptr) {
    return;
  }
  /* An empty focus means "the active object", so a stack pinned while the focus was empty -- one
   * just reset by an object change, or lost with the file, since the focus is runtime-only -- would
   * keep following the active object while its pin claimed otherwise. Naming the object is what
   * makes the pin mean what its icon says. */
  if ((space_outliner.stack_layers_flag & SO_SL_PINNED) != 0 && runtime.stack_focus.object_uid == 0)
  {
    runtime.stack_focus.object_uid = object->id.session_uid;
  }
  /* A focus without a sub-selection is read by the tree as the object's own current one, but the
   * header's enum has no "-1" item to say that with and would draw an empty label. Writing the
   * resolved slot down keeps the two on one value. */
  if (runtime.stack_focus.sub_index < 0) {
    const StackSource &source = *stack_source_for_space(space_outliner);
    Vector<StackSubSelection> selections;
    source.sub_selections_get(ctx, runtime.stack_focus, selections);
    if (!selections.is_empty()) {
      runtime.stack_focus.sub_index = source.default_sub_index(ctx, runtime.stack_focus);
    }
  }
}

void outliner_stack_focus_normalize(bContext &C, SpaceOutliner &space_outliner)
{
  outliner_stack_focus_normalize(outliner_stack_read_context(C), space_outliner);
}

ID *outliner_stack_owner_get(const StackReadContext &ctx, SpaceOutliner &space_outliner)
{
  /* Resolving here, so that a focus left pointing at a deleted object drops itself, and the rows
   * cached under it with it, before anything gets to read them. */
  if (outliner_stack_focus_object_resolve(ctx, space_outliner) == nullptr) {
    return nullptr;
  }
  return stack_source_for_space(space_outliner)
      ->owner_get(ctx, space_outliner.runtime->stack_focus);
}

bool outliner_stack_focus_set(bContext *C,
                              SpaceOutliner &space_outliner,
                              Object &object,
                              const int sub_index,
                              const bool enter_edit_mode)
{
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Base *base = BKE_view_layer_base_find(view_layer, &object);
  if (base == nullptr) {
    BKE_report(CTX_wm_reports(C), RPT_ERROR, "Object is not in the active view layer");
    return false;
  }
  const StackReadContext ctx = outliner_stack_read_context(*C);
  const StackSource &source = *stack_source_for_space(space_outliner);
  if (sub_index < -1) {
    BKE_report(CTX_wm_reports(C), RPT_ERROR, "Stack index is out of range");
    return false;
  }
  /* What a sub-index selects among is the source's own business; an empty list means the source
   * has no notion of one at all, and only the default (-1) is valid then. */
  StackFocus probe_focus;
  probe_focus.object_uid = object.id.session_uid;
  Vector<StackSubSelection> sub_selections;
  source.sub_selections_get(ctx, probe_focus, sub_selections);
  /* A source with sub-selections opens at the object's own current choice. Store it explicitly so
   * the header enum has a concrete current item and can cycle from it with Ctrl+Wheel. */
  const int resolved_sub_index = (sub_index < 0 && !sub_selections.is_empty()) ?
                                     source.default_sub_index(ctx, probe_focus) :
                                     sub_index;
  if (resolved_sub_index >= 0) {
    if (resolved_sub_index >= sub_selections.size()) {
      BKE_report(CTX_wm_reports(C), RPT_ERROR, "Stack index is out of range");
      return false;
    }
  }

  SpaceOutliner_Runtime &runtime = *space_outliner.runtime;
  runtime.stack_focus.object_uid = object.id.session_uid;
  runtime.stack_focus.sub_index = resolved_sub_index;
  /* Entering a stack by hand is what re-arms the follow a Back turned off. */
  runtime.stack_follow_state = SpaceOutliner_Runtime::StackFollowState::Armed;
  outliner_stack_rows_invalidate(space_outliner);
  space_outliner.outlinevis = SO_STACK_LAYERS;
  space_outliner.stack_layers_view = SO_SL_VIEW_STACK;
  if (ScrArea *area = CTX_wm_area(C)) {
    /* Focusing a stack sets the display mode directly, so it skips the RNA update that would show
     * the tool header; sync it here through the same helper that update calls. */
    outliner_tool_header_visibility_sync(C, area, space_outliner);
    ED_area_tag_redraw(area);
  }

  /* A stack whose model partly lives in its own data needs a one-time write before its rows can
   * be read -- identities handed out, a legacy shape brought into line. That is an explicit,
   * undoable step tied to opening the stack here, not a side effect of reading it every redraw
   * -- see #StackSource::focus_will_open. */
  ID *owner = outliner_stack_owner_get(ctx, space_outliner);
  if (owner != nullptr && source.focus_will_open(*C, *owner)) {
    ED_undo_push(C, "Open Layer Stack");
  }

  /* A source with no object mode of its own -- editing a shape key is not a mode -- leaves
   * nothing to enter, and asking for one anyway is not an error. */
  const std::optional<eObjectMode> mode = enter_edit_mode ? source.focus_object_mode() :
                                                            std::nullopt;
  if (mode.has_value()) {
    BKE_view_layer_base_select_and_set_active(view_layer, base);
    if (!object::mode_set(C, *mode)) {
      BKE_report(CTX_wm_reports(C), RPT_WARNING, "Could not enter the stack's edit mode");
    }
    else if (const char *tool_idname = source.focus_tool_idname()) {
      /* The context area is the Outliner, so #WM_toolsystem_ref_set_by_id would look up the
       * wrong tool key. Build the 3D viewport's key for the mode just entered instead. */
      bToolKey tkey{};
      tkey.space_type = SPACE_VIEW3D;
      tkey.mode = WM_toolsystem_mode_from_spacetype(
          *CTX_data_main(C), CTX_data_scene(C), CTX_data_view_layer(C), nullptr, SPACE_VIEW3D);
      WM_toolsystem_ref_set_by_id_ex(C, CTX_wm_workspace(C), &tkey, tool_idname, false);
    }
  }

  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_OUTLINER, nullptr);
  ED_area_tag_refresh(CTX_wm_area(C));
  return true;
}

/**
 * A copy of \a text that lives as long as the program.
 *
 * Dynamic enum items have to point at strings that outlive the item array, which RNA frees
 * without freeing them. Interning keeps the storage bounded by the number of distinct strings
 * rather than by the number of redraws.
 */
const char *stack_enum_string_intern(const StringRefNull text)
{
  static std::set<std::string> pool;
  return pool.emplace(text.c_str()).first->c_str();
}

/** The enum identifier of a sub-selection: its index. */
static const char *stack_sub_index_identifier(const int index)
{
  return stack_enum_string_intern(std::to_string(index));
}

/**
 * The sub-selections of the focused object, as enum items.
 *
 * Built from the source's own sub-selections, so the Outliner offers "which material slot" without
 * knowing that a slot is what a paint stack's sub-index means.
 */
static const EnumPropertyItem *stack_sub_index_itemf_impl(bContext *C,
                                                          SpaceOutliner &space_outliner,
                                                          bool *r_free)
{
  EnumPropertyItem *items = nullptr;
  int items_num = 0;
  if (C != nullptr && space_outliner.runtime != nullptr) {
    const StackReadContext ctx = outliner_stack_read_context(*C);
    /* Read only: the focus is normalized where the state changes (the tree build, the active
     * object changing, the pin being turned on), never while a menu or a header asks for items. */
    Vector<StackSubSelection> selections;
    stack_source_for_space(space_outliner)
        ->sub_selections_get(ctx, space_outliner.runtime->stack_focus, selections);
    for (const int index : selections.index_range()) {
      /* The strings have to outlive the item array, which RNA frees without freeing them: the
       * identifier comes from a pool that only grows, the name from the data-block itself. */
      EnumPropertyItem item = {};
      item.value = index;
      item.identifier = stack_sub_index_identifier(index);
      item.name = (selections[index].preview_id != nullptr) ?
                      selections[index].preview_id->name + 2 :
                      IFACE_("Empty Slot");
      item.description = "";
      /* The material's own preview, which the menu draws beside its name. */
      item.icon = ICON_MATERIAL;
      if (ID *preview_id = selections[index].preview_id) {
        if (const int icon_id = BKE_icon_id_ensure(preview_id)) {
          ui::icon_render_id(C, nullptr, preview_id, ICON_SIZE_ICON, true);
          item.icon = icon_id;
        }
      }
      RNA_enum_item_add(&items, &items_num, &item);
    }
  }
  RNA_enum_item_end(&items, &items_num);
  *r_free = true;
  return items;
}

const EnumPropertyItem *stack_sub_index_itemf(bContext *C,
                                              PointerRNA * /*ptr*/,
                                              PropertyRNA * /*prop*/,
                                              bool *r_free)
{
  SpaceOutliner *space_outliner = (C != nullptr) ? CTX_wm_space_outliner(C) : nullptr;
  if (space_outliner == nullptr) {
    *r_free = true;
    return nullptr;
  }
  return stack_sub_index_itemf_impl(C, *space_outliner, r_free);
}

const EnumPropertyItem *outliner_stack_focus_sub_index_itemf(bContext *C,
                                                              SpaceOutliner &space_outliner,
                                                              bool *r_free)
{
  return stack_sub_index_itemf_impl(C, space_outliner, r_free);
}

wmOperatorStatus stack_sub_index_set_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  outliner_stack_focus_sub_index_set(*space_outliner, RNA_enum_get(op->ptr, "sub_index"));
  return outliner_stack_focus_sub_index_apply(*C, *space_outliner) ? OPERATOR_FINISHED :
                                                                      OPERATOR_CANCELLED;
}

bool stack_sub_index_set_poll(bContext *C)
{
  if (!stack_mode_poll(C)) {
    return false;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  return outliner_stack_focus_object_get(outliner_stack_read_context(*C),
                                         space_outliner->runtime->stack_focus) != nullptr;
}

int outliner_stack_focus_sub_index_get(const SpaceOutliner &space_outliner)
{
  return (space_outliner.runtime != nullptr) ? space_outliner.runtime->stack_focus.sub_index : -1;
}

void outliner_stack_focus_sub_index_set(SpaceOutliner &space_outliner, const int sub_index)
{
  if (space_outliner.runtime != nullptr) {
    space_outliner.runtime->stack_focus.sub_index = sub_index;
  }
}

bool outliner_stack_focus_sub_index_apply(bContext &C, SpaceOutliner &space_outliner)
{
  if (space_outliner.runtime == nullptr) {
    return false;
  }
  Object *object = outliner_stack_focus_object_resolve(outliner_stack_read_context(C),
                                                        space_outliner);
  if (object == nullptr) {
    return false;
  }
  const int sub_index = outliner_stack_focus_sub_index_get(space_outliner);
  /* The sub-selection's own side effects in the data -- moving the material slot pointer along,
   * for one -- are the source's business; the generic path only passes through. */
  stack_source_for_space(space_outliner)->sub_selection_apply(
      C, space_outliner.runtime->stack_focus, *object);
  return outliner_stack_focus_set(&C, space_outliner, *object, sub_index, false);
}

void outliner_stack_sources_undo_reset()
{
  for (const StackSource *source : stack_sources_get()) {
    source->undo_reset();
  }
}

}  // namespace blender::ed::outliner
