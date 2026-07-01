/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Implementation of the Category Tabs C++ -> Python write-path bridge.
 * See interface_category_py_bridge.hh for the rationale and the Python API surface.
 */

#include <cstdlib>
#include <string>

#include "MEM_guardedalloc.h"

#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "interface_intern.hh"
#include "interface_category_py_bridge.hh"

#ifdef WITH_PYTHON
#  include "BPY_extern_run.hh"
#endif

namespace blender::ui {

void category_py_reset_to_defaults(bContext *C, const int space_type)
{
#ifdef WITH_PYTHON
  const char *imports[] = {"bpy", nullptr};
  char cmd[512];
  BLI_snprintf(cmd,
               sizeof(cmd),
               "from bl_ui.glyph_tag_system.api import reset_category_to_defaults\n"
               "import bpy\n"
               "wm = bpy.context.window_manager\n"
               "category = wm.category_tab_save_category\n"
               "wm.category_tab_save_category = ''\n"
               "if category:\n"
               "    reset_category_to_defaults(category, space_type=%d, save=False)\n",
               space_type);
  BPY_run_string_exec(C, imports, cmd);
#else
  UNUSED_VARS(C, space_type);
#endif
}

void category_py_reset_tags(bContext *C, const int space_type)
{
#ifdef WITH_PYTHON
  const char *imports[] = {"bpy", nullptr};
  char cmd[512];
  BLI_snprintf(cmd,
               sizeof(cmd),
               "from bl_ui.glyph_tag_system.api import set_category_tags\n"
               "import bpy\n"
               "wm = bpy.context.window_manager\n"
               "category = wm.category_tab_save_category\n"
               "wm.category_tab_save_category = ''\n"
               "if category:\n"
               "    set_category_tags(category, [], space_type=%d, auto_save=False)\n",
               space_type);
  BPY_run_string_exec(C, imports, cmd);
#else
  UNUSED_VARS(C, space_type);
#endif
}

void category_py_save_glyph_mappings_to_file(bContext *C)
{
#ifdef WITH_PYTHON
  const char *imports[] = {"bpy", nullptr};
  char cmd[1024];
  BLI_snprintf(cmd,
               sizeof(cmd),
               "from bl_ui.glyph_tag_system.api import _save_glyph_mappings_to_file\n"
               "import bpy\n"
               "wm = bpy.context.window_manager\n"
               "category = wm.category_tab_save_category\n"
               "wm.category_tab_save_category = ''\n"
               "if category:\n"
               "    _save_glyph_mappings_to_file(force_discovery_skip=False)\n");
  BPY_run_string_exec(C, imports, cmd);
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
  const std::string category_esc = category_tab_escape_for_python_literal(category);
  const std::string display_name_esc = category_tab_escape_for_python_literal(display_name);
  const std::string icon_key_esc = category_tab_escape_for_python_literal(icon_key);
  const std::string icon_path_esc = category_tab_escape_for_python_literal(icon_path);
  const std::string icon_provider_esc = category_tab_escape_for_python_literal(icon_provider);

  char cmd[8192];
  BLI_snprintf(cmd,
               sizeof(cmd),
               "from bl_ui.glyph_tag_system.api import set_category_data, finalize_category_tag_changes\n"
               "set_category_data('%s', display_name='%s', first_letter='%s', glyph='%s', color='%s', "
               "icon_source='%s', icon_key='%s', icon_path='%s', icon_provider='%s', glyph_mode='%s', space_type=%d, skip_wm_sync=True)\n"
               "finalize_category_tag_changes('%s', space_type=%d, sync_wm=False)\n",
               category_esc.c_str(),
               display_name_esc.c_str(),
               first_letter,
               glyph_hex,
               color_hex,
               icon_source,
               icon_key_esc.c_str(),
               icon_path_esc.c_str(),
               icon_provider_esc.c_str(),
               glyph_mode,
               space_type,
               category_esc.c_str(),
               space_type);
  const char *imports_none[] = {nullptr};
  BPY_run_string_exec(C, imports_none, cmd);
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
  /* Escape every interpolated user string for Python (paths/tags may contain quotes,
   * backslashes or newlines). Do not rely on raw r'''...''' literals for arbitrary input. */
  const std::string tags_esc = category_tab_escape_for_python_literal(tags);
  const std::string glyph_hex_esc = category_tab_escape_for_python_literal(glyph_hex);
  const std::string icon_key_esc = category_tab_escape_for_python_literal(icon_key);
  const std::string icon_path_esc = category_tab_escape_for_python_literal(icon_path);
  const std::string icon_provider_esc = category_tab_escape_for_python_literal(icon_provider);

  const char *imports[] = {"bpy", nullptr};
  char cmd[2048];
  BLI_snprintf(cmd,
               sizeof(cmd),
               "from bl_ui.glyph_tag_system.api import restore_category_tags_from_string, restore_category_glyph_from_snapshot\n"
               "import bpy\n"
               "wm = bpy.context.window_manager\n"
               "category = wm.category_tab_save_category\n"
               "wm.category_tab_save_category = ''\n"
               "if category:\n"
               "    restore_category_tags_from_string(category, '%s', space_type=%d)\n"
               "    restore_category_glyph_from_snapshot(category, '%s', %d, [%f, %f, %f], space_type=%d,\n"
               "        icon_source=%d, icon_key='%s', icon_path='%s', icon_provider='%s')\n",
               tags_esc.c_str(),
               space_type,
               glyph_hex_esc.c_str(),
               glyph_mode,
               color[0], color[1], color[2],
               space_type,
               icon_source,
               icon_key_esc.c_str(),
               icon_path_esc.c_str(),
               icon_provider_esc.c_str());
  BPY_run_string_exec(C, imports, cmd);
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
  const std::string tag_name_esc = category_tab_escape_for_python_literal(tag_name);
  const std::string icon_key_esc = category_tab_escape_for_python_literal(icon_key);
  const char *imports[] = {"bpy", nullptr};
  char cmd[2048];
  BLI_snprintf(cmd,
               sizeof(cmd),
               "from bl_ui.glyph_tag_system.api import update_tag\n"
               "update_tag(tag_name='%s', icon_key='%s', icon_source=%d, auto_save=True)\n",
               tag_name_esc.c_str(),
               icon_key_esc.c_str(),
               icon_source);
  BPY_run_string_exec(C, imports, cmd);
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
  const std::string cat_esc = category_tab_escape_for_python_literal(category);
  const std::string tag_esc = category_tab_escape_for_python_literal(tag);
  char cmd[1280];
  SNPRINTF(cmd,
           "__import__('bl_ui.glyph_tag_system.api', fromlist=[''])."
           "assign_tag_to_category('%s', '%s', %d)",
           cat_esc.c_str(),
           tag_esc.c_str(),
           space_type);
  const char *imports_none[] = {nullptr};
  BPY_run_string_exec(C, imports_none, cmd);
#else
  UNUSED_VARS(C, category, tag, space_type);
#endif
}

bool category_py_mark_all_unassigned_without_tag(bContext *C,
                                                 const int space_type,
                                                 const uint32_t mode_flag)
{
#ifdef WITH_PYTHON
  char cmd[512];
  SNPRINTF(cmd,
           "import bpy; "
           "updated = __import__('bl_ui.glyph_tag_system.api', fromlist=['']).mark_all_unassigned_categories_as_without_tag(%d, %u); "
           "bpy.context.window_manager.report({'INFO'}, f'{updated} unassigned categories marked as \"Without Tag\"')",
           space_type,
           mode_flag);
  const char *imports_none[] = {nullptr};
  return BPY_run_string_exec(C, imports_none, cmd);
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
  const std::string esc_cat = category_tab_escape_for_python_literal(category);
  const std::string esc_ext = category_tab_escape_for_python_literal(extension_id);
  char cmd[1280];
  SNPRINTF(cmd,
           "__import__('bl_ui.glyph_tag_system.api', fromlist=[''])."
           "mark_category_from_extension('%s', '%s', %d, %u)",
           esc_cat.c_str(),
           esc_ext.c_str(),
           space_type,
           mode_flag);
  const char *imports_none[] = {nullptr};
  BPY_run_string_exec(C, imports_none, cmd);
#else
  UNUSED_VARS(C, category, extension_id, space_type, mode_flag);
#endif
}

void category_py_set_category_order(bContext *C,
                                    const char *tag_key,
                                    const Vector<std::string> &order)
{
#ifdef WITH_PYTHON
  /* Build Python list of category IDs. */
  std::string python_list = "[";
  for (int i = 0; i < order.size(); i++) {
    if (i > 0) {
      python_list += ", ";
    }
    python_list += "'" + category_tab_escape_for_python_literal(order[i].c_str()) + "'";
  }
  python_list += "]";

  const std::string escaped_key = category_tab_escape_for_python_literal(tag_key);

  const std::string cmd = "from bl_ui.glyph_tag_system.api import set_category_order\n"
                          "set_category_order('" +
                          escaped_key + "', " + python_list + ")\n";

  const char *imports_none[] = {nullptr};
  BPY_run_string_exec(C, imports_none, cmd.c_str());
#else
  UNUSED_VARS(C, tag_key, order);
#endif
}

void category_py_set_preview_mode(bContext *C, const bool active)
{
#ifdef WITH_PYTHON
  char cmd[256];
  BLI_snprintf(cmd,
               sizeof(cmd),
               "from bl_ui.glyph_tag_system.api import set_preview_mode_active\n"
               "set_preview_mode_active(%s)",
               active ? "True" : "False");
  const char *imports_none[] = {nullptr};
  BPY_run_string_exec(C, imports_none, cmd);
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

  const std::string escaped_id = category_tab_escape_for_python_literal(category_id);
  const std::string escaped_space = category_tab_escape_for_python_literal(space_type_name);

  char python_expr[640];
  SNPRINTF(python_expr,
           "str(__import__('bl_ui.glyph_tag_system.api', fromlist=[''])."
           "get_reserved_category_priority('%s', '%s'))",
           escaped_id.c_str(),
           escaped_space.c_str());

  char *result_str = nullptr;
  const char *imports_none[] = {nullptr};
  const bool success = BPY_run_string_as_string(C, imports_none, python_expr, nullptr, &result_str);
  if (!success || !result_str) {
    return -1;
  }

  const int prio = atoi(result_str);
  MEM_delete(result_str);
  return prio;
#else
  UNUSED_VARS(C, category_id, space_type_name);
  return -1;
#endif
}

std::string category_py_get_category_order_json(bContext *C, const char *tag_key)
{
#ifdef WITH_PYTHON
  const std::string escaped_key = category_tab_escape_for_python_literal(tag_key);

  /* json.dumps converts the Python list to a JSON string for C++ parsing.
   * ensure_ascii=False preserves Unicode characters (not escaped as \uXXXX). */
  const std::string python_expr =
      "json.dumps(__import__('bl_ui.glyph_tag_system.api', fromlist=['']).get_category_order('" +
      escaped_key + "') or [], ensure_ascii=False)";

  char *result_str = nullptr;
  char *err_msg = nullptr;
  BPy_RunErrInfo err_info = {false, nullptr, "", &err_msg};
  const char *imports_json[] = {"json", nullptr};
  const bool success = BPY_run_string_as_string(
      C, imports_json, python_expr.c_str(), &err_info, &result_str);
  if (!success) {
    if (err_msg) {
      MEM_delete(err_msg);
    }
    return "";
  }
  if (!result_str) {
    return "";
  }
  std::string out(result_str);
  MEM_delete(result_str);
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
  const std::string escaped_query = category_tab_escape_for_python_literal(query);
  const std::string escaped_category = category_tab_escape_for_python_literal(category ? category :
                                                                                        "");
  char python_expr[2048];
  SNPRINTF(python_expr,
           "json.dumps([{'unicode': g['unicode'], 'name': g['name']} "
           "for g in __import__('bl_ui.glyph_library.registry', fromlist=['']).search_glyphs('%s', '%s', "
           "%d)])",
           escaped_query.c_str(),
           escaped_category.c_str(),
           max_results);

  const char *imports[] = {"json", nullptr};
  char *result_str = nullptr;
  char *err_msg = nullptr;
  BPy_RunErrInfo err_info = {false, nullptr, "", &err_msg};
  const bool success = BPY_run_string_as_string(C, imports, python_expr, &err_info, &result_str);
  if (!success) {
    if (err_msg) {
      MEM_delete(err_msg);
    }
    return "";
  }
  if (!result_str) {
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
  const std::string escaped_category = category_tab_escape_for_python_literal(category);

  char python_expr[2048];
  SNPRINTF(python_expr,
           "(lambda _r: json.dumps([_r[0].replace('\\\\', '/'), _r[1]]))"
           "(__import__('bl_ui.glyph_tag_system.api', fromlist=[''])"
           "._auto_detect_extension_icon_path('%s'))",
           escaped_category.c_str());

  const char *imports[] = {"json", nullptr};
  char *result_str = nullptr;
  char *err_msg = nullptr;
  BPy_RunErrInfo err_info = {false, nullptr, "", &err_msg};
  const bool success = BPY_run_string_as_string(C, imports, python_expr, &err_info, &result_str);
  if (!success || !result_str) {
    if (err_msg) {
      MEM_delete(err_msg);
    }
    return "";
  }
  std::string out(result_str);
  MEM_delete(result_str);
  if (err_msg) {
    MEM_delete(err_msg);
  }
  return out;
#else
  UNUSED_VARS(C, category);
  return "";
#endif
}

}  // namespace blender::ui
