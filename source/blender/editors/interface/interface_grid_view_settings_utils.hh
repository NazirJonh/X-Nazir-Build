/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edinterface
 *
 * Helpers to read/write #GridViewSettings RNA properties from a #PointerRNA.
 */

#include "BLI_set.hh"
#include "BLI_string_ref.hh"
#include "BLI_utildefines.h"

#include <cstring>
#include <string>

#include "DNA_asset_types.h"

#include "RNA_access.hh"

#include "ED_asset_library.hh"

namespace blender::ui::grid_settings {

inline AssetLibraryReference library_ref_get(PointerRNA &settings)
{
  const int enum_value = RNA_enum_get(&settings, "asset_library_reference");
  return ed::asset::library_reference_from_enum_value(enum_value);
}

inline void library_ref_set(PointerRNA &settings, const AssetLibraryReference &lib_ref)
{
  const int enum_value = ed::asset::library_reference_to_enum_value(&lib_ref);
  RNA_enum_set(&settings, "asset_library_reference", enum_value);
}

inline int preview_size_get(PointerRNA &settings)
{
  return RNA_int_get(&settings, "preview_size");
}

/** Parse comma-separated catalog paths. An empty result means "show all". */
inline Set<std::string> enabled_catalogs_get(PointerRNA &settings)
{
  Set<std::string> result;
  const std::string catalogs_str = RNA_string_get(&settings, "enabled_catalogs");
  if (catalogs_str.empty()) {
    return result;
  }

  const char *start = catalogs_str.c_str();
  while (*start) {
    const char *comma = strchr(start, ',');
    const int len = comma ? int(comma - start) : int(strlen(start));
    if (len > 0) {
      std::string path(start, size_t(len));
      while (!path.empty() && ELEM(path.front(), ' ', '\t')) {
        path.erase(path.begin());
      }
      while (!path.empty() && ELEM(path.back(), ' ', '\t')) {
        path.pop_back();
      }
      if (!path.empty()) {
        result.add_new(path);
      }
    }
    if (!comma) {
      break;
    }
    start = comma + 1;
  }

  return result;
}

inline bool is_catalog_path_enabled(PointerRNA &settings, StringRef catalog_path)
{
  const Set<std::string> enabled = enabled_catalogs_get(settings);
  if (enabled.is_empty()) {
    return false;
  }
  return enabled.contains_as(catalog_path);
}

inline void enabled_catalogs_set(PointerRNA &settings, const Set<std::string> &paths)
{
  if (paths.is_empty()) {
    RNA_string_set(&settings, "enabled_catalogs", "");
    return;
  }

  std::string joined;
  bool first = true;
  for (const std::string &path : paths) {
    if (!first) {
      joined += ',';
    }
    joined += path;
    first = false;
  }
  RNA_string_set(&settings, "enabled_catalogs", joined.c_str());
}

inline void catalog_path_set_enabled(PointerRNA &settings,
                                     StringRef catalog_path,
                                     const bool enabled)
{
  Set<std::string> paths = enabled_catalogs_get(settings);
  if (enabled) {
    paths.add_new(catalog_path);
  }
  else {
    paths.remove(catalog_path);
  }
  enabled_catalogs_set(settings, paths);
}

inline void enabled_catalogs_clear(PointerRNA &settings)
{
  RNA_string_set(&settings, "enabled_catalogs", "");
}

}  // namespace blender::ui::grid_settings
