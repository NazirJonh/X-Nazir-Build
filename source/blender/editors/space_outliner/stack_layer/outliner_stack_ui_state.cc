/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 *
 * Row state of the Stack Layers display: the rows cache, the per-row UI state kept by identity,
 * the row's active content section, and the identity and activation helpers that read the rows.
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
 * The selection an edit just left behind: the row it names reads as selected and active once the
 * tree rebuilds, and every other row reads as not -- an edit renumbers the stack, and without
 * this the tree store would hand the old ordinal's selection to whichever row moved into it.
 *
 * The edit has already run, so the rows re-read here are the renumbered ones and \a select_ordinal
 * addresses them. -1 names nothing -- an edit that took a row away, such as a remove or an
 * ungroup -- and only clears. The map is what carries the selection across the rebuild; a row
 * without a #StackRow::stable_id cannot be addressed by it and keeps the tree store's state.
 */
void stack_row_ui_state_edit_select(const StackReadContext &ctx,
                                    SpaceOutliner &space_outliner,
                                    ID &owner,
                                    const int select_ordinal)
{
  SpaceOutliner_Runtime &runtime = *space_outliner.runtime;
  outliner_stack_rows_ensure(ctx, space_outliner, owner);
  for (StackRowUiState &state : runtime.stack_row_ui_state.values()) {
    state.selected = false;
    state.active = false;
  }
  if (select_ordinal < 0) {
    return;
  }
  const StackItemIdentity identity = outliner_stack_identity_of(space_outliner, select_ordinal);
  if (BLI_uuid_is_nil(identity.row_id)) {
    return;
  }
  StackRowUiState &state = runtime.stack_row_ui_state.lookup_or_add_default(identity.row_id);
  state.selected = true;
  state.active = true;
}

/**
 * The follow of an activation that reached this space from outside its own click: make the row
 * \a select_ordinal names -- the one the source calls active -- the selected and active row here,
 * unless the map already names it active. The map naming it active means the selection on display
 * is the one the activating gesture itself wrote: a Ctrl or Shift click of this Outliner, or the
 * selection a block drop put back -- several rows of it, which collapsing onto the one row would
 * undo before it is ever drawn.
 */
void stack_row_ui_state_follow_select(const StackReadContext &ctx,
                                      SpaceOutliner &space_outliner,
                                      ID &owner,
                                      const int select_ordinal)
{
  SpaceOutliner_Runtime &runtime = *space_outliner.runtime;
  outliner_stack_rows_ensure(ctx, space_outliner, owner);
  if (select_ordinal >= 0) {
    const StackItemIdentity identity = outliner_stack_identity_of(space_outliner, select_ordinal);
    if (!BLI_uuid_is_nil(identity.row_id)) {
      const StackRowUiState *state = runtime.stack_row_ui_state.lookup_ptr(identity.row_id);
      if (state != nullptr && state->active) {
        return;
      }
    }
  }
  stack_row_ui_state_edit_select(ctx, space_outliner, owner, select_ordinal);
}

void outliner_stack_row_ui_resync(Main &bmain,
                                  Scene &scene,
                                  ViewLayer &view_layer,
                                  SpaceOutliner &space_outliner)
{
  if (space_outliner.outlinevis != SO_STACK_LAYERS || space_outliner.runtime == nullptr) {
    return;
  }
  const StackReadContext ctx{&bmain, &scene, &view_layer};
  ID *owner = outliner_stack_owner_get(ctx, space_outliner);
  if (owner == nullptr) {
    return;
  }
  /* The source is what knows which row is active -- the material's active paint layer, the
   * object's active shape key -- and it answers for the owner this space shows, whatever a stale
   * tree store and a stale state map still claim. -1 is a stack whose source names no active row,
   * which nothing here can select on its behalf. */
  const int active_ordinal = outliner_stack_active_ordinal_get(ctx, space_outliner);
  if (active_ordinal < 0) {
    return;
  }
  stack_row_reveal_attached(space_outliner, active_ordinal);
  stack_row_ui_state_follow_select(ctx, space_outliner, *owner, active_ordinal);
}

void stack_row_edit_finished(bContext &C,
                             SpaceOutliner &space_outliner,
                             ID &owner,
                             const int select_ordinal,
                             const bool needs_renumber)
{
  const StackReadContext ctx = outliner_stack_read_context(C);
  /* Invalidating captures the rows' UI state into the identity-keyed map before the rows go
   * away; a renumbering edit then re-targets the selection on the map, which is what carries it
   * across the rebuild. */
  outliner_stack_rows_invalidate(space_outliner);
  if (needs_renumber) {
    stack_row_ui_state_edit_select(ctx, space_outliner, owner, select_ordinal);
  }
  WM_event_add_notifier(&C, NC_SPACE | ND_SPACE_OUTLINER, nullptr);
}

void outliner_stack_row_added_sync(bContext &C, ID &owner, const bUUID &row_id)
{
  Main *bmain = CTX_data_main(&C);
  if (bmain == nullptr) {
    return;
  }
  const StackReadContext ctx = outliner_stack_read_context(C);
  bool synced = false;
  /* The row was added outside this editor, so every Outliner showing the owner has to be told --
   * not only the one the Add was called from. */
  for (bScreen *screen = static_cast<bScreen *>(bmain->screens.first); screen != nullptr;
       screen = static_cast<bScreen *>(screen->id.next))
  {
    for (ScrArea &area : screen->areabase) {
      for (SpaceLink &sl : area.spacedata) {
        if (sl.spacetype != SPACE_OUTLINER) {
          continue;
        }
        SpaceOutliner &space_outliner = reinterpret_cast<SpaceOutliner &>(sl);
        if (space_outliner.outlinevis != SO_STACK_LAYERS ||
            space_outliner.runtime == nullptr)
        {
          continue;
        }
        if (outliner_stack_owner_get(ctx, space_outliner) != &owner) {
          continue;
        }
        outliner_stack_rows_ensure(ctx, space_outliner, owner);
        int ordinal = -1;
        for (const StackRow &row : space_outliner.runtime->stack_rows) {
          if (BLI_uuid_equal(row.stable_id, row_id)) {
            ordinal = row.ordinal;
            break;
          }
        }
        if (ordinal < 0) {
          continue;
        }
        /* A correction is listed under its row's content section: bring that section up, or the
         * row just made is in the data and unseen. */
        stack_row_reveal_attached(space_outliner, ordinal);
        stack_row_ui_state_edit_select(ctx, space_outliner, owner, ordinal);
        /* The same follow-up the Add wrapper gives a row it made: the target mode, the mask and the
         * texture-paint slots all resolve through the active row. */
        outliner_stack_row_activate(&C, space_outliner, ordinal);
        synced = true;
      }
    }
  }
  if (synced) {
    WM_event_add_notifier(&C, NC_SPACE | ND_SPACE_OUTLINER, nullptr);
  }
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
  /* The rows are about to go away, and the drop indicator names one of them.
   *
   * What a drop was aimed at is deliberately *not* cleared with them: it names its row by
   * identity, not by address, and is resolved against whatever rows exist when the drop lands.
   * Clearing it here loses the aim of a drop that is already under way -- importing a dragged
   * asset remaps data-blocks, which reaches this function through #outliner_id_remap, between the
   * poll that recorded the aim and the operator that reads it back. */
  runtime.stack_drop_indicator.ordinal = -1;
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

void stack_row_reveal_attached(SpaceOutliner &space_outliner, const int ordinal)
{
  const StackRow *row = outliner_stack_row_find(space_outliner, ordinal);
  if (row == nullptr || row->parent_section_id.empty()) {
    return;
  }
  const StackRow *parent = outliner_stack_row_find(space_outliner, row->parent_ordinal);
  if (parent == nullptr) {
    return;
  }
  outliner_stack_row_active_section_set(space_outliner, *parent, row->parent_section_id);
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

StackItemIdentity outliner_stack_identity_of(const SpaceOutliner &space_outliner,
                                             const int ordinal)
{
  StackItemIdentity identity;
  const SpaceOutliner_Runtime *runtime = space_outliner.runtime;
  if (runtime == nullptr || runtime->stack_owner_uid == 0) {
    return identity;
  }
  identity.owner_uid = runtime->stack_owner_uid;
  identity.source_type = space_outliner.stack_source;
  identity.ordinal_hint = int16_t(ordinal);
  const StackRow *row = outliner_stack_row_find(space_outliner, ordinal);
  if (row != nullptr) {
    identity.row_id = row->stable_id;
  }
  return identity;
}

int outliner_stack_identity_resolve(const StackReadContext &ctx,
                                    SpaceOutliner &space_outliner,
                                    const StackItemIdentity &identity)
{
  if (!identity.is_valid()) {
    return -1;
  }
  ID *owner = outliner_stack_owner_get(ctx, space_outliner);
  if (owner == nullptr || owner->session_uid != identity.owner_uid ||
      space_outliner.stack_source != identity.source_type)
  {
    return -1;
  }
  outliner_stack_rows_ensure(ctx, space_outliner, *owner);
  if (!BLI_uuid_is_nil(identity.row_id)) {
    for (const StackRow &row : space_outliner.runtime->stack_rows) {
      if (BLI_uuid_equal(row.stable_id, identity.row_id)) {
        return row.ordinal;
      }
    }
    return -1;
  }
  if (identity.ordinal_hint >= 0 &&
      outliner_stack_row_find(space_outliner, identity.ordinal_hint) != nullptr)
  {
    return identity.ordinal_hint;
  }
  return -1;
}

bool outliner_stack_row_activate(bContext *C, SpaceOutliner &space_outliner, const int ordinal)
{
  if (ordinal < 0) {
    return false;
  }
  /* A pinned stack shows an object that is not necessarily the active one; the row is acted on in
   * that object's context, as a pinned panel's data always is. */
  if (!outliner_stack_focus_object_carry(*C, space_outliner)) {
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
  return stack_source_for_space(space_outliner)
      ->row_activate(*C, space_outliner.runtime->stack_focus, *owner, ordinal, *row);
}

bool outliner_stack_row_preview_activate(bContext *C,
                                         SpaceOutliner &space_outliner,
                                         const int ordinal,
                                         const StringRef section_id)
{
  if (ordinal < 0) {
    return false;
  }
  /* The slot a preview click switches is the active object's paint target. */
  if (!outliner_stack_focus_object_carry(*C, space_outliner)) {
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
  return stack_source_for_space(space_outliner)->preview_activate(*C, *owner, *row, section_id);
}

bool outliner_stack_sub_row_activate(bContext *C, SpaceOutliner &space_outliner, const int nr)
{
  /* A content sub-row opens the image the active object's paint slots hold. */
  if (!outliner_stack_focus_object_carry(*C, space_outliner)) {
    return false;
  }
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

int outliner_stack_active_ordinal_get(const StackReadContext &ctx, SpaceOutliner &space_outliner)
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

bool outliner_stack_can_reorder(const bContext &C, SpaceOutliner &space_outliner)
{
  const StackReadContext ctx = outliner_stack_read_context(C);
  const ID *owner = outliner_stack_owner_get(ctx, space_outliner);
  const StackEditor *editor = stack_source_for_space(space_outliner)->editor();
  return owner != nullptr && editor != nullptr && editor->can_reorder(*owner);
}

}  // namespace blender::ed::outliner
