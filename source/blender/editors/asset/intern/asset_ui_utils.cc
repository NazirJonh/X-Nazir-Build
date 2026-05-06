/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 */

#include <algorithm>
#include <fmt/format.h>
#include <optional>
#include <string>

#include "AS_asset_library.hh"
#include "AS_asset_representation.hh"

#include "BKE_context.hh"
#include "BKE_idprop.hh"
#include "BKE_preferences.h"
#include "BKE_preview_image.hh"

#include "BLI_assert.h"
#include "BLI_listbase_iterator.hh"
#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "BLI_string_utils.hh"

#include "BLT_translation.hh"

#include "DNA_ID.h"
#include "DNA_asset_types.h"
#include "DNA_space_enums.h"
#include "DNA_userdef_types.h"

#include "MEM_guardedalloc.h"

#include "UI_interface_c.hh"
#include "UI_interface_icons.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "WM_api.hh"
#include "WM_keymap.hh"
#include "WM_types.hh"
#include "wm_event_types.hh"

#include "ED_asset.hh"

namespace blender::ed::asset {

/* -------------------------------------------------------------------- */
/** \name Brush Hotkey Helpers
 * \{ */

static void cleanup_operator_resources(IDProperty *search_properties,
                                       PointerRNA *op_props_ptr_template)
{
  if (search_properties) {
    IDP_FreeProperty(search_properties);
  }
  if (op_props_ptr_template) {
    WM_operator_properties_free(op_props_ptr_template);
    MEM_delete(op_props_ptr_template);
  }
}

static std::string normalize_asset_path(const std::string &path)
{
  std::string normalized = path;
  std::replace(normalized.begin(), normalized.end(), '\\', '/');
  return normalized;
}

static IDProperty *create_operator_properties(const AssetWeakReference &weak_ref,
                                              StructRNA *op_props_type,
                                              const char *op_name)
{
  IDPropertyTemplate val{};
  IDProperty *search_properties = IDP_New(IDP_GROUP, &val, "wmOpItemProp");
  PointerRNA search_op_props_ptr = RNA_pointer_create_discrete(
      nullptr, op_props_type, search_properties);

  if (STREQ(op_name, "WM_OT_tool_set_by_id")) {
    const std::string normalized_path = normalize_asset_path(weak_ref.relative_asset_identifier);
    RNA_string_set(&search_op_props_ptr, "name", normalized_path.c_str());
  }
  else if (STREQ(op_name, "PAINT_OT_brush_select")) {
    const std::string normalized_path = normalize_asset_path(weak_ref.relative_asset_identifier);
    if (RNA_struct_find_property(&search_op_props_ptr, "brush")) {
      RNA_string_set(&search_op_props_ptr, "brush", normalized_path.c_str());
    }
  }
  else {
    RNA_enum_set(&search_op_props_ptr, "asset_library_type", weak_ref.asset_library_type);
    /* Only set relative_asset_identifier if it's actually present.
     * Keymap entries may have been registered without this property. */
    if (weak_ref.relative_asset_identifier && weak_ref.relative_asset_identifier[0] != '\0') {
      const std::string normalized_path = normalize_asset_path(weak_ref.relative_asset_identifier);
      RNA_string_set(&search_op_props_ptr, "relative_asset_identifier", normalized_path.c_str());
    }
    /* Only set asset_library_identifier if it's actually present.
     * Keymap entries often don't have this property set, so adding an empty string
     * would cause the comparison to fail. */
    if (weak_ref.asset_library_identifier && weak_ref.asset_library_identifier[0] != '\0') {
      RNA_string_set(
          &search_op_props_ptr, "asset_library_identifier", weak_ref.asset_library_identifier);
    }
  }
  return search_properties;
}

/* Keymaps to search for brush hotkeys, from most to least specific. */
static const char *brush_keymap_names[] = {"Sculpt", "3D View", "Window", nullptr};

static std::string find_hotkey_for_operator(const bContext *C,
                                            const char *op_name,
                                            const AssetWeakReference &weak_ref,
                                            bool is_strict)
{
  PointerRNA *op_props_ptr_template = nullptr;
  IDProperty *properties_template = nullptr;
  WM_operator_properties_alloc(&op_props_ptr_template, &properties_template, op_name);

  if (!op_props_ptr_template) {
    return "";
  }

  IDProperty *search_properties = create_operator_properties(
      weak_ref, op_props_ptr_template->type, op_name);

  std::optional<std::string> hotkey = WM_key_event_operator_string(
      C, op_name, wm::OpCallContext::InvokeDefault, search_properties, is_strict);

  cleanup_operator_resources(search_properties, op_props_ptr_template);
  return hotkey.value_or("");
}

static std::string find_hotkey_in_context(const bContext *C,
                                          const char *op_name,
                                          const AssetWeakReference &weak_ref,
                                          const char *keymap_name)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  if (!wm) {
    return "";
  }

  wmKeyMap *default_keymap = nullptr;
  if (wm->runtime->defaultconf) {
    default_keymap = WM_keymap_list_find(
        &wm->runtime->defaultconf->keymaps, keymap_name, SPACE_EMPTY, RGN_TYPE_WINDOW);
  }

  wmKeyMap *keymap = WM_keymap_active(wm, default_keymap);
  if (!keymap) {
    keymap = WM_keymap_find_all(wm, keymap_name, SPACE_EMPTY, RGN_TYPE_WINDOW);
    if (!keymap) {
      return "";
    }
  }

  PointerRNA *op_props_ptr_template = nullptr;
  IDProperty *properties_template = nullptr;
  WM_operator_properties_alloc(&op_props_ptr_template, &properties_template, op_name);

  if (!op_props_ptr_template) {
    return "";
  }

  IDProperty *search_properties = create_operator_properties(
      weak_ref, op_props_ptr_template->type, op_name);

  std::string result = "";

  wmKeyMapItem *kmi = WM_key_event_operator_from_keymap(
      keymap, op_name, search_properties, EVT_TYPE_MASK_ALL, 0);

  if (kmi) {
    std::optional<std::string> hotkey_opt = WM_keymap_item_to_string(kmi, false);
    if (hotkey_opt.has_value()) {
      result = hotkey_opt.value();
    }
  }

  cleanup_operator_resources(search_properties, op_props_ptr_template);

  return result;
}

/** Search all brush keymaps for a BRUSH_OT_asset_activate entry whose brush name
 * (last path component of relative_asset_identifier) matches \a brush_name.
 * \a preferred_library_type: entries whose stored asset_library_type matches this value
 * are returned immediately; the first other name-match is kept as a fallback.
 * Pass -1 to accept the first match regardless of library. */
static std::string find_hotkey_by_brush_name_in_keymaps(const bContext *C,
                                                         const std::string &brush_name,
                                                         const int preferred_library_type)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  if (!wm) {
    return "";
  }

  std::string fallback_hotkey;

  for (int i = 0; brush_keymap_names[i]; i++) {
    wmKeyMap *default_keymap = nullptr;
    if (wm->runtime->defaultconf) {
      default_keymap = WM_keymap_list_find(
          &wm->runtime->defaultconf->keymaps, brush_keymap_names[i], SPACE_EMPTY, RGN_TYPE_WINDOW);
    }
    wmKeyMap *keymap = WM_keymap_active(wm, default_keymap);
    if (!keymap) {
      keymap = WM_keymap_find_all(wm, brush_keymap_names[i], SPACE_EMPTY, RGN_TYPE_WINDOW);
      if (!keymap) {
        continue;
      }
    }

    for (wmKeyMapItem *kmi = static_cast<wmKeyMapItem *>(keymap->items.first); kmi != nullptr;
         kmi = kmi->next)
    {
      if (kmi->flag & KMI_INACTIVE) {
        continue;
      }
      if (!STREQ(kmi->idname, "BRUSH_OT_asset_activate")) {
        continue;
      }
      if (!kmi->properties) {
        continue;
      }

      IDProperty *id_prop = IDP_GetPropertyFromGroup(kmi->properties,
                                                     "relative_asset_identifier");
      if (!id_prop || id_prop->type != IDP_STRING) {
        continue;
      }

      const std::string kmi_path = normalize_asset_path(IDP_string_get(id_prop));
      const size_t last_sep = kmi_path.find_last_of('/');
      const std::string kmi_name = (last_sep != std::string::npos) ?
                                       kmi_path.substr(last_sep + 1) :
                                       kmi_path;

      if (kmi_name != brush_name) {
        continue;
      }

      std::optional<std::string> hotkey = WM_keymap_item_to_string(kmi, false);
      if (!hotkey.has_value()) {
        continue;
      }

      /* If this entry's library type matches the preferred one, return it immediately.
       * Otherwise keep it as a fallback in case no better match is found. */
      IDProperty *lib_type_prop = IDP_GetPropertyFromGroup(kmi->properties, "asset_library_type");
      if (lib_type_prop && lib_type_prop->type == IDP_INT &&
          lib_type_prop->data.val == preferred_library_type)
      {
        return hotkey.value();
      }
      if (fallback_hotkey.empty()) {
        fallback_hotkey = hotkey.value();
      }
    }
  }

  return fallback_hotkey;
}

/**
 * Legacy fallback: search for hotkeys registered under older operators or path formats.
 * Called only after all BRUSH_OT_asset_activate searches have already failed.
 */
static std::string find_alternative_hotkeys(const bContext *C, const AssetWeakReference &weak_ref)
{
  /* Legacy operator used before the asset system: keybindings from older Blender versions
   * or user configs may still point to PAINT_OT_brush_select. */
  std::string hotkey = find_hotkey_for_operator(C, "PAINT_OT_brush_select", weak_ref, false);
  if (!hotkey.empty()) {
    return hotkey;
  }

  /* Legacy tool activation operator: brush assets stored as tool IDs in older configs. */
  hotkey = find_hotkey_for_operator(C, "WM_OT_tool_set_by_id", weak_ref, false);
  if (!hotkey.empty()) {
    return hotkey;
  }

  /* Some keymap entries were registered against the bare brush name without ".blend" extension.
   * Strip the extension from the full relative path and retry BRUSH_OT_asset_activate. */
  const std::string asset_name = normalize_asset_path(weak_ref.relative_asset_identifier);
  const size_t dot_pos = asset_name.find_last_of('.');
  if (dot_pos != std::string::npos) {
    AssetWeakReference name_ref = weak_ref;
    MEM_delete(name_ref.relative_asset_identifier);
    name_ref.relative_asset_identifier = BLI_strdup(asset_name.substr(0, dot_pos).c_str());

    for (int i = 0; brush_keymap_names[i]; i++) {
      hotkey = find_hotkey_in_context(
          C, "BRUSH_OT_asset_activate", name_ref, brush_keymap_names[i]);
      if (!hotkey.empty()) {
        return hotkey;
      }
    }
  }

  return "";
}

static std::string get_brush_asset_hotkey(const bContext *C,
                                          const asset_system::AssetRepresentation &asset)
{
  if (asset.get_id_type() != ID_BR) {
    return "";
  }

  const AssetWeakReference weak_ref = asset.make_weak_reference();

  /* Try context-based lookup first (fast path, works when sculpt context is active). */
  std::string hotkey = find_hotkey_for_operator(C, "BRUSH_OT_asset_activate", weak_ref, true);
  if (!hotkey.empty()) {
    return hotkey;
  }

  hotkey = find_hotkey_for_operator(C, "BRUSH_OT_asset_activate", weak_ref, false);
  if (!hotkey.empty()) {
    return hotkey;
  }

  /* Context-based lookup failed (tooltip context != sculpt context).
   * Search specific keymaps by name. */
  for (int i = 0; brush_keymap_names[i]; i++) {
    hotkey = find_hotkey_in_context(C, "BRUSH_OT_asset_activate", weak_ref, brush_keymap_names[i]);
    if (!hotkey.empty()) {
      return hotkey;
    }
  }

  /* User Library brushes: the keymap entry recorded via UI often omits asset_library_identifier
   * (the property stays at its default empty value), causing the exact search above to fail.
   * Retry with asset_library_identifier cleared so only type + path are compared. */
  if (weak_ref.asset_library_type != ASSET_LIBRARY_ESSENTIALS &&
      weak_ref.asset_library_identifier && weak_ref.asset_library_identifier[0] != '\0')
  {
    AssetWeakReference no_id_ref = weak_ref;
    MEM_delete(no_id_ref.asset_library_identifier);
    no_id_ref.asset_library_identifier = nullptr;

    for (int i = 0; brush_keymap_names[i]; i++) {
      hotkey = find_hotkey_in_context(
          C, "BRUSH_OT_asset_activate", no_id_ref, brush_keymap_names[i]);
      if (!hotkey.empty()) {
        return hotkey;
      }
    }
  }

  /* Name-based fallback: match by the brush name (last path component) across all keymaps.
   * Prefers entries whose stored asset_library_type matches, so two brushes with the same name
   * in different libraries each show their own hotkey. */
  if (weak_ref.asset_library_type != ASSET_LIBRARY_ESSENTIALS &&
      weak_ref.relative_asset_identifier)
  {
    const std::string asset_path = normalize_asset_path(weak_ref.relative_asset_identifier);
    const size_t last_sep = asset_path.find_last_of('/');
    if (last_sep != std::string::npos && last_sep + 1 < asset_path.size()) {
      hotkey = find_hotkey_by_brush_name_in_keymaps(
          C, asset_path.substr(last_sep + 1), weak_ref.asset_library_type);
      if (!hotkey.empty()) {
        return hotkey;
      }
    }
  }

  return find_alternative_hotkeys(C, weak_ref);
}

/** \} */

void asset_tooltip(const bContext *C,
                   const asset_system::AssetRepresentation &asset,
                   ui::TooltipData &tip,
                   const bool include_name)
{
  if (include_name) {
    tooltip_text_field_add(tip, asset.get_name(), {}, ui::TIP_STYLE_HEADER, ui::TIP_LC_MAIN);
    tooltip_text_field_add(tip, {}, {}, ui::TIP_STYLE_SPACER, ui::TIP_LC_NORMAL, false);

    if (C && asset.get_id_type() == ID_BR) {
      const std::string hotkey = get_brush_asset_hotkey(C, asset);
      if (!hotkey.empty()) {
        tooltip_text_field_add(tip,
                               fmt::format(fmt::runtime(TIP_("Shortcut: {}")), hotkey),
                               {},
                               ui::TIP_STYLE_NORMAL,
                               ui::TIP_LC_VALUE);
      }
      else {
        tooltip_text_field_add(
            tip, TIP_("No Shortcut Assigned"), {}, ui::TIP_STYLE_NORMAL, ui::TIP_LC_VALUE, false);
      }
      tooltip_text_field_add(tip, {}, {}, ui::TIP_STYLE_SPACER, ui::TIP_LC_NORMAL, false);
    }
  }

  const AssetMetaData &meta_data = asset.get_metadata();
  if (meta_data.description) {
    tooltip_text_field_add(tip, meta_data.description, {}, ui::TIP_STYLE_HEADER, ui::TIP_LC_MAIN);
  }

  if (asset.remote_file_status() == asset_system::RemoteAssetFileStatus::NO_MATCH) {
    tooltip_text_field_add(
        tip,
        TIP_("This asset was previously downloaded, but it is outdated or inconsistent.\n"
             "Downloading it again is recommended."),
        {},
        ui::TIP_STYLE_NORMAL,
        ui::TIP_LC_ALERT);
  }

  switch (asset.owner_asset_library().library_type()) {
    case ASSET_LIBRARY_CUSTOM: {
      if (asset.is_online_only()) {
        /* Don't show file path or .blend name. Data on disk is just a cache. */
        break;
      }

      tooltip_text_field_add(tip, {}, {}, ui::TIP_STYLE_SPACER, ui::TIP_LC_NORMAL, false);

      const std::string full_blend_path = asset.full_library_path();

      char dir[FILE_MAX], file[FILE_MAX];
      BLI_path_split_dir_file(full_blend_path.c_str(), dir, sizeof(dir), file, sizeof(file));

      if (file[0]) {
        tooltip_text_field_add(tip, file, {}, ui::TIP_STYLE_NORMAL, ui::TIP_LC_MAIN);
      }
      if (dir[0]) {
        tooltip_text_field_add(tip, dir, {}, ui::TIP_STYLE_NORMAL, ui::TIP_LC_MAIN);
      }
      break;
    }
    case ASSET_LIBRARY_LOCAL:
      tooltip_text_field_add(tip, {}, {}, ui::TIP_STYLE_SPACER, ui::TIP_LC_NORMAL, false);
      tooltip_text_field_add(
          tip, TIP_("Asset Library: Current File"), {}, ui::TIP_STYLE_NORMAL, ui::TIP_LC_VALUE);
      break;
    case ASSET_LIBRARY_ESSENTIALS:
    case ASSET_LIBRARY_ONLINE_ESSENTIALS:
      tooltip_text_field_add(tip, {}, {}, ui::TIP_STYLE_SPACER, ui::TIP_LC_NORMAL, false);
      tooltip_text_field_add(
          tip, TIP_("Asset Library: Essentials"), {}, ui::TIP_STYLE_NORMAL, ui::TIP_LC_VALUE);
      break;
    default:
      /* Intentionally empty. */
      break;
  }

  if (asset.is_online_only()) {
    if (std::optional<int64_t> combined_size = asset.online_asset_files_combined_size_in_bytes()) {
      tooltip_text_field_add(tip, {}, {}, ui::TIP_STYLE_SPACER, ui::TIP_LC_NORMAL, false);

      char size_ui_str[BLI_STR_FORMAT_INT64_BYTE_UNIT_SIZE];
      BLI_str_format_byte_unit(size_ui_str, *combined_size, true);
      tooltip_text_field_add(tip,
                             fmt::format(fmt::runtime(TIP_("Download Size: {}")), size_ui_str),
                             {},
                             ui::TIP_STYLE_NORMAL,
                             ui::TIP_LC_VALUE);
    }
  }
}

BIFIconID asset_preview_icon_id(const asset_system::AssetRepresentation &asset)
{
  if (const PreviewImage *preview = asset.get_preview()) {
    if (!BKE_previewimg_is_invalid(preview, ICON_SIZE_ICON)) {
      return preview->runtime->icon_id;
    }
  }

  return ICON_NONE;
}

BIFIconID asset_preview_or_icon(const asset_system::AssetRepresentation &asset)
{
  const BIFIconID preview_icon = asset_preview_icon_id(asset);
  if (preview_icon != ICON_NONE) {
    return preview_icon;
  }

  /* Preview image not found or invalid. Use type icon. */
  return ui::icon_from_idcode(asset.get_id_type());
}

const bUserAssetLibrary *get_asset_library_from_opptr(PointerRNA &ptr)
{
  const int enum_value = RNA_enum_get(&ptr, "asset_library_reference");
  const AssetLibraryReference lib_ref = asset::library_reference_from_enum_value(enum_value);
  return BKE_preferences_asset_library_find_index(&U, lib_ref.custom_library_index);
}

AssetLibraryReference get_asset_library_ref_from_opptr(PointerRNA &ptr)
{
  const int enum_value = RNA_enum_get(&ptr, "asset_library_reference");
  return asset::library_reference_from_enum_value(enum_value);
}

std::optional<AssetLibraryReference> get_user_library_ref_for_save(
    const asset_system::AssetLibrary *preferred_library)
{
  if (preferred_library && !preferred_library->is_read_only()) {
    if (std::optional<AssetLibraryReference> preferred_library_ref =
            preferred_library->library_reference())
    {
      return preferred_library_ref;
    }
    BLI_assert_unreachable();
  }

  /* Fallback to the first enabled on-disk user library. */
  for (const bUserAssetLibrary &asset_library : U.asset_libraries) {
    if (asset_library.flag & (ASSET_LIBRARY_DISABLED | ASSET_LIBRARY_USE_REMOTE_URL)) {
      continue;
    }
    return asset::user_library_to_library_ref(asset_library);
  }

  /* No enabled user asset library found. */
  return {};
}

void visit_library_catalogs_catalog_for_search(
    const Main &bmain,
    const AssetLibraryReference lib,
    const StringRef edit_text,
    const FunctionRef<void(StringPropertySearchVisitParams)> visit_fn)
{
  const asset_system::AssetLibrary *library = AS_asset_library_load(&bmain, lib);
  if (!library) {
    return;
  }

  if (!edit_text.is_empty()) {
    const asset_system::AssetCatalogPath edit_path = edit_text;
    if (!library->catalog_service().find_catalog_by_path(edit_path)) {
      visit_fn(StringPropertySearchVisitParams{edit_path.str(), std::nullopt, ICON_ADD});
    }
  }

  const std::shared_ptr<const asset_system::AssetCatalogTree> full_tree =
      library->catalog_service().catalog_tree();
  full_tree->foreach_item([&](const asset_system::AssetCatalogTreeItem &item) {
    visit_fn(StringPropertySearchVisitParams{item.catalog_path().str(), std::nullopt});
  });
}

}  // namespace blender::ed::asset
