/* SPDX-FileCopyrightText: 2026 Nazir Galimov
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
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <sstream>
#include <string>

#include "BLI_fileops.h"
#include "BLI_serialize.hh"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_utildefines.h"

#include "CLG_log.h"

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

static CLG_LogRef LOG = {"ui.category_tabs"};

static constexpr bool ICON_RESOLVE_DEBUG_ENABLED = false;

/**
 * Diagnose a Python-bridge JSON response that should be an array but is not. Array-shaped
 * responses (string or object arrays, including the empty array) are the expected input and
 * are not reported; only a non-array root (an object such as `{"error": ...}`, `null`, or a
 * scalar) is logged, so a Python-side failure is visible instead of degrading silently to an
 * empty result. Extracts the message of an explicit `{"error": ...}` object when present.
 */
static void category_tab_json_report_non_array(const char *context,
                                               const io::serialize::Value &root)
{
  if (const io::serialize::DictionaryValue *object = root.as_dictionary_value()) {
    if (const std::optional<StringRefNull> error = object->lookup_str("error")) {
      CLOG_WARN(&LOG, "%s: Python bridge returned an error: %s", context, error->c_str());
      return;
    }
  }
  CLOG_WARN(&LOG, "%s: Python bridge returned a non-array JSON response", context);
}

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

  /* Validate Unicode codepoint range. Reject UTF-16 surrogates (0xD800..0xDFFF), which are not
   * valid Unicode scalar values. */
  if (val < 32 || val > 0x10FFFF || (val >= 0xD800 && val <= 0xDFFF)) {
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
    if (g_tag_filter_debug_enabled) {
      printf("[UTF8_TO_HEX] input is null or empty\n");
    }
    return;
  }

  const unsigned int codepoint = BLI_str_utf8_as_unicode_safe(input);
  if (codepoint == BLI_UTF8_ERR || codepoint == 0) {
    output[0] = '\0';
    if (g_tag_filter_debug_enabled) {
      printf("[UTF8_TO_HEX] invalid codepoint: %u\n", codepoint);
    }
    return;
  }

  BLI_snprintf(output, output_max, "%x", codepoint);
  if (g_tag_filter_debug_enabled) {
    printf("[UTF8_TO_HEX] input='%s', codepoint=0x%x, output='%s'\n", input, codepoint, output);
  }
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
  const float visual_effect_margin = (U.category_tabs_visual_effect &&
                                      display_mode == USER_CATEGORY_TABS_GLYPHS_ONLY) ?
                                         UI_TABS_VISUAL_EFFECT_MARGIN :
                                         1.0f;
  const int category_tabs_width = int(
      std::lround(double(UI_PANEL_CATEGORY_MARGIN_WIDTH * zoom * visual_effect_margin)));
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
      icon_path, icon_path, blender::THB_SOURCE_DIRECT, false);
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
    if constexpr (ICON_RESOLVE_DEBUG_ENABLED) {
      printf("[ICON_RESOLVE] Looking for icon_key='%s'\n", icon_key);
    }

    /* DEBUG: Try to find the icon by searching through enum */
    bool found = false;
    for (const EnumPropertyItem *item = rna_enum_icon_items; item->identifier; item++) {
      if (strcmp(item->identifier, icon_key) == 0) {
        icon_id = item->value;
        if constexpr (ICON_RESOLVE_DEBUG_ENABLED) {
          printf("[ICON_RESOLVE] icon_key='%s' -> icon_id=%d (found by search)\n", icon_key, icon_id);
        }
        found = true;
        break;
      }
    }

    if (!found) {
      /* Handle special Blender icons that are not in the standard RNA enum. */
      if (STREQ(icon_key, "FUND")) {
        return ICON_FUND;
      }
      if (STREQ(icon_key, "BLENDER")) {
        return ICON_BLENDER;
      }

      if constexpr (ICON_RESOLVE_DEBUG_ENABLED) {
        printf("[ICON_RESOLVE] icon_key='%s' NOT FOUND in rna_enum_icon_items\n", icon_key);
      }
      /* Also try RNA_enum_value_from_identifier for comparison */
      if (RNA_enum_value_from_identifier(rna_enum_icon_items, icon_key, &icon_id)) {
        if constexpr (ICON_RESOLVE_DEBUG_ENABLED) {
          printf("[ICON_RESOLVE] RNA_enum_value_from_identifier succeeded! icon_id=%d\n", icon_id);
        }
        return icon_id;
      }
      if constexpr (ICON_RESOLVE_DEBUG_ENABLED) {
        printf("[ICON_RESOLVE] RNA_enum_value_from_identifier also failed\n");
      }
    } else {
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
  std::string escaped;
  if (!input) {
    return escaped;
  }

  for (const char *p = input; *p != '\0'; p++) {
    const char c = *p;
    switch (c) {
      case '\\':
        escaped += "\\\\";
        break;
      case '\'':
        escaped += "\\'";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        escaped += c;
        break;
    }
  }

  return escaped;
}

bool category_tab_parse_json_string_array_minimal(const char *json, Vector<std::string> &r_items)
{
  if (!json) {
    return false;
  }

  /* Parse via the shared serialization layer rather than scanning by hand: it handles escapes,
   * whitespace and malformed input robustly (returns null), and fully decodes string values. */
  std::istringstream is(json);
  io::serialize::JsonFormatter formatter;
  std::unique_ptr<io::serialize::Value> root = formatter.deserialize(is);
  if (!root) {
    return false;
  }
  const io::serialize::ArrayValue *array = root->as_array_value();
  if (!array) {
    /* Valid JSON but not the expected array: surface a Python-side failure instead of
     * silently returning nothing. */
    category_tab_json_report_non_array("category_tab_parse_json_string_array_minimal", *root);
    return false;
  }

  bool parsed_any = false;
  for (const std::shared_ptr<io::serialize::Value> &element : array->elements()) {
    const io::serialize::StringValue *value = element->as_string_value();
    if (!value || value->value().empty()) {
      continue;
    }
    r_items.append(value->value());
    parsed_any = true;
  }

  return parsed_any;
}

}  // namespace blender::ui
