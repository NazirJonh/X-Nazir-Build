/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Implementation of the Category Tabs C++ -> Python write-path bridge.
 * See interface_category_py_bridge.hh for the rationale and the Python API surface.
 *
 * Every wrapper below calls into `bl_ui.glyph_tag_system.api` through
 * #BPY_run_module_func and friends: arguments are converted to Python objects through the
 * C-API (#BPy_CallArg), never interpolated into Python source text. This makes string
 * escaping unnecessary for the arbitrary user strings passed through (category/tag names,
 * paths, hex colors): there is no source text for them to break out of.
 */

#include <string>

#include "MEM_guardedalloc.h"

#include "BLI_utildefines.h"

#include "BKE_context.hh"

#include "DNA_windowmanager_types.h"

#include "WM_api.hh"

#include "interface_category_py_bridge.hh"

#ifdef WITH_PYTHON
#  include "BPY_extern_run.hh"
#endif

namespace blender::ui {

#ifdef WITH_PYTHON
static const char *api_module = "bl_ui.glyph_tag_system.api";
/* `BPy_CallArg` argument lists below are plain stack arrays; #BPY_run_module_func and friends
 * take a `Span`, which (unlike `std::array`) has no implicit constructor from a raw C array. */
#  define AS_SPAN(arr) Span<BPy_CallArg>(arr, ARRAY_SIZE(arr))
#endif

void category_py_reset_to_defaults(bContext *C, const int space_type)
{
#ifdef WITH_PYTHON
  wmWindowManager *wm = CTX_wm_manager(C);
  const std::string category = wm->category_tab_save_category;
  wm->category_tab_save_category[0] = '\0';
  if (category.empty()) {
    return;
  }

  const BPy_CallArg args[] = {
      {.type = BPy_CallArg::Type::STRING, .as_string = category.c_str()},
      {.keyword = "space_type", .type = BPy_CallArg::Type::INT, .as_int = space_type},
      {.keyword = "save", .type = BPy_CallArg::Type::BOOL, .as_bool = false},
  };
  BPY_run_module_func(C, api_module, "reset_category_to_defaults", AS_SPAN(args));
#else
  UNUSED_VARS(C, space_type);
#endif
}

void category_py_reset_tags(bContext *C, const int space_type)
{
#ifdef WITH_PYTHON
  wmWindowManager *wm = CTX_wm_manager(C);
  const std::string category = wm->category_tab_save_category;
  wm->category_tab_save_category[0] = '\0';
  if (category.empty()) {
    return;
  }

  const BPy_CallArg args[] = {
      {.type = BPy_CallArg::Type::STRING, .as_string = category.c_str()},
      {.type = BPy_CallArg::Type::STRING_LIST, .as_string_list = {}},
      {.keyword = "space_type", .type = BPy_CallArg::Type::INT, .as_int = space_type},
      {.keyword = "auto_save", .type = BPy_CallArg::Type::BOOL, .as_bool = false},
  };
  BPY_run_module_func(C, api_module, "set_category_tags", AS_SPAN(args));
#else
  UNUSED_VARS(C, space_type);
#endif
}

void category_py_save_glyph_mappings_to_file(bContext *C)
{
#ifdef WITH_PYTHON
  wmWindowManager *wm = CTX_wm_manager(C);
  const std::string category = wm->category_tab_save_category;
  wm->category_tab_save_category[0] = '\0';
  if (category.empty()) {
    return;
  }

  const BPy_CallArg args[] = {
      {.keyword = "force_discovery_skip", .type = BPy_CallArg::Type::BOOL, .as_bool = false},
  };
  BPY_run_module_func(C, api_module, "_save_glyph_mappings_to_file", AS_SPAN(args));
#else
  UNUSED_VARS(C);
#endif
}

void category_py_save_category_data(bContext *C,
                                    const char *category,
                                    const char *display_name,
                                    const char *first_letter,
                                    const char *glyph_hex,
                                    const char *color_hex,
                                    const char *icon_source,
                                    const char *icon_key,
                                    const char *icon_path,
                                    const char *icon_provider,
                                    const char *glyph_mode,
                                    const int space_type)
{
#ifdef WITH_PYTHON
  const BPy_CallArg set_data_args[] = {
      {.type = BPy_CallArg::Type::STRING, .as_string = category},
      {.keyword = "display_name", .type = BPy_CallArg::Type::STRING, .as_string = display_name},
      {.keyword = "first_letter", .type = BPy_CallArg::Type::STRING, .as_string = first_letter},
      {.keyword = "glyph", .type = BPy_CallArg::Type::STRING, .as_string = glyph_hex},
      {.keyword = "color", .type = BPy_CallArg::Type::STRING, .as_string = color_hex},
      {.keyword = "icon_source", .type = BPy_CallArg::Type::STRING, .as_string = icon_source},
      {.keyword = "icon_key", .type = BPy_CallArg::Type::STRING, .as_string = icon_key},
      {.keyword = "icon_path", .type = BPy_CallArg::Type::STRING, .as_string = icon_path},
      {.keyword = "icon_provider", .type = BPy_CallArg::Type::STRING, .as_string = icon_provider},
      {.keyword = "glyph_mode", .type = BPy_CallArg::Type::STRING, .as_string = glyph_mode},
      {.keyword = "space_type", .type = BPy_CallArg::Type::INT, .as_int = space_type},
      {.keyword = "skip_wm_sync", .type = BPy_CallArg::Type::BOOL, .as_bool = true},
  };
  BPY_run_module_func(C, api_module, "set_category_data", AS_SPAN(set_data_args));

  const BPy_CallArg finalize_args[] = {
      {.type = BPy_CallArg::Type::STRING, .as_string = category},
      {.keyword = "space_type", .type = BPy_CallArg::Type::INT, .as_int = space_type},
      {.keyword = "sync_wm", .type = BPy_CallArg::Type::BOOL, .as_bool = false},
  };
  BPY_run_module_func(C, api_module, "finalize_category_tag_changes", AS_SPAN(finalize_args));
#else
  UNUSED_VARS(C, category, display_name, first_letter, glyph_hex, color_hex,
              icon_source, icon_key, icon_path, icon_provider, glyph_mode, space_type);
#endif
}

void category_py_restore_on_cancel(bContext *C,
                                   const char *tags,
                                   const char *glyph_hex,
                                   const int glyph_mode,
                                   const float color[3],
                                   const int space_type,
                                   const int icon_source,
                                   const char *icon_key,
                                   const char *icon_path,
                                   const char *icon_provider)
{
#ifdef WITH_PYTHON
  wmWindowManager *wm = CTX_wm_manager(C);
  const std::string category = wm->category_tab_save_category;
  wm->category_tab_save_category[0] = '\0';
  if (category.empty()) {
    return;
  }

  const BPy_CallArg restore_tags_args[] = {
      {.type = BPy_CallArg::Type::STRING, .as_string = category.c_str()},
      {.type = BPy_CallArg::Type::STRING, .as_string = tags},
      {.keyword = "space_type", .type = BPy_CallArg::Type::INT, .as_int = space_type},
  };
  BPY_run_module_func(
      C, api_module, "restore_category_tags_from_string", AS_SPAN(restore_tags_args));

  const double color_d[3] = {double(color[0]), double(color[1]), double(color[2])};
  const BPy_CallArg restore_glyph_args[] = {
      {.type = BPy_CallArg::Type::STRING, .as_string = category.c_str()},
      {.type = BPy_CallArg::Type::STRING, .as_string = glyph_hex},
      {.type = BPy_CallArg::Type::INT, .as_int = glyph_mode},
      {.type = BPy_CallArg::Type::DOUBLE_LIST, .as_double_list = Span<double>(color_d, 3)},
      {.keyword = "space_type", .type = BPy_CallArg::Type::INT, .as_int = space_type},
      {.keyword = "icon_source", .type = BPy_CallArg::Type::INT, .as_int = icon_source},
      {.keyword = "icon_key", .type = BPy_CallArg::Type::STRING, .as_string = icon_key},
      {.keyword = "icon_path", .type = BPy_CallArg::Type::STRING, .as_string = icon_path},
      {.keyword = "icon_provider", .type = BPy_CallArg::Type::STRING, .as_string = icon_provider},
  };
  BPY_run_module_func(
      C, api_module, "restore_category_glyph_from_snapshot", AS_SPAN(restore_glyph_args));
#else
  UNUSED_VARS(C, tags, glyph_hex, glyph_mode, color, space_type, icon_source,
              icon_key, icon_path, icon_provider);
#endif
}

void category_py_update_tag_icon(bContext *C,
                                 const char *tag_name,
                                 const char *icon_key,
                                 const int icon_source)
{
#ifdef WITH_PYTHON
  const BPy_CallArg args[] = {
      {.keyword = "tag_name", .type = BPy_CallArg::Type::STRING, .as_string = tag_name},
      {.keyword = "icon_key", .type = BPy_CallArg::Type::STRING, .as_string = icon_key},
      {.keyword = "icon_source", .type = BPy_CallArg::Type::INT, .as_int = icon_source},
      {.keyword = "auto_save", .type = BPy_CallArg::Type::BOOL, .as_bool = true},
  };
  BPY_run_module_func(C, api_module, "update_tag", AS_SPAN(args));
#else
  UNUSED_VARS(C, tag_name, icon_key, icon_source);
#endif
}

void category_py_assign_tag(bContext *C,
                            const char *category,
                            const char *tag,
                            const int space_type)
{
#ifdef WITH_PYTHON
  const BPy_CallArg args[] = {
      {.type = BPy_CallArg::Type::STRING, .as_string = category},
      {.type = BPy_CallArg::Type::STRING, .as_string = tag},
      {.type = BPy_CallArg::Type::INT, .as_int = space_type},
  };
  BPY_run_module_func(C, api_module, "assign_tag_to_category", AS_SPAN(args));
#else
  UNUSED_VARS(C, category, tag, space_type);
#endif
}

bool category_py_mark_all_unassigned_without_tag(bContext *C,
                                                 const int space_type,
                                                 const uint32_t mode_flag)
{
#ifdef WITH_PYTHON
  const BPy_CallArg args[] = {
      {.type = BPy_CallArg::Type::INT, .as_int = space_type},
      {.type = BPy_CallArg::Type::INT, .as_int = int64_t(mode_flag)},
  };
  intptr_t updated = 0;
  const bool success = BPY_run_module_func_as_intptr(C,
                                                     api_module,
                                                     "mark_all_unassigned_categories_as_without_tag",
                                                     AS_SPAN(args),
                                                     nullptr,
                                                     &updated);
  if (success) {
    WM_global_reportf(
        RPT_INFO, "%d unassigned categories marked as \"Without Tag\"", int(updated));
  }
  return success;
#else
  UNUSED_VARS(C, space_type, mode_flag);
  return false;
#endif
}

void category_py_mark_from_extension(bContext *C,
                                     const char *category,
                                     const char *extension_id,
                                     const int space_type,
                                     const uint32_t mode_flag)
{
#ifdef WITH_PYTHON
  const BPy_CallArg args[] = {
      {.type = BPy_CallArg::Type::STRING, .as_string = category},
      {.type = BPy_CallArg::Type::STRING, .as_string = extension_id},
      {.type = BPy_CallArg::Type::INT, .as_int = space_type},
      {.type = BPy_CallArg::Type::INT, .as_int = int64_t(mode_flag)},
  };
  BPY_run_module_func(C, api_module, "mark_category_from_extension", AS_SPAN(args));
#else
  UNUSED_VARS(C, category, extension_id, space_type, mode_flag);
#endif
}

void category_py_set_category_order(bContext *C,
                                    const char *tag_key,
                                    const Vector<std::string> &order)
{
#ifdef WITH_PYTHON
  Vector<const char *> order_c;
  order_c.reserve(order.size());
  for (const std::string &item : order) {
    order_c.append(item.c_str());
  }

  const BPy_CallArg args[] = {
      {.type = BPy_CallArg::Type::STRING, .as_string = tag_key},
      {.type = BPy_CallArg::Type::STRING_LIST, .as_string_list = order_c},
  };
  BPY_run_module_func(C, api_module, "set_category_order", AS_SPAN(args));
#else
  UNUSED_VARS(C, tag_key, order);
#endif
}

void category_py_set_preview_mode(bContext *C, const bool active)
{
#ifdef WITH_PYTHON
  const BPy_CallArg args[] = {
      {.type = BPy_CallArg::Type::BOOL, .as_bool = active},
  };
  BPY_run_module_func(C, api_module, "set_preview_mode_active", AS_SPAN(args));
#else
  UNUSED_VARS(C, active);
#endif
}

/* -------------------------------------------------------------------- */
/* Read path */

int category_py_get_reserved_priority(bContext *C,
                                      const char *category_id,
                                      const char *space_type_name)
{
#ifdef WITH_PYTHON
  if (!space_type_name || space_type_name[0] == '\0') {
    space_type_name = "DEFAULT";
  }

  const BPy_CallArg args[] = {
      {.type = BPy_CallArg::Type::STRING, .as_string = category_id},
      {.type = BPy_CallArg::Type::STRING, .as_string = space_type_name},
  };
  intptr_t prio = -1;
  const bool success = BPY_run_module_func_as_intptr(
      C, api_module, "get_reserved_category_priority", AS_SPAN(args), nullptr, &prio);
  return success ? int(prio) : -1;
#else
  UNUSED_VARS(C, category_id, space_type_name);
  return -1;
#endif
}

std::string category_py_get_category_order_json(bContext *C, const char *tag_key)
{
#ifdef WITH_PYTHON
  const BPy_CallArg args[] = {
      {.type = BPy_CallArg::Type::STRING, .as_string = tag_key},
  };
  char *result_str = nullptr;
  const bool success = BPY_run_module_func_as_json(
      C, api_module, "get_category_order", AS_SPAN(args), nullptr, &result_str);
  if (!success || !result_str) {
    return "";
  }
  std::string out(result_str);
  MEM_delete(result_str);
  /* `get_category_order()` returns None for an unknown key; mirror the previous `or []`
   * fallback so callers keep seeing a JSON array. */
  if (out == "null") {
    out = "[]";
  }
  return out;
#else
  UNUSED_VARS(C, tag_key);
  return "";
#endif
}

std::string category_py_search_glyphs_json(bContext *C,
                                           const char *query,
                                           const char *category,
                                           const int max_results)
{
#ifdef WITH_PYTHON
  const BPy_CallArg args[] = {
      {.type = BPy_CallArg::Type::STRING, .as_string = query},
      {.type = BPy_CallArg::Type::STRING, .as_string = category ? category : ""},
      {.type = BPy_CallArg::Type::INT, .as_int = max_results},
  };
  char *result_str = nullptr;
  const bool success = BPY_run_module_func_as_json(
      C, api_module, "search_glyphs_summary", AS_SPAN(args), nullptr, &result_str);
  if (!success || !result_str) {
    return "";
  }
  std::string out(result_str);
  MEM_delete(result_str);
  return out;
#else
  UNUSED_VARS(C, query, category, max_results);
  return "";
#endif
}

std::string category_py_auto_detect_extension_icon_json(bContext *C, const char *category)
{
#ifdef WITH_PYTHON
  const BPy_CallArg args[] = {
      {.type = BPy_CallArg::Type::STRING, .as_string = category},
  };
  char *result_str = nullptr;
  const bool success = BPY_run_module_func_as_json(C,
                                                    api_module,
                                                    "auto_detect_extension_icon_path_normalized",
                                                    AS_SPAN(args),
                                                    nullptr,
                                                    &result_str);
  if (!success || !result_str) {
    return "";
  }
  std::string out(result_str);
  MEM_delete(result_str);
  return out;
#else
  UNUSED_VARS(C, category);
  return "";
#endif
}

#ifdef WITH_PYTHON
#  undef AS_SPAN
#endif

}  // namespace blender::ui
