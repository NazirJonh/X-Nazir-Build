/* SPDX-FileCopyrightText: 2026 Blender Authors. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Shared Category Tab utility helpers.
 */

#include <cctype>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "BLI_fileops.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_utildefines.h"

#include "BKE_icons.hh"
#include "BKE_preview_image.hh"

#include "DNA_userdef_types.h"

#include "RNA_access.hh"
#include "RNA_enum_types.hh"

#include "ED_screen.hh"

#include "UI_interface_c.hh"
#include "UI_interface_icons.hh"

#include "IMB_thumbs.hh"

#include "interface_intern.hh"

namespace blender::ui {

bool hex_codepoint_to_utf8(const char *input, char *utf8_out, size_t utf8_max)
{
  if (!input || !input[0]) {
    return false;
  }

  const char *hex_start = input;

  /* Skip optional "0x" or "0X" prefix. */
  if (input[0] == '0' && (input[1] == 'x' || input[1] == 'X')) {
    hex_start = input + 2;
    if (!hex_start[0]) {
      return false;
    }
  }

  /* Check if remaining string is a valid hex number (1-6 hex digits for Unicode). */
  const size_t hex_len = strlen(hex_start);
  if (hex_len == 0 || hex_len > 6) {
    return false;
  }

  /* Verify all characters are hex digits. */
  for (size_t i = 0; i < hex_len; i++) {
    if (!isxdigit(static_cast<unsigned char>(hex_start[i]))) {
      return false;
    }
  }

  /* Parse hex to unsigned int. */
  const unsigned int val = strtoul(hex_start, nullptr, 16);

  /* Validate Unicode codepoint range. */
  if (val < 32 || val > 0x10FFFF) {
    return false;
  }

  /* Convert to UTF-8 using Blender's built-in function. */
  const int utf8_len = BLI_str_utf8_from_unicode(val, utf8_out, utf8_max);

  /* BLI_str_utf8_from_unicode does NOT null-terminate, so we must do it. */
  if (utf8_len > 0 && size_t(utf8_len) < utf8_max) {
    utf8_out[utf8_len] = '\0';
  }

  return utf8_len > 0;
}

bool process_glyph_input(const char *input, char *output, size_t output_max)
{
  if (!input || !input[0]) {
    output[0] = '\0';
    return false;
  }

  if (hex_codepoint_to_utf8(input, output, output_max)) {
    return true;
  }

  output[0] = '\0';
  return false;
}

void utf8_to_hex_codepoint(const char *input, char *output, size_t output_max)
{
  if (!input || !input[0]) {
    output[0] = '\0';
    printf("[UTF8_TO_HEX] input is null or empty\n");
    return;
  }

  const unsigned int codepoint = BLI_str_utf8_as_unicode_safe(input);
  if (codepoint == BLI_UTF8_ERR || codepoint == 0) {
    output[0] = '\0';
    printf("[UTF8_TO_HEX] invalid codepoint: %u\n", codepoint);
    return;
  }

  BLI_snprintf(output, output_max, "%x", codepoint);
  printf("[UTF8_TO_HEX] input='%s', codepoint=0x%x, output='%s'\n", input, codepoint, output);
}

bool is_display_glyph_codepoint(const unsigned int codepoint)
{
  /* Private Use Areas (font icons like Material Symbols). */
  if ((codepoint >= 0xE000 && codepoint <= 0xF8FF) ||
      (codepoint >= 0xF0000 && codepoint <= 0xFFFFD) ||
      (codepoint >= 0x100000 && codepoint <= 0x10FFFD))
  {
    return true;
  }

  /* Common symbol ranges. */
  if ((codepoint >= 0x2600 && codepoint <= 0x27BF) ||
      (codepoint >= 0x1F300 && codepoint <= 0x1FAFF))
  {
    return true;
  }

  return false;
}

bool is_single_glyph_str(const char *str)
{
  if (!str || !str[0]) {
    return false;
  }
  const int utf8_char_size = BLI_str_utf8_size_safe(str);
  const size_t len = BLI_strnlen(str, 64);
  return (len == 1) || (utf8_char_size > 0 && size_t(utf8_char_size) == len);
}

bool category_tab_first_utf8_char_copy(const char *input, char *output, const size_t output_max)
{
  if (!output || output_max == 0) {
    return false;
  }

  output[0] = '\0';
  if (!input || input[0] == '\0') {
    return false;
  }

  const int first_char_size = BLI_str_utf8_size_safe(input);
  if (first_char_size <= 0 || first_char_size >= int(output_max)) {
    return false;
  }

  memcpy(output, input, first_char_size);
  output[first_char_size] = '\0';
  return true;
}

bool category_tab_glyph_is_fallback_letter(const char *glyph, const char *category)
{
  if (!glyph || glyph[0] == '\0' || !category || category[0] == '\0') {
    return false;
  }

  const int glyph_char_size = BLI_str_utf8_size_safe(glyph);
  if (glyph_char_size <= 0 || glyph[glyph_char_size] != '\0') {
    return false;
  }

  char category_first_char[8] = "";
  if (!category_tab_first_utf8_char_copy(category, category_first_char, sizeof(category_first_char))) {
    return false;
  }

  return STREQ(glyph, category_first_char);
}

float category_tabs_zoom_value_get(const ScrArea *area,
                                   const eUserPref_CategoryTabsDisplayMode display_mode)
{
  if (area) {
    return ED_category_tabs_zoom_get(area);
  }

  switch (display_mode) {
    case USER_CATEGORY_TABS_GLYPHS_ONLY:
      return U.category_tabs_zoom_icon;
    case USER_CATEGORY_TABS_GLYPHS_TEXT:
      return U.category_tabs_zoom_mixed;
    case USER_CATEGORY_TABS_TEXT_ONLY:
    default:
      return U.category_tabs_zoom_text;
  }
}

int category_tabs_min_width_get(const ScrArea *area,
                                const float aspect,
                                const eUserPref_CategoryTabsDisplayMode display_mode)
{
  const float safe_aspect = std::max(aspect, 0.0001f);
  const float category_tabs_zoom = category_tabs_zoom_value_get(area, display_mode);
  const float zoom = (1.0f / safe_aspect) * category_tabs_zoom;
  const int category_tabs_width = int(std::lround(double(UI_PANEL_CATEGORY_MARGIN_WIDTH * zoom)));
  const int legacy_min_width =
      int(std::ceil(double(UI_PANEL_CATEGORY_MIN_WIDTH * UI_SCALE_FAC / safe_aspect)));

  return std::max(category_tabs_width, legacy_min_width);
}

int category_tab_icon_id_resolve_from_path(const char *icon_path)
{
  if (!(icon_path && icon_path[0] != '\0')) {
    return ICON_NONE;
  }

  if (!BLI_exists(icon_path)) {
    return ICON_NONE;
  }

  PreviewImage *preview = BKE_previewimg_cached_thumbnail_read(
      icon_path, icon_path, THB_SOURCE_DIRECT, false);
  if (!preview) {
    return ICON_NONE;
  }

  BKE_previewimg_ensure(preview, ICON_SIZE_ICON);
  if (BKE_previewimg_is_invalid(preview, ICON_SIZE_ICON)) {
    return ICON_NONE;
  }

  const int icon_id = BKE_icon_preview_ensure(nullptr, preview);
  return (icon_id > 0) ? icon_id : ICON_NONE;
}

int category_tab_icon_id_resolve_from_key_path(const char *icon_key, const char *icon_path)
{
  if (icon_key && icon_key[0] != '\0') {
    int icon_id = ICON_NONE;
    if (RNA_enum_value_from_identifier(rna_enum_icon_items, icon_key, &icon_id)) {
      return icon_id;
    }
  }

  return category_tab_icon_id_resolve_from_path(icon_path);
}

void category_tab_split_tags(const char *tags,
                             Vector<std::string> &r_tags,
                             const char *delimiters)
{
  if (!tags || tags[0] == '\0') {
    return;
  }

  if (!delimiters || delimiters[0] == '\0') {
    delimiters = ",;";
  }

  const char *token_start = tags;
  const auto is_delimiter = [delimiters](const char c) {
    return strchr(delimiters, c) != nullptr;
  };

  for (const char *p = tags;; p++) {
    const bool at_end = (*p == '\0');
    if (!at_end && !is_delimiter(*p)) {
      continue;
    }

    const char *start = token_start;
    const char *end = p;

    while (start < end && std::isspace(static_cast<unsigned char>(*start))) {
      start++;
    }
    while (end > start && std::isspace(static_cast<unsigned char>(*(end - 1)))) {
      end--;
    }

    if (start < end) {
      r_tags.append(std::string(start, end - start));
    }

    if (at_end) {
      break;
    }

    token_start = p + 1;
  }
}

std::string category_tab_escape_for_python_literal(const char *input)
{
  std::string escaped = input ? input : "";

  size_t pos = 0;
  while ((pos = escaped.find("\\", pos)) != std::string::npos) {
    escaped.replace(pos, 1, "\\\\");
    pos += 2;
  }

  pos = 0;
  while ((pos = escaped.find("'", pos)) != std::string::npos) {
    escaped.replace(pos, 1, "\\'");
    pos += 2;
  }

  return escaped;
}

bool category_tab_parse_json_string_array_minimal(const char *json, Vector<std::string> &r_items)
{
  if (!json) {
    return false;
  }

  const char *p = json;
  while (*p && *p != '[') {
    p++;
  }
  if (*p != '[') {
    return false;
  }
  p++;

  bool parsed_any = false;
  while (*p && *p != ']') {
    while (*p && ELEM(*p, ' ', '\n', '\r', '\t', ',')) {
      p++;
    }
    if (*p == ']') {
      break;
    }

    if (*p != '"') {
      p++;
      continue;
    }
    p++;

    const char *start = p;
    while (*p && *p != '"') {
      if (*p == '\\' && *(p + 1)) {
        p += 2;
      }
      else {
        p++;
      }
    }

    if (*p != '"') {
      return parsed_any;
    }

    const std::string value(start, p - start);
    if (!value.empty()) {
      r_items.append(value);
      parsed_any = true;
    }
    p++;
  }

  return parsed_any;
}

std::string category_tab_decode_json_unicode(const char *str)
{
  if (!str) {
    return "";
  }

  std::string result;
  const size_t len = strlen(str);
  size_t i = 0;

  while (i < len) {
    if (i + 5 < len && str[i] == '\\' && str[i + 1] == 'u') {
      char hex_str[5] = {str[i + 2], str[i + 3], str[i + 4], str[i + 5], 0};

      uint32_t codepoint = 0;
      for (int j = 0; j < 4; j++) {
        const char c = hex_str[j];
        codepoint <<= 4;
        if (c >= '0' && c <= '9') {
          codepoint |= (c - '0');
        }
        else if (c >= 'a' && c <= 'f') {
          codepoint |= (c - 'a' + 10);
        }
        else if (c >= 'A' && c <= 'F') {
          codepoint |= (c - 'A' + 10);
        }
      }

      char utf8_buf[5];
      int utf8_len = 0;

      if (codepoint <= 0x7F) {
        utf8_buf[0] = (char)codepoint;
        utf8_len = 1;
      }
      else if (codepoint <= 0x7FF) {
        utf8_buf[0] = (char)(0xC0 | (codepoint >> 6));
        utf8_buf[1] = (char)(0x80 | (codepoint & 0x3F));
        utf8_len = 2;
      }
      else if (codepoint <= 0xFFFF) {
        utf8_buf[0] = (char)(0xE0 | (codepoint >> 12));
        utf8_buf[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        utf8_buf[2] = (char)(0x80 | (codepoint & 0x3F));
        utf8_len = 3;
      }
      else {
        utf8_buf[0] = (char)(0xF0 | (codepoint >> 18));
        utf8_buf[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
        utf8_buf[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        utf8_buf[3] = (char)(0x80 | (codepoint & 0x3F));
        utf8_len = 4;
      }

      result.append(utf8_buf, utf8_len);
      i += 6;
    }
    else {
      result += str[i];
      i++;
    }
  }

  return result;
}

}  // namespace blender::ui
