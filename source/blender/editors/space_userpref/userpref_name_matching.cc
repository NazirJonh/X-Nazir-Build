/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spuserpref
 *
 * Preferences → Assets → Name Matching panel.
 */

#include "BKE_context.hh"
#include "BKE_name_matching.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_string_ref.hh"
#include "BLI_string_utf8.h"
#include "BLI_string_utils.hh"
#include "BLI_utildefines.h"

#include "BLT_translation.hh"

#include "ED_asset_name_matching.hh"

#include "DNA_screen_types.h"
#include "DNA_userdef_types.h"
#include "DNA_windowmanager_types.h"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"

#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "UI_tree_view.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "userpref_intern.hh"

#include <climits>
#include <string>

namespace blender {

static void name_matching_clamp_active_map_type()
{
  const int count = U.name_match_map_types.count();
  if (count <= 0) {
    U.active_name_match_map_type = 0;
  }
  else {
    CLAMP(U.active_name_match_map_type, short(0), short(count - 1));
  }
}

static void name_matching_clamp_active_filter_tag()
{
  const int count = U.name_match_filter_tags.count();
  if (count <= 0) {
    U.active_name_match_filter_tag = 0;
  }
  else {
    CLAMP(U.active_name_match_filter_tag, short(0), short(count - 1));
  }
}

static bUserNameMatchMapType *name_matching_active_map_type()
{
  name_matching_clamp_active_map_type();
  return static_cast<bUserNameMatchMapType *>(
      BLI_findlink(&U.name_match_map_types, U.active_name_match_map_type));
}

static bUserNameMatchFilterTag *name_matching_active_filter_tag()
{
  name_matching_clamp_active_filter_tag();
  return static_cast<bUserNameMatchFilterTag *>(
      BLI_findlink(&U.name_match_filter_tags, U.active_name_match_filter_tag));
}

/* -------------------------------------------------------------------- */
/** \name Operators
 * \{ */

static wmOperatorStatus preferences_name_match_map_type_add_exec(bContext * /*C*/,
                                                                 wmOperator *op)
{
  char name[64];
  char identifier[64];
  RNA_string_get(op->ptr, "name", name);
  RNA_string_get(op->ptr, "identifier", identifier);

  if (identifier[0] == '\0') {
    /* Derive a simple CUSTOM_ slug from the display name. */
    SNPRINTF_UTF8(identifier, "CUSTOM_%s", name[0] ? name : "map_type");
    for (char *c = identifier; *c; c++) {
      if (!(((*c >= 'A') && (*c <= 'Z')) || ((*c >= 'a') && (*c <= 'z')) ||
            ((*c >= '0') && (*c <= '9')) || (*c == '_')))
      {
        *c = '_';
      }
    }
  }

  bUserNameMatchMapType *map_type = BKE_name_matching_map_type_add(&U, name, identifier, 0);
  if (map_type == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "Could not add map type");
    return OPERATOR_CANCELLED;
  }

  U.active_name_match_map_type = short(BLI_findindex(&U.name_match_map_types, map_type));
  U.runtime.is_dirty = true;
  WM_main_add_notifier(NC_WINDOW, nullptr);
  return OPERATOR_FINISHED;
}

static void PREFERENCES_OT_name_match_map_type_add(wmOperatorType *ot)
{
  ot->name = "Add Name Match Map Type";
  ot->idname = "PREFERENCES_OT_name_match_map_type_add";
  ot->description = "Add a custom map type for asset name matching";
  ot->exec = preferences_name_match_map_type_add_exec;
  ot->flag = OPTYPE_REGISTER;

  RNA_def_string(ot->srna, "name", "Custom", 64, "Name", "");
  RNA_def_string(ot->srna, "identifier", nullptr, 64, "Identifier", "Leave empty to auto-generate");
}

static bool preferences_name_match_map_type_remove_poll(bContext * /*C*/)
{
  const bUserNameMatchMapType *map_type = name_matching_active_map_type();
  return map_type && (map_type->flag & USER_NAME_MATCH_MAP_TYPE_BUILTIN) == 0;
}

static wmOperatorStatus preferences_name_match_map_type_remove_exec(bContext *C, wmOperator *op)
{
  /* Prefer explicit index when set by callers; otherwise remove the active item. */
  int index = RNA_int_get(op->ptr, "index");
  if (index < 0) {
    index = U.active_name_match_map_type;
  }
  bUserNameMatchMapType *map_type = static_cast<bUserNameMatchMapType *>(
      BLI_findlink(&U.name_match_map_types, index));
  const std::string identifier = map_type ? map_type->identifier : "";
  if (!BKE_name_matching_map_type_remove(&U, map_type)) {
    BKE_report(op->reports, RPT_ERROR, "Cannot remove built-in or missing map type");
    return OPERATOR_CANCELLED;
  }
  ed::asset::name_match_map_type_id_replace(*CTX_data_main(C), identifier, "");
  name_matching_clamp_active_map_type();
  U.runtime.is_dirty = true;
  WM_main_add_notifier(NC_WINDOW, nullptr);
  return OPERATOR_FINISHED;
}

static void PREFERENCES_OT_name_match_map_type_remove(wmOperatorType *ot)
{
  ot->name = "Remove Name Match Map Type";
  ot->idname = "PREFERENCES_OT_name_match_map_type_remove";
  ot->description = "Remove a custom map type";
  ot->exec = preferences_name_match_map_type_remove_exec;
  ot->poll = preferences_name_match_map_type_remove_poll;
  ot->flag = OPTYPE_REGISTER;

  RNA_def_int(ot->srna, "index", -1, -1, INT_MAX, "Index", "Map type index, or -1 for active", -1, INT_MAX);
}

static wmOperatorStatus preferences_name_match_token_add_exec(bContext * /*C*/, wmOperator *op)
{
  int map_index = RNA_int_get(op->ptr, "map_type_index");
  if (map_index < 0) {
    map_index = U.active_name_match_map_type;
  }
  bUserNameMatchMapType *map_type = static_cast<bUserNameMatchMapType *>(
      BLI_findlink(&U.name_match_map_types, map_index));
  if (map_type == nullptr) {
    return OPERATOR_CANCELLED;
  }

  /* Route through #BKE_name_matching_token_add so a placeholder can never land as a
   * case-insensitive duplicate of an existing token (its own uniqueness contract), unlike
   * #BLI_uniquename's case-sensitive clash check. */
  bUserNameMatchToken *token = BKE_name_matching_token_add(map_type, "Token");
  for (int suffix = 1; token == nullptr && suffix < 1000; suffix++) {
    char candidate[sizeof(bUserNameMatchToken::value)];
    SNPRINTF(candidate, "Token.%03d", suffix);
    token = BKE_name_matching_token_add(map_type, candidate);
  }
  if (token == nullptr) {
    return OPERATOR_CANCELLED;
  }
  U.runtime.is_dirty = true;
  WM_main_add_notifier(NC_WINDOW, nullptr);
  return OPERATOR_FINISHED;
}

static void PREFERENCES_OT_name_match_token_add(wmOperatorType *ot)
{
  ot->name = "Add Name Match Token";
  ot->idname = "PREFERENCES_OT_name_match_token_add";
  ot->description = "Add a token to a map type";
  ot->exec = preferences_name_match_token_add_exec;
  ot->flag = OPTYPE_REGISTER;

  RNA_def_int(ot->srna,
              "map_type_index",
              -1,
              -1,
              INT_MAX,
              "Map Type Index",
              "Map type index, or -1 for active",
              -1,
              INT_MAX);
}

static wmOperatorStatus preferences_name_match_token_remove_exec(bContext * /*C*/, wmOperator *op)
{
  int map_index = RNA_int_get(op->ptr, "map_type_index");
  if (map_index < 0) {
    map_index = U.active_name_match_map_type;
  }
  const int token_index = RNA_int_get(op->ptr, "token_index");
  bUserNameMatchMapType *map_type = static_cast<bUserNameMatchMapType *>(
      BLI_findlink(&U.name_match_map_types, map_index));
  if (map_type == nullptr) {
    return OPERATOR_CANCELLED;
  }
  bUserNameMatchToken *token = static_cast<bUserNameMatchToken *>(
      BLI_findlink(&map_type->tokens, token_index));
  if (token == nullptr) {
    return OPERATOR_CANCELLED;
  }
  BKE_name_matching_token_remove(map_type, token);
  U.runtime.is_dirty = true;
  WM_main_add_notifier(NC_WINDOW, nullptr);
  return OPERATOR_FINISHED;
}

static void PREFERENCES_OT_name_match_token_remove(wmOperatorType *ot)
{
  ot->name = "Remove Name Match Token";
  ot->idname = "PREFERENCES_OT_name_match_token_remove";
  ot->description = "Remove a token from a map type";
  ot->exec = preferences_name_match_token_remove_exec;
  ot->flag = OPTYPE_REGISTER;

  RNA_def_int(ot->srna,
              "map_type_index",
              -1,
              -1,
              INT_MAX,
              "Map Type Index",
              "Map type index, or -1 for active",
              -1,
              INT_MAX);
  RNA_def_int(ot->srna, "token_index", 0, 0, INT_MAX, "Token Index", "", 0, INT_MAX);
}

static wmOperatorStatus preferences_name_match_filter_tag_add_exec(bContext * /*C*/,
                                                                   wmOperator *op)
{
  char name[64];
  RNA_string_get(op->ptr, "name", name);
  bUserNameMatchFilterTag *tag = BKE_name_matching_filter_tag_add(&U, name);
  if (tag == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "Could not add filter tag");
    return OPERATOR_CANCELLED;
  }
  U.active_name_match_filter_tag = short(BLI_findindex(&U.name_match_filter_tags, tag));
  U.runtime.is_dirty = true;
  WM_main_add_notifier(NC_WINDOW, nullptr);
  return OPERATOR_FINISHED;
}

static void PREFERENCES_OT_name_match_filter_tag_add(wmOperatorType *ot)
{
  ot->name = "Add Name Match Filter Tag";
  ot->idname = "PREFERENCES_OT_name_match_filter_tag_add";
  ot->description = "Add a filter tag for asset name matching";
  ot->exec = preferences_name_match_filter_tag_add_exec;
  ot->flag = OPTYPE_REGISTER;

  RNA_def_string(ot->srna, "name", "Tag", 64, "Name", "");
}

static bool preferences_name_match_filter_tag_remove_poll(bContext * /*C*/)
{
  return name_matching_active_filter_tag() != nullptr;
}

static wmOperatorStatus preferences_name_match_filter_tag_remove_exec(bContext * /*C*/,
                                                                      wmOperator *op)
{
  int index = RNA_int_get(op->ptr, "index");
  if (index < 0) {
    index = U.active_name_match_filter_tag;
  }
  bUserNameMatchFilterTag *tag = static_cast<bUserNameMatchFilterTag *>(
      BLI_findlink(&U.name_match_filter_tags, index));
  if (tag == nullptr) {
    return OPERATOR_CANCELLED;
  }
  BKE_name_matching_filter_tag_remove(&U, tag);
  name_matching_clamp_active_filter_tag();
  U.runtime.is_dirty = true;
  WM_main_add_notifier(NC_WINDOW, nullptr);
  return OPERATOR_FINISHED;
}

static void PREFERENCES_OT_name_match_filter_tag_remove(wmOperatorType *ot)
{
  ot->name = "Remove Name Match Filter Tag";
  ot->idname = "PREFERENCES_OT_name_match_filter_tag_remove";
  ot->description = "Remove a filter tag";
  ot->exec = preferences_name_match_filter_tag_remove_exec;
  ot->poll = preferences_name_match_filter_tag_remove_poll;
  ot->flag = OPTYPE_REGISTER;

  RNA_def_int(ot->srna, "index", -1, -1, INT_MAX, "Index", "Filter tag index, or -1 for active", -1, INT_MAX);
}

void userpref_name_matching_operatortypes()
{
  WM_operatortype_append(PREFERENCES_OT_name_match_map_type_add);
  WM_operatortype_append(PREFERENCES_OT_name_match_map_type_remove);
  WM_operatortype_append(PREFERENCES_OT_name_match_token_add);
  WM_operatortype_append(PREFERENCES_OT_name_match_token_remove);
  WM_operatortype_append(PREFERENCES_OT_name_match_filter_tag_add);
  WM_operatortype_append(PREFERENCES_OT_name_match_filter_tag_remove);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Map Types Tree View
 * \{ */

class NameMatchMapTypeListItem : public ui::AbstractTreeViewItem {
  bUserNameMatchMapType &map_type_;
  int index_ = 0;

 public:
  NameMatchMapTypeListItem(bUserNameMatchMapType &map_type, const int index)
      : map_type_(map_type), index_(index)
  {
    label_ = map_type_.name;
  }

  void build_row(ui::Layout &row) override
  {
    row.label(label_, ICON_TEXTURE);

    if (map_type_.flag & USER_NAME_MATCH_MAP_TYPE_BUILTIN) {
      ui::Layout &sub = row.row(true);
      sub.active_set(false);
      sub.alignment_set(ui::LayoutAlign::Right);
      sub.label(IFACE_("Built-In"), ICON_NONE);
    }
  }

  void on_activate(bContext & /*C*/) override
  {
    U.active_name_match_map_type = short(index_);
    U.runtime.is_dirty = true;
  }

  std::optional<bool> should_be_active() const override
  {
    return U.active_name_match_map_type == index_;
  }

  bool supports_renaming() const override
  {
    return true;
  }

  bool rename(const bContext & /*C*/, StringRefNull new_name) override
  {
    STRNCPY_UTF8(map_type_.name, new_name.c_str());
    BLI_uniquename(&U.name_match_map_types,
                   &map_type_,
                   "Map Type",
                   '.',
                   offsetof(bUserNameMatchMapType, name),
                   sizeof(map_type_.name));
    label_ = map_type_.name;
    U.runtime.is_dirty = true;
    return true;
  }

  void delete_item(bContext *C) override
  {
    if (map_type_.flag & USER_NAME_MATCH_MAP_TYPE_BUILTIN) {
      BKE_report(CTX_wm_reports(C), RPT_ERROR, "Cannot remove built-in map type");
      return;
    }
    const std::string identifier = map_type_.identifier;
    if (!BKE_name_matching_map_type_remove(&U, &map_type_)) {
      return;
    }
    ed::asset::name_match_map_type_id_replace(*CTX_data_main(C), identifier, "");
    name_matching_clamp_active_map_type();
    U.runtime.is_dirty = true;
  }

  void build_context_menu(bContext & /*C*/, ui::Layout &column) const override
  {
    column.op("UI_OT_view_item_rename", IFACE_("Rename"), ICON_NONE);
    if ((map_type_.flag & USER_NAME_MATCH_MAP_TYPE_BUILTIN) == 0) {
      column.separator();
      column.op("UI_OT_view_item_delete", IFACE_("Delete"), ICON_NONE);
    }
  }

  bool should_be_filtered_visible(StringRefNull filter_string) const override
  {
    if (filter_string.is_empty()) {
      return true;
    }
    return BLI_strcasestr(label_.c_str(), filter_string.c_str()) != nullptr;
  }
};

struct NameMatchMapTypeList : public ui::AbstractTreeView {
  void build_tree() override
  {
    this->is_flat_ = true;

    int index = 0;
    for (bUserNameMatchMapType &map_type : U.name_match_map_types) {
      add_tree_item<NameMatchMapTypeListItem>(map_type, index);
      index++;
    }
  }
};

static void draw_map_type_list(const bContext &C, ui::Layout &layout)
{
  ui::Block *block = layout.block();
  ui::AbstractTreeView *tree_view = block_add_view(
      *block, "Name Match Map Types", std::make_unique<NameMatchMapTypeList>());
  tree_view->set_context_menu_title("Map Type");
  tree_view->set_default_rows(6);
  ui::TreeViewBuilder::build_tree_view(C, *tree_view, layout);
}

static void draw_active_map_type_settings(ui::Layout &layout, bUserNameMatchMapType &map_type)
{
  PointerRNA map_ptr = RNA_pointer_create_discrete(nullptr, RNA_NameMatchMapType, &map_type);
  const int map_index = BLI_findindex(&U.name_match_map_types, &map_type);

  layout.prop(&map_ptr, "name", UI_ITEM_NONE, IFACE_("Name"), ICON_NONE);
  layout.prop(&map_ptr, "identifier", UI_ITEM_NONE, IFACE_("Identifier"), ICON_NONE);

  layout.separator();
  layout.label(IFACE_("Tokens"), ICON_NONE);

  ui::Layout &tokens_col = layout.column(true);
  int token_index = 0;
  PropertyRNA *tokens_prop = RNA_struct_find_property(&map_ptr, "tokens");
  RNA_PROP_BEGIN (&map_ptr, token_ptr, tokens_prop) {
    ui::Layout &token_row = tokens_col.row(true);
    token_row.prop(&token_ptr, "value", UI_ITEM_NONE, "", ICON_NONE);
    PointerRNA props = token_row.op("preferences.name_match_token_remove",
                                    std::nullopt,
                                    ICON_REMOVE,
                                    wm::OpCallContext::ExecDefault,
                                    ui::ITEM_R_ICON_ONLY);
    RNA_int_set(&props, "map_type_index", map_index);
    RNA_int_set(&props, "token_index", token_index);
    token_index++;
  }
  RNA_PROP_END;

  PointerRNA add_token = tokens_col.op(
      "preferences.name_match_token_add", IFACE_("Add Token"), ICON_ADD);
  RNA_int_set(&add_token, "map_type_index", map_index);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Filter Tags Tree View
 * \{ */

class NameMatchFilterTagListItem : public ui::AbstractTreeViewItem {
  bUserNameMatchFilterTag &tag_;
  int index_ = 0;

 public:
  NameMatchFilterTagListItem(bUserNameMatchFilterTag &tag, const int index)
      : tag_(tag), index_(index)
  {
    label_ = tag_.name;
  }

  void build_row(ui::Layout &row) override
  {
    row.label(label_, ICON_BOOKMARKS);
  }

  void on_activate(bContext & /*C*/) override
  {
    U.active_name_match_filter_tag = short(index_);
    U.runtime.is_dirty = true;
  }

  std::optional<bool> should_be_active() const override
  {
    return U.active_name_match_filter_tag == index_;
  }

  bool supports_renaming() const override
  {
    return true;
  }

  bool rename(const bContext & /*C*/, StringRefNull new_name) override
  {
    STRNCPY_UTF8(tag_.name, new_name.c_str());
    BLI_uniquename(&U.name_match_filter_tags,
                   &tag_,
                   "Tag",
                   '.',
                   offsetof(bUserNameMatchFilterTag, name),
                   sizeof(tag_.name));
    label_ = tag_.name;
    U.runtime.is_dirty = true;
    return true;
  }

  void delete_item(bContext * /*C*/) override
  {
    BKE_name_matching_filter_tag_remove(&U, &tag_);
    name_matching_clamp_active_filter_tag();
    U.runtime.is_dirty = true;
  }

  void build_context_menu(bContext & /*C*/, ui::Layout &column) const override
  {
    column.op("UI_OT_view_item_rename", IFACE_("Rename"), ICON_NONE);
    column.separator();
    column.op("UI_OT_view_item_delete", IFACE_("Delete"), ICON_NONE);
  }

  bool should_be_filtered_visible(StringRefNull filter_string) const override
  {
    if (filter_string.is_empty()) {
      return true;
    }
    return BLI_strcasestr(label_.c_str(), filter_string.c_str()) != nullptr;
  }
};

struct NameMatchFilterTagList : public ui::AbstractTreeView {
  void build_tree() override
  {
    this->is_flat_ = true;

    int index = 0;
    for (bUserNameMatchFilterTag &tag : U.name_match_filter_tags) {
      add_tree_item<NameMatchFilterTagListItem>(tag, index);
      index++;
    }
  }
};

static void draw_filter_tag_list(const bContext &C, ui::Layout &layout)
{
  ui::Block *block = layout.block();
  ui::AbstractTreeView *tree_view = block_add_view(
      *block, "Name Match Filter Tags", std::make_unique<NameMatchFilterTagList>());
  tree_view->set_context_menu_title("Filter Tag");
  tree_view->set_default_rows(5);
  ui::TreeViewBuilder::build_tree_view(C, *tree_view, layout);
}

static void draw_active_filter_tag_settings(ui::Layout &layout, bUserNameMatchFilterTag &tag)
{
  PointerRNA tag_ptr = RNA_pointer_create_discrete(nullptr, RNA_NameMatchFilterTag, &tag);
  layout.prop(&tag_ptr, "name", UI_ITEM_NONE, IFACE_("Name"), ICON_NONE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Panel
 * \{ */

static void userpref_name_matching_panel_draw(const bContext * /*C*/, Panel * /*panel*/)
{
  /* Parent panel only hosts sub-panels; nothing of its own to draw. Builtins are seeded once at
   * #BKE_blendfile_userdef_from_defaults / versioning / homefile-read time (see
   * #BKE_name_matching_userdef_ensure_defaults) and, since built-in rows can't be removed, stay
   * valid for the lifetime of the session -- no need to re-check on every redraw. */
}

static void userpref_name_matching_map_types_panel_draw(const bContext *C, Panel *panel)
{
  name_matching_clamp_active_map_type();

  ui::Layout &layout = *panel->layout;
  ui::Layout &row = layout.row(false);

  draw_map_type_list(*C, row);

  ui::Layout &col = row.column(true);
  col.op("preferences.name_match_map_type_add", "", ICON_ADD);

  ui::Layout &sub = col.row(true);
  const bUserNameMatchMapType *active = name_matching_active_map_type();
  const bool can_remove = active && (active->flag & USER_NAME_MATCH_MAP_TYPE_BUILTIN) == 0;
  sub.enabled_set(can_remove);
  sub.op("preferences.name_match_map_type_remove", "", ICON_REMOVE);

  if (active == nullptr) {
    return;
  }

  layout.separator();
  draw_active_map_type_settings(layout, *const_cast<bUserNameMatchMapType *>(active));
}

static void userpref_name_matching_filter_tags_panel_draw(const bContext *C, Panel *panel)
{
  name_matching_clamp_active_filter_tag();

  ui::Layout &layout = *panel->layout;
  ui::Layout &row = layout.row(false);

  draw_filter_tag_list(*C, row);

  ui::Layout &col = row.column(true);
  col.op("preferences.name_match_filter_tag_add", "", ICON_ADD);

  ui::Layout &sub = col.row(true);
  const bool can_remove = name_matching_active_filter_tag() != nullptr;
  sub.enabled_set(can_remove);
  sub.op("preferences.name_match_filter_tag_remove", "", ICON_REMOVE);

  bUserNameMatchFilterTag *active = name_matching_active_filter_tag();
  if (active == nullptr) {
    return;
  }

  layout.separator();
  draw_active_filter_tag_settings(layout, *active);
}

/** Filter Tags is implemented but intentionally hidden from the UI for now (see caller). */
static bool userpref_name_matching_filter_tags_panel_poll(const bContext * /*C*/,
                                                          PanelType * /*pt*/)
{
  return false;
}

static PanelType *name_matching_subpanel_register(ARegionType &region_type,
                                                  PanelType *parent,
                                                  const char *idname,
                                                  const char *label,
                                                  void (*draw)(const bContext *, Panel *))
{
  PanelType *panel_type = MEM_new_zeroed<PanelType>(__func__);
  STRNCPY_UTF8(panel_type->idname, idname);
  STRNCPY_UTF8(panel_type->label, label);
  STRNCPY_UTF8(panel_type->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  STRNCPY(panel_type->context, "assets");
  panel_type->space_type = SPACE_USERPREF;
  panel_type->region_type = RGN_TYPE_WINDOW;
  panel_type->flag = 0;
  panel_type->draw = draw;
  STRNCPY_UTF8(panel_type->parent_id, parent->idname);
  panel_type->parent = parent;
  BLI_addtail(&parent->children, BLI_genericNodeN(panel_type));
  BLI_addtail(&region_type.paneltypes, panel_type);
  return panel_type;
}

void userpref_name_matching_panel_register(ARegionType &region_type)
{
  PanelType *parent = MEM_new_zeroed<PanelType>(__func__);
  STRNCPY_UTF8(parent->idname, "USERPREF_PT_name_matching");
  STRNCPY_UTF8(parent->label, N_("Name Matching"));
  STRNCPY_UTF8(parent->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  STRNCPY(parent->context, "assets");
  parent->space_type = SPACE_USERPREF;
  parent->region_type = RGN_TYPE_WINDOW;
  parent->flag = PANEL_TYPE_DEFAULT_CLOSED;
  parent->draw = userpref_name_matching_panel_draw;
  BLI_addtail(&region_type.paneltypes, parent);

  name_matching_subpanel_register(region_type,
                                  parent,
                                  "USERPREF_PT_name_matching_map_types",
                                  N_("Map Types"),
                                  userpref_name_matching_map_types_panel_draw);

  /* Filter Tags is fully implemented (DNA, RNA, BKE CRUD, operators) but the feature is
   * intentionally hidden from the UI for now: the panel stays registered (so scripts/tests can
   * still find it) but never polls true. Remove the poll override to bring it back -- see
   * #shelf_name_match_filter_includes_tags for what's still missing before that's useful. */
  PanelType *filter_tags_panel = name_matching_subpanel_register(
      region_type,
      parent,
      "USERPREF_PT_name_matching_filter_tags",
      N_("Filter Tags"),
      userpref_name_matching_filter_tags_panel_draw);
  filter_tags_panel->poll = userpref_name_matching_filter_tags_panel_poll;
}

/** \} */

}  // namespace blender
