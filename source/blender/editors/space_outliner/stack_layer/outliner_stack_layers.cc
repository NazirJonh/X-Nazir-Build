/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 *
 * Navigation and activation for the Stack Layers display mode.
 *
 * Everything here is phrased in rows and ordinals. What a row *is* -- a paint layer, a shape key,
 * whatever comes next -- is the business of the #StackSource the space is set to, so the operators
 * below never mention a material, an image or a node.
 */

#include <algorithm>
#include <climits>
#include <functional>

#include "DNA_defs.h"
#include "DNA_ID.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_context.hh"
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
#include "ED_outliner_stack_automation.hh"
#include "ED_screen.hh"
#include "ED_undo.hh"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"
#include "RNA_enum_types.hh"
#include "RNA_define.hh"

#include "UI_interface_c.hh"
#include "UI_interface_icons.hh"
#include "UI_interface_layout.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "../outliner_intern.hh"
#include "../tree/tree_iterator.hh"
#include "outliner_stack_source.hh"

namespace blender::ed::outliner {

/** Defined below: the shared resolver of the operators that address one row, marker first. */
int stack_operator_ordinal_get(bContext &C, SpaceOutliner &space_outliner, wmOperator &op);
const ed::outliner::StackRow *outliner_stack_row_resolve(const SpaceOutliner &space_outliner,
                                                         const bUUID &row_id,
                                                         int ordinal_hint);

bool stack_row_activate_poll(bContext *C)
{
  if (!ED_operator_outliner_active(C)) {
    return false;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner == nullptr || space_outliner->runtime == nullptr) {
    return false;
  }
  const StackReadContext ctx = outliner_stack_read_context(*C);
  const ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  return owner != nullptr && stack_source_for_space(*space_outliner)->is_editable(*owner);
}

wmOperatorStatus stack_back_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (RNA_boolean_get(op->ptr, "keep_view")) {
    /* Staying in the Stack Layers mode only steps back to the objects list. */
    space_outliner->stack_layers_view = SO_SL_VIEW_OBJECTS;
  }
  else {
    /* A plain invocation leaves the Stack Layers mode for the regular View Layer browsing. */
    space_outliner->outlinevis = SO_VIEW_LAYER;
    space_outliner->stack_layers_view = SO_SL_VIEW_OBJECTS;
    /* Leaving the mode through an operator skips #rna_SpaceOutliner_display_mode_update, so the
     * tool header that carries only the Stack Layers controls is synced here through the same
     * helper the RNA update calls. */
    ScrArea *area = CTX_wm_area(C);
    if (area != nullptr) {
      outliner_tool_header_visibility_sync(area, *space_outliner);
      ED_area_tag_redraw(area);
    }
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

wmOperatorStatus stack_row_activate_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  return outliner_stack_row_activate(C, *space_outliner, ordinal) ? OPERATOR_FINISHED :
                                                                    OPERATOR_CANCELLED;
}

/**
 * Remember every row's UI state by identity, before the rows go away.
 *
 * Clicks and collapses live in the tree store, keyed by owner and ordinal: state a row keeps only
 * while no edit renumbers it. Read into #SpaceOutliner_Runtime::stack_row_ui_state here, while the
 * tree and the rows still agree on what each ordinal means, this is what re-attaches collapsed and
 * selected to the same rows after the edit -- whichever ordinals they land on.
 *
 * Called from #outliner_stack_rows_invalidate, which every path that re-reads the rows goes
 * through, so nothing that renumbers has to remember to ask.
 */
void stack_row_ui_state_capture(SpaceOutliner &space_outliner)
{
  SpaceOutliner_Runtime &runtime = *space_outliner.runtime;
  /* Once an edit has re-read the rows, the tree store still addresses the previous ordinals.
   * Capturing it again would overwrite identity state with the row now sitting at each old
   * ordinal.
   */
  if (runtime.stack_owner_uid == 0 || runtime.stack_rows_rebuilt_since_build) {
    return;
  }
  tree_iterator::all(space_outliner, [&](TreeElement *te) {
    const TreeStoreElem *tselem = TREESTORE(te);
    if (tselem->type != TSE_STACK_LAYER) {
      return;
    }
    const StackRow *row = outliner_stack_row_find(space_outliner, int(tselem->nr));
    if (row == nullptr || BLI_uuid_is_nil(row->stable_id)) {
      return;
    }
    StackRowUiState &state = runtime.stack_row_ui_state.lookup_or_add_default(row->stable_id);
    state.closed = (tselem->flag & TSE_CLOSED) != 0;
    state.selected = (tselem->flag & TSE_SELECTED) != 0;
    state.active = (tselem->flag & TSE_ACTIVE) != 0;
  });
}

/**
 * The ordinal of the row the operator's \a marker names, or -1.
 *
 * A marker is the row's identity -- #StackRow::stable_id, as an earlier call of the mode handed it
 * out. A script that re-addresses a row by its marker gets the same row after an edit moved it,
 * where a remembered ordinal is the position-trap the seam's own code avoids through that same
 * marker; the position properties stay for UI buttons, which speak for the row on screen at the
 * moment of the call.
 */
static int stack_operator_marker_ordinal_get(bContext &C,
                                             SpaceOutliner &space_outliner,
                                             wmOperator &op)
{
  char marker_str[UUID_STRING_SIZE];
  RNA_string_get(op.ptr, "marker", marker_str);
  if (marker_str[0] == '\0') {
    return -1;
  }
  bUUID marker;
  if (!BLI_uuid_parse_string(&marker, marker_str)) {
    return -1;
  }
  const StackReadContext ctx = outliner_stack_read_context(C);
  ID *owner = outliner_stack_owner_get(ctx, space_outliner);
  if (owner == nullptr) {
    return -1;
  }
  outliner_stack_rows_ensure(ctx, space_outliner, *owner);
  const StackRow *row = outliner_stack_row_resolve(space_outliner, marker, -1);
  return (row != nullptr) ? row->ordinal : -1;
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

const char *outliner_stack_focus_name_get(const SpaceOutliner &space_outliner)
{
  return (space_outliner.runtime != nullptr) ?
             space_outliner.runtime->stack_focus_name.c_str() :
             "";
}

void outliner_stack_rows_invalidate(SpaceOutliner &space_outliner)
{
  SpaceOutliner_Runtime &runtime = *space_outliner.runtime;
  /* The rows and the tree still agree on what each ordinal means; whatever the user did with
   * them is only about to be renumbered, so this is the last chance to keep it by identity. */
  stack_row_ui_state_capture(space_outliner);
  runtime.stack_rows.clear();
  runtime.stack_row_index.clear();
  runtime.stack_owner_uid = 0;
  runtime.stack_state_hash = 0;
  runtime.stack_rows_valid = false;
}

void outliner_stack_row_ui_state_capture_now(SpaceOutliner &space_outliner, const ID &owner)
{
  SpaceOutliner_Runtime &runtime = *space_outliner.runtime;
  if (space_outliner.treestore == nullptr) {
    return;
  }
  BLI_mempool_iter iter;
  BLI_mempool_iternew(space_outliner.treestore, &iter);
  while (const TreeStoreElem *tselem = static_cast<const TreeStoreElem *>(
             BLI_mempool_iterstep(&iter)))
  {
    if (tselem->type != TSE_STACK_LAYER || tselem->id != &owner) {
      continue;
    }
    const StackRow *row = outliner_stack_row_find(space_outliner, int(tselem->nr));
    if (row == nullptr || BLI_uuid_is_nil(row->stable_id)) {
      continue;
    }
    StackRowUiState &state = runtime.stack_row_ui_state.lookup_or_add_default(row->stable_id);
    state.closed = (tselem->flag & TSE_CLOSED) != 0;
    state.selected = (tselem->flag & TSE_SELECTED) != 0;
    state.active = (tselem->flag & TSE_ACTIVE) != 0;
  }
}

void outliner_stack_row_ui_state_sync(SpaceOutliner &space_outliner, const ID &owner)
{
  SpaceOutliner_Runtime &runtime = *space_outliner.runtime;
  /* The tree store speaks the ordinals of the tree it was filled by, so it can only be read
   * against the rows that tree was built from: the same owner, and not one an edit has re-read
   * since -- #outliner_stack_rows_ensure marks those, and the build that follows clears the
   * mark. */
  if (space_outliner.treestore == nullptr || runtime.stack_owner_uid != owner.session_uid ||
      runtime.stack_rows_rebuilt_since_build)
  {
    return;
  }
  outliner_stack_row_ui_state_capture_now(space_outliner, owner);
}

void outliner_stack_row_ui_state_prune(SpaceOutliner_Runtime &runtime, Set<UUID> &&seen)
{
  /* An entry unseen at this build and the previous one names a row the stack no longer has. */
  Vector<UUID> forgotten;
  for (const UUID &key : runtime.stack_row_ui_state.keys()) {
    if (!seen.contains(key) && !runtime.stack_row_ui_state_seen.contains(key)) {
      forgotten.append(key);
    }
  }
  for (const UUID &key : forgotten) {
    runtime.stack_row_ui_state.remove(key);
  }
  runtime.stack_row_ui_state_seen = std::move(seen);
}

StringRef outliner_stack_row_active_section_get(const SpaceOutliner &space_outliner,
                                                const StackRow &row)
{
  /* A row without sections has nothing to switch between; its content stays collapsed. */
  if (row.content_sections.is_empty()) {
    return {};
  }

  const SpaceOutliner_Runtime &runtime = *space_outliner.runtime;

  /* No stable_id means no persistent UI state: always use default section. */
  if (BLI_uuid_is_nil(row.stable_id)) {
    return row.content_sections[0].identifier;
  }

  const StackRowUiState *state = runtime.stack_row_ui_state.lookup_ptr(row.stable_id);
  if (state == nullptr || state->active_content_section.empty()) {
    /* No stored state: default to first section. */
    return row.content_sections[0].identifier;
  }

  /* Validate that the stored section still exists in the row's current data. */
  for (const StackContentSection &section : row.content_sections) {
    if (section.identifier == state->active_content_section) {
      return state->active_content_section;
    }
  }

  /* Stored section no longer exists (e.g., mask was deleted): fall back to first section. */
  return row.content_sections[0].identifier;
}

/** See #outliner_stack_row_resolve. */
const ed::outliner::StackRow *outliner_stack_row_resolve(const SpaceOutliner &space_outliner,
                                                         const bUUID &row_id,
                                                         const int ordinal_hint)
{
  if (!BLI_uuid_is_nil(row_id)) {
    for (const StackRow &row : space_outliner.runtime->stack_rows) {
      if (BLI_uuid_equal(row.stable_id, row_id)) {
        return &row;
      }
    }
  }
  return outliner_stack_row_find(space_outliner, ordinal_hint);
}

bool outliner_stack_row_active_section_set(SpaceOutliner &space_outliner,
                                           const StackRow &row,
                                           const StringRefNull section_id)
{
  /* Rows without content sections don't support section switching. */
  if (row.content_sections.is_empty()) {
    return false;
  }

  /* Validate that the section exists in the row. */
  bool section_exists = false;
  for (const StackContentSection &section : row.content_sections) {
    if (section.identifier == section_id) {
      section_exists = true;
      break;
    }
  }

  if (!section_exists) {
    return false;
  }

  /* No stable_id means no persistent UI state: cannot store selection. */
  if (BLI_uuid_is_nil(row.stable_id)) {
    return false;
  }

  SpaceOutliner_Runtime &runtime = *space_outliner.runtime;
  StackRowUiState &state = runtime.stack_row_ui_state.lookup_or_add_default(row.stable_id);
  state.active_content_section = section_id;

  return true;
}

bool stack_rows_are_siblings(const StackRow &a, const StackRow &b)
{
  return a.parent_ordinal == b.parent_ordinal && a.parent_section_id == b.parent_section_id;
}

Vector<StackAttachedPlacement> outliner_stack_attached_rows_plan(
    Span<StackRow> rows, FunctionRef<StringRef(const StackRow &)> active_section_get)
{
  Vector<StackAttachedPlacement> plan;
  /* Ordinal to index, plain rows only: an attached row hangs off the plain row its
   * #StackRow::parent_ordinal names, never off another attached row. */
  Map<int16_t, int64_t> plain_indices;
  for (const int64_t index : rows.index_range()) {
    if (rows[index].parent_section_id.empty()) {
      plain_indices.add(rows[index].ordinal, index);
    }
  }
  /* The rows run bottom to top, so walking them backward lists the plan top first -- the order a
   * stack is displayed in. */
  for (int64_t index = rows.size() - 1; index >= 0; index--) {
    const StackRow &row = rows[index];
    if (row.parent_section_id.empty() || !row.supported) {
      continue;
    }
    const int64_t *parent_index = plain_indices.lookup_ptr(row.parent_ordinal);
    if (parent_index == nullptr) {
      continue;
    }
    const StackRow &parent = rows[*parent_index];
    /* An unsupported parent has no sections of its own to list anything under. */
    if (!parent.supported) {
      continue;
    }
    if (active_section_get(parent) != StringRef(row.parent_section_id)) {
      continue;
    }
    plan.append({index, *parent_index});
  }
  return plan;
}

void outliner_stack_rows_ensure(const StackReadContext &ctx,
                                SpaceOutliner &space_outliner,
                                ID &owner)
{
  SpaceOutliner_Runtime &runtime = *space_outliner.runtime;
  const StackSource &source = *stack_source_for_space(space_outliner);
  const uint64_t state_hash = source.state_hash(ctx, owner);
  if (runtime.stack_rows_valid && runtime.stack_owner_uid == owner.session_uid &&
      runtime.stack_state_hash == state_hash)
  {
    return;
  }
  runtime.stack_rows.clear();
  source.rows_build(ctx, runtime.stack_focus, owner, runtime.stack_rows);
  /* Rows are addressed by ordinal all over the mode; one map spares every read a walk. */
  runtime.stack_row_index.clear();
  for (const int64_t index : runtime.stack_rows.index_range()) {
    runtime.stack_row_index.add(int(runtime.stack_rows[index].ordinal), int(index));
  }
  runtime.stack_owner_uid = owner.session_uid;
  runtime.stack_state_hash = state_hash;
  runtime.stack_rows_valid = true;
  /* Whatever tree the tree store was last filled by speaks the previous rows' ordinals. */
  runtime.stack_rows_rebuilt_since_build = true;
  /* Headers and scripts ask for the owner's name through RNA, where there is no context of their
   * own to resolve the focus with. */
  runtime.stack_focus_name = owner.name + 2;
}

const StackRow *outliner_stack_row_find(const SpaceOutliner &space_outliner, const int ordinal)
{
  const int *index = space_outliner.runtime->stack_row_index.lookup_ptr(ordinal);
  return (index != nullptr) ? &space_outliner.runtime->stack_rows[*index] : nullptr;
}

ID *outliner_stack_owner_get(const StackReadContext &ctx, SpaceOutliner &space_outliner)
{
  /* Resolving here, so that a focus left pointing at a deleted object drops itself, and the rows
   * cached under it with it, before anything gets to read them. */
  if (outliner_stack_focus_object_resolve(ctx, space_outliner) == nullptr) {
    return nullptr;
  }
  return stack_source_for_space(space_outliner)->owner_get(
      ctx, space_outliner.runtime->stack_focus);
}

/**
 * The ordinal the operator was given, or the row the user is pointing at when it was left at -1.
 *
 * A marker naming the row by identity comes first, when the operator has one. Then selection:
 * clicking a row is how a layer manager says "this one", and it is what the context menu and the
 * header buttons act on. The activated row -- the one the paint tools write into -- is the
 * fallback, since a stack can be worked on without anything being selected.
 */
int stack_operator_ordinal_get(bContext &C, SpaceOutliner &space_outliner, wmOperator &op)
{
  const int marker_ordinal = stack_operator_marker_ordinal_get(C, space_outliner, op);
  if (marker_ordinal >= 0) {
    return marker_ordinal;
  }
  const int ordinal = RNA_int_get(op.ptr, "ordinal");
  if (ordinal >= 0) {
    return ordinal;
  }
  Vector<int> selected;
  stack_selected_ordinals_get(space_outliner, selected);
  if (!selected.is_empty()) {
    return selected.first();
  }
  return outliner_stack_active_ordinal_get(outliner_stack_read_context(C), space_outliner);
}

/** Ordinals of the stack rows the user has selected, bottom-up. Empty when none are. */
void stack_selected_ordinals_get(SpaceOutliner &space_outliner, Vector<int> &r_ordinals)
{
  r_ordinals.clear();
  tree_iterator::all(space_outliner, [&](const TreeElement *te) {
    const TreeStoreElem *tselem = TREESTORE(te);
    if (tselem->type == TSE_STACK_LAYER && (tselem->flag & TSE_SELECTED)) {
      r_ordinals.append_non_duplicates(int(tselem->nr));
    }
  });
  std::sort(r_ordinals.begin(), r_ordinals.end());
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
  outliner_stack_rows_invalidate(space_outliner);
  space_outliner.outlinevis = SO_STACK_LAYERS;
  space_outliner.stack_layers_view = SO_SL_VIEW_STACK;
  ScrArea *area = CTX_wm_area(C);
  if (area != nullptr) {
    for (ARegion &region : area->regionbase) {
      if (region.regiontype != RGN_TYPE_TOOL_HEADER) {
        continue;
      }
      const bool hidden = region.flag & RGN_FLAG_HIDDEN_BY_USER;
      if (bool(region.flag & RGN_FLAG_HIDDEN) != hidden) {
        SET_FLAG_FROM_TEST(region.flag, hidden, RGN_FLAG_HIDDEN);
        ED_area_tag_region_size_update(area, &region);
      }
      ED_region_tag_redraw(&region);
    }
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
      tkey.mode = WM_toolsystem_mode_from_spacetype(*CTX_data_main(C),
                                                    CTX_data_scene(C),
                                                    CTX_data_view_layer(C),
                                                    nullptr,
                                                    SPACE_VIEW3D);
      WM_toolsystem_ref_set_by_id_ex(C, CTX_wm_workspace(C), &tkey, tool_idname, false);
    }
  }

  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_OUTLINER, nullptr);
  ED_area_tag_refresh(CTX_wm_area(C));
  return true;
}

bool outliner_stack_row_activate(bContext *C, SpaceOutliner &space_outliner, const int ordinal)
{
  if (ordinal < 0) {
    return false;
  }
  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, space_outliner);
  if (owner == nullptr) {
    return false;
  }
  outliner_stack_rows_ensure(ctx, space_outliner, *owner);
  const StackRow *row = outliner_stack_row_find(space_outliner, ordinal);
  if (row == nullptr) {
    return false;
  }
  const bool result = stack_source_for_space(space_outliner)
                          ->row_activate(*C, space_outliner.runtime->stack_focus, *owner, ordinal, *row);
  return result;
}

bool outliner_stack_row_preview_activate(bContext *C,
                                         SpaceOutliner &space_outliner,
                                         const int ordinal,
                                         const StringRef section_id)
{
  if (ordinal < 0) {
    return false;
  }
  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, space_outliner);
  if (owner == nullptr) {
    return false;
  }
  outliner_stack_rows_ensure(ctx, space_outliner, *owner);
  const StackRow *row = outliner_stack_row_find(space_outliner, ordinal);
  if (row == nullptr) {
    return false;
  }
  const bool result = stack_source_for_space(space_outliner)
                          ->preview_activate(*C, *owner, *row, section_id);
  return result;
}

bool outliner_stack_sub_row_activate(bContext *C, SpaceOutliner &space_outliner, const int nr)
{
  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, space_outliner);
  if (owner == nullptr) {
    return false;
  }
  outliner_stack_rows_ensure(ctx, space_outliner, *owner);
  const StackRow *row = outliner_stack_row_find(space_outliner, nr / STACK_ROW_SUB_ROW_STRIDE);
  if (row == nullptr) {
    return false;
  }
  const int role = nr % STACK_ROW_SUB_ROW_STRIDE;
  Span<StackSubRow> sub_rows;
  const StringRef active_section = outliner_stack_row_active_section_get(space_outliner, *row);
  for (const StackContentSection &section : row->content_sections) {
    if (section.identifier == active_section) {
      sub_rows = section.sub_rows;
      break;
    }
  }
  for (const StackSubRow &sub_row : sub_rows) {
    if (sub_row.role == role) {
      return stack_source_for_space(space_outliner)
          ->sub_row_activate(*C, space_outliner.runtime->stack_focus, *owner, *row, sub_row);
    }
  }
  return false;
}

int outliner_stack_active_ordinal_get(const StackReadContext &ctx,
                                      SpaceOutliner &space_outliner)
{
  ID *owner = outliner_stack_owner_get(ctx, space_outliner);
  if (owner == nullptr) {
    return -1;
  }
  /* The rows may never have been built in this space: a header button is reachable before the
   * tree has been drawn once. */
  outliner_stack_rows_ensure(ctx, space_outliner, *owner);
  const StackSource &source = *stack_source_for_space(space_outliner);
  for (const StackRow &row : space_outliner.runtime->stack_rows) {
    if (source.row_is_active(ctx, space_outliner.runtime->stack_focus, *owner, row)) {
      return row.ordinal;
    }
  }
  return -1;
}

bool outliner_stack_row_is_active(const StackReadContext &ctx,
                                  SpaceOutliner &space_outliner,
                                  const int ordinal)
{
  const StackRow *row = outliner_stack_row_find(space_outliner, ordinal);
  ID *owner = outliner_stack_owner_get(ctx, space_outliner);
  if (row == nullptr || owner == nullptr) {
    return false;
  }
  return stack_source_for_space(space_outliner)
      ->row_is_active(ctx, space_outliner.runtime->stack_focus, *owner, *row);
}

void outliner_stack_sources_undo_reset()
{
  for (const StackSource *source : stack_sources_get()) {
    source->undo_reset();
  }
}

}  // namespace blender::ed::outliner
