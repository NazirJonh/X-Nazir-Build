/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spbuttons
 */

#include "AS_asset_representation.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "BKE_asset.hh"
#include "BKE_context.hh"
#include "BKE_idprop.hh"
#include "BKE_lib_id.hh"
#include "BKE_modifier.hh"
#include "BKE_object.hh"
#include "BKE_screen.hh"

#include "BLI_listbase.h"
#include "BLI_rect.h"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "DNA_ID.h"
#include "DNA_modifier_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_enums.h"
#include "DNA_space_types.h"
#include "DNA_theme_types.h"
#include "DNA_userdef_types.h"

#include "ED_object.hh"
#include "ED_screen.hh"

#include "BLI_time.h"

#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"
#include "GPU_vertex_format.hh"

#include "RNA_access.hh"

#include "UI_interface_c.hh"
#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

namespace blender {

/* Static storage for drop coordinates - updated in poll, used in copy.
 * Stores the region-relative Y coordinate with scroll offset applied. */
static int g_last_drop_region_y = 0;
/* Stores the screen coordinates for visual indicator drawing. */
static int g_last_drop_screen_xy[2] = {0, 0};
/* Flag to indicate if a drag is currently active */
static bool g_drag_active = false;
/* Calculated insert index for visual indicator */
static int g_insert_index = -1;
/* Last time poll was called, to clear drag state if poll stops being called */
static double g_last_poll_time = 0.0;
/* Store the region for redraw when drag exits */
static ARegion *g_active_region = nullptr;

static bool drag_is_geometry_nodes_modifier(const wmDrag *drag)
{
  if (drag->type == WM_DRAG_ID) {
    const bNodeTree *node_tree = reinterpret_cast<const bNodeTree *>(
        WM_drag_get_local_ID(const_cast<wmDrag *>(drag), ID_NT));
    return node_tree && (node_tree->type == NTREE_GEOMETRY);
  }

  if (drag->type == WM_DRAG_ASSET) {
    const wmDragAsset *asset_data = WM_drag_get_asset_data(const_cast<wmDrag *>(drag), ID_NT);
    if (!asset_data) {
      return false;
    }

    const AssetMetaData *metadata = &asset_data->asset->get_metadata();
    const IDProperty *tree_type = BKE_asset_metadata_idprop_find(metadata, "type");
    if (!tree_type) {
      return false;
    }
    const int type_val = IDP_int_get(tree_type);
    if (type_val != NTREE_GEOMETRY) {
      return false;
    }

    const IDProperty *traits_flag = BKE_asset_metadata_idprop_find(
        metadata, "geometry_node_asset_traits_flag");
    if (!traits_flag) {
      return false;
    }
    const int traits = IDP_int_get(traits_flag);
    return traits && (traits & GEO_NODE_ASSET_MODIFIER);
  }

  if (drag->type == WM_DRAG_ASSET_LIST) {
    const ListBaseT<wmDragAssetListItem> *asset_list = WM_drag_asset_list_get(drag);
    if (!asset_list || BLI_listbase_is_empty(asset_list)) {
      return false;
    }

    int asset_count = BLI_listbase_count(asset_list);
    if (asset_count != 1) {
      return false;
    }

    const wmDragAssetListItem *item = static_cast<const wmDragAssetListItem *>(asset_list->first);
    if (item->is_external) {
      const wmDragAsset *asset_data = item->asset_data.external_info;
      if (!asset_data) {
        return false;
      }

      const AssetMetaData *metadata = &asset_data->asset->get_metadata();
      const IDProperty *tree_type = BKE_asset_metadata_idprop_find(metadata, "type");
      if (!tree_type) {
        return false;
      }
      const int type_val = IDP_int_get(tree_type);
      if (type_val != NTREE_GEOMETRY) {
        return false;
      }

      const IDProperty *traits_flag = BKE_asset_metadata_idprop_find(
          metadata, "geometry_node_asset_traits_flag");
      if (!traits_flag) {
        return false;
      }
      const int traits = IDP_int_get(traits_flag);
      return traits && (traits & GEO_NODE_ASSET_MODIFIER);
    }
    else {
      const ID *id = item->asset_data.local_id;
      if (id && GS(id->name) == ID_NT) {
        const bNodeTree *node_tree = reinterpret_cast<const bNodeTree *>(id);
        return node_tree && (node_tree->type == NTREE_GEOMETRY);
      }
    }

    return false;
  }

  return false;
}

/**
 * Convert window coordinates to panel coordinates (tot space).
 * Returns the Y coordinate in "tot space" where panels are laid out.
 *
 * In panel regions:
 * - Panel ofsy is negative, starting from 0 at top of total area (tot.ymax) and going down
 * - v2d.cur shows the visible portion in tot coordinates
 * - region_y is 0 at bottom of region, positive going up
 *
 * To convert region_y to tot space:
 * - Pixel 0 (bottom of region) corresponds to cur.ymin
 * - Pixel region_height (top of region) corresponds to cur.ymax
 * - view_y = cur.ymin + region_y
 */
static int window_y_to_panel_space_y(const ARegion *region, int window_y)
{
  int region_y = window_y - region->winrct.ymin;

  const View2D *v2d = &region->v2d;
  float view_y = ui::view2d_region_to_view_y(v2d, float(region_y));

  return int(view_y);
}

/**
 * Check if a panel is an individual modifier panel (not the modifiers header or a subpanel).
 * Individual modifier panels have:
 * - context "modifier"
 * - no parent panel
 * - idname starting with "MOD_PT_" (not "DATA_PT_modifiers" which is the header)
 */
static bool is_modifier_panel(const Panel &panel)
{
  if (panel.type == nullptr) {
    return false;
  }
  /* Modifier panels have context "modifier" set in MOD_ui_common.cc */
  if (panel.type->context == nullptr || !STREQ(panel.type->context, "modifier")) {
    return false;
  }
  /* Only main panels (no parent) count as individual modifiers */
  if (panel.type->parent != nullptr) {
    return false;
  }
  /* Filter out the "Modifiers" header panel - only individual modifier panels count.
   * Individual modifiers have idname like "MOD_PT_Bevel", "MOD_PT_Boolean", etc.
   * The header is "DATA_PT_modifiers". */
  if (!STRPREFIX(panel.type->idname, "MOD_PT_")) {
    return false;
  }
  /* Exclude the ghost panel from index calculation. */
  if (STREQ(panel.type->idname, "MOD_PT_Ghost")) {
    return false;
  }
  return true;
}

static int panel_real_ofsy(const Panel &panel)
{
  return panel.ofsy;
}

static int get_modifier_index(const Object *ob, const ModifierData *target_md)
{
  int index = 0;
  for (ModifierData *md = (ModifierData *)ob->modifiers.first; md; md = md->next) {
    if (md == target_md) {
      return index;
    }
    index++;
  }
  return -1;
}

/**
 * Calculate the insertion index based on drop Y coordinate.
 * Uses panel boundaries to determine insertion position, which works
 * correctly even when panels are collapsed (have smaller sizey).
 *
 * \param panel_space_y: Y coordinate in tot space (same as panel ofsy)
 */
static int calculate_modifier_insert_index(const bContext *C,
                                           const ARegion *region,
                                           int panel_space_y)
{
  Object *object = CTX_data_active_object(C);
  if (!object) {
    return -1;
  }

  const int mod_count = BLI_listbase_count(&object->modifiers);

  if (mod_count == 0) {
    return 0;
  }

  const float view_y = float(panel_space_y);
  const View2D *v2d = &region->v2d;

  const bool view_at_bottom = std::fabs(v2d->cur.ymin - v2d->tot.ymin) <= 1.0f;
  if (view_at_bottom && view_y <= (v2d->cur.ymin + float(UI_UNIT_Y))) {
    return mod_count;
  }

  struct PanelBoundary {
    float y;
    int index;
    bool is_top;
  };

  Vector<PanelBoundary> boundaries;

  for (const Panel &panel : region->panels) {
    if (!is_modifier_panel(panel)) {
      continue;
    }

    PointerRNA *ptr = ui::panel_custom_data_get(&panel);
    if (!ptr || !ptr->data) {
      continue;
    }
    ModifierData *md = (ModifierData *)ptr->data;
    int modifier_index = get_modifier_index(object, md);

    if (modifier_index == -1) {
      continue;
    }

    const int real_ofsy = panel_real_ofsy(panel);
    const int real_size_y = ui::panel_size_y(&panel);
    const float panel_bottom = float(real_ofsy);
    const float panel_top = float(real_ofsy + real_size_y);

    boundaries.append({panel_top, modifier_index, true});
    boundaries.append({panel_bottom, modifier_index + 1, false});
  }

  if (boundaries.is_empty()) {
    return mod_count;
  }

  std::sort(boundaries.begin(),
            boundaries.end(),
            [](const PanelBoundary &a, const PanelBoundary &b) { return a.y > b.y; });

  int insert_index = mod_count;

  if (view_y > boundaries[0].y) {
    return 0;
  }

  for (int i = 0; i < boundaries.size() - 1; i++) {
    const float upper = boundaries[i].y;
    const float lower = boundaries[i + 1].y;

    if (view_y <= upper && view_y >= lower) {
      const float mid = (upper + lower) / 2.0f;
      insert_index = (view_y > mid) ? boundaries[i].index : boundaries[i + 1].index;
      return insert_index;
    }
  }

  return mod_count;
}

static bool buttons_geometry_nodes_modifier_drop_poll(bContext *C,
                                                      wmDrag *drag,
                                                      const wmEvent *event)
{
  SpaceProperties *sbuts = CTX_wm_space_properties(C);
  if (!sbuts || sbuts->mainb != BCONTEXT_MODIFIER) {
    g_drag_active = false;
    return false;
  }

  ARegion *region = CTX_wm_region(C);
  if (!region) {
    g_drag_active = false;
    return false;
  }

  if (!drag_is_geometry_nodes_modifier(drag)) {
    g_drag_active = false;
    return false;
  }

  g_active_region = region;

  if (event) {
    g_last_drop_screen_xy[0] = event->xy[0];
    g_last_drop_screen_xy[1] = event->xy[1];
    g_last_drop_region_y = window_y_to_panel_space_y(region, event->xy[1]);
    g_insert_index = calculate_modifier_insert_index(C, region, g_last_drop_region_y);
    g_drag_active = true;
    g_last_poll_time = BLI_time_now_seconds();
    ED_region_tag_redraw(region);
  }
  else {
    g_drag_active = true;
    g_last_poll_time = BLI_time_now_seconds();
    ED_region_tag_redraw(region);
  }

  return true;
}

static void buttons_geometry_nodes_modifier_drop_copy(bContext *C, wmDrag *drag, wmDropBox *drop)
{
  ID *id = WM_drag_get_local_ID_or_import_from_asset(C, drag, ID_NT);
  if (id) {
    WM_operator_properties_id_lookup_set_from_id(drop->ptr, id);
    RNA_boolean_set(drop->ptr, "show_datablock_in_modifier", (drag->type != WM_DRAG_ASSET));
    RNA_int_set(drop->ptr, "insert_index", g_insert_index);
  }

  g_drag_active = false;
  g_active_region = nullptr;

  ARegion *region = CTX_wm_region(C);
  if (region) {
    ED_region_tag_redraw(region);
  }
}

static std::string buttons_geometry_nodes_modifier_drop_tooltip(bContext * /*C*/,
                                                                wmDrag *drag,
                                                                const int /*xy*/[2],
                                                                wmDropBox *drop)
{
  if (drag->type == WM_DRAG_ASSET_LIST) {
    return "";
  }

  const std::string name = WM_drag_get_item_name(drag);
  return TIP_("Add \"") + name + TIP_("\" modifier to active object");
}

/**
 * Draw drop indicator line in the properties region during drag.
 * This is called by Blender's drag-drop system via draw_in_view callback.
 * The line is drawn at the boundary where insertion will occur.
 */
static void buttons_geometry_nodes_modifier_drop_draw_in_view(bContext *C,
                                                              wmWindow * /*win*/,
                                                              wmDrag *drag,
                                                              const int xy[2])
{
  /* Only draw in modifiers tab */
  SpaceProperties *sbuts = CTX_wm_space_properties(C);
  if (!sbuts || sbuts->mainb != BCONTEXT_MODIFIER) {
    return;
  }

  ARegion *region = CTX_wm_region(C);
  Object *object = CTX_data_active_object(C);

  if (!region || !object || !drag_is_geometry_nodes_modifier(drag)) {
    return;
  }

  /* Auto-scroll at edges */
  View2D *v2d = &region->v2d;
  const float edge_margin = 30.0f * U.pixelsize;
  const float scroll_speed = 10.0f * U.pixelsize;

  bool did_scroll = false;

  /* Check if mouse is near top edge - scroll up */
  if (float(xy[1]) > float(region->winrct.ymax) - edge_margin) {
    const float scroll_amount = scroll_speed;
    if (v2d->cur.ymax + scroll_amount <= v2d->tot.ymax) {
      v2d->cur.ymin += scroll_amount;
      v2d->cur.ymax += scroll_amount;
      ED_region_tag_redraw(region);
      did_scroll = true;
    }
  }
  /* Check if mouse is near bottom edge - scroll down */
  else if (float(xy[1]) < float(region->winrct.ymin) + edge_margin) {
    const float scroll_amount = scroll_speed;
    if (v2d->cur.ymin - scroll_amount >= v2d->tot.ymin) {
      v2d->cur.ymin -= scroll_amount;
      v2d->cur.ymax -= scroll_amount;
      ED_region_tag_redraw(region);
      did_scroll = true;
    }
  }

  if (did_scroll) {
    return;
  }

  const int new_drop_region_y = window_y_to_panel_space_y(region, xy[1]);
  const int new_insert_index = calculate_modifier_insert_index(C, region, new_drop_region_y);
  if (new_insert_index != g_insert_index) {
    g_insert_index = new_insert_index;
    ED_region_tag_redraw(region);
  }
  g_drag_active = true;
  g_last_poll_time = BLI_time_now_seconds();

  const int mod_count = BLI_listbase_count(&object->modifiers);

  const Panel *ghost_panel = nullptr;
  for (const Panel &panel : region->panels) {
    if (panel.type == nullptr) {
      continue;
    }
    if (STREQ(panel.type->idname, "MOD_PT_Ghost")) {
      ghost_panel = &panel;
      break;
    }
  }

  /* Collect modifier panels with their boundaries */
  struct ModPanel {
    float top;
    float bottom;
    int modifier_index;
  };
  Vector<ModPanel> panels;

  for (const Panel &panel : region->panels) {
    if (!is_modifier_panel(panel)) {
      continue;
    }
    PointerRNA *ptr = ui::panel_custom_data_get(&panel);
    if (!ptr || !ptr->data) {
      continue;
    }
    ModifierData *md = static_cast<ModifierData *>(ptr->data);
    const int modifier_index = get_modifier_index(object, md);
    if (modifier_index == -1) {
      continue;
    }

    ModPanel mp;
    const int real_ofsy = panel_real_ofsy(panel);
    const int real_size_y = ui::panel_size_y(&panel);
    mp.bottom = float(real_ofsy);
    mp.top = float(real_ofsy + real_size_y);
    mp.modifier_index = modifier_index;
    panels.append(mp);
  }

  float line_tot_y = 0;
  const float line_margin = 4.0f * U.pixelsize; // Margin between drop indicator line and panel

  const int insert_index = std::clamp(g_insert_index, 0, mod_count);

  if (ghost_panel) {
    const int real_ofsy = panel_real_ofsy(*ghost_panel);
    const int real_size_y = ui::panel_size_y(ghost_panel);
    const float ghost_bottom = float(real_ofsy);
    const float ghost_top = float(real_ofsy + real_size_y);

    if (insert_index == 0) {
      line_tot_y = ghost_bottom + line_margin;
    }
    else {
      line_tot_y = ghost_top - line_margin;
    }
  }
  else {
    if (panels.is_empty()) {
      return;
    }

    std::sort(panels.begin(), panels.end(), [](const ModPanel &a, const ModPanel &b) {
      return a.modifier_index < b.modifier_index;
    });

    if (insert_index == 0) {
      line_tot_y = panels.first().top + line_margin;
    }
    else if (insert_index >= mod_count) {
      line_tot_y = panels.last().bottom - line_margin;
    }
    else {
      const ModPanel *prev_panel = nullptr;
      for (const ModPanel &panel : panels) {
        if (panel.modifier_index == insert_index - 1) {
          prev_panel = &panel;
          break;
        }
      }
      if (prev_panel) {
        line_tot_y = prev_panel->bottom + line_margin;
      }
      else {
        line_tot_y = panels.first().top + line_margin;
      }
    }
  }

  const float tolerance = 1.0f;
  if (line_tot_y < v2d->cur.ymin - tolerance || line_tot_y > v2d->cur.ymax + tolerance) {
    return;
  }

  const float pixel_y = ui::view2d_view_to_region_y(v2d, line_tot_y);

  /* Set up pixel-space coordinate system for drawing */
  GPU_matrix_push();
  wmOrtho2_region_pixelspace(region);

  const float x1 = UI_PANEL_MARGIN_X;
  const float x2 = float(BLI_rcti_size_x(&region->winrct)) - UI_PANEL_MARGIN_X;

  GPU_blend(GPU_BLEND_ALPHA);
  float col[4];
  ui::theme::get_color_4fv(TH_TEXT_HI, col);
  col[3] = 0.8f;

  GPUVertFormat *format = immVertexFormat();
  uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
  immUniformColor4fv(col);

  const float line_width = 2.0f * U.pixelsize;

  immRectf(pos, x1, pixel_y - line_width, x2, pixel_y + line_width);
  immUnbindProgram();
  GPU_blend(GPU_BLEND_NONE);

  GPU_matrix_pop();
}

bool ED_buttons_modifier_drop_active(int *r_index)
{
  if (g_drag_active) {
    if ((BLI_time_now_seconds() - g_last_poll_time) > 0.1) {
      g_drag_active = false;
      if (g_active_region) {
        ED_region_tag_redraw(g_active_region);
        g_active_region = nullptr;
      }
      return false;
    }

    if (r_index) {
      *r_index = g_insert_index;
    }
    return true;
  }
  return false;
}

static void buttons_geometry_nodes_modifier_drop_on_exit(wmDropBox * /*drop*/, wmDrag * /*drag*/)
{
  g_drag_active = false;
  g_insert_index = -1;
  g_last_poll_time = 0.0;

  if (g_active_region) {
    ED_region_tag_redraw(g_active_region);
    g_active_region = nullptr;
  }
}

void buttons_dropboxes()
{
  ListBaseT<wmDropBox> *lb = WM_dropboxmap_find(
      "Property Editor", SPACE_PROPERTIES, RGN_TYPE_WINDOW);

  wmDropBox *drop = WM_dropbox_add(lb,
                                   "OBJECT_OT_modifier_add_node_group",
                                   buttons_geometry_nodes_modifier_drop_poll,
                                   buttons_geometry_nodes_modifier_drop_copy,
                                   WM_drag_free_imported_drag_ID,
                                   buttons_geometry_nodes_modifier_drop_tooltip);

  drop->draw_in_view = buttons_geometry_nodes_modifier_drop_draw_in_view;
  drop->on_exit = buttons_geometry_nodes_modifier_drop_on_exit;
}

}  // namespace blender
