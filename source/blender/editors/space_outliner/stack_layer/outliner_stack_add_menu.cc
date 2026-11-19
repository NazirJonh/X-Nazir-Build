/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 *
 * The registry-driven lists of the Stack Layers Add menu: the Favorites submenu the user pins
 * kinds into, and the effect lists, each entry with a pin button at its right edge -- the same
 * choice-and-pin row the asset library column selector draws.
 *
 * The vocabulary is the source's own (#StackEditor::add_kinds and #effect_menu_kinds), so an
 * effect added to the kind table appears here, and in the search over the menu, without a menu
 * change. The root menu that ties these lists to the quick layers, and the search over all of it,
 * are Python (`space_outliner.py`): add-ons extend that root through `Menu.append`, the way they
 * extend the node editor's Add menu, and every item drawn here lands in its search results.
 */

#include "DNA_space_types.h"
#include "DNA_userdef_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_context.hh"
#include "BKE_icons.hh"
#include "BKE_preferences.h"
#include "BKE_report.hh"
#include "BKE_screen.hh"

#include "BLI_assert.h"
#include "BLI_listbase_iterator.hh"
#include "BLI_string.h"
#include "BLI_string_ref.hh"
#include "BLI_string_utf8.h"
#include "BLI_vector.hh"

#include "ED_outliner_stack.hh"

#include "BLT_translation.hh"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../outliner_intern.hh"
#include "outliner_stack_layers_intern.hh"
#include "outliner_stack_source.hh"

namespace blender::ed::outliner {

/* -------------------------------------------------------------------- */
/** \name Shared helpers
 * \{ */

/** Whether the menu is being drawn for the search over it, whose context flags it. */
static bool stack_add_menu_searching(const bContext &C)
{
  return CTX_data_int_get(&C, "is_menu_search").value_or(false);
}

/** The Stack Layers editor of the Outliner in the menu's context, or null outside one. */
static const StackEditor *stack_add_menu_editor_get(const bContext &C)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(&C);
  if (space_outliner == nullptr || space_outliner->runtime == nullptr) {
    return nullptr;
  }
  return stack_source_for_space(*space_outliner)->editor();
}

/** Where the lists live: the Stack Layers stack view of a source that declares kinds at all.
 *
 * Why this is deliberately wider than the Python root menu's poll: see #stack_add_menu_poll. */
static bool stack_add_menu_source_supported(const SpaceOutliner &space_outliner)
{
  if (space_outliner.outlinevis != SO_STACK_LAYERS ||
      space_outliner.stack_layers_view != SO_SL_VIEW_STACK)
  {
    return false;
  }
  return stack_source_for_space(space_outliner)->editor() != nullptr;
}

/** Poll for the C++ Add menu lists (Favorites, Effects).
 *
 * Deliberately wider than the Python root menu's poll (space_outliner.py), which inlines these
 * lists only for the paint-material source: the lists themselves are valid wherever a source
 * declares a vocabulary of its own, and this condition stays the one authority on that, not the
 * root's narrower copy. */
static bool stack_add_menu_poll(const bContext *C, MenuType * /*mt*/)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner == nullptr || space_outliner->runtime == nullptr) {
    return false;
  }
  return stack_add_menu_source_supported(*space_outliner);
}

/** \return the place in the source's full kind list that \a identifier names, or -1. */
static int stack_add_kind_index_by_identifier(const Vector<StackAddKindInfo> &kinds,
                                              const StringRefNull identifier)
{
  for (const int64_t index : kinds.index_range()) {
    if (kinds[index].identifier == identifier) {
      return int(index);
    }
  }
  return -1;
}

/** The kind's own look: the glyph icon where the font provides one, the built-in icon otherwise. */
static int stack_add_menu_kind_icon(const StackAddKindInfo &info)
{
  return (info.glyph != 0) ? BKE_icon_glyph_ensure(info.glyph) : info.icon;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Effect lists
 *
 * The groups are the source's own folding (#StackEffectMenuKind), drawn one MenuType apiece; the
 * main list holds what no group claims. Every entry gets the pin at its right edge, so a click
 * adds and a pin keeps, the way the library selector's rows do.
 * \{ */

/** One of the Add menu's folded effect lists, with the menu it draws as and the look its entry in
 * the Effects list carries. The Effects list's submenu rows and the registration below are both
 * written from this table, so a new group is one entry here plus the kinds that claim its view --
 * no menu change beyond the rows themselves. */
struct StackAddMenuGroupInfo {
  /** The view of the source's folded list the group draws (#stack_effect_menu_group). The main
   * list is what no group claims, and stays in the Effects menu itself. */
  StackAddKindsView view;
  const char *idname;
  const char *label;
  int icon;
  /** Whether the group's list closes with the Anchor Point Reader, and is set apart from the
   * other submenus by a separator: the reader is an operator of its own rather than a kind of the
   * list, and it belongs to the Anchor Point list, the one that works on a point the user picks
   * there. */
  bool has_anchor_reader;
};

static constexpr StackAddMenuGroupInfo stack_add_menu_groups[] = {
    {StackAddKindsView::CorrectionEffects,
     OUTLINER_MENU_STACK_ADD_EFFECT_CORRECTION,
     N_("Correction"),
     ICON_MODIFIER,
     false},
    {StackAddKindsView::TransformEffects,
     OUTLINER_MENU_STACK_ADD_EFFECT_TRANSFORM,
     N_("Transform"),
     ICON_MOD_UVPROJECT,
     false},
    {StackAddKindsView::NewEffects,
     OUTLINER_MENU_STACK_ADD_EFFECT_FILTER,
     N_("Filter"),
     ICON_SOLO_ON,
     false},
    {StackAddKindsView::Filter2Effects,
     OUTLINER_MENU_STACK_ADD_EFFECT_FILTER2,
     N_("Filter 2"),
     ICON_SOLO_ON,
     false},
    {StackAddKindsView::PatternEffects,
     OUTLINER_MENU_STACK_ADD_EFFECT_PATTERN,
     N_("Generate Patterns"),
     ICON_TEXTURE,
     false},
    {StackAddKindsView::AnchorEffects,
     OUTLINER_MENU_STACK_ADD_EFFECT_ANCHOR,
     N_("Anchor Point"),
     ICON_EMPTY_AXIS,
     true},
};

/** The table entry whose menu \a menu draws. The menus are registered from the table itself, so
 * the lookup always answers. */
static const StackAddMenuGroupInfo *stack_add_menu_group_info_get(const Menu &menu)
{
  for (const StackAddMenuGroupInfo &group : stack_add_menu_groups) {
    if (STREQ(group.idname, menu.type->idname)) {
      return &group;
    }
  }
  return nullptr;
}

/** The pin toggle at an entry's right edge.
 *
 * A fixed #UI_UNIT_X wide icon button, not another menu item: the layout spreads an aligned row's
 * buttons proportionally, so a second full-width button would put the pin in the middle of a menu
 * twice as wide as its longest label. At one unit the row is the label plus the pin, as in the
 * asset library selector (#library_selector_add_pin_toggle). */
static void stack_add_menu_pin_button(ui::Layout &row,
                                      const StringRefNull identifier,
                                      const bool is_pinned)
{
  ui::Block *block = row.block();
  ui::block_layout_set_current(block, &row);
  /* The icon shows the state, not the action: SOLO_ON for a favorite, SOLO_OFF for what is not.
   * The click does the opposite of what is shown. Uses the same icons as Toggle Asset Favorite for
   * visual consistency across favorite UI. */
  ui::Button *but = ui::uiDefIconButO(
      block,
      ui::ButtonType::But,
      "OUTLINER_OT_stack_add_favorite_set",
      wm::OpCallContext::ExecDefault,
      is_pinned ? ICON_SOLO_ON : ICON_SOLO_OFF,
      0,
      0,
      short(UI_UNIT_X),
      short(UI_UNIT_Y),
      is_pinned ? TIP_("Remove from Favorites") : TIP_("Add to Favorites"));
  PointerRNA *props = ui::button_operator_ptr_ensure(but);
  RNA_string_set(props, "identifier", identifier.c_str());
  RNA_boolean_set(props, "pinned", !is_pinned);
  /* The pin edits the preferences, not the scene, so it must not land in the undo stack. The
   * asset library selector's pin clears the same flag (#library_selector_add_pin_toggle). */
  ui::button_flag_disable(but, ui::BUT_UNDO);
  if (!is_pinned) {
    /* Only the favorites carry a visible mark; the pin to add one shows when the row is hovered. */
    ui::button_pushbutton_show_on_row_hover_set(but, true);
  }
}

/** Lay the menu's rows out as a column of plain items, so the pin of a row ends at the column's
 * right edge (see #ui::block_menu_rows_fill_column_set). */
static void stack_add_menu_columns_fill(ui::Layout &layout, const bool has_submenus)
{
  ui::block_menu_rows_fill_column_set(layout.block(), true, has_submenus);
}

/** One effect entry: the Add button, and the pin that keeps it in the Favorites. */
static void stack_add_menu_effect_row(ui::Layout &layout,
                                      const StackEffectMenuKind &entry,
                                      const bool show_pin)
{
  ui::Layout &row = layout.row(true);
  const bool pinned = BKE_preferences_paint_add_favorite_contains(&U,
                                                                  entry.info.identifier.c_str());
  PointerRNA props = row.op("outliner.stack_layer_effect_add",
                            IFACE_(entry.info.name.c_str()),
                            stack_add_menu_kind_icon(entry.info));
  RNA_enum_set(&props, "type", entry.kind);
  /* The stable identifier rides along: the exec prefers it over the numeric type, whose place in
   * the source's list moves when the list is rebuilt between this draw and the exec. */
  RNA_string_set(&props, "kind_id", entry.info.identifier.c_str());
  RNA_string_set(&props, "label", entry.info.name.c_str());
  if (show_pin) {
    stack_add_menu_pin_button(row, entry.info.identifier, pinned);
  }
}

/** One folded list's rows: the entries whose view is \a view, each with its pin. Shared by the
 * Effects menu's main list and each group's own menu. */
static void stack_add_menu_group_rows_fill(const bContext *C,
                                           ui::Layout &layout,
                                           const StackAddKindsView view)
{
  const StackEditor *editor = stack_add_menu_editor_get(*C);
  if (editor == nullptr) {
    return;
  }
  Vector<StackEffectMenuKind> effect_kinds;
  /* The registry's virtual takes the context mutable (#StackEditor::effect_menu_kinds resolves the
   * active target through it), while this draw receives it const and only reads; drop the const
   * here rather than change the registry's shared API for one reader. */
  editor->effect_menu_kinds(const_cast<bContext &>(*C), effect_kinds);
  for (const StackEffectMenuKind &entry : effect_kinds) {
    if (stack_effect_menu_group(entry) != view) {
      continue;
    }
    stack_add_menu_effect_row(layout, entry, !stack_add_menu_searching(*C));
  }
}

/** One group's menu: its list, and the reader the Anchor Point list closes with. */
static void stack_add_menu_group_draw(const bContext *C, Menu *menu)
{
  ui::Layout &layout = *menu->layout;
  /* Harmless again when the root already asked for it: the menus drawn as submenus own a block of
   * their own, and the Effects list that holds submenus sets the arrow room itself. */
  stack_add_menu_columns_fill(layout, false);
  const StackAddMenuGroupInfo *group = stack_add_menu_group_info_get(*menu);
  if (group == nullptr) {
    return;
  }
  stack_add_menu_group_rows_fill(C, layout, group->view);
  if (group->has_anchor_reader) {
    /* The reader is picked from the popover that lists the points, not added blind from here, so
     * it is an operator of its own rather than a kind of the list above. */
    layout.op("outliner.stack_layer_effect_add_anchor_reader",
              IFACE_("Anchor Point Reader"),
              ICON_EMPTY_AXIS);
  }
}

/** The Add Effect menu: the main list, then the groups as submenus, like the source folds them. */
static void stack_add_menu_effects_draw(const bContext *C, Menu *menu)
{
  ui::Layout &layout = *menu->layout;
  /* This list is inlined into the root menu, whose submenu items carry arrows. */
  stack_add_menu_columns_fill(layout, true);
  /* A popup opens over the item chosen last time, which for this menu would hide the Search entry
   * behind whatever was added before. The search is what the menu is for, so the popup always
   * opens with it under the cursor, as the first item does when nothing is remembered. The memory
   * is read after the draw, so clearing it here takes effect. */
  ui::block_flag_disable(layout.block(), ui::BLOCK_POPUP_MEMORY);
  stack_add_menu_group_rows_fill(C, layout, StackAddKindsView::Effects);
  layout.separator();
  for (const StackAddMenuGroupInfo &group : stack_add_menu_groups) {
    if (group.has_anchor_reader) {
      /* The Anchor Point list closes the menu, apart from the other submenus. */
      layout.separator();
    }
    layout.menu(group.idname, IFACE_(group.label), group.icon);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Favorites
 * \{ */

static void stack_add_menu_favorites_draw(const bContext *C, Menu *menu)
{
  ui::Layout &layout = *menu->layout;
  const bool searching = stack_add_menu_searching(*C);
  stack_add_menu_columns_fill(layout, false);
  const StackEditor *editor = stack_add_menu_editor_get(*C);
  Vector<StackAddKindInfo> kinds;
  if (editor != nullptr) {
    editor->add_kinds(kinds);
  }

  int shown = 0;
  for (const bUserPaintAddFavorite &favorite : U.paint_add_favorites) {
    shown++;
    ui::Layout &row = layout.row(true);
    const int index = stack_add_kind_index_by_identifier(kinds, favorite.identifier);
    if (index >= 0) {
      const StackAddKindInfo &info = kinds[index];
      /* The full list, not the folded Effects view: a favorite names a kind by its stable
       * identifier, so what it adds does not follow the target the user happens to be on. */
      PointerRNA props = row.op(
          "outliner.stack_layer_add", IFACE_(info.name.c_str()), stack_add_menu_kind_icon(info));
      RNA_enum_set(&props, "type", index);
      /* The favorite is the stable identifier; the index is only what the exec falls back to when
       * an older caller passed the numeric type alone. */
      RNA_string_set(&props, "kind_id", favorite.identifier);
      RNA_string_set(&props, "label", info.name.c_str());
    }
    else {
      /* The source no longer declares the kind: keep the row so the user sees what to unpin, and
       * let only the unpin work. The label is the identifier verbatim: it is a code, not prose,
       * and there is nothing to translate it into. */
      ui::Layout &name = row.row(true);
      name.enabled_set(false);
      name.label(favorite.identifier, ICON_ERROR);
    }
    if (!searching) {
      stack_add_menu_pin_button(row, favorite.identifier, true);
    }
  }

  if (shown == 0 && !searching) {
    /* An empty submenu reads as broken; say how the pins get here instead. Nothing for the search
     * to list, so it draws nothing at all.
     * NOTE: This label uses ICON_PINNED to mark the concept of "Favorites", not a toggle state.
     * The toggle itself (pin button in rows) uses ICON_SOLO_ON/SOLO_OFF for consistency with
     * Toggle Asset Favorite; they must not be confused. */
    layout.label(IFACE_("No Favorites Pinned"), ICON_PINNED);
    layout.label(IFACE_("Use the pin button beside an effect in the Add menu"), ICON_NONE);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Pin and reorder operators
 * \{ */

/** The Favorites changed: keep the preferences save-on-exit dirty-state, and let the Outliner
 * redraw its Add menu. The dirty half is written out here the way RNA's own
 * #rna_userdef_is_dirty_update_impl does it (rna_userdef.cc), which this module cannot include;
 * the RNA favorites API calls this function, so the pair lives in one place. */
void outliner_stack_add_favorites_tag_changed()
{
  /* Notifies through the window manager without a context: global notifiers reach every window,
   * matching how preferences updates are handled elsewhere (e.g. userdef dirty-tag updates). */
  if (U.runtime.is_dirty == false) {
    U.runtime.is_dirty = true;
    WM_main_add_notifier(NC_WINDOW, nullptr);
  }
  WM_main_add_notifier(NC_SPACE | ND_SPACE_OUTLINER, nullptr);
}

static wmOperatorStatus stack_add_favorite_set_exec(bContext * /*C*/, wmOperator *op)
{
  char identifier[sizeof(bUserPaintAddFavorite::identifier)];
  RNA_string_get(op->ptr, "identifier", identifier);
  const bool pinned = RNA_boolean_get(op->ptr, "pinned");
  if (identifier[0] == '\0') {
    BKE_report(op->reports, RPT_ERROR, "Cannot pin an Add menu item without an identifier");
    return OPERATOR_CANCELLED;
  }

  const bool changed = pinned ?
                           (BKE_preferences_paint_add_favorite_add(&U, identifier) != nullptr) :
                           BKE_preferences_paint_add_favorite_remove(&U, identifier);
  if (!changed) {
    return OPERATOR_CANCELLED;
  }
  outliner_stack_add_favorites_tag_changed();
  return OPERATOR_FINISHED;
}

void OUTLINER_OT_stack_add_favorite_set(wmOperatorType *ot)
{
  ot->name = "Pin Effect";
  ot->idname = "OUTLINER_OT_stack_add_favorite_set";
  ot->description =
      "Pin an Add menu item to the Favorites submenu, or unpin one that is pinned already";
  ot->exec = stack_add_favorite_set_exec;
  ot->poll = stack_mode_poll;

  RNA_def_string(ot->srna,
                 "identifier",
                 nullptr,
                 sizeof(bUserPaintAddFavorite::identifier),
                 "Identifier",
                 "The stable Add kind identifier, e.g. \"PAINT\" or \"FILTER_BLUR\"");
  /* Stated rather than toggled, so an Unpin entry cannot flip back the way a toggle would. */
  RNA_def_boolean(ot->srna, "pinned", true, "Pinned", "Pin the item when true, unpin when false");
}

static const EnumPropertyItem stack_add_favorite_move_items[] = {
    {PAINT_ADD_FAVORITE_MOVE_UP,
     "UP",
     ICON_TRIA_UP,
     "Move Up",
     "Move the favorite one slot up"},
    {PAINT_ADD_FAVORITE_MOVE_DOWN,
     "DOWN",
     ICON_TRIA_DOWN,
     "Move Down",
     "Move the favorite one slot down"},
    {PAINT_ADD_FAVORITE_MOVE_TOP, "TOP", 0, "Move to Top", "Move the favorite to the top"},
    {PAINT_ADD_FAVORITE_MOVE_BOTTOM,
     "BOTTOM",
     0,
     "Move to Bottom",
     "Move the favorite to the bottom"},
    {0, nullptr, 0, nullptr, nullptr},
};

static wmOperatorStatus stack_add_favorite_reorder_exec(bContext * /*C*/, wmOperator *op)
{
  char identifier[sizeof(bUserPaintAddFavorite::identifier)];
  RNA_string_get(op->ptr, "identifier", identifier);
  if (identifier[0] == '\0') {
    BKE_report(op->reports, RPT_ERROR, "No Add menu item named");
    return OPERATOR_CANCELLED;
  }
  const int direction = RNA_enum_get(op->ptr, "direction");
  if (!BKE_preferences_paint_add_favorite_move(
          &U, identifier, eBKE_PaintAddFavoriteMoveDirection(direction)))
  {
    BKE_report(op->reports, RPT_ERROR, "The favorite is not pinned, or the move changes nothing");
    return OPERATOR_CANCELLED;
  }
  outliner_stack_add_favorites_tag_changed();
  return OPERATOR_FINISHED;
}

void OUTLINER_OT_stack_add_favorite_reorder(wmOperatorType *ot)
{
  ot->name = "Reorder Favorite";
  ot->idname = "OUTLINER_OT_stack_add_favorite_reorder";
  ot->description = "Move a favorite among the Favorites submenu's items";
  ot->exec = stack_add_favorite_reorder_exec;
  ot->poll = stack_mode_poll;

  RNA_def_string(ot->srna,
                 "identifier",
                 nullptr,
                 sizeof(bUserPaintAddFavorite::identifier),
                 "Identifier",
                 "The pinned Add kind identifier");
  RNA_def_enum(ot->srna,
               "direction",
               stack_add_favorite_move_items,
               PAINT_ADD_FAVORITE_MOVE_UP,
               "Direction",
               "Where to move the favorite");
}

/** Shift+A in the Outliner opens the Add menu only in the paint-material stack: the keymap entry
 * follows the Python root menu, which inlines these lists for that source alone (see
 * #stack_add_menu_source_supported for where the lists themselves are valid). The keymap entry
 * stands before `outliner.expanded_toggle`: a poll that fails leaves the event for it, so every
 * other Outliner mode keeps the toggle. */
static bool stack_add_menu_call_poll(bContext *C)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner == nullptr || space_outliner->runtime == nullptr ||
      !stack_add_menu_source_supported(*space_outliner))
  {
    return false;
  }
  return space_outliner->stack_source == SO_STACK_SRC_PAINT_MATERIAL;
}

static wmOperatorStatus stack_add_menu_call_exec(bContext *C, wmOperator * /*op*/)
{
  /* Same call `wm.call_menu` makes, with the operator context the search item and the menu's own
   * operators are built for. */
  WM_menu_name_call(C, OUTLINER_MENU_STACK_LAYER_ADD, wm::OpCallContext::ExecRegionWin);
  return OPERATOR_FINISHED;
}

void OUTLINER_OT_stack_add_menu_call(wmOperatorType *ot)
{
  /* Not the bare "Add": the keymap editor lists both this entry and `outliner.expanded_toggle`
   * under the same Shift+A, and this one has to say what it opens. */
  ot->name = "Add Stack Layer...";
  ot->idname = "OUTLINER_OT_stack_add_menu_call";
  ot->description = "Open the Stack Layers Add menu";
  ot->exec = stack_add_menu_call_exec;
  ot->poll = stack_add_menu_call_poll;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Registration
 * \{ */

static void stack_add_menu_register(const char *idname,
                                    const char *label,
                                    void (*draw)(const bContext *C, Menu *menu))
{
  MenuType *mt = MEM_new_zeroed<MenuType>(__func__);
  STRNCPY_UTF8(mt->idname, idname);
  STRNCPY_UTF8(mt->label, label);
  STRNCPY_UTF8(mt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  mt->draw = draw;
  mt->poll = stack_add_menu_poll;
  /* The lists read the active target and the source's own vocabulary, so the search over the Add
   * menu has to draw them again per context. */
  mt->flag = MenuTypeFlag::ContextDependent;
  WM_menutype_add(mt);
}

void outliner_stack_add_menus_register()
{
  stack_add_menu_register(
      OUTLINER_MENU_STACK_ADD_FAVORITES, N_("Favorites"), stack_add_menu_favorites_draw);
  stack_add_menu_register(
      OUTLINER_MENU_STACK_ADD_EFFECTS, N_("Effects"), stack_add_menu_effects_draw);
  for (const StackAddMenuGroupInfo &group : stack_add_menu_groups) {
    stack_add_menu_register(group.idname, N_(group.label), stack_add_menu_group_draw);
  }

#ifndef NDEBUG
  /* Verify the registered menus kept their ContextDependent flag: without it the lists would
   * not refresh on context changes (e.g. switching the stack source), and the compiler would
   * not catch the removal. */
  MenuType *menu_type = WM_menutype_find(OUTLINER_MENU_STACK_ADD_FAVORITES, false);
  BLI_assert(menu_type != nullptr);
  BLI_assert(flag_is_set(menu_type->flag, MenuTypeFlag::ContextDependent));
#endif
}

/** \} */

}  // namespace blender::ed::outliner
