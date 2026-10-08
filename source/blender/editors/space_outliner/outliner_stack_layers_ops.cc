/* SPDX-FileCopyrightText: 2026 Blender Authors
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
#include <cstdio>
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
#include "WM_types.hh"

#include "outliner_intern.hh"
#include "tree/tree_iterator.hh"
#include "outliner_stack_source.hh"

namespace blender::ed::outliner {

/**
 * The identity property every operator that addresses an existing row shares: the row's marker,
 * which survives the renumbering an edit puts the position through. Read through
 * #stack_operator_ordinal_get, so a caller names a row either way.
 */
static void rna_def_stack_row_marker(wmOperatorType *ot)
{
  RNA_def_string(ot->srna,
                 "marker",
                 nullptr,
                 0,
                 "Marker",
                 "The row's identity marker, as this mode issued it; takes precedence over the "
                 "position, which is only the row's at the time of the call");
}

void OUTLINER_OT_stack_layer_focus(wmOperatorType *ot)
{
  ot->name = "Focus Stack";
  ot->idname = "OUTLINER_OT_stack_layer_focus";
  ot->description = "Show the layer stack of an object in the Outliner";
  ot->exec = stack_focus_exec;
  ot->poll = ED_operator_outliner_active;
  ot->flag = OPTYPE_REGISTER;

  RNA_def_string(ot->srna, "object", nullptr, MAX_ID_NAME - 2, "Object", "Object to focus");
  RNA_def_int(ot->srna,
              "sub_index",
              -1,
              -1,
              SHRT_MAX,
              "Index",
              "Stack index within the object, such as a material slot; -1 uses the active one",
              -1,
              SHRT_MAX);
  RNA_def_boolean(ot->srna,
                  "enter_edit_mode",
                  true,
                  "Enter Edit Mode",
                  "Enter the edit mode the source works in");
}

void OUTLINER_OT_stack_layers_back(wmOperatorType *ot)
{
  ot->name = "Back to Stack Objects";
  ot->idname = "OUTLINER_OT_stack_layers_back";
  ot->description =
      "Return to the View Layer; hold Alt to stay and show the objects that have a layer stack";
  ot->invoke = stack_back_invoke;
  ot->exec = stack_back_exec;
  ot->poll = ED_operator_outliner_active;
  ot->flag = OPTYPE_REGISTER;
  RNA_def_boolean(ot->srna,
                  "keep_view",
                  false,
                  "Keep Mode",
                  "Stay in Stack Layers and show the objects that have a layer stack, rather "
                  "than leaving the mode");
}

void OUTLINER_OT_stack_layer_pin_toggle(wmOperatorType *ot)
{
  ot->name = "Pin Stack";
  ot->idname = "OUTLINER_OT_stack_layer_pin_toggle";
  ot->description = "Keep the focused stack when the active object changes";
  ot->exec = stack_pin_toggle_exec;
  ot->poll = ED_operator_outliner_active;
  ot->flag = OPTYPE_REGISTER;
}

void OUTLINER_OT_stack_layer_activate(wmOperatorType *ot)
{
  ot->name = "Activate Stack Layer";
  ot->idname = "OUTLINER_OT_stack_layer_activate";
  ot->description = "Make a stack layer the one the rest of Blender acts on";
  ot->exec = stack_row_activate_exec;
  ot->poll = stack_row_activate_poll;
  ot->flag = OPTYPE_UNDO | OPTYPE_INTERNAL;

  RNA_def_int(ot->srna,
              "ordinal",
              0,
              0,
              SHRT_MAX,
              "Ordinal",
              "Position of the layer in the stack",
              0,
              SHRT_MAX);
  rna_def_stack_row_marker(ot);
}

/* -------------------------------------------------------------------- */
/** \name Stack Layer Preview Section Activate Operator
 * \{ */

static wmOperatorStatus stack_preview_section_activate_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner == nullptr) {
    return OPERATOR_CANCELLED;
  }

  const int ordinal = RNA_int_get(op->ptr, "ordinal");
  char section_id[64];
  RNA_string_get(op->ptr, "section_id", section_id);

  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  if (owner == nullptr) {
    return OPERATOR_CANCELLED;
  }
  outliner_stack_rows_ensure(ctx, *space_outliner, *owner);
  const StackRow *row = outliner_stack_row_find(*space_outliner, ordinal);
  if (row == nullptr) {
    return OPERATOR_CANCELLED;
  }

  /* A preview click is also a layer click: bind its texture maps as the paint target and make its
   * row the Outliner's active selection before showing the requested content section. */
  if (!outliner_stack_row_activate(C, *space_outliner, ordinal)) {
    return OPERATOR_CANCELLED;
  }
  tree_iterator::all(*space_outliner, [&](TreeElement *te) {
    TreeStoreElem *tselem = TREESTORE(te);
    if (tselem->type == TSE_STACK_LAYER && int(tselem->nr) == ordinal) {
      outliner_item_select(C, space_outliner, te, OL_ITEM_SELECT | OL_ITEM_ACTIVATE);
    }
  });

  /* A row whose source declares no content sections keeps its flat content rather than a
   * switchable section. Its preview click must activate the row without failing. */
  if (!row->content_sections.is_empty() &&
      !outliner_stack_row_active_section_set(*space_outliner, *row, section_id))
  {
    return OPERATOR_CANCELLED;
  }
  outliner_stack_row_preview_activate(C, *space_outliner, ordinal, section_id);

  ED_region_tag_redraw(CTX_wm_region(C));
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_OUTLINER, nullptr);

  return OPERATOR_FINISHED;
}

static bool stack_preview_section_activate_poll(bContext *C)
{
  if (!ED_operator_outliner_active(C)) {
    return false;
  }
  const SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  return space_outliner != nullptr && space_outliner->outlinevis == SO_STACK_LAYERS;
}

void OUTLINER_OT_stack_preview_section_activate(wmOperatorType *ot)
{
  ot->name = "Activate Preview Section";
  ot->idname = "OUTLINER_OT_stack_preview_section_activate";
  ot->description = "Switch the active content section";
  ot->exec = stack_preview_section_activate_exec;
  ot->poll = stack_preview_section_activate_poll;
  ot->flag = OPTYPE_INTERNAL;

  RNA_def_int(ot->srna,
              "ordinal",
              0,
              0,
              SHRT_MAX,
              "Ordinal",
              "Position of the layer in the stack",
              0,
              SHRT_MAX);
  RNA_def_string(ot->srna,
                 "section_id",
                 nullptr,
                 64,
                 "Section ID",
                 "Identifier of the content section to activate, one the row's source declares");
  rna_def_stack_row_marker(ot);
}

/** \} */

void OUTLINER_OT_stack_layer_clear_target(wmOperatorType *ot)
{
  ot->name = "Clear Stack Target";
  ot->idname = "OUTLINER_OT_stack_layer_clear_target";
  ot->description = "Stop directing edits at the activated stack layer";
  ot->exec = stack_target_clear_exec;
  ot->poll = ED_operator_outliner_active;
  ot->flag = OPTYPE_UNDO | OPTYPE_INTERNAL;
}

void OUTLINER_OT_stack_layer_move(wmOperatorType *ot)
{
  static const EnumPropertyItem direction_items[] = {
      {0, "UP", 0, "Up", "Move the layer towards the top of the stack"},
      {1, "DOWN", 0, "Down", "Move the layer towards the bottom of the stack"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  ot->name = "Move Stack Layer";
  ot->idname = "OUTLINER_OT_stack_layer_move";
  ot->description = "Change the position of a layer in the stack";
  ot->exec = stack_row_move_exec;
  ot->poll = stack_row_reorder_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_int(ot->srna,
              "ordinal",
              -1,
              -1,
              SHRT_MAX,
              "Ordinal",
              "Layer to move; -1 uses the active one",
              -1,
              SHRT_MAX);
  RNA_def_enum(ot->srna, "direction", direction_items, 0, "Direction", "Which way to move it");
  RNA_def_int(ot->srna,
              "to_ordinal",
              -1,
              -1,
              SHRT_MAX,
              "Target",
              "Position to move the layer to; -1 uses the direction instead",
              -1,
              SHRT_MAX);
  rna_def_stack_row_marker(ot);
}

void OUTLINER_OT_stack_layer_copy(wmOperatorType *ot)
{
  ot->name = "Copy Stack Layers";
  ot->idname = "OUTLINER_OT_stack_layer_copy";
  ot->description = "Copy the selected stack layers to the Stack Layers clipboard";
  ot->exec = stack_row_copy_exec;
  ot->poll = stack_row_clipboard_poll;
}

void OUTLINER_OT_stack_layer_paste(wmOperatorType *ot)
{
  ot->name = "Paste Stack Layers";
  ot->idname = "OUTLINER_OT_stack_layer_paste";
  ot->description = "Paste copied stack layers above the active layer";
  ot->exec = stack_row_paste_exec;
  ot->poll = stack_row_clipboard_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

void OUTLINER_OT_stack_layer_add(wmOperatorType *ot)
{
  ot->name = "Add Stack Layer";
  ot->idname = "OUTLINER_OT_stack_layer_add";
  ot->description = "Add a layer to the stack";
  ot->exec = stack_row_add_exec;
  ot->invoke = stack_row_add_invoke;
  ot->ui = stack_row_add_ui;
  ot->poll = stack_add_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* What the kinds are is the source's own vocabulary, built into items as the menu asks for
   * them; a source that declares none is polled out entirely. */
  PropertyRNA *prop = RNA_def_enum(
      ot->srna, "type", rna_enum_dummy_NULL_items, 0, "Type", "What the new layer holds");
  RNA_def_enum_funcs(prop, stack_add_type_itemf);
  RNA_def_int(ot->srna,
              "ordinal",
              -1,
              -1,
              SHRT_MAX,
              "Ordinal",
              "Row to anchor the new layer to; it lands above that row, or inside it when the row "
              "is a folder. -1 uses the selected or active row, or the top of the stack",
              -1,
              SHRT_MAX);
  /* Read only by the kinds that asked for a colour; the others leave it at its white default. */
  static const float fill_color_default[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  PropertyRNA *fill_prop = RNA_def_float_color(ot->srna,
                                               "fill_color",
                                               4,
                                               fill_color_default,
                                               0.0f,
                                               FLT_MAX,
                                               "Fill Color",
                                               "Color a fill layer starts out filled with",
                                               0.0f,
                                               1.0f);
  RNA_def_property_subtype(fill_prop, PROP_COLOR_GAMMA);
  /* #layout.tag_button writes its tag name into every operator it attaches; the Add never reads
   * it, but the property keeps that write from warning on every redraw. */
  PropertyRNA *tag_prop = RNA_def_string(ot->srna,
                                             "tag_name",
                                             nullptr,
                                             0,
                                             "Tag Name",
                                             "Tag the button that placed the call belongs to");
  RNA_def_property_flag(tag_prop, PROP_HIDDEN);
  /* The data-block a kind with #StackAddKindInfo::source_id_type is made from, by name: what a UI
   * picked, and what a repeat or a script names again. Bounded, since the exec reads it into an ID
   * name buffer. */
  PropertyRNA *source_prop = RNA_def_string(ot->srna,
                                            "source",
                                            nullptr,
                                            MAX_ID_NAME - 2,
                                            "Source",
                                            "Name of the data-block the new layer is made from, "
                                            "for a kind that is made from one");
  RNA_def_property_flag(source_prop, PROP_HIDDEN);
  rna_def_stack_row_marker(ot);
}

void OUTLINER_OT_stack_focus_sub_index(wmOperatorType *ot)
{
  ot->name = "Set Stack";
  ot->idname = "OUTLINER_OT_stack_focus_sub_index";
  ot->description = "Choose which of the object's stacks to show, such as a material slot";
  ot->exec = stack_sub_index_set_exec;
  ot->invoke = WM_enum_search_invoke;
  ot->poll = stack_sub_index_set_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  PropertyRNA *prop = RNA_def_enum(
      ot->srna, "sub_index", rna_enum_dummy_NULL_items, 0, "Stack", "");
  RNA_def_enum_funcs(prop, stack_sub_index_itemf);
  RNA_def_property_flag(prop, PROP_ENUM_NO_TRANSLATE);
  ot->prop = prop;
}

void OUTLINER_OT_stack_layer_group(wmOperatorType *ot)
{
  ot->name = "Group Stack Layers";
  ot->idname = "OUTLINER_OT_stack_layer_group";
  ot->description = "Put a run of layers into a group, composited on its own and laid over the "
                    "rest as one layer";
  ot->exec = stack_rows_group_exec;
  ot->poll = stack_row_add_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_int(ot->srna,
              "ordinal",
              -1,
              -1,
              SHRT_MAX,
              "Ordinal",
              "Bottom layer of the run; -1 uses the active one",
              -1,
              SHRT_MAX);
  RNA_def_int(ot->srna,
              "to_ordinal",
              -1,
              -1,
              SHRT_MAX,
              "Top",
              "Top layer of the run; -1 groups the bottom layer on its own",
              -1,
              SHRT_MAX);
  rna_def_stack_row_marker(ot);
}

void OUTLINER_OT_stack_layer_group_add(wmOperatorType *ot)
{
  ot->name = "Add Layer Group";
  ot->idname = "OUTLINER_OT_stack_layer_group_add";
  ot->description = "Add an empty group above the active layer, to put layers into";
  ot->exec = stack_group_add_exec;
  ot->poll = stack_row_add_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_int(ot->srna,
              "ordinal",
              -1,
              -1,
              SHRT_MAX,
              "Ordinal",
              "Layer to sit above; -1 uses the active row",
              -1,
              SHRT_MAX);
  rna_def_stack_row_marker(ot);
}

/**
 * Whether the row an ungroup would land on is a group.
 *
 * Only a group folder can be unwrapped, and the context menu leans on this poll to show the entry
 * only where it applies. The poll cannot read the operator's marker, so it answers for what the
 * operator will do without one: the selection if there is one, the active row otherwise. The exec
 * still takes a marker, and the grouping editor refuses an ungroup for anything that is not a
 * group.
 */
bool stack_row_ungroup_poll(bContext *C)
{
  if (!stack_row_add_poll(C)) {
    return false;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  Vector<int> selected;
  stack_selected_ordinals_get(*space_outliner, selected);
  const int ordinal = !selected.is_empty() ? selected.first() :
                                             outliner_stack_active_ordinal_get(
                                                 outliner_stack_read_context(*C), *space_outliner);
  const StackRow *row = (ordinal < 0) ?
                                    nullptr :
                                    outliner_stack_row_find(*space_outliner, ordinal);
  return row != nullptr && row->can_hold_children;
}

void OUTLINER_OT_stack_layer_ungroup(wmOperatorType *ot)
{
  ot->name = "Ungroup Stack Layers";
  ot->idname = "OUTLINER_OT_stack_layer_ungroup";
  ot->description = "Put the layers of a group back into the stack around it";
  ot->exec = stack_row_ungroup_exec;
  ot->poll = stack_row_ungroup_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_int(ot->srna,
              "ordinal",
              -1,
              -1,
              SHRT_MAX,
              "Ordinal",
              "Group to unwrap; -1 uses the active row",
              -1,
              SHRT_MAX);
  rna_def_stack_row_marker(ot);
}

/**
 * Whether the row a color tag would land on is a group.
 *
 * Color tags belong to group folders, and the UI that offers them -- the context menu's color
 * swatches -- leans on this poll to appear only where they apply. The poll cannot read the
 * operator's marker, so it answers for what the operator will do without one: the selection if
 * there is one, the active row otherwise. The exec still takes a marker, and the source refuses
 * a tag for anything that is not a group.
 */
bool stack_row_color_tag_poll(bContext *C)
{
  if (!stack_row_add_poll(C)) {
    return false;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  Vector<int> selected;
  stack_selected_ordinals_get(*space_outliner, selected);
  const int ordinal = !selected.is_empty() ? selected.first() :
                                             outliner_stack_active_ordinal_get(
                                                 outliner_stack_read_context(*C), *space_outliner);
  const StackRow *row = (ordinal < 0) ?
                                    nullptr :
                                    outliner_stack_row_find(*space_outliner, ordinal);
  return row != nullptr && row->can_hold_children;
}

static wmOperatorStatus stack_row_color_tag_set_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  if (ordinal < 0) {
    return OPERATOR_CANCELLED;
  }
  const int color_tag = RNA_enum_get(op->ptr, "color");
  return outliner_stack_row_color_tag_set(C, *space_outliner, ordinal, color_tag) ?
             OPERATOR_FINISHED :
             OPERATOR_CANCELLED;
}

void OUTLINER_OT_stack_layer_color_tag_set(wmOperatorType *ot)
{
  /* Match collection_color_tag_set's color enum: 8 colors, NONE at index 0 (becomes -1). */
  static const EnumPropertyItem color_items[] = {
      {-1, "NONE", ICON_X, "None", ""},
      {0, "COLOR_01", ICON_COLLECTION_COLOR_01, "Color 01", ""},
      {1, "COLOR_02", ICON_COLLECTION_COLOR_02, "Color 02", ""},
      {2, "COLOR_03", ICON_COLLECTION_COLOR_03, "Color 03", ""},
      {3, "COLOR_04", ICON_COLLECTION_COLOR_04, "Color 04", ""},
      {4, "COLOR_05", ICON_COLLECTION_COLOR_05, "Color 05", ""},
      {5, "COLOR_06", ICON_COLLECTION_COLOR_06, "Color 06", ""},
      {6, "COLOR_07", ICON_COLLECTION_COLOR_07, "Color 07", ""},
      {7, "COLOR_08", ICON_COLLECTION_COLOR_08, "Color 08", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  ot->name = "Set Layer Group Color";
  ot->idname = "OUTLINER_OT_stack_layer_color_tag_set";
  ot->description = "Set the color tag of a layer group folder";
  ot->exec = stack_row_color_tag_set_exec;
  ot->poll = stack_row_color_tag_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_enum(ot->srna, "color", color_items, -1, "Color", "Color tag for the group");
  RNA_def_int(ot->srna,
              "ordinal",
              -1,
              -1,
              SHRT_MAX,
              "Ordinal",
              "Group to set color for; -1 uses the active row",
              -1,
              SHRT_MAX);
  rna_def_stack_row_marker(ot);
}

/**
 * Whether the row a fill color would land on is a fill row.
 *
 * A re-fill is refused for any row that does not stand for a colour, and the context menu leans on
 * this poll to show the entry only where it applies. The poll cannot read the operator's marker,
 * so it answers for what the operator will do without one: the selection if there is one, the
 * active row otherwise. The row's colour slot is what says it is a fill -- the source drew it
 * there for exactly this question.
 */
bool stack_row_fill_color_poll(bContext *C)
{
  if (!stack_row_add_poll(C)) {
    return false;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  Vector<int> selected;
  stack_selected_ordinals_get(*space_outliner, selected);
  const int ordinal = !selected.is_empty() ? selected.first() :
                                             outliner_stack_active_ordinal_get(
                                                 outliner_stack_read_context(*C), *space_outliner);
  const StackRow *row = (ordinal < 0) ?
                                    nullptr :
                                    outliner_stack_row_find(*space_outliner, ordinal);
  if (row == nullptr) {
    return false;
  }
  for (const StackRowPreview &slot : row->preview_slots) {
    if (slot.is_color_swatch) {
      return true;
    }
  }
  return false;
}

/**
 * What the Fill color picker remembers while its dialog is open: the source's session, whose
 * "before" pixels everything rolls back to or commits from, and whether any preview tick ran. No
 * ordinal lives here -- the target is re-resolved through #stack_operator_ordinal_get on every
 * phase, with the marker first.
 *
 * Owned by the operator: invoke allocates, exec and cancel free. File-static state is banned
 * here -- popups of different windows are not mutually exclusive. The editor is the source's own
 * singleton, so keeping it here is keeping no per-dialog state: it only says whose session the
 * pointer is, so exec and cancel free and commit through the same vocabulary that made it.
 */
struct FillColorPreviewData {
  const StackColorEditor *color_editor = nullptr;
  StackColorSession *session = nullptr;
  bool did_preview = false;
};

/** The source's color vocabulary, or null when its rows have no fill to re-fill. */
static const StackColorEditor *stack_fill_color_editor_get(SpaceOutliner &space_outliner)
{
  const StackEditor *editor = stack_source_for_space(space_outliner)->editor();
  return (editor != nullptr) ? editor->color() : nullptr;
}

static void stack_row_fill_color_preview_data_free(FillColorPreviewData *preview_data)
{
  if (preview_data == nullptr) {
    return;
  }
  if (preview_data->color_editor != nullptr) {
    preview_data->color_editor->color_session_free(preview_data->session);
  }
  MEM_delete(preview_data);
}

static wmOperatorStatus stack_row_fill_color_set_exec(bContext *C, wmOperator *op)
{
  /* The commit owns two history entries, pixels on top: the memfile step covers the marker
   * (and the DNA around it), the source's own entry the texture -- which the memfile alone
   * cannot restore, since live image pixels of unpacked images are not part of it (the same
   * reason every paint operator pushes image undo rather than relying on global undo). The
   * first Ctrl+Z therefore lands on the texture, the second on the marker. Deliberately no
   * #OPTYPE_UNDO on this operator: the automatic push would land on top of the source's entry
   * and the first undo would revert the marker while leaving the texture painted. */
  FillColorPreviewData *preview_data = static_cast<FillColorPreviewData *>(op->customdata);
  op->customdata = nullptr;

  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  wmOperatorStatus status = OPERATOR_CANCELLED;
  if (ordinal >= 0) {
    float gamma_color[4];
    float color[4];
    RNA_float_get_array(op->ptr, "color", gamma_color);
    srgb_to_linearrgb_v4(color, gamma_color);
    /* Without a dialog around it (a scripted call) there is no session yet: a throwaway one
     * stands in, so the commit below still covers the texture. */
    const StackColorEditor *color_editor = (preview_data != nullptr) ?
                                               preview_data->color_editor :
                                               stack_fill_color_editor_get(*space_outliner);
    StackColorSession *commit_session = (preview_data != nullptr) ? preview_data->session :
                                                                    nullptr;
    StackColorSession *owned_session = nullptr;
    if (commit_session == nullptr && color_editor != nullptr) {
      owned_session = color_editor->color_session_new();
      commit_session = owned_session;
    }
    if (outliner_stack_row_fill_color_set(C, *space_outliner, ordinal, color, commit_session)) {
      status = OPERATOR_FINISHED;
      /* No ticks, no pixels moved: an OK straight away commits no history entries. */
      if ((preview_data != nullptr && preview_data->did_preview) || preview_data == nullptr) {
        ED_undo_push(C, op->type->name);
        if (color_editor != nullptr && commit_session != nullptr) {
          color_editor->color_session_push_undo(*commit_session, op->type->name);
        }
      }
    }
    else if (commit_session != nullptr && color_editor != nullptr) {
      /* A refused bake must not strand preview pixels without history: put back whatever the
       * session captured. */
      color_editor->color_session_restore(*commit_session);
    }
    if (owned_session != nullptr && color_editor != nullptr) {
      color_editor->color_session_free(owned_session);
    }
  }
  stack_row_fill_color_preview_data_free(preview_data);
  return status;
}

static wmOperatorStatus stack_row_fill_color_set_invoke(bContext *C,
                                                        wmOperator *op,
                                                        const wmEvent * /*event*/)
{
  /* The dialog starts at the colour the row stands for now, which the source already put in the
   * row's swatch -- a picker that opens at white is a picker that loses the current colour. */
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  /* One resolution of the row for both the starting colour and the dialog anchor: the swatch
   * click, the context menu and scripts all address it through the same ordinal. */
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  PropertyRNA *color_prop = RNA_struct_find_property(op->ptr, "color");
  if (!RNA_property_is_set(op->ptr, color_prop)) {
    if (const StackRow *row = (ordinal < 0) ?
                                  nullptr :
                                  outliner_stack_row_find(*space_outliner, ordinal))
    {
      for (const StackRowPreview &slot : row->preview_slots) {
        if (slot.is_color_swatch) {
          /* The swatch is scene linear; the picker property is gamma. */
          float picker_color[4];
          linearrgb_to_srgb_v4(picker_color, slot.color);
          RNA_property_float_set_array(op->ptr, color_prop, picker_color);
          break;
        }
      }
    }
  }
  /* The session owns its "before" pixels from here on: the first preview tick captures the
   * pristine canvas into it (later ticks keep those originals), the commit turns it into one
   * undo entry, the cancel restores from it. */
  FillColorPreviewData *preview_data = MEM_new<FillColorPreviewData>(__func__);
  preview_data->color_editor = stack_fill_color_editor_get(*space_outliner);
  preview_data->session = (preview_data->color_editor != nullptr) ?
                              preview_data->color_editor->color_session_new() :
                              nullptr;
  preview_data->did_preview = false;
  op->customdata = preview_data;
  /* Open beside the row's swatch, not on top of it: the picker must not hide the colour it
   * edits. Without an anchor -- no region, a tree the draw has not laid out yet, a row the tree
   * does not name -- keep the old at-the-mouse placement. */
  std::optional<rcti> anchor_rect;
  if (ARegion *region = CTX_wm_region(C)) {
    rcti rect;
    if (ordinal >= 0 &&
        outliner_stack_row_fill_swatch_anchor_rect(*space_outliner, *region, ordinal, rect))
    {
      anchor_rect = rect;
    }
  }
  return WM_operator_props_dialog_popup(C,
                                        op,
                                        260,
                                        IFACE_("Fill Color"),
                                        IFACE_("Fill"),
                                        false,
                                        std::nullopt,
                                        false,
                                        std::move(anchor_rect));
}

/**
 * The operator behind a Fill color picker properties change, resolved the way the glyph picker
 * resolves its own target: the running operators owning these properties first, the block's
 * active operator (which the dialog registers for its child popups) second.
 */
static wmOperator *stack_row_fill_color_op_from_properties(bContext *C, const PointerRNA *ptr)
{
  if (ptr == nullptr || ptr->data == nullptr) {
    return nullptr;
  }
  if (wmWindowManager *wm = CTX_wm_manager(C)) {
    for (wmOperator *op = static_cast<wmOperator *>(wm->runtime->operators.last); op;
         op = op->prev)
    {
      if (op != nullptr && op->properties == ptr->data) {
        return op;
      }
    }
  }
  if (wmOperator *active_op = ui::context_active_operator_get(C)) {
    if (active_op->ptr != nullptr && active_op->ptr->data == ptr->data) {
      return active_op;
    }
  }
  return nullptr;
}

/** A picker tick: show the colour on the texture now, record nothing, push no undo. */
static void stack_row_fill_color_preview_update(bContext *C,
                                                 PointerRNA *ptr,
                                                 PropertyRNA * /*prop*/)
{
  wmOperator *op = stack_row_fill_color_op_from_properties(C, ptr);
  if (op == nullptr || op->type == nullptr ||
      !STREQ(op->type->idname, "OUTLINER_OT_stack_layer_fill_color_set"))
  {
    return;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner == nullptr) {
    return;
  }
  FillColorPreviewData *preview_data = static_cast<FillColorPreviewData *>(op->customdata);
  if (preview_data == nullptr) {
    return;
  }
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  if (ordinal < 0) {
    return;
  }
  float gamma_color[4];
  float linear_color[4];
  RNA_float_get_array(ptr, "color", gamma_color);
  srgb_to_linearrgb_v4(linear_color, gamma_color);
  /* A refused tick stays silent: this runs per drag motion, and every tick reporting would spam
   * the status bar. */
  if (!outliner_stack_row_fill_color_preview(
          C, *space_outliner, ordinal, linear_color, preview_data->session))
  {
    return;
  }
  preview_data->did_preview = true;
  ED_region_tag_redraw(CTX_wm_region(C));
}

/** Esc / close without confirming: put the pristine pixels back, write no history. */
static void stack_row_fill_color_set_cancel(bContext *C, wmOperator *op)
{
  FillColorPreviewData *preview_data = static_cast<FillColorPreviewData *>(op->customdata);
  op->customdata = nullptr;
  if (preview_data == nullptr) {
    return;
  }
  /* Nothing was ever pushed onto the undo stack for this session, so there is nothing to
   * discard either -- and unlike a flat re-fill with the start colour, the restore keeps
   * brushwork the layer already carried when the picker opened. */
  if (preview_data->color_editor != nullptr && preview_data->session != nullptr) {
    preview_data->color_editor->color_session_restore(*preview_data->session);
    ED_region_tag_redraw(CTX_wm_region(C));
  }
  stack_row_fill_color_preview_data_free(preview_data);
}

/** The dialog is the picker: the full standard picker -- wheel, channel sliders, hex, eyedropper
 * and the paint palette -- builds right into it, and every edit flows through the property's
 * update callback below. */
static void stack_row_fill_color_set_ui(bContext *C, wmOperator *op)
{
  template_color_picker_full(C, op->layout, op->ptr, "color", true);
}

void OUTLINER_OT_stack_layer_fill_color_set(wmOperatorType *ot)
{
  ot->name = "Set Fill Color";
  ot->idname = "OUTLINER_OT_stack_layer_fill_color_set";
  ot->description = "Re-fill a fill layer with a new color, replacing what was drawn on it";
  ot->exec = stack_row_fill_color_set_exec;
  ot->invoke = stack_row_fill_color_set_invoke;
  ot->ui = stack_row_fill_color_set_ui;
  ot->poll = stack_row_fill_color_poll;
  ot->cancel = stack_row_fill_color_set_cancel;
  /* Deliberately no OPTYPE_UNDO: exec pushes its history itself -- a memfile step for the
   * marker, then the source's own undo entry for the texture on top, so the first Ctrl+Z lands
   * on the texture. The automatic push would land on top of that entry instead. */
  ot->flag = OPTYPE_REGISTER;

  static const float color_default[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  PropertyRNA *prop = RNA_def_float_color(ot->srna,
                                          "color",
                                          4,
                                          color_default,
                                          0.0f,
                                          FLT_MAX,
                                          "Color",
                                          "Color the fill layer stands for",
                                          0.0f,
                                          1.0f);
  RNA_def_property_subtype(prop, PROP_COLOR_GAMMA);
  /* Live texture preview per picker tick (pixels only, no marker, no undo); the bake stays in
   * exec, the rollback in cancel. A plain string update cannot deliver the context this needs,
   * hence the runtime callback. */
  RNA_def_property_update_runtime_with_context_and_property(
      prop, stack_row_fill_color_preview_update);
  RNA_def_int(ot->srna,
              "ordinal",
              -1,
              -1,
              SHRT_MAX,
              "Ordinal",
              "Fill layer to re-fill; -1 uses the active row",
              -1,
              SHRT_MAX);
  rna_def_stack_row_marker(ot);
}

void OUTLINER_OT_stack_layer_merge_down(wmOperatorType *ot)
{
  ot->name = "Merge Layer Down";
  ot->idname = "OUTLINER_OT_stack_layer_merge_down";
  ot->description =
      "Collapse a layer and the one below it into one group row that composites just the same";
  ot->exec = stack_row_merge_down_exec;
  ot->poll = stack_row_add_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_int(ot->srna,
              "ordinal",
              -1,
              -1,
              SHRT_MAX,
              "Ordinal",
              "Layer to merge into the one below it; -1 uses the active one",
              -1,
              SHRT_MAX);
  rna_def_stack_row_marker(ot);
}

void OUTLINER_OT_stack_layer_duplicate(wmOperatorType *ot)
{
  ot->name = "Duplicate Stack Layer";
  ot->idname = "OUTLINER_OT_stack_layer_duplicate";
  ot->description = "Copy a layer, maps and all, and put the copy above it";
  ot->exec = stack_row_duplicate_exec;
  ot->poll = stack_row_add_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_int(ot->srna,
              "ordinal",
              -1,
              -1,
              SHRT_MAX,
              "Ordinal",
              "Layer to copy; -1 uses the active one",
              -1,
              SHRT_MAX);
  rna_def_stack_row_marker(ot);
}

void OUTLINER_OT_stack_layer_rename(wmOperatorType *ot)
{
  ot->name = "Rename Stack Layer";
  ot->idname = "OUTLINER_OT_stack_layer_rename";
  ot->description = "Give a layer of the stack a new name";
  ot->exec = stack_row_rename_exec;
  ot->invoke = stack_row_rename_invoke;
  ot->poll = stack_row_add_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_int(ot->srna,
              "ordinal",
              -1,
              -1,
              SHRT_MAX,
              "Ordinal",
              "Layer to rename; -1 uses the active one",
              -1,
              SHRT_MAX);
  RNA_def_string(ot->srna, "name", nullptr, MAX_NAME, "Name", "New name for the layer");
  rna_def_stack_row_marker(ot);
}

void OUTLINER_OT_stack_layer_mask(wmOperatorType *ot)
{
  static const EnumPropertyItem initial_color_items[] = {
      {0, "WHITE", 0, "Add White Mask", "Add a mask filled with white"},
      {1, "BLACK", 0, "Add Black Mask", "Add a mask filled with black"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  ot->name = "Stack Layer Mask";
  ot->idname = "OUTLINER_OT_stack_layer_mask";
  ot->description = "Add or remove the mask that modulates a layer";
  ot->exec = stack_row_mask_exec;
  ot->poll = stack_row_mask_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_int(ot->srna,
              "ordinal",
              -1,
              -1,
              SHRT_MAX,
              "Ordinal",
              "Layer to mask; -1 uses the active one",
              -1,
              SHRT_MAX);
  RNA_def_boolean(
      ot->srna, "add", true, "Add", "Add a mask, rather than removing the one already there");
  RNA_def_enum(ot->srna,
               "initial_color",
               initial_color_items,
               0,
               "Initial Color",
               "Initial fill color for a newly added mask");
  rna_def_stack_row_marker(ot);
}

void OUTLINER_OT_stack_layer_mask_toggle(wmOperatorType *ot)
{
  ot->name = "Toggle Stack Layer Mask";
  ot->idname = "OUTLINER_OT_stack_layer_mask_toggle";
  ot->description =
      "Turn a layer's mask on or off, keeping the mask image and its paint content";
  ot->exec = stack_row_mask_toggle_exec;
  ot->poll = stack_row_mask_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_int(ot->srna,
              "ordinal",
              -1,
              -1,
              SHRT_MAX,
              "Ordinal",
              "Layer whose mask to toggle; -1 uses the active one",
              -1,
              SHRT_MAX);
  rna_def_stack_row_marker(ot);
}

void OUTLINER_OT_stack_layer_visibility_toggle(wmOperatorType *ot)
{
  ot->name = "Toggle Stack Layer Visibility";
  ot->idname = "OUTLINER_OT_stack_layer_visibility_toggle";
  /* Phrased as an edit rather than a view setting on purpose: for a paint stack this mutes the
   * layer's nodes, so it changes what renders. */
  ot->description = "Turn a layer of the stack on or off";
  ot->exec = stack_row_visibility_toggle_exec;
  ot->poll = stack_row_visibility_toggle_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_int(ot->srna,
              "ordinal",
              -1,
              -1,
              SHRT_MAX,
              "Ordinal",
              "Layer to toggle; -1 uses the active one",
              -1,
              SHRT_MAX);
  rna_def_stack_row_marker(ot);
}

void OUTLINER_OT_stack_layer_remove(wmOperatorType *ot)
{
  ot->name = "Remove Stack Layer";
  ot->idname = "OUTLINER_OT_stack_layer_remove";
  ot->description = "Delete a layer from the stack";
  ot->exec = stack_row_remove_exec;
  ot->poll = stack_row_remove_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_int(ot->srna,
              "ordinal",
              -1,
              -1,
              SHRT_MAX,
              "Ordinal",
              "Layer to remove; -1 uses the active one",
              -1,
              SHRT_MAX);
  rna_def_stack_row_marker(ot);
}

}  // namespace blender::ed::outliner
