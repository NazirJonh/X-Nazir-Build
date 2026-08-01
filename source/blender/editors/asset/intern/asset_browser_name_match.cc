/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 *
 * List lifecycle helpers for #FileAssetSelectParams::filter_name_match_map_types.
 */

#include "ED_asset_name_matching.hh"
#include "ED_fileselect.hh"

#include "BKE_context.hh"
#include "BKE_name_matching.hh"
#include "BKE_preferences.h"
#include "BKE_screen.hh"

#include "BLO_read_write.hh"

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_string_utf8.h"

#include "BLT_translation.hh"

#include "DNA_asset_types.h"
#include "DNA_space_enums.h"
#include "DNA_space_types.h"
#include "DNA_userdef_types.h"

#include "RNA_access.hh"

#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include <string>

namespace blender::ed::asset {

void ED_asset_browser_name_match_map_types_free(ListBaseT<AssetNameMatchIdLink> &list)
{
  if (BLI_listbase_head_is_plausible(&list)) {
    list.free_no_destruct();
  }
  else {
    list.clear_no_delete();
  }
}

void ED_asset_browser_name_match_map_types_copy(ListBaseT<AssetNameMatchIdLink> &dst,
                                                const ListBaseT<AssetNameMatchIdLink> &src)
{
  ED_asset_browser_name_match_map_types_free(dst);
  if (!BLI_listbase_head_is_plausible(&src)) {
    return;
  }
  for (const AssetNameMatchIdLink &link : src) {
    AssetNameMatchIdLink *copy = MEM_new<AssetNameMatchIdLink>(__func__);
    STRNCPY_UTF8(copy->id, link.id);
    BLI_addtail(&dst, copy);
  }
}

void ED_asset_browser_name_match_map_types_blend_write(BlendWriter *writer,
                                                       const ListBaseT<AssetNameMatchIdLink> &list)
{
  writer->write_struct_list(&list);
}

void ED_asset_browser_name_match_map_types_blend_read(BlendDataReader *reader,
                                                      ListBaseT<AssetNameMatchIdLink> &list)
{
  BLO_read_struct_list(reader, AssetNameMatchIdLink, &list);
  if (!BLI_listbase_head_is_plausible(&list)) {
    list.clear_no_delete();
  }
}

void ED_asset_browser_name_match_notify(const bContext *C)
{
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_FILE_PARAMS, nullptr);
}

bool ED_asset_browser_name_match_filter_enabled(const FileAssetSelectParams &params)
{
  return (params.asset_flags & FILE_ASSET_FILTER_NAME_MATCH_ENABLED) != 0;
}

void ED_asset_browser_name_match_filter_set_enabled(FileAssetSelectParams &params, const bool enabled)
{
  if (enabled) {
    params.asset_flags |= FILE_ASSET_FILTER_NAME_MATCH_ENABLED;
  }
  else {
    params.asset_flags &= ~FILE_ASSET_FILTER_NAME_MATCH_ENABLED;
  }
}

static bool name_match_map_type_is_active(const FileAssetSelectParams &params,
                                          const char *identifier)
{
  if (identifier == nullptr || identifier[0] == '\0' ||
      !BLI_listbase_head_is_plausible(&params.filter_name_match_map_types))
  {
    return false;
  }
  return BLI_findstring(&params.filter_name_match_map_types,
                        identifier,
                        offsetof(AssetNameMatchIdLink, id)) != nullptr;
}

static bool name_match_map_type_deactivate(FileAssetSelectParams &params, const char *identifier)
{
  if (identifier == nullptr || identifier[0] == '\0' ||
      !BLI_listbase_head_is_plausible(&params.filter_name_match_map_types))
  {
    return false;
  }
  AssetNameMatchIdLink *link = static_cast<AssetNameMatchIdLink *>(BLI_findstring(
      &params.filter_name_match_map_types, identifier, offsetof(AssetNameMatchIdLink, id)));
  if (link == nullptr) {
    return false;
  }
  BLI_freelinkN(&params.filter_name_match_map_types, link);
  return true;
}

static void name_match_map_type_activate(FileAssetSelectParams &params, const char *identifier)
{
  if (identifier == nullptr || identifier[0] == '\0' ||
      name_match_map_type_is_active(params, identifier))
  {
    return;
  }
  if (!BLI_listbase_head_is_plausible(&params.filter_name_match_map_types)) {
    params.filter_name_match_map_types.clear_no_delete();
  }
  AssetNameMatchIdLink *link = MEM_new<AssetNameMatchIdLink>(__func__);
  STRNCPY_UTF8(link->id, identifier);
  BLI_addtail(&params.filter_name_match_map_types, link);
  params.asset_flags |= FILE_ASSET_FILTER_NAME_MATCH_ENABLED;
}

void ED_asset_browser_name_match_map_type_toggle(FileAssetSelectParams &params,
                                                 const StringRef identifier)
{
  const std::string id(identifier);
  if (!name_match_map_type_deactivate(params, id.c_str())) {
    name_match_map_type_activate(params, id.c_str());
  }
}

void ED_asset_browser_name_match_clear_selection(FileAssetSelectParams &params)
{
  ED_asset_browser_name_match_map_types_free(params.filter_name_match_map_types);
  ED_asset_browser_name_match_filter_set_enabled(params, false);
}

bool ED_asset_browser_name_match_entry_visible(const bool filter_enabled,
                                               const bool is_image_asset,
                                               const bool stored_map_type_ids_nonempty,
                                               const NameMatchResolvedFilter &resolved,
                                               const StringRef asset_name,
                                               const Span<StringRef> metadata_tag_names)
{
  if (!filter_enabled) {
    return true;
  }
  if (!is_image_asset) {
    /* Name matching only applies to Images (map-type/token matching against a texture
     * filename); other asset types (Materials, Brushes, ...) are unaffected by this filter and
     * stay visible regardless of its state. */
    return true;
  }
  if (!stored_map_type_ids_nonempty) {
    return true;
  }
  /* Stored IDs exist but none resolved → hide all Images (do not use passes()' show-all). */
  if (!resolved.active) {
    return false;
  }
  return BKE_name_match_resolved_asset_passes(resolved, asset_name, metadata_tag_names);
}

static FileAssetSelectParams *browser_name_match_params_from_context(const bContext *C)
{
  const SpaceFile *sfile = CTX_wm_space_file(C);
  return sfile ? ED_fileselect_get_asset_params(sfile) : nullptr;
}

static void asset_browser_name_match_panel_draw(const bContext *C, Panel *panel)
{
  FileAssetSelectParams *params = browser_name_match_params_from_context(C);
  if (params == nullptr) {
    return;
  }

  ui::Layout &layout = *panel->layout;
  /* The popover remains interactive while the filter is disabled so the user can select a map
   * type and enable the filter in one action. A disabled panel layout would recursively mark all
   * descendant buttons as disabled during Layout::resolve(). */
  layout.enabled_set(true);

  const bool filter_enabled = ED_asset_browser_name_match_filter_enabled(*params);
  {
    ui::Layout &label_row = layout.row(false);
    label_row.enabled_set(filter_enabled);
    label_row.label(IFACE_("Map Types"), ICON_NONE);
  }
  for (const bUserNameMatchMapType &map_type : U.name_match_map_types) {
    if (map_type.identifier[0] == '\0') {
      continue;
    }
    const bool active = name_match_map_type_is_active(*params, map_type.identifier);
    ui::Layout &row = layout.row(false);
    const char *display_name = map_type.name[0] != '\0' ? map_type.name : map_type.identifier;
    PointerRNA props = row.op("ASSET_OT_browser_name_match_map_type_toggle",
                              display_name,
                              active ? ICON_CHECKBOX_HLT : ICON_CHECKBOX_DEHLT);
    if (props.type != nullptr) {
      RNA_string_set(&props, "identifier", map_type.identifier);
    }
  }
  layout.separator();
  /* Always interactive: clearing the map-type selection should not require the filter to be
   * enabled first, matching the "always clickable" map type buttons above. */
  layout.op("ASSET_OT_browser_name_match_clear", IFACE_("Clear Filter"), ICON_X);
  layout.separator();
  PointerRNA preferences_props = layout.op(
      "SCREEN_OT_userpref_show", IFACE_("Open Preferences..."), ICON_PREFERENCES);
  if (preferences_props.type != nullptr) {
    RNA_enum_set(&preferences_props, "section", USER_SECTION_ASSETS);
  }
}

void ED_asset_browser_name_match_panel_register()
{
  if (WM_paneltype_find("ASSETBROWSER_PT_name_match", true)) {
    return;
  }

  PanelType *pt = MEM_new_zeroed<PanelType>(__func__);
  STRNCPY_UTF8(pt->idname, "ASSETBROWSER_PT_name_match");
  STRNCPY_UTF8(pt->label, N_("Name Match Filter"));
  STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->description = N_("Select map types for name matching in the Asset Browser");
  pt->draw = asset_browser_name_match_panel_draw;
  pt->ui_units_x = 10;
  WM_paneltype_add(pt);
}

}  // namespace blender::ed::asset
