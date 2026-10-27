/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 *
 * Add, effect and remove operators of the Stack Layers display, and the enum items that list the
 * kinds of rows the source can add.
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

/* -------------------------------------------------------------------- */
/** \name Add and remove
 *
 * The generic half of the two operators that create and destroy rows. What a row *is* stays behind
 * #StackEditor; these only turn the operator's arguments into a call of the source's `row_add` or
 * `row_remove` and keep the ordinals and the selection consistent across the rebuild.
 * \{ */

/** The Add kind \a kind as the space's source declares it; false when it declares no such kind. */
static bool stack_add_kind_info_get(const bContext &C, const int kind, StackAddKindInfo &r_info)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(&C);
  if (space_outliner == nullptr || space_outliner->runtime == nullptr) {
    return false;
  }
  const StackEditor *editor = stack_source_for_space(*space_outliner)->editor();
  if (editor == nullptr) {
    return false;
  }
  Vector<StackAddKindInfo> kinds;
  editor->add_kinds(kinds);
  if (!kinds.index_range().contains(kind)) {
    return false;
  }
  r_info = std::move(kinds[kind]);
  return true;
}

wmOperatorStatus stack_row_add_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  /* The value is the kind's place in the source's full list, which is what a repeat or the redo
   * panel stores. It is not the Effects menu's identifier: that one is resolved per target when
   * the menu is built, so a value left over from another target mode still names a real kind here
   * rather than being looked up by name. A value that names none is stale (the source changed
   * under a repeat) and says so instead of silently adding nothing. */
  const int kind = RNA_enum_get(op->ptr, "type");
  StackAddKindInfo kind_info;
  if (!stack_add_kind_info_get(*C, kind, kind_info)) {
    BKE_report(
        op->reports, RPT_ERROR, "That Add kind is no longer part of this stack's vocabulary");
    return OPERATOR_CANCELLED;
  }

  StackAddArgs args;
  /* A kind made from an existing data-block is handed it by name rather than by a pointer the UI
   * held on to, so the call stays what the info log shows: a repeat or a script finds the same
   * data-block again. */
  char source_name[MAX_ID_NAME - 2];
  RNA_string_get(op->ptr, "source", source_name);
  if (kind_info.source_id_type != 0) {
    args.source = (source_name[0] != '\0') ?
                      BKE_libblock_find_name(
                          CTX_data_main(C), kind_info.source_id_type, source_name) :
                      nullptr;
    if (args.source == nullptr) {
      BKE_reportf(op->reports,
                  RPT_ERROR,
                  RPT_("No data-block named \"%s\" to make the layer from"),
                  source_name);
      return OPERATOR_CANCELLED;
    }
  }

  /* The row the Add is anchored to: a marker or the explicit "ordinal" property when a script gave
   * one, otherwise the selection and then the active row. -1 names no row and puts the new layer on
   * top of the stack. */
  const int anchor_ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  if (anchor_ordinal >= 0) {
    const StackRow *anchor_row = outliner_stack_row_find(*space_outliner, anchor_ordinal);
    if (anchor_row != nullptr) {
      args.section_id = outliner_stack_row_active_section_get(*space_outliner, *anchor_row);
    }
  }
  float gamma_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  float linear_color[4];
  RNA_float_get_array(op->ptr, "fill_color", gamma_color);
  /* The operator's colour is a picker colour (gamma); the description stores scene linear. */
  srgb_to_linearrgb_v4(linear_color, gamma_color);
  const bool empty = kind_info.color_optional &&
                     RNA_enum_get(op->ptr, "fill_mode") == STACK_FILL_MODE_EMPTY;
  if (kind_info.takes_color && !empty) {
    args.color = linear_color;
  }
  args.empty = empty;
  if (empty) {
    /* The Empty buttons ask which channels the new row should start with; none is the default. */
    args.channel_mask = uint16_t(RNA_enum_get(op->ptr, "fill_channels"));
  }
  const int added = outliner_stack_row_add(C, *space_outliner, kind, anchor_ordinal, args);
  return added >= 0 ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

/** The RNA-Main collection the Add searches a source kind's data-blocks in. Null for a type with
 * no collection to search, where the dialog falls back to naming the data-block exactly. */
static const char *stack_add_source_collection_name(const short idcode)
{
  switch (idcode) {
    case ID_AC:
      return "actions";
    case ID_CA:
      return "cameras";
    case ID_CU_LEGACY:
      return "curves";
    case ID_CV:
      return "hair_curves";
    case ID_GP:
      return "grease_pencils";
    case ID_IM:
      return "images";
    case ID_LA:
      return "lights";
    case ID_MA:
      return "materials";
    case ID_ME:
      return "meshes";
    case ID_NT:
      return "node_groups";
    case ID_TE:
      return "textures";
    case ID_WO:
      return "worlds";
    default:
      return nullptr;
  }
}

/**
 * The Add's invoke: a kind whose creation takes a colour opens the color picker first, a kind made
 * from a data-block the call names none of opens the search for it, the rest run straight through.
 * A confirm dialog rather than a redo popup: every change in a redo popup re-runs the whole add.
 */
wmOperatorStatus stack_row_add_invoke(bContext *C, wmOperator *op, const wmEvent * /*event*/)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  StackAddKindInfo kind_info;
  const bool kind_known = space_outliner != nullptr && space_outliner->runtime != nullptr &&
                          stack_add_kind_info_get(*C, RNA_enum_get(op->ptr, "type"), kind_info);
  PropertyRNA *source_prop = RNA_struct_find_property(op->ptr, "source");
  const bool needs_source = kind_known && kind_info.source_id_type != 0 &&
                            source_prop != nullptr && !RNA_property_is_set(op->ptr, source_prop);
  if (!kind_known || (!kind_info.takes_color && !needs_source)) {
    return stack_row_add_exec(C, op);
  }
  if (kind_info.color_optional) {
    /* The Add button opens on Fill, whatever the last Add did: Empty is the exception, chosen for
     * this row. A script that named fill_mode or fill_channels itself keeps what it named, so a
     * repeat and the redo panel can still reproduce the last choice. */
    PropertyRNA *fill_mode_prop = RNA_struct_find_property(op->ptr, "fill_mode");
    if (fill_mode_prop != nullptr && !RNA_property_is_set(op->ptr, fill_mode_prop)) {
      RNA_enum_set(op->ptr, "fill_mode", STACK_FILL_MODE_FILL);
    }
    PropertyRNA *fill_channels_prop = RNA_struct_find_property(op->ptr, "fill_channels");
    if (fill_channels_prop != nullptr && !RNA_property_is_set(op->ptr, fill_channels_prop)) {
      RNA_enum_set(op->ptr, "fill_channels", 0);
    }
  }
  if (needs_source && kind_info.source_browse_prop != nullptr) {
    /* The popover's assignment does the Add: nothing is created until a data-block is picked. */
    const StackReadContext ctx = outliner_stack_read_context(*C);
    if (ID *owner = outliner_stack_owner_get(ctx, *space_outliner)) {
      ui::id_browser_popover_invoke(
          C, RNA_id_pointer_create(owner), kind_info.source_browse_prop, false);
      return OPERATOR_FINISHED;
    }
  }
  return WM_operator_props_dialog_popup(C, op, 220, std::nullopt, IFACE_("Add"));
}

/**
 * The Fill's colour, all in one place: the button showing what is assigned, the inline picker and
 * the RGBA sliders. Every control edits the operator's `fill_color`, so they all follow each other
 * through #stack_add_props_update. A source with a palette of its own adds it through
 * #StackSource::add_kind_draw_extra.
 */
static void stack_add_fill_color_draw(wmOperator &op)
{
  PropertyRNA *prop = RNA_struct_find_property(op.ptr, "fill_color");
  if (prop == nullptr) {
    return;
  }
  op.layout->prop(op.ptr, prop, RNA_NO_INDEX, 0, UI_ITEM_NONE, IFACE_("Fill Color"), ICON_NONE);
  /* A visible row between the label/colour button and the picker below it. The explicit line type
   * is what guarantees a separator row outside a menu block, where the default is only a spacer. */
  op.layout->separator(1.0f, ui::LayoutSeparatorType::Line);
  ui::template_color_picker(op.layout, op.ptr, "fill_color", true, false, false, true);
  ui::Layout &sliders = op.layout->column(true);
  sliders.prop(op.ptr, prop, 0, 0, ui::ITEM_R_SLIDER, IFACE_("Red"), ICON_NONE);
  sliders.prop(op.ptr, prop, 1, 0, ui::ITEM_R_SLIDER, IFACE_("Green"), ICON_NONE);
  sliders.prop(op.ptr, prop, 2, 0, ui::ITEM_R_SLIDER, IFACE_("Blue"), ICON_NONE);
  sliders.prop(op.ptr, prop, 3, 0, ui::ITEM_R_SLIDER, IFACE_("Alpha"), ICON_NONE);
}

/**
 * Only the choices the kind asks for are the user's here: a colour for a kind that takes one, the
 * source data-block for a kind made from one. The anchor, the kind and the marker are what the
 * button that placed the call already decided. Anything more specific -- an empty row's channels,
 * a palette beside the colour -- is the source's, drawn through
 * #StackSource::add_kind_draw_extra.
 */
void stack_row_add_ui(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  StackAddKindInfo kind_info;
  if (space_outliner == nullptr || space_outliner->runtime == nullptr ||
      !stack_add_kind_info_get(*C, RNA_enum_get(op->ptr, "type"), kind_info))
  {
    return;
  }
  bool empty = false;
  if (kind_info.color_optional) {
    /* Fill or empty, in that order; the colour below belongs to Fill only. */
    op->layout->prop(op->ptr, "fill_mode", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
    op->layout->separator();
    empty = RNA_enum_get(op->ptr, "fill_mode") != STACK_FILL_MODE_FILL;
  }
  if (kind_info.takes_color && !empty) {
    if (kind_info.color_optional) {
      stack_add_fill_color_draw(*op);
    }
    else {
      op->layout->prop(op->ptr, "fill_color", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    }
  }
  if (kind_info.source_id_type != 0) {
    const char *collection_name = stack_add_source_collection_name(kind_info.source_id_type);
    if (collection_name != nullptr) {
      PointerRNA main_ptr = RNA_main_pointer_create(CTX_data_main(C));
      op->layout->prop_search(
          op->ptr, "source", &main_ptr, collection_name, std::nullopt, ICON_NONE);
    }
    else {
      op->layout->prop(op->ptr, "source", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    }
  }
  stack_source_for_space(*space_outliner)->add_kind_draw_extra(
      *C, kind_info, *op, *op->layout, empty);
}

bool stack_row_add_poll(bContext *C)
{
  if (!stack_mode_poll(C)) {
    return false;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const StackReadContext ctx = outliner_stack_read_context(*C);
  const ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  return owner != nullptr && stack_source_for_space(*space_outliner)->can_edit(*owner);
}

/**
 * The Add operator's own poll, on top of #stack_row_add_poll: a source that declares no kinds of
 * rows gets no Add at all, rather than an Add that would refuse whatever it was asked for.
 */
bool stack_add_poll(bContext *C)
{
  if (!stack_row_add_poll(C)) {
    return false;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  Vector<StackAddKindInfo> kinds;
  if (const StackEditor *editor = stack_source_for_space(*space_outliner)->editor()) {
    editor->add_kinds(kinds);
  }
  return !kinds.is_empty();
}

/** Add one enum item for the row kind \a kind, named and described as \a info says. */
static void stack_add_kind_item_add(EnumPropertyItem *&items,
                                    int &items_num,
                                    const int kind,
                                    const StackAddKindInfo &info)
{
  EnumPropertyItem item = {};
  item.value = kind;
  item.identifier = stack_enum_string_intern(info.identifier);
  item.name = stack_enum_string_intern(IFACE_(info.name.c_str()));
  item.description = stack_enum_string_intern(TIP_(info.description.c_str()));
  item.icon = (info.glyph != 0) ? BKE_icon_glyph_ensure(info.glyph) : info.icon;
  if (item.icon == 0) {
    item.icon = info.icon;
  }
  RNA_enum_item_add(&items, &items_num, &item);
}

/**
 * The kinds of rows the source can add, as enum items.
 *
 * Built from the editor's own list, so the Add UI offers the domain's vocabulary without the
 * Outliner knowing it: the item's value is the kind's place in that list, which is what
 * #StackEditor::row_add reads back, and the identifier is the stable name scripts see.
 *
 * The Effects menu asks the editor for its own folded view (#StackEditor::effect_menu_kinds), so
 * an editor whose vocabulary repeats a correction per target can offer one line per thing. Its
 * identifiers are that view's resolved role, not the stable name of the full list: the Effects
 * menu is UI, and scripts name a kind through `stack_layer_add`.
 */
static const EnumPropertyItem *stack_add_kinds_itemf(bContext *C,
                                                     const bool effects_only,
                                                     bool *r_free)
{
  EnumPropertyItem *items = nullptr;
  int items_num = 0;
  SpaceOutliner *space_outliner = (C == nullptr) ? nullptr : CTX_wm_space_outliner(C);
  if (space_outliner != nullptr) {
    const StackSource &source = *stack_source_for_space(*space_outliner);
    /* The kinds are the source's vocabulary and do not depend on whether an owner can be edited
     * right now: gating the items on the owner would empty them while the header still drew a
     * button, and assigning `type` from Python then fails on every redraw. The operator's own poll
     * is what refuses to run without an editable owner. */
    if (const StackEditor *editor = source.editor()) {
      if (effects_only) {
        Vector<StackEffectMenuKind> effect_kinds;
        editor->effect_menu_kinds(*C, effect_kinds);
        for (const StackEffectMenuKind &entry : effect_kinds) {
          stack_add_kind_item_add(items, items_num, entry.kind, entry.info);
        }
      }
      else {
        Vector<StackAddKindInfo> kinds;
        editor->add_kinds(kinds);
        for (const int64_t index : kinds.index_range()) {
          stack_add_kind_item_add(items, items_num, int(index), kinds[index]);
        }
      }
    }
  }
  RNA_enum_item_end(&items, &items_num);
  *r_free = true;
  return items;
}

const EnumPropertyItem *stack_add_type_itemf(bContext *C,
                                             PointerRNA * /*ptr*/,
                                             PropertyRNA * /*prop*/,
                                             bool *r_free)
{
  return stack_add_kinds_itemf(C, false, r_free);
}

const EnumPropertyItem *stack_effect_type_itemf(bContext *C,
                                                PointerRNA * /*ptr*/,
                                                PropertyRNA * /*prop*/,
                                                bool *r_free)
{
  return stack_add_kinds_itemf(C, true, r_free);
}

/** The same question every mutating stack operator asks: is there a stack here to remove from,
 * and does its source take edits at all. */
bool stack_row_remove_poll(bContext *C)
{
  if (!stack_mode_poll(C)) {
    return false;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  const StackSource *source = stack_source_for_space(*space_outliner);
  return owner != nullptr && source->can_edit(*owner);
}

wmOperatorStatus stack_row_remove_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  /* A marker names the row by identity and wins over the position; without one, the position the
   * operator was given addresses the single row, and -1 falls through to the selection. */
  int ordinal = stack_operator_marker_ordinal_get(*C, *space_outliner, *op);
  if (ordinal == STACK_MARKER_UNRESOLVED) {
    BKE_report(op->reports, RPT_ERROR, "The marker names no row in this stack");
    return OPERATOR_CANCELLED;
  }
  if (ordinal < 0) {
    ordinal = RNA_int_get(op->ptr, "ordinal");
  }
  if (ordinal >= 0) {
    return outliner_stack_row_remove(C, *space_outliner, ordinal) ? OPERATOR_FINISHED :
                                                                    OPERATOR_CANCELLED;
  }
  /* The whole selection goes at once, the highest ordinal first: a removal renumbers the rows
   * above it, so walking down the stack keeps every remaining ordinal what it was when read. */
  Vector<int> selected;
  stack_selected_ordinals_get(*space_outliner, selected);
  if (selected.is_empty()) {
    /* Nothing picked: the row the user was on, which is what a bare Remove has always meant. */
    const int active_ordinal = outliner_stack_active_ordinal_get(outliner_stack_read_context(*C),
                                                                 *space_outliner);
    if (active_ordinal < 0) {
      return OPERATOR_CANCELLED;
    }
    selected.append(active_ordinal);
  }
  bool removed_any = false;
  for (int64_t index = selected.size() - 1; index >= 0; index--) {
    removed_any |= outliner_stack_row_remove(C, *space_outliner, selected[index]);
  }
  return removed_any ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

int outliner_stack_row_add(bContext *C,
                           SpaceOutliner &space_outliner,
                           const int kind,
                           const int ordinal,
                           const StackAddArgs &args)
{
  /* Inserting a row renumbers everything above it. */
  int new_ordinal = -1;
  const bool ok = stack_mutate(
      *C,
      space_outliner,
      true,
      [&](const StackSource & /*source*/,
          const StackEditor &editor,
          const StackFocus &focus,
          ID &owner,
          int &r_select_ordinal) {
        r_select_ordinal = editor.row_add(*C, focus, owner, kind, ordinal, args);
        return r_select_ordinal >= 0;
      },
      &new_ordinal);
  if (!ok) {
    return -1;
  }
  /* A layer the user just asked for is the one they mean to work on next. */
  outliner_stack_row_activate(C, space_outliner, new_ordinal);
  /* A row attached to its parent's content section is only listed while that section is the one
   * showing, so a row just added brings its section up: a mask correction added while the channels
   * were on show would otherwise be there and not be seen. */
  stack_row_reveal_attached(space_outliner, new_ordinal);
  return new_ordinal;
}

bool outliner_stack_row_remove(bContext *C, SpaceOutliner &space_outliner, const int ordinal)
{
  if (ordinal < 0) {
    return false;
  }
  /* Taking a row out renumbers everything that was above it, and the row that inherits the removed
   * one's ordinal must not inherit its selection with it. */
  return stack_mutate(
      *C,
      space_outliner,
      true,
      [&](const StackSource & /*source*/,
          const StackEditor &editor,
          const StackFocus &focus,
          ID &owner,
          int & /*r_select_ordinal*/) {
        return editor.row_remove(*C, focus, owner, ordinal);
      });
}

/** \} */

}  // namespace blender::ed::outliner
