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

#include <cfloat>
#include <climits>
#include <string>

#include "DNA_ID.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_context.hh"

#include "BLI_string.h"

#include "ED_screen.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../outliner_intern.hh"
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

/** The `ordinal` property of a row operator; \a description says what -1 means for it. */
static void rna_def_stack_row_ordinal(wmOperatorType *ot, const char *description)
{
  RNA_def_int(ot->srna, "ordinal", -1, -1, SHRT_MAX, "Ordinal", description, -1, SHRT_MAX);
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
  RNA_def_string(ot->srna,
                 "library",
                 nullptr,
                 MAX_ID_NAME - 2,
                 "Library",
                 "Name of the library the object is linked from; empty for a local object");
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

static wmOperatorStatus stack_row_action_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  if (ordinal < 0) {
    return OPERATOR_CANCELLED;
  }
  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  const StackRow *row = (owner != nullptr) ? outliner_stack_row_find(*space_outliner, ordinal) :
                                             nullptr;
  const StackEditor *editor = stack_source_for_space(*space_outliner)->editor();
  if (row == nullptr || editor == nullptr || !editor->row_action(*C, *owner, ordinal, *row)) {
    return OPERATOR_CANCELLED;
  }
  ED_region_tag_redraw(CTX_wm_region(C));
  return OPERATOR_FINISHED;
}

void OUTLINER_OT_stack_row_action(wmOperatorType *ot)
{
  ot->name = "Row Action";
  ot->idname = "OUTLINER_OT_stack_row_action";
  ot->description = "Run the row's own action";
  ot->exec = stack_row_action_exec;
  ot->poll = ED_operator_outliner_active;
  /* No undo: the action is UI state, not data. */
  ot->flag = OPTYPE_INTERNAL;
  RNA_def_int(ot->srna, "ordinal", -1, -1, SHRT_MAX, "Ordinal", "", -1, SHRT_MAX);
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

/**
 * The name a button for one kind shows: the kinds are the source's own vocabulary, so the operator
 * cannot name them itself, and two icon buttons that both read "Add Stack Layer" cannot be told
 * apart. The caller that knows the kind passes its name in `label`.
 */
static std::string stack_row_add_get_name(wmOperatorType * /*ot*/, PointerRNA *ptr)
{
  if (ptr == nullptr || !RNA_struct_property_is_set(ptr, "label")) {
    return "";
  }
  return RNA_string_get(ptr, "label");
}

void OUTLINER_OT_stack_layer_add(wmOperatorType *ot)
{
  ot->name = "Add Stack Layer";
  ot->idname = "OUTLINER_OT_stack_layer_add";
  ot->description = "Add a layer to the stack";
  ot->get_name = stack_row_add_get_name;
  ot->exec = stack_row_add_exec;
  ot->invoke = stack_row_add_invoke;
  ot->ui = stack_row_add_ui;
  ot->poll = stack_add_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* What the kinds are is the source's own vocabulary, built into items as the menu asks for them;
   * a source that declares none is polled out entirely. */
  PropertyRNA *prop = RNA_def_enum(
      ot->srna, "type", rna_enum_dummy_NULL_items, 0, "Type", "What the new layer holds");
  RNA_def_enum_funcs(prop, stack_add_type_itemf);
  PropertyRNA *label_prop = RNA_def_string(ot->srna,
                                           "label",
                                           nullptr,
                                           64,
                                           "Label",
                                           "Name shown for this button, e.g. \"Add Fill Layer\"");
  RNA_def_property_flag(label_prop, PROP_HIDDEN | PROP_SKIP_SAVE);
  RNA_def_int(ot->srna,
              "ordinal",
              -1,
              -1,
              SHRT_MAX,
              "Ordinal",
              "Row to anchor the new layer to; it lands above that row. -1 uses the selected or "
              "active row, or the top of the stack",
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
  /* The data-block a kind with #StackAddKindInfo::source_id_type is made from, by name: what a UI
   * picked, and what a repeat or a script names again. */
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

/**
 * The Stack Layers header carries the add and the remove of a mask as two icon buttons of this one
 * operator, so what each does is read from `add` rather than from the shared definition.
 */
static std::string stack_row_mask_get_description(bContext * /*C*/,
                                                  wmOperatorType *ot,
                                                  PointerRNA *ptr)
{
  if (ptr == nullptr || !RNA_struct_property_is_set(ptr, "add")) {
    return ot->description;
  }
  return RNA_boolean_get(ptr, "add") ? "Add a mask to the active layer: a coverage factor laid over "
                                       "it, painted in Mask mode" :
                                       "Remove the active layer's mask together with its map";
}

void OUTLINER_OT_stack_layer_mask(wmOperatorType *ot)
{
  static const EnumPropertyItem initial_color_items[] = {
      {0, "WHITE", 0, "Add White Mask", "Add a mask that shows the layer everywhere"},
      {1, "BLACK", 0, "Add Black Mask", "Add a mask that hides the layer until it is painted in"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  ot->name = "Stack Layer Mask";
  ot->idname = "OUTLINER_OT_stack_layer_mask";
  ot->description = "Add or remove the mask that modulates a layer";
  ot->get_description = stack_row_mask_get_description;
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
  rna_def_stack_row_marker(ot);
  RNA_def_boolean(
      ot->srna, "add", true, "Add", "Add a mask, rather than removing the one already there");
  RNA_def_enum(ot->srna,
               "initial_color",
               initial_color_items,
               0,
               "Initial Color",
               "Initial fill of a newly added mask");
}

void OUTLINER_OT_stack_layer_mask_toggle(wmOperatorType *ot)
{
  ot->name = "Toggle Stack Layer Mask";
  ot->idname = "OUTLINER_OT_stack_layer_mask_toggle";
  ot->description = "Turn a layer's mask on or off, keeping the mask and its paint content";
  ot->exec = stack_row_mask_toggle_exec;
  /* Only while the mask slot is the one showing, so the context menu lists it then and not before. */
  ot->poll = stack_row_mask_section_poll;
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

void OUTLINER_OT_stack_layer_group(wmOperatorType *ot)
{
  ot->name = "Group Stack Layers";
  ot->idname = "OUTLINER_OT_stack_layer_group";
  ot->description =
      "Put a run of layers into a folder, composited on its own and laid over the rest as one "
      "layer";
  ot->exec = stack_rows_group_exec;
  ot->poll = stack_row_add_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  rna_def_stack_row_ordinal(ot, "Bottom layer of the run; -1 uses the active one");
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
  ot->description = "Add an empty folder above the active layer, to put layers into";
  ot->exec = stack_group_add_exec;
  ot->poll = stack_row_add_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  rna_def_stack_row_ordinal(ot, "Layer to sit above; -1 uses the active row");
  rna_def_stack_row_marker(ot);
}

/**
 * Whether the row an ungroup would land on is a folder.
 *
 * Only a folder can be unwrapped, and the context menu leans on this poll to show the entry only
 * where it applies. The poll cannot read the operator's marker, so it answers for what the
 * operator will do without one: the selection if there is one, the active row otherwise. The exec
 * still takes a marker, and the grouping editor refuses an ungroup for anything that is not a
 * folder.
 */
static bool stack_row_ungroup_poll(bContext *C)
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
  const StackRow *row = (ordinal < 0) ? nullptr : outliner_stack_row_find(*space_outliner, ordinal);
  return row != nullptr && row->can_hold_children;
}

void OUTLINER_OT_stack_layer_ungroup(wmOperatorType *ot)
{
  ot->name = "Ungroup Stack Layers";
  ot->idname = "OUTLINER_OT_stack_layer_ungroup";
  ot->description = "Put the layers of a folder back into the stack around it";
  ot->exec = stack_row_ungroup_exec;
  ot->poll = stack_row_ungroup_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  rna_def_stack_row_ordinal(ot, "Folder to unwrap; -1 uses the active row");
  rna_def_stack_row_marker(ot);
}

void OUTLINER_OT_stack_layer_merge_down(wmOperatorType *ot)
{
  ot->name = "Merge Layer Down";
  ot->idname = "OUTLINER_OT_stack_layer_merge_down";
  ot->description =
      "Collapse a layer and the one below it into one folder that composites just the same";
  ot->exec = stack_row_merge_down_exec;
  ot->poll = stack_row_add_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  rna_def_stack_row_ordinal(ot, "Layer to merge into the one below it; -1 uses the active one");
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

void OUTLINER_OT_stack_layer_mask_copy(wmOperatorType *ot)
{
  ot->name = "Copy Stack Layer Mask";
  ot->idname = "OUTLINER_OT_stack_layer_mask_copy";
  ot->description = "Copy the mask of the layer whose mask is showing";
  ot->exec = stack_row_mask_copy_exec;
  /* Only while the mask slot is the one showing, so the context menu lists it then and not before,
   * and Ctrl+C copies the layer itself the rest of the time. */
  ot->poll = stack_row_mask_section_poll;

  rna_def_stack_row_ordinal(ot, "Layer whose mask to copy; -1 uses the active one");
  rna_def_stack_row_marker(ot);
}

void OUTLINER_OT_stack_layer_mask_paste(wmOperatorType *ot)
{
  ot->name = "Paste Stack Layer Mask";
  ot->idname = "OUTLINER_OT_stack_layer_mask_paste";
  ot->description = "Replace the active layer's mask with the copied one";
  ot->exec = stack_row_mask_paste_exec;
  /* Only while a mask is on the clipboard: copying a layer empties it, so Ctrl+V pastes whichever
   * of the two was copied last. */
  ot->poll = stack_row_mask_paste_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  rna_def_stack_row_ordinal(ot, "Layer to paste the mask onto; -1 uses the active one");
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

}  // namespace blender::ed::outliner
