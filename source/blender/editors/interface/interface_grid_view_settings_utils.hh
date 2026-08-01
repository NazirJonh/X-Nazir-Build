/* SPDX-FileCopyrightText: 2026 Nazir Galimov
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
#include "BLI_vector.hh"

#include <cstring>
#include <string>
#include <utility>

#include "DNA_asset_types.h"

#include "BKE_idtype.hh"

#include "RNA_access.hh"

#include "ED_asset_library.hh"

namespace blender::ui::grid_settings {

/** Split \a str on commas, trimming surrounding whitespace from each non-empty token. */
inline Vector<std::string> split_comma_separated(const std::string &str)
{
  Vector<std::string> tokens;
  const char *start = str.c_str();
  while (*start) {
    const char *comma = strchr(start, ',');
    const int len = comma ? int(comma - start) : int(strlen(start));
    if (len > 0) {
      std::string token(start, size_t(len));
      while (!token.empty() && ELEM(token.front(), ' ', '\t')) {
        token.erase(token.begin());
      }
      while (!token.empty() && ELEM(token.back(), ' ', '\t')) {
        token.pop_back();
      }
      if (!token.empty()) {
        tokens.append(std::move(token));
      }
    }
    if (!comma) {
      break;
    }
    start = comma + 1;
  }
  return tokens;
}

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

/**
 * Serialize a set of catalog paths into one string. A catalog path may contain any character,
 * including commas and surrounding spaces, so commas and backslashes are backslash-escaped
 * (`,`->`\,`, `\`->`\\`) to make the encoding lossless and reversible with #catalogs_split.
 */
inline std::string catalogs_join(const Set<std::string> &paths)
{
  std::string joined;
  bool first = true;
  for (const std::string &path : paths) {
    if (!first) {
      joined += ',';
    }
    for (const char c : path) {
      if (ELEM(c, '\\', ',')) {
        joined += '\\';
      }
      joined += c;
    }
    first = false;
  }
  return joined;
}

/**
 * Inverse of #catalogs_join: split on unescaped commas, unescaping `\,` and `\\`. Empty tokens
 * (from a leading, trailing, or doubled comma) are skipped.
 */
inline Vector<std::string> catalogs_split(const std::string &str)
{
  Vector<std::string> tokens;
  std::string token;
  bool in_token = false;
  bool escaped = false;
  for (const char c : str) {
    if (escaped) {
      token += c;
      escaped = false;
      continue;
    }
    if (c == '\\') {
      escaped = true;
      in_token = true;
      continue;
    }
    if (c == ',') {
      if (in_token) {
        tokens.append(std::move(token));
        token.clear();
        in_token = false;
      }
      continue;
    }
    token += c;
    in_token = true;
  }
  if (in_token) {
    tokens.append(std::move(token));
  }
  return tokens;
}

/** Parse the serialized catalog paths. An empty result means "show all". A malformed string with
 * repeated tokens is merged (Set::add), never #Set::add_new which is UB on a duplicate key. */
inline Set<std::string> enabled_catalogs_get(PointerRNA &settings)
{
  Set<std::string> result;
  for (std::string &path : catalogs_split(RNA_string_get(&settings, "enabled_catalogs"))) {
    result.add(std::move(path));
  }
  return result;
}

inline bool is_catalog_path_enabled(PointerRNA &settings, StringRef catalog_path)
{
  const Set<std::string> enabled = enabled_catalogs_get(settings);
  if (enabled.is_empty()) {
    /* Empty means "show all" (see #enabled_catalogs_get), so every catalog counts as enabled. */
    return true;
  }
  return enabled.contains_as(catalog_path);
}

inline void enabled_catalogs_set(PointerRNA &settings, const Set<std::string> &paths)
{
  /* #catalogs_join returns "" for an empty set, which is the "show all" encoding. */
  RNA_string_set(&settings, "enabled_catalogs", catalogs_join(paths).c_str());
}

inline void catalog_path_set_enabled(PointerRNA &settings,
                                     StringRef catalog_path,
                                     const bool enabled)
{
  Set<std::string> paths = enabled_catalogs_get(settings);
  if (enabled) {
    paths.add(catalog_path);
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

/**
 * Parse the comma-separated #GridViewSettings.filter_id_types list of ID-type names (e.g.
 * "Image,Material" — the same names #BKE_idtype_idcode_to_name() returns) into ID codes. Unknown
 * names are silently skipped. An empty result means "show all types".
 */
inline Set<short> filter_id_types_get(PointerRNA &settings)
{
  Set<short> result;
  for (const std::string &name :
       split_comma_separated(RNA_string_get(&settings, "filter_id_types")))
  {
    const short idcode = BKE_idtype_idcode_from_name(name.c_str());
    if (idcode != 0) {
      result.add(idcode);
    }
  }
  return result;
}

inline bool is_id_type_filter_enabled(PointerRNA &settings, const short idcode)
{
  const Set<short> filter = filter_id_types_get(settings);
  if (filter.is_empty()) {
    /* Empty means "show all types" (see #filter_id_types_get). */
    return true;
  }
  return filter.contains(idcode);
}

inline void filter_id_types_set(PointerRNA &settings, const Set<short> &idcodes)
{
  if (idcodes.is_empty()) {
    RNA_string_set(&settings, "filter_id_types", "");
    return;
  }

  std::string joined;
  bool first = true;
  for (const short idcode : idcodes) {
    if (!first) {
      joined += ',';
    }
    joined += BKE_idtype_idcode_to_name(idcode);
    first = false;
  }
  RNA_string_set(&settings, "filter_id_types", joined.c_str());
}

}  // namespace blender::ui::grid_settings
