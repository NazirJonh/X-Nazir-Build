/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 */

#include "BLI_name_matching.hh"

#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_utildefines.h"

#include <cctype>
#include <cstdint>
#include <cstring>

namespace {

bool is_name_match_delimiter(const char c)
{
  return ELEM(c, '_', '-', '.', ' ');
}

bool chars_equal_case_insensitive(const char a, const char b)
{
  return std::tolower(static_cast<unsigned char>(a)) ==
         std::tolower(static_cast<unsigned char>(b));
}

bool region_equals_case_insensitive(const blender::StringRef haystack,
                                    const int64_t start,
                                    const blender::StringRef needle)
{
  if (start < 0 || start + needle.size() > haystack.size()) {
    return false;
  }
  for (int64_t i = 0; i < needle.size(); i++) {
    if (!chars_equal_case_insensitive(haystack[start + i], needle[i])) {
      return false;
    }
  }
  return true;
}

/**
 * Known image extensions stripped after the duplicate-suffix pass.
 * Compared case-insensitively including the leading dot.
 */
bool strip_known_image_extension(std::string &name)
{
  static const blender::StringRef extensions[] = {
      ".png",
      ".jpg",
      ".jpeg",
      ".exr",
      ".tif",
      ".tiff",
      ".webp",
      ".bmp",
      ".tga",
      ".hdr",
      ".psd",
  };

  for (const blender::StringRef ext : extensions) {
    if (name.size() > size_t(ext.size()) &&
        region_equals_case_insensitive(name, int64_t(name.size()) - ext.size(), ext))
    {
      name.resize(name.size() - size_t(ext.size()));
      return true;
    }
  }
  return false;
}

bool strip_trailing_duplicate_suffix(std::string &name)
{
  /* Blender's duplicate suffix is always exactly 3 digits, e.g. `.001`. Matching any digit count
   * here would also strip legitimate trailing numbers from names like `brick_2.5` or `wood.2048`. */
  constexpr size_t suffix_digits = 3;
  const size_t dot = name.find_last_of('.');
  if (dot == std::string::npos || name.size() - dot - 1 != suffix_digits) {
    return false;
  }
  for (size_t i = dot + 1; i < name.size(); i++) {
    if (!std::isdigit(static_cast<unsigned char>(name[i]))) {
      return false;
    }
  }
  name.resize(dot);
  return true;
}

}  // namespace

namespace blender {

std::string BLI_name_matching_normalize_asset_name(const StringRef name)
{
  std::string result = name;
  strip_trailing_duplicate_suffix(result);
  strip_known_image_extension(result);
  return result;
}

bool BLI_name_matching_token_matches(const StringRef normalized_name, const StringRef token)
{
  if (token.is_empty()) {
    return false;
  }
  if (token.size() > normalized_name.size()) {
    return false;
  }

  const int64_t last_start = normalized_name.size() - token.size();
  for (int64_t start = 0; start <= last_start; start++) {
    if (!region_equals_case_insensitive(normalized_name, start, token)) {
      continue;
    }
    const bool left_ok = (start == 0) || is_name_match_delimiter(normalized_name[start - 1]);
    const int64_t end = start + token.size();
    const bool right_ok = (end == normalized_name.size()) ||
                          is_name_match_delimiter(normalized_name[end]);
    if (left_ok && right_ok) {
      return true;
    }
  }
  return false;
}

bool BLI_name_matching_map_type_matches_name(const StringRef normalized_name,
                                             const Span<StringRef> tokens)
{
  for (const StringRef token : tokens) {
    if (BLI_name_matching_token_matches(normalized_name, token)) {
      return true;
    }
  }
  return false;
}

bool BLI_name_matching_filter_tag_matches(const StringRef normalized_name,
                                          const Span<StringRef> metadata_tag_names,
                                          const StringRef filter_tag)
{
  if (filter_tag.is_empty()) {
    return false;
  }

  for (const StringRef meta_tag : metadata_tag_names) {
    if (meta_tag.size() == filter_tag.size() &&
        BLI_strncasecmp(meta_tag.data(), filter_tag.data(), size_t(filter_tag.size())) == 0)
    {
      return true;
    }
  }

  return BLI_name_matching_token_matches(normalized_name, filter_tag);
}

bool BLI_name_matching_asset_passes_include_filter(
    const StringRef asset_name,
    const Span<StringRef> metadata_tag_names,
    const Span<Span<StringRef>> active_map_type_token_lists,
    const Span<StringRef> active_filter_tags)
{
  if (active_map_type_token_lists.is_empty() && active_filter_tags.is_empty()) {
    return true;
  }

  const std::string normalized = BLI_name_matching_normalize_asset_name(asset_name);

  for (const Span<StringRef> tokens : active_map_type_token_lists) {
    if (BLI_name_matching_map_type_matches_name(normalized, tokens)) {
      return true;
    }
  }

  for (const StringRef filter_tag : active_filter_tags) {
    if (BLI_name_matching_filter_tag_matches(normalized, metadata_tag_names, filter_tag)) {
      return true;
    }
  }

  return false;
}

}  // namespace blender
