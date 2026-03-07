/* SPDX-FileCopyrightText: 2026 Blender Authors. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Shared Category Tab utility helpers.
 */

#include <cctype>
#include <cstdlib>
#include <cstring>

#include "BLI_fileops.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_utildefines.h"

#include "BKE_icons.hh"
#include "BKE_preview_image.hh"

#include "RNA_access.hh"
#include "RNA_enum_types.hh"

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
    return;
  }

  const unsigned int codepoint = BLI_str_utf8_as_unicode_safe(input);
  if (codepoint == BLI_UTF8_ERR || codepoint == 0) {
    output[0] = '\0';
    return;
  }

  BLI_snprintf(output, output_max, "%x", codepoint);
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

}  // namespace blender::ui
