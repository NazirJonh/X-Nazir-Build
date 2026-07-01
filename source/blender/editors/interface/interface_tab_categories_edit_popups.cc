/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Category Tab Edit Popup - glyph and icon grid selector popups and their picker
 * operators (glyph search call into Python, builtin icon grid).
 *
 * Split from interface_tab_categories_edit.cc; see
 * interface_tab_categories_edit_intern.hh for the shared internal contract.
 */

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "MEM_guardedalloc.h"

#include "DNA_ID.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"
#include "DNA_userdef_types.h"

#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_rect.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_time.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "BKE_context.hh"
#include "BKE_idprop.hh"
#include "BKE_screen.hh"

#include "ED_screen.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"
#include "RNA_path.hh"
#include "RNA_prototypes.hh"

#include "BLF_api.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "UI_glyph_grid_view.hh"
#include "UI_tree_view.hh"

#include "interface_intern.hh"
#include "interface_category_py_bridge.hh"
#include "interface_tag_bar.hh"
#include "regions/interface_regions_intern.hh"
#include "interface_tab_categories_edit_intern.hh"

#ifdef WITH_PYTHON
#  include "BPY_extern.hh"
#  include "BPY_extern_run.hh"
#endif

namespace blender::ui {

/* Import internal template functions with callback support */
using internal::uiTemplateGlyphInputRowWithCallback;
using internal::uiTemplateGlyphSearchResultsWithCallback;
using internal::uiTemplateGlyphSelectorWithCallback;

/* -------------------------------------------------------------------- */
/** \name Glyph Search Helper Functions
 * \{ */

static void glyph_search_parse_object_array(const char *json,
                                            const int max_results,
                                            blender::Vector<std::pair<std::string, std::string>> &r_results)
{
  const char *p = json;

  while (*p && *p != '[') {
    p++;
  }
  if (*p != '[') {
    return;
  }
  p++;

  int parsed_count = 0;
  while (*p && *p != ']') {
    while (*p && *p != '{') {
      p++;
    }
    if (*p == '{') {
      p++;
    }

    std::string unicode;
    std::string name;

    while (*p && *p != '}') {
      if (strncmp(p, "\"unicode\":", 10) == 0) {
        p += 10;
        while (*p && *p != '"') {
          p++;
        }
        if (*p == '"') {
          p++;
        }
        const char *start = p;
        while (*p && *p != '"') {
          p++;
        }
        const std::string raw_unicode(start, p - start);
        unicode = category_tab_decode_json_unicode(raw_unicode.c_str());
      }
      else if (strncmp(p, "\"name\":", 7) == 0) {
        p += 7;
        while (*p && *p != '"') {
          p++;
        }
        if (*p == '"') {
          p++;
        }
        const char *start = p;
        while (*p && *p != '"') {
          p++;
        }
        name = std::string(start, p - start);
      }
      else {
        p++;
      }
    }

    if (!unicode.empty() && !name.empty()) {
      parsed_count++;
      r_results.append({unicode, name});
      if (r_results.size() >= max_results) {
        break;
      }
    }

    while (*p && *p != ',' && *p != ']') {
      p++;
    }
    if (*p == ',' || *p == '}') {
      p++;
    }
  }
}

blender::Vector<std::pair<std::string, std::string>> glyph_search_call_python(
    bContext *C, const char *query, const char *category, int max_results)
{
  blender::Vector<std::pair<std::string, std::string>> results;

  /* Clamp to a sane upper bound; treat negative/oversized inputs as "no extra limit". */
  if (max_results < 0 || max_results > GLYPH_SEARCH_MAX_RESULTS) {
    max_results = GLYPH_SEARCH_MAX_RESULTS;
  }

  /* Run the search via the centralized Python bridge; parse its JSON result here
   * (the JSON parsing helpers stay at this call site). */
  const std::string json = category_py_search_glyphs_json(
      C, query, category ? category : "", max_results);
  if (json.empty()) {
    return results;
  }

  /* Bail out on an error object or an empty array. */
  if (strncmp(json.c_str(), "{\"error\":", 9) == 0 || strcmp(json.c_str(), "[]") == 0) {
    return results;
  }

  glyph_search_parse_object_array(json.c_str(), max_results, results);

  if (results.is_empty()) {
    blender::Vector<std::string> string_results;
    if (category_tab_parse_json_string_array_minimal(json.c_str(), string_results)) {
      for (const std::string &value : string_results) {
        const std::string unicode = category_tab_decode_json_unicode(value.c_str());
        if (!unicode.empty()) {
          results.append({unicode, value});
          if (results.size() >= max_results) {
            break;
          }
        }
      }
    }
  }

  return results;
}

/* -------------------------------------------------------------------- */
/** \name Glyph Cache
 * \{ */

/* Static cache for glyphs - persists until explicitly cleared or Blender exits */
static blender::Vector<std::pair<std::string, std::string>> g_glyph_cache;
static bool g_glyph_cache_valid = false;

/**
 * Clear the glyph cache. Call this when glyphs need to be reloaded.
 */
static void glyph_cache_clear()
{
  g_glyph_cache.clear();
  g_glyph_cache_valid = false;
}

/**
 * Get glyphs from cache, or load them if cache is invalid.
 * Returns a reference to the cached glyph vector.
 */
static const blender::Vector<std::pair<std::string, std::string>>& glyph_cache_get(bContext *C)
{
  if (!g_glyph_cache_valid) {
    g_glyph_cache = glyph_search_call_python(C, "", "", GLYPH_SEARCH_MAX_RESULTS);
    g_glyph_cache_valid = true;
  }
  return g_glyph_cache;
}

/** \} */

/**
 * Callback for when a search result button is clicked.
 * Sets the glyph code and updates the preview.
 */
void glyph_search_result_button_cb(bContext *C, void *arg1, void *arg2)
{
  wmOperator *op = static_cast<wmOperator *>(arg1);
  const char *glyph_unicode = static_cast<const char *>(arg2);

  if (!op || !glyph_unicode) {
    return;
  }

  /* Convert unicode to hex codepoint */
  char hex_code[16] = "";
  utf8_to_hex_codepoint(glyph_unicode, hex_code, sizeof(hex_code));

  /* Set the glyph property */
  RNA_string_set(op->ptr, "glyph", hex_code);

  /* Clear the search field */
  RNA_string_set(op->ptr, "glyph_search", "");

  /* Trigger live update to refresh preview */
  category_tab_edit_live_update_cb(C, op, 0);

  /* Trigger redraw */
  WM_main_add_notifier(NC_WINDOW, nullptr);

  /* Free the glyph unicode copy that was allocated with MEM_new_array */
  MEM_delete_void(static_cast<void *>(const_cast<char *>(glyph_unicode)));
}

/* -------------------------------------------------------------------- */
/** \name Glyph Grid Popup
 * \{ */

/**
 * Data structure for glyph grid popup.
 * Stores the operator, glyphs, categories and UI state.
 */
struct GlyphGridPopupData {
  wmOperator *op; /* The picker operator (WM_OT_glyph_picker_grid) */
  wmOperator *target_op; /* The target operator whose property should be updated (e.g., wm.category_tag_create) */
  IDProperty *target_op_properties;
  Vector<std::pair<std::string, std::string>> glyphs; /* (unicode, name) pairs */
  std::string current_category; /* Currently selected category filter */
  Vector<std::string> categories; /* All available categories */
  std::string search_string; /* Search filter */
  PopupBlockHandle *popup_handle;

  GlyphGridPopupData(wmOperator *op_,
                     wmOperator *target_op_,
                     IDProperty *target_op_properties_,
                     Vector<std::pair<std::string, std::string>> glyphs_,
                     std::string category_,
                     Vector<std::string> categories_)
      : op(op_),
        target_op(target_op_),
        target_op_properties(target_op_properties_),
        glyphs(std::move(glyphs_)),
        current_category(std::move(category_)),
        categories(std::move(categories_)),
        search_string(""),
        popup_handle(nullptr)
  {
  }
};

/* -------------------------------------------------------------------- */
/** \name Glyph Category Tree View
 * \{ */

/**
 * Tree view for displaying and selecting glyph categories.
 * Similar to Asset Shelf's catalog tree.
 */
class GlyphCategoryTreeView : public AbstractTreeView {
  /** Reference to the popup data to update selected category */
  GlyphGridPopupData *popup_data_;

 public:
  GlyphCategoryTreeView(GlyphGridPopupData *popup_data) : popup_data_(popup_data) {}

  void build_tree() override
  {
    /* "ALL" item - shows all glyphs regardless of category (default) */
    BasicTreeViewItem &all_item = this->add_tree_item<BasicTreeViewItem>(IFACE_("ALL"));
    all_item.set_on_activate_fn([this](bContext &C, BasicTreeViewItem &) {
      popup_data_->current_category = "ALL";
      send_redraw_notifier(C);
    });
    all_item.set_is_active_fn([this]() -> bool {
      return popup_data_->current_category == "ALL" || popup_data_->current_category.empty();
    });
    all_item.uncollapse_by_default();

    /* Add each category as a tree item */
    for (const std::string &category : popup_data_->categories) {
      if (category == "ALL") {
        continue; /* Skip ALL, already added above */
      }

      BasicTreeViewItem &category_item = this->add_tree_item<BasicTreeViewItem>(category);
      category_item.set_on_activate_fn([this, category](bContext &C, BasicTreeViewItem &) {
        popup_data_->current_category = category;
        send_redraw_notifier(C);
      });
      category_item.set_is_active_fn([this, category]() -> bool {
        return popup_data_->current_category == category;
      });
    }

    /* Keep the popup open when clicking a category */
    this->set_popup_keep_open();
  }

 private:
  void send_redraw_notifier(const bContext &C)
  {
    UNUSED_VARS(C);
    WM_main_add_notifier(NC_WINDOW, nullptr);
  }
};

/** \} */

constexpr int GLYPH_POPUP_LEFT_COL_WIDTH = 12;
constexpr int GLYPH_POPUP_RIGHT_COL_WIDTH = 45;

/**
 * Block creation function for glyph grid popup.
 * Creates a popup with category tree view (left), search (top right), and grid view (right).
 * Similar layout to Asset Shelf popup.
 */
static Block *glyph_grid_popup_block_create(bContext *C, ARegion *region, void *arg)
{
  GlyphGridPopupData *popup_data = static_cast<GlyphGridPopupData *>(arg);

  /* Sync search string from picker operator if available */
  if (popup_data->op) {
    char search_buf[256] = "";
    RNA_string_get(popup_data->op->ptr, "glyph_search", search_buf);
    popup_data->search_string = search_buf;
  }

  /* Create block */
  Block *block = block_begin(C, region, "glyph_grid_popup", EmbossType::Emboss);
  block_flag_enable(block, BLOCK_LOOP | BLOCK_MOVEMOUSE_QUIT);
  block_theme_style_set(block, BLOCK_THEME_STYLE_POPUP);

  /* Set popup size - using only right column width (left column temporarily disabled) */
  const int popup_width = GLYPH_POPUP_RIGHT_COL_WIDTH * UI_UNIT_X;
  const int popup_height = 175 * UI_UNIT_Y; /* 5x larger height (35 * 5 = 175) with scroll */
  /* Center the popup on screen */
  block_bounds_set_centered(block, 6 * UI_SCALE_FAC);

  /* Create main layout */
  Layout &layout = block_layout(block,
                                LayoutDirection::Vertical,
                                LayoutType::Panel,
                                0,
                                0,
                                popup_width,
                                popup_height,
                                0,
                                style_get());

  /* Create layout for popup content */
  Layout &row = layout.row(false);

  /* NOTE: Left column with category tree view is temporarily disabled.
   * May be re-enabled in the future when more categories are added.
   * Original code:
   *   Layout &left_col = row.column(false);
   *   left_col.ui_units_x_set(GLYPH_POPUP_LEFT_COL_WIDTH);
   *   left_col.ui_units_y_set(150);
   *   left_col.fixed_size_set(true);
   *   std::unique_ptr<GlyphCategoryTreeView> category_tree_ptr =
   *       std::make_unique<GlyphCategoryTreeView>(popup_data);
   *   AbstractTreeView *category_tree =
   *       block_add_view(*block, "glyph_category_tree", std::move(category_tree_ptr));
   *   TreeViewBuilder::build_tree_view(*C, *category_tree, left_col);
   */

  /* Main column: Search + Grid view */
  Layout &right_col = row.column(false);
  right_col.ui_units_y_set(150); /* Set minimum height for right column */
  right_col.fixed_size_set(true);

  /* Add search field at top of right column - only if picker operator available */
  Layout *search_row_ptr = nullptr;
  if (popup_data->op) {
    search_row_ptr = &right_col.row(false);

    /* Use prop() to create search field, similar to Asset Shelf */
    search_row_ptr->prop(popup_data->op->ptr,
                    "glyph_search",
                    /* Force the button to be active in a semi-modal state. */
                    ITEM_R_TEXT_BUT_FORCE_SEMI_MODAL_ACTIVE,
                    "",
                    ICON_VIEWZOOM);
  }

  /* Grid view below search - with scroll support */
  Layout &grid_col = right_col.column(false);
  /* Enable vertical scroll for the grid */
  grid_col.ui_units_x_set(GLYPH_POPUP_RIGHT_COL_WIDTH);
  grid_col.fixed_size_set(true);

  /* Create scrollable container for grid */
  Layout &scroll_col = grid_col.column(false);

  /* Create Grid View */
  std::unique_ptr<GlyphGridView> grid_view_ptr = std::make_unique<GlyphGridView>();
  GlyphGridView *grid_view = grid_view_ptr.get();

  grid_view->set_glyphs(popup_data->glyphs);
  /* Don't set category filter for now - categories will be handled by Python API */
  grid_view->set_search_filter(popup_data->search_string);

  /* Set tile size - larger for better glyph visibility */
  grid_view->set_tile_size(UI_UNIT_X * 2, UI_UNIT_Y * 2);

  /* Set the selection callback */
  grid_view->set_on_glyph_select_fn(
      [popup_data](bContext &C, const std::string &unicode) {
        /* Convert unicode to hex codepoint */
        char hex_code[16] = "";
        utf8_to_hex_codepoint(unicode.c_str(), hex_code, sizeof(hex_code));

        /* Set the glyph property of the picker operator if available */
        if (popup_data->op) {
          RNA_string_set(popup_data->op->ptr, "glyph", hex_code);
        }

        /* Get target property - from picker operator or use default "glyph" */
        char target_prop[256] = "glyph";  /* Default to "glyph" property */
        if (popup_data->op) {
          RNA_string_get(popup_data->op->ptr, "target_property", target_prop);
        }

        if (target_prop[0] != '\0') {
          const char *target_prop_path = target_prop;
          if (STRPREFIX(target_prop_path, "window_manager.")) {
            target_prop_path += strlen("window_manager.");
          }

          PointerRNA target_ptr;
          PropertyRNA *prop;
          int index;

          /* Use the directly stored target operator from popup data */
          wmOperator *target_op = popup_data->target_op;

          if (!target_op && popup_data->target_op_properties) {
            wmWindowManager *wm = CTX_wm_manager(&C);
            if (wm) {
              for (wmOperator *op_iter = static_cast<wmOperator *>(wm->runtime->operators.last);
                   op_iter;
                   op_iter = op_iter->prev)
              {
                if (op_iter && op_iter->properties == popup_data->target_op_properties) {
                  target_op = op_iter;
                  break;
                }
              }
            }
          }

          /* Fallback: if no target_op stored, try to find it through context */
          if (!target_op) {
            if (!popup_data->target_op_properties) {
              target_op = context_active_operator_get(&C);
            }
          }

          bool resolved = false;
          const bool is_active_operator_path = STRPREFIX(target_prop_path, "active_operator.");
          const char *target_prop_path_for_operator = is_active_operator_path ?
                                                          (target_prop_path + strlen("active_operator.")) :
                                                          target_prop_path;

          /* Primary path for invoke_props_dialog operators: write directly to the captured
           * operator IDProperty group. This avoids routing through unrelated active operators. */
          if (!resolved && popup_data->target_op_properties) {
            if (strchr(target_prop_path_for_operator, '.') == nullptr &&
                strchr(target_prop_path_for_operator, '[') == nullptr)
            {
              IDProperty *idprop = IDP_GetPropertyFromGroup(popup_data->target_op_properties,
                                                            target_prop_path_for_operator);
              if (!idprop) {
                idprop = IDP_NewString(hex_code, target_prop_path_for_operator);
                IDP_AddToGroup(popup_data->target_op_properties, idprop);
                resolved = true;
              }
              else if (idprop->type == IDP_STRING) {
                IDP_AssignString(idprop, hex_code);
                resolved = true;
              }

              /* After writing via IDProperty, find the owning operator and call
               * RNA_property_update so that Python's check() is triggered and the
               * props-dialog redraws (updating the glyph preview). */
              if (resolved) {
                wmOperator *owner_op = target_op;
                if (!owner_op) {
                  wmWindowManager *wm_find = CTX_wm_manager(&C);
                  if (wm_find) {
                    for (wmOperator *op_iter =
                             static_cast<wmOperator *>(wm_find->runtime->operators.last);
                         op_iter;
                         op_iter = op_iter->prev)
                    {
                      if (op_iter && op_iter->properties == popup_data->target_op_properties) {
                        owner_op = op_iter;
                        break;
                      }
                    }
                  }
                }
                if (owner_op && owner_op->ptr) {
                  PropertyRNA *glyph_rna_prop = RNA_struct_find_property(
                      owner_op->ptr, target_prop_path_for_operator);
                  if (glyph_rna_prop) {
                    RNA_property_update(&C, owner_op->ptr, glyph_rna_prop);
                  }
                }
              }
            }
          }

          if (!resolved && target_op && target_op->ptr) {
            if (RNA_path_resolve_full(
                    target_op->ptr, target_prop_path_for_operator, &target_ptr, &prop, &index))
            {
              RNA_property_string_set(&target_ptr, prop, hex_code);
              RNA_property_update(&C, &target_ptr, prop);
              resolved = true;
            }
          }

          if (STRPREFIX(target_prop_path, "active_operator.") && !resolved) {
            const char *active_op_path = target_prop_path + strlen("active_operator.");
            wmOperator *active_op = context_active_operator_get(&C);
            if (active_op && active_op->ptr) {
              if (RNA_path_resolve_full(active_op->ptr, active_op_path, &target_ptr, &prop, &index))
              {
                RNA_property_string_set(&target_ptr, prop, hex_code);
                RNA_property_update(&C, &target_ptr, prop);
                resolved = true;
              }
            }
          }
          if (!resolved) {
            /* Try resolving from window manager as root. */
            wmWindowManager *wm = CTX_wm_manager(&C);
            PointerRNA root_ptr = RNA_id_pointer_create(&wm->id);
            if (RNA_path_resolve_full(&root_ptr, target_prop_path, &target_ptr, &prop, &index)) {
              RNA_property_string_set(&target_ptr, prop, hex_code);
              RNA_property_update(&C, &target_ptr, prop);
              resolved = true;
            }
          }

          if (!resolved) {
            /* Fallback: try resolving from the picker operator itself (relative path). */
            if (popup_data->op) {
              if (RNA_path_resolve_full(popup_data->op->ptr, target_prop_path, &target_ptr, &prop, &index)) {
                RNA_property_string_set(&target_ptr, prop, hex_code);
                RNA_property_update(&C, &target_ptr, prop);
                resolved = true;
              }
            }
          }
        }

        /* Clear the search field if picker operator available */
        if (popup_data->op) {
          RNA_string_set(popup_data->op->ptr, "glyph_search", "");

          /* Trigger live update to refresh preview - only for category tab edit dialog */
          if (popup_data->op->idname && STREQ(popup_data->op->idname, "SCREEN_OT_category_tab_edit_dialog")) {
            category_tab_edit_live_update_cb(&C, popup_data->op, 0);
          }
        }

        /* Close the popup */
        if (popup_data->popup_handle) {
          popup_data->popup_handle->menuretval = RETURN_OK;
        }

        /* Trigger redraw */
        WM_main_add_notifier(NC_WINDOW, nullptr);
      });

  /* Add grid view to block and build it */
  AbstractGridView *grid = block_add_view(*block, "glyph_grid", std::move(grid_view_ptr));

  /* Build the grid view */
  GridViewBuilder builder(*block);
  builder.build_grid_view(*C, *grid, scroll_col);
  
  /* Add a large spacer to ensure minimum height */
  scroll_col.separator_spacer();
  
  return block;
}

/**
 * Free function for glyph grid popup data.
 */
static void glyph_grid_popup_free(void *arg)
{
  GlyphGridPopupData *popup_data = static_cast<GlyphGridPopupData *>(arg);
  delete popup_data;
}

/**
 * Callback for the "More glyphs" button.
 * Opens the Grid View popup for selecting glyphs with category tree and search.
 *
 * Note: arg1 is the target operator (e.g., SCREEN_OT_category_tab_edit_dialog or wm.category_tag_create)
 * whose property should be updated when a glyph is selected.
 */
void glyph_more_glyphs_button_cb(bContext *C, void *arg1, void * /*arg2*/)
{
  wmOperator *target_op = static_cast<wmOperator *>(arg1);

  if (!target_op) {
    return;
  }

  /* Get the current category from target_op->ptr */
  char current_category[64] = "";
  RNA_string_get(target_op->ptr, "category", current_category);

  /* Predefined glyph categories (will be expanded in the future) */
  Vector<std::string> all_categories;
  all_categories.append("ALL"); /* Default category - all glyphs */
  /* Future categories will be added here:
   * all_categories.append("Actions");
   * all_categories.append("Activities");
   * all_categories.append("Android");
   * all_categories.append("Audio & Video");
   * all_categories.append("Business");
   * all_categories.append("Communicate");
   * all_categories.append("Hardware");
   * all_categories.append("Home");
   * all_categories.append("Household");
   * all_categories.append("Images");
   * all_categories.append("Maps");
   * all_categories.append("Others");
   * all_categories.append("Privacy");
   * all_categories.append("Social");
   * all_categories.append("Text");
   * all_categories.append("Transit");
   * all_categories.append("Travel");
   * all_categories.append("UI actions");
   */

  /* Get glyphs from cache (loads from Python on first call) */
  const auto &cached_glyphs = glyph_cache_get(C);

  if (cached_glyphs.is_empty()) {
    /* No glyphs found, show a message */
    WM_global_report(RPT_WARNING, "No glyphs found");
    return;
  }

  /* Create a copy of glyphs for popup data (popup needs ownership) */
  blender::Vector<std::pair<std::string, std::string>> glyphs = cached_glyphs;

  /* Direct popup path (without WM_OT_glyph_picker_grid):
   * use the dialog operator as `op` so glyph_search/glyph updates and live preview
   * callbacks are applied immediately in the open edit dialog. */
  GlyphGridPopupData *popup_data = new GlyphGridPopupData(
      target_op,
      target_op,
      target_op ? target_op->properties : nullptr,
      std::move(glyphs),
      current_category,
      all_categories);

  /* Create and show popup */
  PopupBlockHandle *handle = popup_block_create(
      C, nullptr, nullptr, glyph_grid_popup_block_create, nullptr, popup_data, glyph_grid_popup_free, false);

  /* Store handle for closing popup from callback */
  popup_data->popup_handle = handle;

  /* Make it a popup */
  handle->popup = true;

  /* Add handlers */
  wmWindow *window = CTX_wm_window(C);
  popup_handlers_add(C, &window->runtime->modalhandlers, handle, 0);
  WM_event_add_mousemove(window);
}

/**
 * Operator to open the glyph picker grid from Python or other places.
 */
static wmOperatorStatus glyph_picker_grid_invoke(bContext *C,
                                                  wmOperator *op,
                                                  const wmEvent * /*event*/)
{
  /* Rebuild the glyph cache each time the picker opens: the Python glyph registry can change
   * between opens (script reload, add-on (un)install, language change), so a process-lifetime
   * cache would otherwise serve stale glyphs/names. The cache still avoids re-querying Python
   * across the many redraws and the "more glyphs" button within a single picker session. */
  glyph_cache_clear();

  /* Predefined glyph categories */
  Vector<std::string> all_categories;
  all_categories.append("ALL");

  /* Get glyphs from cache */
  const auto &cached_glyphs = glyph_cache_get(C);
  if (cached_glyphs.is_empty()) {
    WM_global_report(RPT_WARNING, "No glyphs found");
    return OPERATOR_CANCELLED;
  }

  /* Create a copy of glyphs for popup data */
  blender::Vector<std::pair<std::string, std::string>> glyphs = cached_glyphs;

  /* Get initial category and search string from operator properties if provided */
  char initial_category[64] = "";
  RNA_string_get(op->ptr, "category", initial_category);

  /* Try to get the target operator from target_operator_properties_ptr property first.
   * This is set by glyph_more_glyphs_default_cb before invoking this operator. */
  wmOperator *target_op = nullptr;
  IDProperty *target_op_properties = nullptr;
  bool has_explicit_target = false;
  char target_op_props_ptr_str[64] = "";
  RNA_string_get(op->ptr, "target_operator_properties_ptr", target_op_props_ptr_str);

  if (target_op_props_ptr_str[0] != '\0') {
    has_explicit_target = true;
    const uintptr_t target_props_ptr = uintptr_t(strtoull(target_op_props_ptr_str, nullptr, 10));

    if (target_props_ptr != 0) {
      wmWindowManager *wm = CTX_wm_manager(C);
      target_op_properties = reinterpret_cast<IDProperty *>(target_props_ptr);
      for (wmOperator *op_iter = static_cast<wmOperator *>(wm->runtime->operators.last);
           op_iter;
           op_iter = op_iter->prev)
      {
        if (op_iter && op_iter->properties == target_op_properties) {
          target_op = op_iter;
          break;
        }
      }
    }
  }

  /* Try to get the target operator from target_operator_ptr property next.
   * This is set by glyph_more_glyphs_default_cb before invoking this operator. */
  char target_op_ptr_str[64] = "";
  RNA_string_get(op->ptr, "target_operator_ptr", target_op_ptr_str);

  if (!target_op && target_op_ptr_str[0] != '\0') {
    has_explicit_target = true;
    /* Try to find the operator by pointer in wm->runtime->operators */
    const uintptr_t target_op_ptr = uintptr_t(strtoull(target_op_ptr_str, nullptr, 10));

    if (target_op_ptr != 0) {
      wmWindowManager *wm = CTX_wm_manager(C);
      for (wmOperator *op_iter = static_cast<wmOperator *>(wm->runtime->operators.last);
           op_iter;
           op_iter = op_iter->prev)
      {
        if ((uintptr_t)op_iter == target_op_ptr) {
          target_op = op_iter;
          break;
        }
      }
    }
  }

  /* Fallback: try context_active_operator_get only when no explicit target was provided.
   * If explicit target data was provided but couldn't be resolved to a live operator,
   * keep target_op null and rely on target_op_properties in the select callback. */
  if (!target_op) {
    if (!has_explicit_target) {
      wmOperator *active_op = context_active_operator_get(C);
      if (active_op && active_op != op) {
        target_op = active_op;
      }
    }
  }

  /* Create popup data with both picker op and target op */
  GlyphGridPopupData *popup_data = new GlyphGridPopupData(
      op, target_op, target_op_properties, std::move(glyphs), initial_category, all_categories);

  /* Create and show popup */
  PopupBlockHandle *handle = popup_block_create(
      C, nullptr, nullptr, glyph_grid_popup_block_create, nullptr, popup_data, glyph_grid_popup_free, false);

  popup_data->popup_handle = handle;
  handle->popup = true;

  /* Add handlers */
  wmWindow *window = CTX_wm_window(C);
  popup_handlers_add(C, &window->runtime->modalhandlers, handle, 0);
  WM_event_add_mousemove(window);

  return OPERATOR_RUNNING_MODAL;
}

void WM_OT_glyph_picker_grid(wmOperatorType *ot)
{
  ot->name = "Glyph Picker";
  ot->idname = "WM_OT_glyph_picker_grid";
  ot->description = "Open a grid-based glyph picker popup";

  ot->invoke = glyph_picker_grid_invoke;
  ot->poll = ED_operator_regionactive;

  /* Properties */
   RNA_def_string(ot->srna, "category", nullptr, 64, "Category", "Initial category to show");
   RNA_def_string(ot->srna, "glyph", nullptr, 16, "Glyph", "Selected glyph hex code (output)");
   RNA_def_string(ot->srna, "glyph_search", nullptr, 64, "Search", "Search string");
   RNA_def_string(ot->srna, "target_property", nullptr, 256, "Target Property", "RNA path to property that will receive the glyph hex code");
   RNA_def_string(ot->srna, "target_operator_ptr", nullptr, 64, "Target Operator Pointer", "Internal: pointer to target operator properties");
   RNA_def_string(ot->srna, "target_operator_properties_ptr", nullptr, 64, "Target Operator Properties Pointer", "Internal: pointer to target operator properties data");
 }

/** \} */

/** \} */

/* -------------------------------------------------------------------- */
/** \name Icon Grid Popup (Builtin Icons MVP)
 * \{ */

struct IconGridPopupItem {
  std::string identifier;
  std::string name;
  int icon;
};

struct IconGridPopupData {
  wmOperator *op; /* Dialog operator (SCREEN_OT_category_tab_edit_dialog). */
  wmOperator *target_op;
  IDProperty *target_op_properties;
  Vector<IconGridPopupItem> icons;
  std::string search_string;
  PopupBlockHandle *popup_handle;

  IconGridPopupData(wmOperator *op_,
                    wmOperator *target_op_,
                    IDProperty *target_op_properties_,
                    Vector<IconGridPopupItem> icons_)
      : op(op_),
        target_op(target_op_),
        target_op_properties(target_op_properties_),
        icons(std::move(icons_)),
        search_string(""),
        popup_handle(nullptr)
  {
  }
};

class IconGridView : public AbstractGridView {
 public:
  using OnIconSelectFn = std::function<void(bContext &C, const IconGridPopupItem &item)>;

 private:
  Vector<IconGridPopupItem> icons_;
  OnIconSelectFn on_icon_select_fn_;
  std::string search_filter_;

 public:
  void set_icons(const Vector<IconGridPopupItem> &icons)
  {
    icons_ = icons;
  }

  void set_search_filter(StringRef search_filter)
  {
    search_filter_ = search_filter;
  }

  void set_on_icon_select_fn(OnIconSelectFn fn)
  {
    on_icon_select_fn_ = fn;
  }

 protected:
  void build_items() override
  {
    std::string search_lower = search_filter_;
    if (!search_lower.empty()) {
      std::transform(search_lower.begin(), search_lower.end(), search_lower.begin(), ::tolower);
    }

    for (int64_t i = 0; i < icons_.size(); i++) {
      const IconGridPopupItem &icon_item = icons_[i];

      if (!search_lower.empty()) {
        std::string identifier_lower = icon_item.identifier;
        std::transform(
            identifier_lower.begin(), identifier_lower.end(), identifier_lower.begin(), ::tolower);

        std::string name_lower = icon_item.name;
        std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);

        if (identifier_lower.find(search_lower) == std::string::npos &&
            name_lower.find(search_lower) == std::string::npos)
        {
          continue;
        }
      }

      std::string view_id = "icon_" + std::to_string(i);
      PreviewGridItem &item = this->add_item<PreviewGridItem>(view_id,
                                                              icon_item.identifier,
                                                              BIFIconID(icon_item.icon));
      item.hide_label();

      if (on_icon_select_fn_) {
        item.set_on_activate_fn([this, i](bContext &C, PreviewGridItem & /*new_active*/) {
          on_icon_select_fn_(C, icons_[i]);
        });
      }
    }
  }
};

static Vector<IconGridPopupItem> icon_grid_builtin_icons_collect()
{
  Vector<IconGridPopupItem> items;

  for (const EnumPropertyItem *item = rna_enum_icon_items; item->identifier != nullptr; item++) {
    if (item->identifier[0] == '\0') {
      continue; /* Separator row. */
    }

    IconGridPopupItem icon_item;
    icon_item.identifier = item->identifier;
    icon_item.name = item->name ? item->name : item->identifier;
    icon_item.icon = item->value;
    items.append(std::move(icon_item));
  }

  return items;
}

static bool icon_grid_writeback_icon_key(bContext &C,
                                         IconGridPopupData *popup_data,
                                         const char *icon_key)
{
  if (!popup_data || !icon_key || icon_key[0] == '\0') {
    return false;
  }

  bool resolved = false;

  /* First, try to resolve via target_property (RNA path) - same approach as glyph picker.
   * This allows writing directly to any RNA property without needing a target operator. */
  char target_property[256] = "";
  if (popup_data->op && popup_data->op->ptr) {
    RNA_string_get(popup_data->op->ptr, "target_property", target_property);
  }

  if (target_property[0] != '\0') {
    /* Try resolving from window manager as root (like glyph picker does) */
    wmWindowManager *wm = CTX_wm_manager(&C);
    if (wm) {
      PointerRNA root_ptr = RNA_id_pointer_create(&wm->id);
      PointerRNA target_ptr;
      PropertyRNA *target_prop;
      int index;
      if (RNA_path_resolve_full(&root_ptr, target_property, &target_ptr, &target_prop, &index)) {
        RNA_property_string_set(&target_ptr, target_prop, icon_key);
        RNA_property_update(&C, &target_ptr, target_prop);
        resolved = true;
      }
    }

    /* Fallback: try resolving from picker operator itself (relative path) */
    if (!resolved && popup_data->op && popup_data->op->ptr) {
      PointerRNA target_ptr;
      PropertyRNA *target_prop;
      int index;
      if (RNA_path_resolve_full(popup_data->op->ptr, target_property, &target_ptr, &target_prop, &index)) {
        RNA_property_string_set(&target_ptr, target_prop, icon_key);
        RNA_property_update(&C, &target_ptr, target_prop);
        resolved = true;
      }
    }
  }

  if (popup_data->op && popup_data->op->ptr) {
    RNA_string_set(popup_data->op->ptr, "icon_key", icon_key);
    PointerRNA target_ptr;
    PropertyRNA *target_prop;
    int index;
    if (RNA_path_resolve_full(popup_data->op->ptr, "icon_key", &target_ptr, &target_prop, &index)) {
      RNA_property_update(&C, &target_ptr, target_prop);
    }
    resolved = true;
  }

  if (popup_data->target_op_properties) {
    IDProperty *idprop = IDP_GetPropertyFromGroup(popup_data->target_op_properties, "icon_key");
    if (!idprop) {
      idprop = IDP_NewString(icon_key, "icon_key");
      IDP_AddToGroup(popup_data->target_op_properties, idprop);
      resolved = true;
    }
    else if (idprop->type == IDP_STRING) {
      IDP_AssignString(idprop, icon_key);
      resolved = true;
    }
  }

  /* CRITICAL FIX: Always try RNA update for target_op to trigger Python callbacks.
   * IDProperty update alone doesn't trigger RNA property update callbacks. */
  if (popup_data->target_op && popup_data->target_op->ptr) {
    PointerRNA target_ptr;
    PropertyRNA *target_prop;
    int index;
    if (RNA_path_resolve_full(popup_data->target_op->ptr, "icon_key", &target_ptr, &target_prop, &index)) {
      RNA_property_string_set(&target_ptr, target_prop, icon_key);
      RNA_property_update(&C, &target_ptr, target_prop);
      resolved = true;
    }
  }

  if (!resolved && popup_data->op && popup_data->op->ptr) {
    RNA_string_set(popup_data->op->ptr, "icon_key", icon_key);
    resolved = true;
  }

  return resolved;
}

static Block *icon_grid_popup_block_create(bContext *C, ARegion *region, void *arg)
{
  IconGridPopupData *popup_data = static_cast<IconGridPopupData *>(arg);

  if (popup_data->op) {
    char search_buf[256] = "";
    RNA_string_get(popup_data->op->ptr, "icon_search", search_buf);
    popup_data->search_string = search_buf;
  }

  Block *block = block_begin(C, region, "icon_grid_popup", EmbossType::Emboss);
  block_flag_enable(block, BLOCK_LOOP | BLOCK_MOVEMOUSE_QUIT);
  block_theme_style_set(block, BLOCK_THEME_STYLE_POPUP);
  block_bounds_set_centered(block, 6 * UI_SCALE_FAC);

  const int popup_width = 46 * UI_UNIT_X;
  const int popup_height = 60 * UI_UNIT_Y;

  Layout &layout = block_layout(block,
                                LayoutDirection::Vertical,
                                LayoutType::Panel,
                                0,
                                0,
                                popup_width,
                                popup_height,
                                0,
                                style_get());

  if (popup_data->op) {
    Layout &search_row = layout.row(false);
    search_row.prop(popup_data->op->ptr,
                    "icon_search",
                    ITEM_R_TEXT_BUT_FORCE_SEMI_MODAL_ACTIVE,
                    "",
                    ICON_VIEWZOOM);
  }

  Layout &grid_col = layout.column(false);
  grid_col.ui_units_x_set(45);
  grid_col.fixed_size_set(true);

  std::unique_ptr<IconGridView> grid_view_ptr = std::make_unique<IconGridView>();
  IconGridView *grid_view = grid_view_ptr.get();
  grid_view->set_icons(popup_data->icons);
  grid_view->set_search_filter(popup_data->search_string);
  grid_view->set_tile_size(UI_UNIT_X * 1.8f, UI_UNIT_Y * 1.8f);

  grid_view->set_on_icon_select_fn([popup_data](bContext &C, const IconGridPopupItem &item) {
    if (popup_data->op) {
      category_tab_set_string_if_supported(
          popup_data->op->ptr, "icon_key", item.identifier.c_str(), &C);
      category_tab_set_int_or_enum_if_supported(popup_data->op->ptr, "icon_source", 1, &C);
      category_tab_set_int_or_enum_if_supported(popup_data->op->ptr, "display_mode_ui", 1, &C);
      category_tab_set_int_or_enum_if_supported(popup_data->op->ptr, "custom_icon_mode_ui", 0, &C);
      category_tab_set_string_if_supported(popup_data->op->ptr, "icon_search", "", &C);
    }

    icon_grid_writeback_icon_key(C, popup_data, item.identifier.c_str());

    /* Call live update for Edit Category dialog */
    if (popup_data->op && popup_data->op->idname &&
        STREQ(popup_data->op->idname, "SCREEN_OT_category_tab_edit_dialog"))
    {
      category_tab_edit_live_update_cb(&C, popup_data->op, 0);
    }
    /* Also call live update for Create/Edit Tag Python operators via target_op */
    else if (popup_data->target_op && popup_data->target_op->idname &&
             (STREQ(popup_data->target_op->idname, "wm.category_tag_create") ||
              STREQ(popup_data->target_op->idname, "wm.category_tag_edit")))
    {
      if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
        printf("[ICON_GRID DEBUG] Icon selected for Python tag operator: '%s'\n", popup_data->target_op->idname);
        printf("[ICON_GRID DEBUG]   -> icon_key='%s'\n", item.identifier.c_str());
      }
      /* CRITICAL: Update target_op properties before calling live update callback.
       * This ensures template_icon_preview in Python sees the updated icon_key. */
      category_tab_set_string_if_supported(
          popup_data->target_op->ptr, "icon_key", item.identifier.c_str(), &C);
      category_tab_set_int_or_enum_if_supported(popup_data->target_op->ptr, "icon_source", 1, &C);
      category_tab_set_int_or_enum_if_supported(popup_data->target_op->ptr, "display_mode_ui", 1, &C);
      category_tab_set_int_or_enum_if_supported(popup_data->target_op->ptr, "custom_icon_mode_ui", 0, &C);

      /* For Python tag operators, call the C++ live update callback */
      tag_icon_live_update_cb(&C, popup_data->target_op, 0);
    }
    else {
      if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
        printf("[ICON_GRID DEBUG] Icon selected but target_op mismatch:\n");
        printf("[ICON_GRID DEBUG]   -> popup_data->target_op=%p\n", (void*)popup_data->target_op);
        if (popup_data->target_op && popup_data->target_op->idname) {
          printf("[ICON_GRID DEBUG]   -> idname='%s'\n", popup_data->target_op->idname);
        }
      }
    }

    if (popup_data->popup_handle) {
      popup_data->popup_handle->menuretval = RETURN_OK;
    }

    WM_main_add_notifier(NC_WINDOW, nullptr);
  });

  AbstractGridView *grid = block_add_view(*block, "icon_grid", std::move(grid_view_ptr));
  GridViewBuilder builder(*block);
  builder.build_grid_view(*C, *grid, grid_col);

  return block;
}

static void icon_grid_popup_free(void *arg)
{
  IconGridPopupData *popup_data = static_cast<IconGridPopupData *>(arg);
  delete popup_data;
}

void icon_more_icons_button_cb(bContext *C, void *arg1, void * /*arg2*/)
{
  wmOperator *target_op = static_cast<wmOperator *>(arg1);
  if (!target_op) {
    return;
  }

  Vector<IconGridPopupItem> icons = icon_grid_builtin_icons_collect();
  if (icons.is_empty()) {
    WM_global_report(RPT_WARNING, "No built-in icons found");
    return;
  }

  IconGridPopupData *popup_data = new IconGridPopupData(
      target_op, target_op, target_op->properties, std::move(icons));

  PopupBlockHandle *handle = popup_block_create(
      C, nullptr, nullptr, icon_grid_popup_block_create, nullptr, popup_data, icon_grid_popup_free, false);

  popup_data->popup_handle = handle;
  handle->popup = true;

  wmWindow *window = CTX_wm_window(C);
  popup_handlers_add(C, &window->runtime->modalhandlers, handle, 0);
  WM_event_add_mousemove(window);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Icon Picker Operator
 * \{ */

/**
 * Operator to open icon picker popup for selecting Blender icons.
 * Used by tag create/edit dialogs.
 */
static wmOperatorStatus category_tab_icon_picker_invoke(bContext *C, wmOperator *op, const wmEvent * /*event*/)
{
  wmOperator *target_op = nullptr;
  IDProperty *target_op_properties = nullptr;
  
  /* Try to get target operator from target_operator_ptr property (string pointer) */
  char target_op_ptr_str[64] = "";
  RNA_string_get(op->ptr, "target_operator_ptr", target_op_ptr_str);
  
  if (target_op_ptr_str[0] != '\0') {
    /* Parse pointer from hex string */
    const uintptr_t target_ptr = uintptr_t(strtoull(target_op_ptr_str, nullptr, 10));
    if (target_ptr != 0) {
      wmWindowManager *wm = CTX_wm_manager(C);
      for (wmOperator *op_iter = static_cast<wmOperator *>(wm->runtime->operators.last); op_iter;
           op_iter = op_iter->prev)
      {
        if ((uintptr_t)op_iter == target_ptr) {
          target_op = op_iter;
          target_op_properties = op_iter->properties;
          break;
        }
      }
    }
  }
  
  /* Fallback: try to get from pointer property (for C++ calls) */
  if (!target_op) {
    PointerRNA target_op_ptr = RNA_pointer_get(op->ptr, "target_operator");
    target_op = static_cast<wmOperator *>(target_op_ptr.data);
    if (target_op) {
      target_op_properties = target_op->properties;
    }
  }
  
  /* Fallback: use active operator from context (the operator whose draw() method called this) */
  if (!target_op) {
    ARegion *region_ctx = CTX_wm_region(C);

    /* Debug: scan all regions and blocks to understand what's happening */
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[ICON_PICKER DEBUG] CTX_wm_region=%p, regiontype=%d\n", (void*)region_ctx, region_ctx ? region_ctx->regiontype : -1);

      if (region_ctx) {
        printf("[ICON_PICKER DEBUG] Scanning region_ctx blocks:\n");
        for (Block &block : region_ctx->runtime->uiblocks) {
          printf("[ICON_PICKER DEBUG]   block='%s', ui_operator=%p\n",
                 block.name.c_str(), (void*)block.ui_operator);
          if (block.ui_operator) {
            printf("[ICON_PICKER DEBUG]     -> idname='%s'\n", block.ui_operator->idname);
          }
        }
      }

      bScreen *screen = CTX_wm_screen(C);
      if (screen) {
        printf("[ICON_PICKER DEBUG] Scanning popup regions in screen->regionbase:\n");
        for (ARegion &region : screen->regionbase) {
          if (&region == region_ctx) continue;
          printf("[ICON_PICKER DEBUG]   popup region regiontype=%d\n", region.regiontype);
          for (Block &block : region.runtime->uiblocks) {
            printf("[ICON_PICKER DEBUG]     block='%s', ui_operator=%p\n",
                   block.name.c_str(), (void*)block.ui_operator);
            if (block.ui_operator) {
              printf("[ICON_PICKER DEBUG]       -> idname='%s'\n", block.ui_operator->idname);
            }
          }
        }
      }
    }

    /* PRIORITY: First scan popup regions for Python tag operators.
     * This is critical because context_active_operator_get scans the current region first
     * and finds the parent C++ dialog instead of the Python tag operator popup. */
    bScreen *screen = CTX_wm_screen(C);
    if (screen) {
      for (ARegion &region : screen->regionbase) {
        if (&region == region_ctx) continue;
        for (Block &block : region.runtime->uiblocks) {
          if (block.ui_operator && block.ui_operator->idname[0] != '\0') {
            const char *idname = block.ui_operator->idname;
            /* Look specifically for Python tag operators */
            if (STREQ(idname, "WM_OT_category_tag_create") ||
                STREQ(idname, "WM_OT_category_tag_edit"))
            {
              target_op = block.ui_operator;
              target_op_properties = target_op->properties;
              if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
                printf("[ICON_PICKER DEBUG] Found Python tag operator in popup region: '%s'\n", idname);
              }
              break;
            }
          }
        }
        if (target_op) break;
      }
    }

    /* Fallback: use context_active_operator_get */
    if (!target_op) {
      target_op = context_active_operator_get(C);
      if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
        printf("[ICON_PICKER DEBUG] context_active_operator_get returned: %p\n", (void*)target_op);
        if (target_op) {
          printf("[ICON_PICKER DEBUG]   -> idname='%s'\n", target_op->idname);
        }
      }
    }
    /* Don't use the picker operator itself as target */
    if (target_op == op) {
      target_op = nullptr;
    }
    else if (target_op != nullptr) {
      target_op_properties = target_op->properties;
    }
  }

  /* Check if we have a target_property (RNA path) - this allows direct property write
   * without needing a target operator, similar to how glyph picker works. */
  char target_property[256] = "";
  RNA_string_get(op->ptr, "target_property", target_property);

  /* If we have target_property but no target_op, we can still work by writing directly
   * to the RNA path when icon is selected. Create popup_data with nullptr target_op. */
  if (!target_op && target_property[0] == '\0') {
    WM_global_report(RPT_ERROR, "No target operator or target_property specified");
    return OPERATOR_CANCELLED;
  }

  Vector<IconGridPopupItem> icons = icon_grid_builtin_icons_collect();
  if (icons.is_empty()) {
    WM_global_report(RPT_WARNING, "No built-in icons found");
    return OPERATOR_CANCELLED;
  }

  IconGridPopupData *popup_data = new IconGridPopupData(
      op, target_op, target_op_properties, std::move(icons));

  PopupBlockHandle *handle = popup_block_create(
      C, nullptr, nullptr, icon_grid_popup_block_create, nullptr, popup_data, icon_grid_popup_free, false);

  popup_data->popup_handle = handle;
  handle->popup = true;

  wmWindow *window = CTX_wm_window(C);
  popup_handlers_add(C, &window->runtime->modalhandlers, handle, 0);
  WM_event_add_mousemove(window);

  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus category_tab_icon_picker_exec(bContext * /*C*/, wmOperator * /*op*/)
{
  /* Should not be called - this operator only works via invoke */
  return OPERATOR_CANCELLED;
}

static void category_tab_icon_picker_cancel(bContext * /*C*/, wmOperator * /*op*/)
{
  /* Cleanup handled by popup_free callback */
}

void SCREEN_OT_category_tab_icon_picker(wmOperatorType *ot)
{
  ot->name = "Category Tab Icon Picker";
  ot->idname = "SCREEN_OT_category_tab_icon_picker";
  ot->description = "Open icon picker popup to select a Blender icon";

  ot->invoke = category_tab_icon_picker_invoke;
  ot->exec = category_tab_icon_picker_exec;
  ot->cancel = category_tab_icon_picker_cancel;

  /* Target operator pointer (for Python calls) */
  RNA_def_string(
      ot->srna, "target_operator_ptr", nullptr, 64, "Target Operator Pointer",
      "Internal: pointer to target operator (hex string)");
  /* Target property RNA path (alternative to target_operator) */
  RNA_def_string(ot->srna, "target_property", nullptr, 256, "Target Property",
                 "RNA path to property that will receive the selected icon key");
  
  /* Icon picker state properties - needed for live update and preview */
  RNA_def_string(ot->srna, "icon_key", nullptr, 128, "Icon Key",
                 "Selected Blender icon identifier");
  RNA_def_int(ot->srna, "icon_source", 0, 0, 2, "Icon Source",
              "Source of the icon (0=auto, 1=manual, 2=off)", 0, 2);
  RNA_def_int(ot->srna, "display_mode_ui", 0, 0, 2, "Display Mode",
              "Display mode (0=GLYPH, 1=ICON)", 0, 2);
  RNA_def_int(ot->srna, "custom_icon_mode_ui", 0, 0, 1, "Custom Icon Mode",
              "Custom icon mode (0=blender icon, 1=custom path)", 0, 1);
  RNA_def_string(ot->srna, "icon_search", nullptr, 256, "Icon Search",
                 "Search string for filtering icons in picker");
}

/** \} */

}  // namespace blender::ui
