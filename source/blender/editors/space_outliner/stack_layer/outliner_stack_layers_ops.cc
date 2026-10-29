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
#include "BKE_report.hh"

#include "BLI_string.h"

#include "ED_screen.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"
#include "RNA_prototypes.hh"

#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../outliner_intern.hh"
#include "outliner_stack_layers_intern.hh"
#include "outliner_stack_source.hh"

namespace blender::ed::outliner {

/**
 * The identity property every operator that addresses an existing row shares: the row's marker,
 * which survives the renumbering an edit puts the position through. Read through
 * #stack_operator_ordinal_get, so a caller names a row either way.
 */
void rna_def_stack_row_marker(wmOperatorType *ot)
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
void rna_def_stack_row_ordinal(wmOperatorType *ot, const char *description)
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

static wmOperatorStatus stack_target_clear_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner == nullptr) {
    return OPERATOR_CANCELLED;
  }
  return stack_source_for_space(*space_outliner)->target_clear(*C) ? OPERATOR_FINISHED :
                                                                    OPERATOR_CANCELLED;
}

/** Whether there is an activated row to stop aiming at; the menu entry leans on this. */
static bool stack_target_clear_poll(bContext *C)
{
  if (!stack_row_add_poll(C)) {
    return false;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  return outliner_stack_active_ordinal_get(outliner_stack_read_context(*C), *space_outliner) >= 0;
}

void OUTLINER_OT_stack_layer_clear_target(wmOperatorType *ot)
{
  ot->name = "Clear Target";
  ot->idname = "OUTLINER_OT_stack_layer_clear_target";
  ot->description = "Stop directing edits at the activated stack layer";
  ot->exec = stack_target_clear_exec;
  ot->poll = stack_target_clear_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/**
 * The tags #StackRow::color_tag names. Values are the row contract (zero clears, 1 to
 * #STACK_ROW_COLOR_NUM are the palette), so the operator stays independent of any source's storage;
 * a source maps these onto its own.
 */
static const EnumPropertyItem stack_row_color_items[] = {
    {0, "NONE", ICON_X, "None", "Remove the group's color tag"},
    {1, "COLOR_01", ICON_COLLECTION_COLOR_01, "Color 01", ""},
    {2, "COLOR_02", ICON_COLLECTION_COLOR_02, "Color 02", ""},
    {3, "COLOR_03", ICON_COLLECTION_COLOR_03, "Color 03", ""},
    {4, "COLOR_04", ICON_COLLECTION_COLOR_04, "Color 04", ""},
    {5, "COLOR_05", ICON_COLLECTION_COLOR_05, "Color 05", ""},
    {6, "COLOR_06", ICON_COLLECTION_COLOR_06, "Color 06", ""},
    {7, "COLOR_07", ICON_COLLECTION_COLOR_07, "Color 07", ""},
    {8, "COLOR_08", ICON_COLLECTION_COLOR_08, "Color 08", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

/**
 * Whether the row a color tag would land on is a group.
 *
 * Color tags belong to group folders, and the UI that offers them -- the context menu's color
 * swatches -- leans on this poll to appear only where they apply. The poll cannot read the
 * operator's marker, so it answers for the row the operator would pick without one: the first
 * selected row if there is one, the active row otherwise. A caller that does pass a marker or an
 * ordinal is not checked here; the source refuses a tag for any row that is not a group, so such a
 * call cancels instead of tagging something the mode would not draw.
 */
static bool stack_row_color_tag_poll(bContext *C)
{
  if (!stack_row_add_poll(C)) {
    return false;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  Vector<int> selected;
  stack_selected_ordinals_get(*space_outliner, selected);
  const int ordinal = !selected.is_empty() ?
                          selected.first() :
                          outliner_stack_active_ordinal_get(outliner_stack_read_context(*C),
                                                            *space_outliner);
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
  ot->name = "Set Layer Group Color";
  ot->idname = "OUTLINER_OT_stack_layer_color_tag_set";
  ot->description = "Set the color tag of a layer group folder";
  ot->exec = stack_row_color_tag_set_exec;
  ot->poll = stack_row_color_tag_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_enum(ot->srna, "color", stack_row_color_items, 0, "Color", "Color tag for the group");
  rna_def_stack_row_ordinal(ot, "Group to set color for; -1 uses the selected or active row");
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

void OUTLINER_OT_stack_layer_lock_toggle(wmOperatorType *ot)
{
  ot->name = "Toggle Stack Layer Lock";
  ot->idname = "OUTLINER_OT_stack_layer_lock_toggle";
  ot->description = "Protect a layer of the stack from painting, or lift that protection";
  ot->exec = stack_row_lock_toggle_exec;
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
 * The Add dialog's own choices change what it shows: a palette swatch writes `fill_color` and the
 * inline picker beside it has to follow, and switching from Fill to Empty takes the colour controls
 * away. Redraw the popup on either.
 */
static void stack_add_props_update(bContext *C,
                                   PointerRNA * /*ptr*/,
                                   PropertyRNA * /*prop*/)
{
  if (C == nullptr) {
    return;
  }
  if (ARegion *region = CTX_wm_region(C)) {
    ED_region_tag_redraw(region);
  }
}

/* Fill first: it is what the Add button means, and the default the dialog starts on. */
static const EnumPropertyItem stack_fill_mode_items[] = {
    {STACK_FILL_MODE_FILL, "FILL", 0, "Fill", "Start the layer with a flat colour"},
    {STACK_FILL_MODE_EMPTY, "EMPTY", 0, "Empty", "Start the layer with the channels chosen below"},
    {0, nullptr, 0, nullptr, nullptr},
};

/**
 * The channel flag items the space's source offers for an empty row.
 *
 * The choices are the source's own vocabulary -- for a paint stack, the channels of the material's
 * set -- so the operator only asks the source and copies what it answers, and a source with no
 * channel choices leaves the property with none.
 */
static const EnumPropertyItem *stack_add_fill_channels_itemf(bContext *C,
                                                            PointerRNA * /*ptr*/,
                                                            PropertyRNA * /*prop*/,
                                                            bool *r_free)
{
  EnumPropertyItem *items = nullptr;
  int items_num = 0;
  SpaceOutliner *space_outliner = (C == nullptr) ? nullptr : CTX_wm_space_outliner(C);
  if (space_outliner != nullptr && space_outliner->runtime != nullptr) {
    Vector<EnumPropertyItem> channel_items;
    stack_source_for_space(*space_outliner)->add_kind_channel_items(*C, channel_items);
    for (const EnumPropertyItem &item : channel_items) {
      RNA_enum_item_add(&items, &items_num, &item);
    }
  }
  RNA_enum_item_end(&items, &items_num);
  *r_free = true;
  return items;
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
  RNA_def_property_update_runtime_with_context_and_property(fill_prop, stack_add_props_update);
  /* Only read by a kind with #StackAddKindInfo::color_optional: start the Fill with that colour,
   * or with no channels at all. The Add's UI draws it as the two buttons, Fill first. */
  PropertyRNA *fill_mode_prop = RNA_def_enum(
      ot->srna,
      "fill_mode",
      stack_fill_mode_items,
      STACK_FILL_MODE_FILL,
      "Start With",
      "Whether the new Fill layer starts with a colour, or with no channels at all");
  RNA_def_property_update_runtime_with_context_and_property(fill_mode_prop, stack_add_props_update);
  /* The channels an empty Fill starts with, as a flag mask over the source's own channel bits.
   * Zero is Empty until the user picks the channels its image set needs; the items come from the
   * source, so the generic operator carries no channel vocabulary. #RNA_def_enum_flag puts the flag
   * in place before the items, which is the order makesrna requires of a flag enum. */
  PropertyRNA *fill_channels_prop = RNA_def_enum_flag(ot->srna,
                                                      "fill_channels",
                                                      rna_enum_dummy_NULL_items,
                                                      0,
                                                      "Fill Channels",
                                                      "Channels an Empty Fill layer starts with");
  RNA_def_enum_funcs(fill_channels_prop, stack_add_fill_channels_itemf);
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

void OUTLINER_OT_stack_layer_effect_add(wmOperatorType *ot)
{
  /* The same operation as the Add, with the same properties and body; only the offered kinds
   * differ, so the header's Effects dropdown can list the corrections as a plain enum menu. */
  OUTLINER_OT_stack_layer_add(ot);
  ot->name = "Add Effect";
  ot->idname = "OUTLINER_OT_stack_layer_effect_add";
  ot->description = "Add a correction to the active layer or its mask";
  PropertyRNA *prop = RNA_struct_type_find_property(ot->srna, "type");
  RNA_def_enum_funcs(prop, stack_effect_type_itemf);
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
  /* Black leads, as in the Add Mask menu. White stays the default value, so the quick-add button
   * still makes the mask that shows the layer everywhere. */
  static const EnumPropertyItem initial_color_items[] = {
      {1,
       "BLACK",
       ICON_MOD_MASK,
       "Add Black Mask",
       "Add a mask that hides the layer until it is painted in"},
      {0,
       "WHITE",
       ICON_CLIPUV_HLT,
       "Add White Mask",
       "Add a mask that shows the layer everywhere"},
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

void OUTLINER_OT_stack_layer_mask_image(wmOperatorType *ot)
{
  ot->name = "Add Image Mask";
  ot->idname = "OUTLINER_OT_stack_layer_mask_image";
  ot->description = "Give the layer a mask that reads an image";
  ot->invoke = stack_row_mask_image_invoke;
  ot->exec = stack_row_mask_image_exec;
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
  /* The image the mask reads, picked in the ID Browser the invoke opens. Never remembered: the
   * next click must offer the browser again, not re-add the image of the last one. A script names
   * one, and the redo panel keeps the value of its own run.
   *
   * The runtime variant of the definition: an operator type is registered at startup, after RNA is
   * up, and the build only links the runtime half of the definition API (see RNA_def_pointer's
   * #ifndef RNA_RUNTIME in rna_define.cc). */
  PropertyRNA *image_prop = RNA_def_pointer_runtime(
      ot->srna, "image", RNA_Image, "Image", "Image the mask reads its coverage from");
  RNA_def_property_flag(image_prop, PROP_SKIP_SAVE);
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

void OUTLINER_OT_stack_layer_corrections_merge(wmOperatorType *ot)
{
  ot->name = "Merge Corrections";
  ot->idname = "OUTLINER_OT_stack_layer_corrections_merge";
  ot->description =
      "Bake the selected corrections of a layer into one Paint correction texture, stacked as they "
      "look now. Irreversible: the undo history is cleared";
  ot->invoke = stack_corrections_merge_invoke;
  ot->exec = stack_corrections_bake_exec;
  ot->poll = stack_corrections_merge_poll;
  /* No OPTYPE_UNDO: the bake invalidates every undo step rather than adding one. */
  ot->flag = OPTYPE_REGISTER;
}

void OUTLINER_OT_stack_layer_correction_to_texture(wmOperatorType *ot)
{
  ot->name = "Convert to Texture";
  ot->idname = "OUTLINER_OT_stack_layer_correction_to_texture";
  ot->description =
      "Bake a Fill, Material or Gradient correction into a Paint correction texture. Irreversible: "
      "the undo history is cleared";
  ot->invoke = stack_correction_texture_invoke;
  ot->exec = stack_corrections_bake_exec;
  ot->poll = stack_correction_texture_poll;
  ot->flag = OPTYPE_REGISTER;
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
