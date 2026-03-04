/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include "DNA_screen_types.h"
#include "DNA_userdef_types.h"

#include "BKE_context.hh"
#include "BLI_math_base.h"
#include "BLI_math_vector.h"
#include "BLI_string.h"
#include "BLI_string_ref.hh"
#include "BLI_string_utf8.h"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "UI_interface.hh"
#include "interface_intern.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "BLF_api.hh"

#include "BLT_translation.hh"

namespace blender::ui {

/* -------------------------------------------------------------------- */
/** \name Glyph Utility Functions
 * \{ */

/* Convert hex codepoint to UTF-8 character */
static bool hex_codepoint_to_utf8(const char *hex, char *utf8_out, size_t utf8_max)
{
  if (!hex || !hex[0]) {
    return false;
  }

  /* Skip optional "0x" prefix */
  const char *hex_start = hex;
  if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
    hex_start = hex + 2;
  }

  /* Check if remaining string is valid hex */
  size_t hex_len = strlen(hex_start);
  if (hex_len == 0 || hex_len > 6) {
    return false;
  }

  for (size_t i = 0; i < hex_len; i++) {
    if (!isxdigit(static_cast<unsigned char>(hex_start[i]))) {
      return false;
    }
  }

  /* Convert hex string to integer */
  uint val = uint(strtoul(hex_start, nullptr, 16));

  /* Validate Unicode codepoint range */
  if (val < 32 || val > 0x10FFFF) {
    return false;
  }

  /* Convert to UTF-8 using Blender's built-in function */
  const int utf8_len = BLI_str_utf8_from_unicode(val, utf8_out, utf8_max);

  /* BLI_str_utf8_from_unicode does NOT null-terminate, so we must do it */
  if (utf8_len > 0 && size_t(utf8_len) < utf8_max) {
    utf8_out[utf8_len] = '\0';
  }

  return utf8_len > 0;
}

/* Process glyph input - convert hex code to UTF-8 character */
static bool process_glyph_input(const char *input, char *output, size_t output_max)
{
  if (!input || !input[0]) {
    output[0] = '\0';
    return false;
  }

  /* Try to convert as hex codepoint first */
  if (hex_codepoint_to_utf8(input, output, output_max)) {
    return true;
  }

  /* Invalid input - return empty string */
  output[0] = '\0';
  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Glyph Input Row Template
 *
 * Creates a row with glyph input fields and buttons:
 * - Left side: Search field (optional) + More glyphs button (centered)
 * - Right side: Code field + Paste button (right aligned)
 * \{ */

/* Callback data for the default "More glyphs" button */
struct GlyphButtonCallbackData {
  char *category;           /* Category for glyph search */
  char *glyph_propname;     /* Property name to update with selected glyph */
  PointerRNA *ptr;          /* PointerRNA containing the property */
};

/* Callback function for the default "More glyphs" button */
static void glyph_more_glyphs_default_cb(bContext *C, void *arg1, void * /*arg2*/)
{
  printf("[GLYPH CALLBACK] === glyph_more_glyphs_default_cb START ===\n");

  GlyphButtonCallbackData *data = static_cast<GlyphButtonCallbackData *>(arg1);
  if (!data) {
    printf("[GLYPH CALLBACK] ERROR: data is NULL!\n");
    return;
  }

  printf("[GLYPH CALLBACK] data->category = '%s'\n", data->category ? data->category : "NULL");
  printf("[GLYPH CALLBACK] data->glyph_propname = '%s'\n", data->glyph_propname ? data->glyph_propname : "NULL");

  /* Open glyph grid popup using direct operator call */
  wmOperatorType *ot = WM_operatortype_find("WM_OT_glyph_picker_grid", false);
  if (ot) {
    printf("[GLYPH CALLBACK] Found WM_OT_glyph_picker_grid operator\n");

    PointerRNA op_ptr = WM_operator_properties_create_ptr(ot);

    /* Set category */
    if (data->category && data->category[0] != '\0') {
      RNA_string_set(&op_ptr, "category", data->category);
      printf("[GLYPH CALLBACK] Set category = '%s'\n", data->category);
    }

    /* Set target_property with active_operator prefix
     * This tells the glyph picker to find the active operator and update its property
     * The grid select callback has special handling for "active_operator.xxx" paths */
    if (data->glyph_propname && data->glyph_propname[0] != '\0') {
      char target_prop[128];
      SNPRINTF(target_prop, "active_operator.%s", data->glyph_propname);
      RNA_string_set(&op_ptr, "target_property", target_prop);
      printf("[GLYPH CALLBACK] Set target_property = '%s'\n", target_prop);
    }

    /* НОВОЕ: Передаем точный указатель на целевой оператор */
    wmOperator *current_active_op = context_active_operator_get(C);
    if (current_active_op) {
      char target_op_ptr_str[64];
      SNPRINTF(target_op_ptr_str, "%llu", (unsigned long long)(uintptr_t)current_active_op);
      RNA_string_set(&op_ptr, "target_operator_ptr", target_op_ptr_str);
      printf("[GLYPH TEMPLATE CALLBACK] Set target_operator_ptr = '%s' (op=%p, idname='%s')\n", 
             target_op_ptr_str, (void*)current_active_op, 
             current_active_op->idname ? current_active_op->idname : "NULL");
    }

    printf("[GLYPH CALLBACK] Calling WM_operator_name_call_ptr...\n");
    WM_operator_name_call_ptr(C, ot, wm::OpCallContext::InvokeDefault, &op_ptr, nullptr);
    printf("[GLYPH CALLBACK] WM_operator_name_call_ptr returned\n");

    WM_operator_properties_free(&op_ptr);
  }
  else {
    printf("[GLYPH CALLBACK] ERROR: WM_OT_glyph_picker_grid operator not found!\n");
  }

  /* Clean up callback data */
  if (data->category) {
    MEM_delete_void(static_cast<void *>(data->category));
  }
  if (data->glyph_propname) {
    MEM_delete_void(static_cast<void *>(data->glyph_propname));
  }
  MEM_delete(data);

  printf("[GLYPH CALLBACK] === glyph_more_glyphs_default_cb END ===\n");
}

/* Internal implementation with callback support */
static void ui_template_glyph_input_row_impl(Layout *layout,
                                             bContext *C,
                                             PointerRNA *ptr,
                                             const char *glyph_propname,
                                             const char *search_propname,
                                             bool has_search,
                                             bool has_code,
                                             const char *category,
                                             ButtonHandleFunc more_glyphs_callback,
                                             void *callback_user_data)
{
  if (!layout || !ptr || !glyph_propname) {
    return;
  }

  /* Get the glyph property */
  PropertyRNA *glyph_prop = RNA_struct_find_property(ptr, glyph_propname);
  if (!glyph_prop) {
    return;
  }

  /* Main row with split layout */
  Layout &main_row = layout->row(false);
  Layout &split_main = main_row.split(0.69f, false);

  /* Left side: Search Glyph (optional) + More glyphs button - centered */
  Layout &left_side = split_main.row(true);
  left_side.alignment_set(LayoutAlign::Right);

  /* Add search field if requested */
  if (has_search && search_propname) {
    PropertyRNA *search_prop = RNA_struct_find_property(ptr, search_propname);
    if (search_prop) {
      left_side.prop(ptr, search_propname, UI_ITEM_NONE, IFACE_("Glyph"), ICON_VIEWZOOM);
    }
  }

  /* More glyphs button */
  Block *more_glyphs_block = left_side.block();
  block_layout_set_current(more_glyphs_block, &left_side);

  char glyph_btn[8] = "";
  process_glyph_input("f02f", glyph_btn, sizeof(glyph_btn));
  Button *glyph_but = uiDefBut(more_glyphs_block,
                                    ButtonType::But,
                                    glyph_btn,
                                    0,
                                    0,
                                    UI_UNIT_X * 1.5f,
                                    UI_UNIT_Y,
                                    nullptr,
                                    0,
                                    0,
                                    std::nullopt);
  glyph_but->tip_quick_func = [](const Button *) { return "More glyphs"; };

  /* Use provided callback if available, otherwise use default behavior */
  if (more_glyphs_callback) {
    printf("[GLYPH TEMPLATE] Using custom callback\n");
    button_func_set(glyph_but, more_glyphs_callback, callback_user_data, nullptr);
  }
  else if (category && category[0] != '\0') {
    printf("[GLYPH TEMPLATE] === Setting up DEFAULT callback ===\n");
    printf("[GLYPH TEMPLATE] category = '%s'\n", category);
    printf("[GLYPH TEMPLATE] glyph_propname = '%s'\n", glyph_propname);
    printf("[GLYPH TEMPLATE] ptr = %p\n", (void *)ptr);

    /* Default behavior: Open glyph grid popup with target operator info */
    GlyphButtonCallbackData *data = MEM_new<GlyphButtonCallbackData>(__func__);

    /* Copy category */
    data->category = MEM_new_array<char>(strlen(category) + 1, __func__);
    strcpy(data->category, category);

    /* Copy glyph property name */
    data->glyph_propname = MEM_new_array<char>(strlen(glyph_propname) + 1, __func__);
    strcpy(data->glyph_propname, glyph_propname);

    /* Store ptr for reference */
    data->ptr = ptr;

    printf("[GLYPH TEMPLATE] Created GlyphButtonCallbackData, setting button callback...\n");
    button_func_set(glyph_but, glyph_more_glyphs_default_cb, data, nullptr);
    printf("[GLYPH TEMPLATE] Button callback set successfully\n");
  }
  else {
    printf("[GLYPH TEMPLATE] WARNING: No callback set! category='%s'\n", category ? category : "NULL");
  }
  (void)glyph_but;

  /* Right side: Code field + Paste button - right aligned */
  if (has_code) {
    Layout &right_side = split_main.row(true);
    right_side.alignment_set(LayoutAlign::Right);

    /* Code field with Paste button */
    Layout &row_glyph = right_side.row(true);
    row_glyph.prop(ptr, glyph_propname, UI_ITEM_NONE, IFACE_("Code"), ICON_NONE);

    Layout &row_glyph_btn = row_glyph.row(true);
    /* Paste button - allows pasting hex code from clipboard (Ctrl+V) */
    PointerRNA paste_ptr = row_glyph_btn.op("SCREEN_OT_category_tab_paste_glyph", "", ICON_PASTEDOWN);
    if (category && category[0] != '\0') {
      RNA_string_set(&paste_ptr, "category", category);
    }
  }
}

/* Public API wrapper - calls internal implementation with no callbacks */
void uiTemplateGlyphInputRow(Layout *layout,
                             bContext *C,
                             PointerRNA *ptr,
                             const char *glyph_propname,
                             const char *search_propname,
                             bool has_search,
                             bool has_code,
                             const char *category)
{
  ui_template_glyph_input_row_impl(layout, C, ptr, glyph_propname, search_propname,
                                   has_search, has_code, category, nullptr, nullptr);
}

/* Internal API with callback support */
namespace internal {
void uiTemplateGlyphInputRowWithCallback(Layout *layout,
                                        bContext *C,
                                        PointerRNA *ptr,
                                        const char *glyph_propname,
                                        const char *search_propname,
                                        bool has_search,
                                        bool has_code,
                                        const char *category,
                                        ButtonHandleFunc more_glyphs_callback,
                                        void *callback_user_data)
{
  ui_template_glyph_input_row_impl(layout, C, ptr, glyph_propname, search_propname,
                                   has_search, has_code, category, more_glyphs_callback, callback_user_data);
}
}  // namespace internal

/** \} */

/* -------------------------------------------------------------------- */
/** \name Glyph Preview Template
 *
 * Creates a centered preview button showing a glyph with custom color.
 * \{ */

/* Storage for preview callback data - per button instance */
struct GlyphPreviewData {
  bool is_set;
  char glyph_unicode[8];
  float color[3];
  float size_multiplier;
};

/* Draw callback for preview button */
static void glyph_preview_draw_cb(const bContext * /*C*/,
                                  rcti *rect,
                                  const char *glyph_unicode,
                                  const float color[3],
                                  float size_multiplier)
{
  if (!glyph_unicode || glyph_unicode[0] == '\0') {
    return;
  }

  const uiStyle *style = style_get_dpi();
  const int fontid = BLF_default();
  const float font_size = style->widget.points * UI_SCALE_FAC * size_multiplier;
  BLF_size(fontid, font_size);

  /* Set color - use TH_TAB_TEXT_HI when custom color is (0.0, 0.0, 0.0) */
  if (is_zero_v3(color)) {
    uchar theme_col_tab_text_hi[3];
    theme::get_color_3ubv(TH_TAB_TEXT_HI, theme_col_tab_text_hi);
    BLF_color3ubv(fontid, theme_col_tab_text_hi);
  }
  else {
    BLF_color3fv_alpha(fontid, color, 1.0f);
  }

  /* Calculate center position with proper baseline adjustment */
  const float glyph_width = BLF_width(fontid, glyph_unicode, BLF_DRAW_STR_DUMMY_MAX);
  const int ascender_i = BLF_ascender(fontid);
  const int descender_i = BLF_descender(fontid);
  const float ascender = float(ascender_i);
  const float descender = float(descender_i);
  const float glyph_height = ascender - descender;

  const float rect_center_x = (rect->xmin + rect->xmax) * 0.5f;
  const float rect_center_y = (rect->ymin + rect->ymax) * 0.5f;

  const float x = rect_center_x - glyph_width * 0.5f;
  const float y = rect_center_y - glyph_height * 0.5f - descender;

  BLF_position(fontid, x, y, 0.0f);
  BLF_draw(fontid, glyph_unicode, BLF_DRAW_STR_DUMMY_MAX);
}



void uiTemplateGlyphPreview(Layout *layout,
                            bContext * /*C*/,
                            const char *glyph_unicode,
                            PointerRNA *ptr,
                            const char *color_propname,
                            float size_multiplier)
{
  if (!layout || !glyph_unicode || glyph_unicode[0] == '\0') {
    return;
  }

  /* Get color from RNA property */
  float color[3] = {0.0f, 0.0f, 0.0f};
  if (ptr && color_propname) {
    PropertyRNA *prop = RNA_struct_find_property(ptr, color_propname);
    if (prop) {
      RNA_property_float_get_array(ptr, prop, color);
    }
  }

  /* Create centered row for preview */
  Layout &preview_row = layout->row(false);
  preview_row.alignment_set(LayoutAlign::Center);

  /* Get block and create preview button */
  Block *preview_block = preview_row.block();
  block_layout_set_current(preview_block, &preview_row);

  const uiStyle *style = style_get_dpi();
  const int preview_size = int(style->widget.points * UI_SCALE_FAC * 3.0f * size_multiplier);

  /* Create local preview data for this button instance */
  GlyphPreviewData *preview_data = MEM_new<GlyphPreviewData>("GlyphPreviewData");
  preview_data->is_set = true;
  STRNCPY(preview_data->glyph_unicode, glyph_unicode);
  copy_v3_v3(preview_data->color, color);
  preview_data->size_multiplier = size_multiplier;

  /* Create preview button using Extra type with custom draw callback */
  Button *preview_but = uiDefBut(preview_block,
                                      ButtonType::Extra,
                                      "",
                                      0,
                                      0,
                                      preview_size,
                                      preview_size,
                                      preview_data,
                                      0.0f,
                                      0.0f,
                                      std::nullopt);

  /* Set custom draw callback with captured preview data */
  button_func_drawextra_set(preview_block, 
    [preview_data](const bContext *C, rcti *rect) {
      if (!preview_data || !preview_data->is_set) {
        return;
      }
      glyph_preview_draw_cb(C,
                           rect,
                           preview_data->glyph_unicode,
                           preview_data->color,
                           preview_data->size_multiplier);
    });
    
  preview_but->tip_quick_func = [glyph_unicode](const Button *) {
    return std::string("Glyph: ") + glyph_unicode;
  };
  
  /* Set cleanup function to free preview data when button is destroyed */
  preview_but->funcN = [](bContext *, void *arg1, void *) {
    if (arg1) {
      MEM_delete(static_cast<GlyphPreviewData *>(arg1));
    }
  };
  preview_but->func_arg1 = preview_data;
}

/* -------------------------------------------------------------------- */
/** \name Glyph Search Results Template
 * \{ */

/* Internal implementation with callback support */
static void ui_template_glyph_search_results_impl(Layout *layout,
                                                  bContext *C,
                                                  PointerRNA *ptr,
                                                  const char *search_propname,
                                                  const char *category,
                                                  const char *color_propname,
                                                  int max_results,
                                                  ButtonHandleFunc result_callback,
                                                  void *callback_user_data)
{
  if (!layout || !ptr || !search_propname || !category) {
    return;
  }

  /* Get search query */
  char search_query[64] = "";
  PropertyRNA *search_prop = RNA_struct_find_property(ptr, search_propname);
  if (!search_prop) {
    return;
  }
  RNA_property_string_get(ptr, search_prop, search_query);

  /* Only show results if search query is not empty */
  if (search_query[0] == '\0') {
    return;
  }

  /* Create a row for search results - same style as color presets */
  Layout &results_row = layout->row(true);
  results_row.alignment_set(LayoutAlign::Center);
  results_row.emboss_set(EmbossType::Pulldown);

  /* Get block for creating buttons */
  Block *result_block = results_row.block();
  block_layout_set_current(result_block, &results_row);

  /* Call Python API to search glyphs */
  auto search_results = glyph_search_call_python(C, search_query, category, max_results);

  /* Get category color for tinting glyph buttons */
  float category_color[3] = {0.0f, 0.0f, 0.0f};
  if (color_propname) {
    PropertyRNA *color_prop = RNA_struct_find_property(ptr, color_propname);
    if (color_prop) {
      RNA_property_float_get_array(ptr, color_prop, category_color);
    }
  }

  /* Convert float RGB (0.0-1.0) to uchar RGB (0-255) for button_color_set */
  uchar category_color_uchar[4];
  if (is_zero_v3(category_color)) {
    /* No custom color - use active tab text color */
    theme::get_color_3ubv(TH_TAB_TEXT_HI, category_color_uchar);
    category_color_uchar[3] = 255; /* Alpha */
  }
  else {
    /* Use custom category color */
    category_color_uchar[0] = uchar(category_color[0] * 255.0f);
    category_color_uchar[1] = uchar(category_color[1] * 255.0f);
    category_color_uchar[2] = uchar(category_color[2] * 255.0f);
    category_color_uchar[3] = 255; /* Alpha */
  }

  if (search_results.is_empty()) {
    /* No results found */
    results_row.label(IFACE_("No glyphs found"), ICON_NONE);
  }
  else {
    /* Create buttons for each search result - same style as color presets */
    for (const auto &result : search_results) {
      const std::string &glyph_unicode = result.first;
      const std::string &glyph_name = result.second;

      /* Create button with glyph - same size and style as color presets */
      Button *result_but = uiDefBut(result_block,
                                    ButtonType::But,
                                    glyph_unicode.c_str(),
                                    0,
                                    0,
                                    UI_UNIT_X * 1.5f,
                                    UI_UNIT_Y,
                                    nullptr,
                                    0,
                                    0,
                                    std::nullopt);

      /* Apply category color to the button - same as color presets */
      button_color_set(result_but, category_color_uchar);
      result_but->drawflag |= BUT_TEXT_USE_COL;

      /* Set tooltip with glyph name */
      result_but->tip_quick_func = [glyph_name](const Button *) { return glyph_name; };

      /* Add callback if provided */
      if (result_callback) {
        /* Copy glyph unicode for callback - will be freed by callback */
        char *glyph_unicode_copy = MEM_new_array<char>(glyph_unicode.length() + 1, __func__);
        strcpy(glyph_unicode_copy, glyph_unicode.c_str());
        button_func_set(result_but, result_callback, callback_user_data, glyph_unicode_copy);
      }
    }
  }
}

/* Public API wrapper - calls internal implementation with no callbacks */
void uiTemplateGlyphSearchResults(Layout *layout,
                                  bContext *C,
                                  PointerRNA *ptr,
                                  const char *search_propname,
                                  const char *category,
                                  const char *color_propname,
                                  int max_results)
{
  ui_template_glyph_search_results_impl(layout, C, ptr, search_propname, category,
                                        color_propname, max_results, nullptr, nullptr);
}

/* Internal API with callback support */
namespace internal {
void uiTemplateGlyphSearchResultsWithCallback(Layout *layout,
                                              bContext *C,
                                              PointerRNA *ptr,
                                              const char *search_propname,
                                              const char *category,
                                              const char *color_propname,
                                              int max_results,
                                              ButtonHandleFunc result_callback,
                                              void *callback_user_data)
{
  ui_template_glyph_search_results_impl(layout, C, ptr, search_propname, category,
                                        color_propname, max_results, result_callback, callback_user_data);
}
}  // namespace internal

/** \} */

/* -------------------------------------------------------------------- */
/** \name Combined Glyph Selector Template
 * \{ */

/* Internal implementation with callback support */
static void ui_template_glyph_selector_impl(Layout *layout,
                                            bContext *C,
                                            PointerRNA *ptr,
                                            const char *glyph_propname,
                                            const char *search_propname,
                                            const char *color_propname,
                                            const char *category,
                                            bool show_preview,
                                            bool show_search,
                                            bool show_code,
                                            ButtonHandleFunc more_glyphs_callback,
                                            void *callback_user_data,
                                            ButtonHandleFunc result_callback)
{
  printf("[GLYPH SELECTOR IMPL] === ui_template_glyph_selector_impl START ===\n");
  printf("[GLYPH SELECTOR IMPL] glyph_propname = '%s'\n", glyph_propname ? glyph_propname : "NULL");
  printf("[GLYPH SELECTOR IMPL] search_propname = '%s'\n", search_propname ? search_propname : "NULL");
  printf("[GLYPH SELECTOR IMPL] category = '%s'\n", category ? category : "NULL");
  printf("[GLYPH SELECTOR IMPL] more_glyphs_callback = %p\n", (void *)more_glyphs_callback);

  if (!layout || !ptr || !glyph_propname) {
    printf("[GLYPH SELECTOR IMPL] ERROR: Invalid parameters! layout=%p, ptr=%p, glyph_propname=%s\n",
           (void *)layout, (void *)ptr, glyph_propname ? glyph_propname : "NULL");
    return;
  }

  /* Input row with search/code fields and buttons */
  if (show_search || show_code) {
    printf("[GLYPH SELECTOR IMPL] Calling ui_template_glyph_input_row_impl...\n");
    ui_template_glyph_input_row_impl(layout, C, ptr, glyph_propname, search_propname,
                                     show_search, show_code, category,
                                     more_glyphs_callback, callback_user_data);
  }

  /* Search results (only if search is enabled and search_propname is provided) */
  if (show_search && search_propname) {
    ui_template_glyph_search_results_impl(layout, C, ptr, search_propname, category,
                                          color_propname, 50, result_callback, callback_user_data);
  }

  /* Preview (if requested) */
  if (show_preview) {
    /* Get current glyph value */
    char glyph_value[16] = "";
    PropertyRNA *glyph_prop = RNA_struct_find_property(ptr, glyph_propname);
    if (glyph_prop) {
      RNA_property_string_get(ptr, glyph_prop, glyph_value);

      /* Only show preview if there's a glyph value */
      if (glyph_value[0] != '\0') {
        /* Convert hex code to UTF-8 if needed */
        char glyph_unicode[8] = "";
        if (hex_codepoint_to_utf8(glyph_value, glyph_unicode, sizeof(glyph_unicode))) {
          uiTemplateGlyphPreview(layout, C, glyph_unicode, ptr, color_propname, 2.0f);
        }
      }
    }
  }
}

/* Public API wrapper - calls internal implementation with no callbacks */
void uiTemplateGlyphSelector(Layout *layout,
                             bContext *C,
                             PointerRNA *ptr,
                             const char *glyph_propname,
                             const char *search_propname,
                             const char *color_propname,
                             const char *category,
                             bool show_preview,
                             bool show_search,
                             bool show_code)
{
  printf("[GLYPH TEMPLATE SELECTOR] === uiTemplateGlyphSelector called ===\n");
  printf("[GLYPH TEMPLATE SELECTOR] glyph_propname = '%s'\n", glyph_propname ? glyph_propname : "NULL");
  printf("[GLYPH TEMPLATE SELECTOR] search_propname = '%s'\n", search_propname ? search_propname : "NULL");
  printf("[GLYPH TEMPLATE SELECTOR] category = '%s'\n", category ? category : "NULL");
  printf("[GLYPH TEMPLATE SELECTOR] show_preview = %d, show_search = %d, show_code = %d\n",
         show_preview, show_search, show_code);
  printf("[GLYPH TEMPLATE SELECTOR] ptr = %p\n", (void *)ptr);

  ui_template_glyph_selector_impl(layout, C, ptr, glyph_propname, search_propname,
                                  color_propname, category, show_preview, show_search, show_code,
                                  nullptr, nullptr, nullptr);

  printf("[GLYPH TEMPLATE SELECTOR] === uiTemplateGlyphSelector finished ===\n");
}

/* Internal API with callback support */
namespace internal {
void uiTemplateGlyphSelectorWithCallback(Layout *layout,
                                        bContext *C,
                                        PointerRNA *ptr,
                                        const char *glyph_propname,
                                        const char *search_propname,
                                        const char *color_propname,
                                        const char *category,
                                        bool show_preview,
                                        bool show_search,
                                        bool show_code,
                                        ButtonHandleFunc more_glyphs_callback,
                                        void *callback_user_data,
                                        ButtonHandleFunc result_callback)
{
  ui_template_glyph_selector_impl(layout, C, ptr, glyph_propname, search_propname,
                                  color_propname, category, show_preview, show_search, show_code,
                                  more_glyphs_callback, callback_user_data, result_callback);
}
}  // namespace internal

/** \} */

}  // namespace blender::ui
