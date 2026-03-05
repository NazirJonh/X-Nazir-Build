/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include "DNA_ID.h"
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
#include "UI_interface_c.hh"
#include "interface_intern.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "BLF_api.hh"

#include "BLT_translation.hh"

#include "BKE_idprop.hh"

namespace blender::ui {

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
  wmOperator *target_op;    /* Resolved target operator owning the property */
  IDProperty *target_op_properties;
};

/* Callback data for default glyph search result buttons. */
struct GlyphSearchResultCallbackData {
  char *glyph_propname;
  char *search_propname;
  wmOperator *target_op;
  IDProperty *target_op_properties;
};

/* Try to resolve the operator that owns the given OperatorProperties PointerRNA. */
static wmOperator *glyph_find_operator_from_properties_ptr(const bContext *C, const PointerRNA *ptr)
{
  if (!C || !ptr || !ptr->data) {
    return nullptr;
  }

  wmWindowManager *wm = CTX_wm_manager(C);
  IDProperty *properties = static_cast<IDProperty *>(ptr->data);

  if (wm && ptr->owner_id == &wm->id) {
    for (wmOperator *op = static_cast<wmOperator *>(wm->runtime->operators.last); op; op = op->prev)
    {
      if (op && op->properties == properties) {
        return op;
      }
    }
  }

  if (wm) {
    for (wmOperator *op = static_cast<wmOperator *>(wm->runtime->operators.last); op; op = op->prev) {
      if (!op || !op->ptr) {
        continue;
      }

      if (op->ptr == ptr || op->ptr->data == ptr->data || op->properties == ptr->data) {
        return op;
      }
    }
  }

  wmOperator *active_op = context_active_operator_get(C);
  if (active_op && active_op->ptr) {
    if (active_op->ptr == ptr || active_op->ptr->data == ptr->data || active_op->properties == ptr->data) {
      return active_op;
    }
  }

  return nullptr;
}

/* Default callback for glyph search result buttons when no custom callback is provided. */
static void glyph_search_result_default_cb(bContext *C, void *arg1, void *arg2)
{
  GlyphSearchResultCallbackData *data = static_cast<GlyphSearchResultCallbackData *>(arg1);
  const char *glyph_unicode = static_cast<const char *>(arg2);

  if (!data || !glyph_unicode || glyph_unicode[0] == '\0') {
    if (glyph_unicode) {
      MEM_delete_void(static_cast<void *>(const_cast<char *>(glyph_unicode)));
    }
    if (data) {
      if (data->glyph_propname) {
        MEM_delete_void(static_cast<void *>(data->glyph_propname));
      }
      if (data->search_propname) {
        MEM_delete_void(static_cast<void *>(data->search_propname));
      }
      MEM_delete(data);
    }
    return;
  }

  const uint codepoint = BLI_str_utf8_as_unicode_safe(glyph_unicode);
  if (codepoint == BLI_UTF8_ERR || codepoint > 0x10FFFF) {
    MEM_delete_void(static_cast<void *>(const_cast<char *>(glyph_unicode)));
    if (data->glyph_propname) {
      MEM_delete_void(static_cast<void *>(data->glyph_propname));
    }
    if (data->search_propname) {
      MEM_delete_void(static_cast<void *>(data->search_propname));
    }
    MEM_delete(data);
    return;
  }

  char hex_code[16];
  SNPRINTF(hex_code, "%x", codepoint);

  const char *glyph_propname = (data->glyph_propname && data->glyph_propname[0] != '\0') ?
                                   data->glyph_propname :
                                   "glyph";

  const char *search_propname = (data->search_propname && data->search_propname[0] != '\0') ?
                                    data->search_propname :
                                    "glyph_search";

  wmOperator *target_op = data->target_op;
  if (!target_op && data->target_op_properties) {
    wmWindowManager *wm = CTX_wm_manager(C);
    if (wm) {
      for (wmOperator *op_iter = static_cast<wmOperator *>(wm->runtime->operators.last); op_iter;
           op_iter = op_iter->prev)
      {
        if (op_iter && op_iter->properties == data->target_op_properties) {
          target_op = op_iter;
          break;
        }
      }
    }
  }

  bool glyph_applied = false;

  /* Primary path for invoke_props_dialog: write directly to captured operator IDProperties. */
  if (data->target_op_properties && strchr(glyph_propname, '.') == nullptr &&
      strchr(glyph_propname, '[') == nullptr)
  {
    IDProperty *idprop = IDP_GetPropertyFromGroup(data->target_op_properties, glyph_propname);
    if (!idprop) {
      idprop = IDP_NewString(hex_code, glyph_propname);
      IDP_AddToGroup(data->target_op_properties, idprop);
      glyph_applied = true;
    }
    else if (idprop->type == IDP_STRING) {
      IDP_AssignString(idprop, hex_code);
      glyph_applied = true;
    }
  }

  if (!glyph_applied && target_op && target_op->ptr) {
    PropertyRNA *glyph_prop = RNA_struct_find_property(target_op->ptr, glyph_propname);
    if (glyph_prop) {
      RNA_property_string_set(target_op->ptr, glyph_prop, hex_code);
      RNA_property_update(C, target_op->ptr, glyph_prop);
      glyph_applied = true;
    }
  }

  if (glyph_applied && target_op && target_op->ptr) {
    PropertyRNA *search_prop = RNA_struct_find_property(target_op->ptr, search_propname);
    if (search_prop) {
      RNA_property_string_set(target_op->ptr, search_prop, "");
      RNA_property_update(C, target_op->ptr, search_prop);
    }
  }
  else if (glyph_applied && data->target_op_properties && strchr(search_propname, '.') == nullptr &&
           strchr(search_propname, '[') == nullptr)
  {
    IDProperty *search_idprop = IDP_GetPropertyFromGroup(data->target_op_properties, search_propname);
    if (!search_idprop) {
      search_idprop = IDP_NewString("", search_propname);
      IDP_AddToGroup(data->target_op_properties, search_idprop);
    }
    else if (search_idprop->type == IDP_STRING) {
      IDP_AssignString(search_idprop, "");
    }
  }

  if (glyph_applied) {
    WM_main_add_notifier(NC_WINDOW, nullptr);
  }

  MEM_delete_void(static_cast<void *>(const_cast<char *>(glyph_unicode)));
  if (data->glyph_propname) {
    MEM_delete_void(static_cast<void *>(data->glyph_propname));
  }
  if (data->search_propname) {
    MEM_delete_void(static_cast<void *>(data->search_propname));
  }
  MEM_delete(data);
}

/* Callback function for the default "More glyphs" button */
static void glyph_more_glyphs_default_cb(bContext *C, void *arg1, void * /*arg2*/)
{
  GlyphButtonCallbackData *data = static_cast<GlyphButtonCallbackData *>(arg1);
  if (!data) {
    return;
  }

  wmOperator *target_op = data->target_op;

  /* Open glyph grid popup using direct operator call */
  wmOperatorType *ot = WM_operatortype_find("WM_OT_glyph_picker_grid", false);
  if (ot) {
    PointerRNA op_ptr = WM_operator_properties_create_ptr(ot);

    /* Set category */
    if (data->category && data->category[0] != '\0') {
      RNA_string_set(&op_ptr, "category", data->category);
    }

    if (data->glyph_propname && data->glyph_propname[0] != '\0') {
      char target_prop[128];
      SNPRINTF(target_prop, "%s", data->glyph_propname);
      RNA_string_set(&op_ptr, "target_property", target_prop);
    }

    if (target_op) {
      char target_op_ptr_str[64];
      SNPRINTF(target_op_ptr_str, "%llu", (unsigned long long)(uintptr_t)target_op);
      RNA_string_set(&op_ptr, "target_operator_ptr", target_op_ptr_str);
    }

    if (data->target_op_properties) {
      char target_op_props_ptr_str[64];
      SNPRINTF(target_op_props_ptr_str,
               "%llu",
               (unsigned long long)(uintptr_t)data->target_op_properties);
      RNA_string_set(&op_ptr, "target_operator_properties_ptr", target_op_props_ptr_str);
    }

    WM_operator_name_call_ptr(C, ot, wm::OpCallContext::InvokeDefault, &op_ptr, nullptr);

    WM_operator_properties_free(&op_ptr);
  }

  /* Clean up callback data */
  if (data->category) {
    MEM_delete_void(static_cast<void *>(data->category));
  }
  if (data->glyph_propname) {
    MEM_delete_void(static_cast<void *>(data->glyph_propname));
  }
  MEM_delete(data);
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
    button_func_set(glyph_but, more_glyphs_callback, callback_user_data, nullptr);
  }
  else {
    /* Default behavior: Open glyph grid popup with target operator info */
    GlyphButtonCallbackData *data = MEM_new<GlyphButtonCallbackData>(__func__);

    /* Copy category (optional) */
    if (category && category[0] != '\0') {
      data->category = MEM_new_array<char>(strlen(category) + 1, __func__);
      strcpy(data->category, category);
    }
    else {
      data->category = nullptr;
    }

    /* Copy glyph property name */
    data->glyph_propname = MEM_new_array<char>(strlen(glyph_propname) + 1, __func__);
    strcpy(data->glyph_propname, glyph_propname);

    /* Resolve and store target operator now (while PointerRNA is valid in this draw call). */
    data->target_op = glyph_find_operator_from_properties_ptr(C, ptr);
    data->target_op_properties = static_cast<IDProperty *>(ptr->data);
    button_func_set(glyph_but, glyph_more_glyphs_default_cb, data, nullptr);
  }

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

  /* Create centered row for preview */
  Layout &preview_row = layout->row(false);
  preview_row.alignment_set(LayoutAlign::Center);

  /* Get block and create preview button */
  Block *preview_block = preview_row.block();
  block_layout_set_current(preview_block, &preview_row);

  const uiStyle *style = style_get_dpi();
  const int preview_size = int(style->widget.points * UI_SCALE_FAC * 2.0f * size_multiplier);

  /* Copy callback data to keep it valid across redraws while popup stays open. */
  const std::string glyph_unicode_copy = glyph_unicode;
  const std::string color_propname_copy = color_propname ? color_propname : "";
  const PointerRNA preview_ptr = ptr ? *ptr : PointerRNA_NULL;
  const bool has_preview_ptr = ptr != nullptr;

  /* Create preview button using Extra type with custom draw callback */
  Button *preview_but = uiDefBut(preview_block,
                                       ButtonType::Extra,
                                      "",
                                      0,
                                       0,
                                       preview_size,
                                       preview_size,
                                       nullptr,
                                       0.0f,
                                       0.0f,
                                       std::nullopt);

  /* Read color from RNA on every redraw for live-update preview in dialogs/popups. */
  button_func_drawextra_set(preview_block,
                            [glyph_unicode_copy, color_propname_copy, preview_ptr, has_preview_ptr, size_multiplier](
                                const bContext *C, rcti *rect) {
                              float draw_color[3] = {0.0f, 0.0f, 0.0f};
                              if (has_preview_ptr && !color_propname_copy.empty()) {
                                PointerRNA preview_ptr_local = preview_ptr;
                                PropertyRNA *prop = RNA_struct_find_property(
                                    &preview_ptr_local, color_propname_copy.c_str());
                                if (prop) {
                                  RNA_property_float_get_array(&preview_ptr_local, prop, draw_color);
                                }
                              }
                              glyph_preview_draw_cb(
                                  C, rect, glyph_unicode_copy.c_str(), draw_color, size_multiplier);
                            });

  preview_but->tip_quick_func = [glyph_unicode_copy](const Button *) {
    return std::string("Glyph: ") + glyph_unicode_copy;
  };
}

/* -------------------------------------------------------------------- */
/** \name Glyph Search Results Template
 * \{ */

/* Internal implementation with callback support */
static void ui_template_glyph_search_results_impl(Layout *layout,
                                                  bContext *C,
                                                  PointerRNA *ptr,
                                                  const char *glyph_propname,
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

      /* Add callback if provided, otherwise use default behavior. */
      if (result_callback) {
        /* Copy glyph unicode for callback - will be freed by callback */
        char *glyph_unicode_copy = MEM_new_array<char>(glyph_unicode.length() + 1, __func__);
        strcpy(glyph_unicode_copy, glyph_unicode.c_str());
        button_func_set(result_but, result_callback, callback_user_data, glyph_unicode_copy);
      }
      else {
        GlyphSearchResultCallbackData *default_data = MEM_new<GlyphSearchResultCallbackData>(__func__);

        const char *target_glyph_propname =
            (glyph_propname && glyph_propname[0] != '\0') ? glyph_propname : "glyph";
        const char *target_search_propname =
            (search_propname && search_propname[0] != '\0') ? search_propname : "glyph_search";

        default_data->glyph_propname = MEM_new_array<char>(strlen(target_glyph_propname) + 1,
                                                           __func__);
        strcpy(default_data->glyph_propname, target_glyph_propname);

        default_data->search_propname = MEM_new_array<char>(strlen(target_search_propname) + 1,
                                                            __func__);
        strcpy(default_data->search_propname, target_search_propname);

        default_data->target_op = glyph_find_operator_from_properties_ptr(C, ptr);
        default_data->target_op_properties = static_cast<IDProperty *>(ptr->data);

        char *glyph_unicode_copy = MEM_new_array<char>(glyph_unicode.length() + 1, __func__);
        strcpy(glyph_unicode_copy, glyph_unicode.c_str());

        button_func_set(result_but, glyph_search_result_default_cb, default_data, glyph_unicode_copy);
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
  ui_template_glyph_search_results_impl(layout, C, ptr, nullptr, search_propname, category,
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
  ui_template_glyph_search_results_impl(layout, C, ptr, nullptr, search_propname, category,
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
  if (!layout || !ptr || !glyph_propname) {
    return;
  }

  /* Input row with search/code fields and buttons */
  if (show_search || show_code) {
    ui_template_glyph_input_row_impl(layout, C, ptr, glyph_propname, search_propname,
                                     show_search, show_code, category,
                                     more_glyphs_callback, callback_user_data);
  }

  /* Search results (only if search is enabled and search_propname is provided) */
  if (show_search && search_propname) {
    ui_template_glyph_search_results_impl(layout, C, ptr, glyph_propname, search_propname, category,
                                          color_propname, 50, result_callback, callback_user_data);
  }

  /* Preview (if requested) */
  if (show_preview) {
    /* Get current glyph value */
    char glyph_value[16] = "";
    PropertyRNA *glyph_prop = RNA_struct_find_property(ptr, glyph_propname);
    if (glyph_prop) {
      RNA_property_string_get(ptr, glyph_prop, glyph_value);

      char glyph_unicode[8] = "";
      if (glyph_value[0] != '\0') {
        /* Convert hex code to UTF-8 if needed */
        hex_codepoint_to_utf8(glyph_value, glyph_unicode, sizeof(glyph_unicode));
      }
      else if (C && category && category[0] != '\0' && is_single_glyph_str(category)) {
        /* Glyph-only category: show default glyph when no explicit glyph is set. */
        wmWindowManager *wm = CTX_wm_manager(C);
        bool is_fallback = false;
        const char *default_glyph = panel_category_glyph_lookup(
            wm, category, nullptr, &is_fallback, nullptr);
        if (default_glyph && default_glyph[0] != '\0') {
          STRNCPY(glyph_unicode, default_glyph);
        }
      }

      if (glyph_unicode[0] != '\0') {
        uiTemplateGlyphPreview(layout, C, glyph_unicode, ptr, color_propname, 2.0f);
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
  ui_template_glyph_selector_impl(layout, C, ptr, glyph_propname, search_propname,
                                  color_propname, category, show_preview, show_search, show_code,
                                  nullptr, nullptr, nullptr);
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
