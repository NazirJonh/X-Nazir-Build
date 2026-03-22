/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spbuttons
 *
 * Drag-and-drop support for the Property Editor.
 * Currently supports dropping Geometry Nodes assets onto the modifier stack.
 */

#include "UI_interface_types.hh"
#include "buttons_intern.hh"

#include "AS_asset_representation.hh"

#include <algorithm>
#include <cmath>

#include "BKE_asset.hh"
#include "BKE_context.hh"
#include "BKE_idprop.hh"
#include "BKE_modifier.hh"
#include "BKE_object.hh"
#include "BKE_screen.hh"

#include "BLI_listbase.h"
#include "BLI_rect.h"
#include "BLI_time.h"

#include "BLT_translation.hh"

#include "DNA_ID.h"
#include "DNA_modifier_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_userdef_types.h"

#include "ED_buttons.hh"
#include "ED_screen.hh"

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

/* -------------------------------------------------------------------- */
/** \name Drag State
 * \{ */

/**
 * Global drag state for modifier stack drag-and-drop.
 *
 * The current drag and drop API doesn't allow us to easily pass along the
 * required custom data to all callbacks that need it (insert_index, panel positions).
 * Therefore we use a global variable for this, following the pattern in
 * sequencer_drag_drop.cc.
 *
 * The state is linked to the dropbox via `drop->draw_data` in the `on_enter` callback,
 * and unlinked in `on_exit`. This provides explicit lifecycle management.
 *
 * Thread Safety: This is NOT thread-safe. Drag-and-drop operations are
 * single-threaded in Blender's UI system, so this is acceptable.
 *
 * Lifetime: State is cleared on drop (copy callback) or exit (on_exit callback).
 * A timeout mechanism (DRAG_TIMEOUT_SECONDS) provides additional safety.
 */

/** Timeout in seconds after which drag state is cleared if poll stops being called. */
static constexpr double DRAG_TIMEOUT_SECONDS = 0.1;

/** State for tracking drag operations in the Property Editor. */
struct DragState {
  /** Cursor position in screen coordinates. */
  int screen_xy[2] = {0, 0};
  /** Y coordinate in panel space (tot space with scroll offset applied). */
  int panel_space_y = 0;
  /** Whether a drag operation is currently active. */
  bool active = false;
  /** Last time poll was called, to clear drag state if poll stops being called. */
  double last_poll_time = 0.0;
  /** Region for redraw when drag exits. */
  ARegion *region = nullptr;
  /** Calculated insert index for the modifier (-1 means append at end). */
  int insert_index = -1;
};

static DragState g_drag_state;

/** Check if the drag has timed out (no recent poll calls). */
static bool drag_is_timed_out()
{
  return (BLI_time_now_seconds() - g_drag_state.last_poll_time) > DRAG_TIMEOUT_SECONDS;
}

/** Clear all drag state. */
static void drag_state_clear()
{
  g_drag_state.screen_xy[0] = 0;
  g_drag_state.screen_xy[1] = 0;
  g_drag_state.panel_space_y = 0;
  g_drag_state.active = false;
  g_drag_state.last_poll_time = 0.0;
  g_drag_state.region = nullptr;
  g_drag_state.insert_index = -1;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Coordinate Conversion Utilities
 * \{ */

/**
 * Convert window Y coordinate to panel space Y coordinate (tot space).
 *
 * In panel regions:
 * - Panel ofsy is negative, starting from 0 at top of total area (tot.ymax) and going down
 * - v2d.cur shows the visible portion in tot coordinates
 * - region_y is 0 at bottom of region, positive going up
 */
static int window_y_to_panel_space_y(const ARegion *region, int window_y)
{
  const int region_y = window_y - region->winrct.ymin;
  const View2D *v2d = &region->v2d;
  return int(ui::view2d_region_to_view_y(v2d, float(region_y)));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Panel Utilities
 * \{ */

/**
 * Check if a panel is an individual modifier panel.
 * Excludes the modifiers header, subpanels, and the ghost panel.
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
  /* Only main panels (no parent) count as individual modifiers. */
  if (panel.type->parent != nullptr) {
    return false;
  }
  /* Filter out the "Modifiers" header panel.
   * Individual modifiers have idname like "MOD_PT_Bevel", "MOD_PT_Boolean", etc. */
  if (!STRPREFIX(panel.type->idname, "MOD_PT_")) {
    return false;
  }
  /* Exclude the ghost panel. */
  if (STREQ(panel.type->idname, "MOD_PT_Ghost")) {
    return false;
  }
  return true;
}

/** Get the index of a modifier in the object's modifier list. */
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

/** \} */

/* -------------------------------------------------------------------- */
/** \name Drag Type Detection
 * \{ */

/**
 * Check if a drag contains a valid geometry nodes modifier asset.
 * Supports: local node trees and single assets from the Asset Browser.
 */
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
    if (!tree_type || IDP_int_get(tree_type) != NTREE_GEOMETRY) {
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

  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Insert Index Calculation
 * \{ */

/**
 * Calculate the insertion index based on the drop Y coordinate.
 * Uses panel boundaries to determine insertion position.
 */
static int calculate_insert_index(const bContext *C, const ARegion *region, int panel_space_y)
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

  /* If at bottom of scroll area and cursor is near bottom, append at end. */
  const bool view_at_bottom = std::fabs(v2d->cur.ymin - v2d->tot.ymin) <= 1.0f;
  if (view_at_bottom && view_y <= (v2d->cur.ymin + float(UI_UNIT_Y))) {
    return mod_count;
  }

  /* Collect panel boundaries. */
  struct PanelBoundary {
    float y;
    int index;
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
    ModifierData *md = static_cast<ModifierData *>(ptr->data);
    const int modifier_index = get_modifier_index(object, md);
    if (modifier_index == -1) {
      continue;
    }

    const int real_ofsy = panel.ofsy;
    const int real_size_y = ui::panel_size_y(&panel);
    const float panel_bottom = float(real_ofsy);
    const float panel_top = float(real_ofsy + real_size_y);

    boundaries.append({panel_top, modifier_index});
    boundaries.append({panel_bottom, modifier_index + 1});
  }

  if (boundaries.is_empty()) {
    return mod_count;
  }

  /* Sort by Y coordinate (descending). */
  std::sort(boundaries.begin(), boundaries.end(), [](const auto &a, const auto &b) {
    return a.y > b.y;
  });

  /* If above all panels, insert at beginning. */
  if (view_y > boundaries[0].y) {
    return 0;
  }

  /* Find which gap the cursor is in. */
  for (int i = 0; i < boundaries.size() - 1; i++) {
    const float upper = boundaries[i].y;
    const float lower = boundaries[i + 1].y;

    if (view_y <= upper && view_y >= lower) {
      const float mid = (upper + lower) / 2.0f;
      return (view_y > mid) ? boundaries[i].index : boundaries[i + 1].index;
    }
  }

  return mod_count;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Drop Drawing
 * \{ */

/**
 * Draw drop indicator line in the properties region during drag.
 * Also handles auto-scroll at region edges.
 */
static void draw_drop_indicator(bContext *C, wmDrag *drag, const int xy[2])
{
  SpaceProperties *sbuts = CTX_wm_space_properties(C);
  if (!sbuts || sbuts->mainb != BCONTEXT_MODIFIER) {
    return;
  }

  ARegion *region = CTX_wm_region(C);
  Object *object = CTX_data_active_object(C);

  if (!region || !object || !drag_is_geometry_nodes_modifier(drag)) {
    return;
  }

  /* Auto-scroll at edges. */
  View2D *v2d = &region->v2d;
  const float edge_margin = 30.0f * U.pixelsize;
  const float scroll_speed = 10.0f * U.pixelsize;

  if (float(xy[1]) > float(region->winrct.ymax) - edge_margin) {
    if (v2d->cur.ymax + scroll_speed <= v2d->tot.ymax) {
      v2d->cur.ymin += scroll_speed;
      v2d->cur.ymax += scroll_speed;
      ED_region_tag_redraw(region);
    }
    return;
  }
  if (float(xy[1]) < float(region->winrct.ymin) + edge_margin) {
    if (v2d->cur.ymin - scroll_speed >= v2d->tot.ymin) {
      v2d->cur.ymin -= scroll_speed;
      v2d->cur.ymax -= scroll_speed;
      ED_region_tag_redraw(region);
    }
    return;
  }

  /* Update insert index based on current position. */
  const int new_panel_space_y = window_y_to_panel_space_y(region, xy[1]);
  const int new_insert_index = calculate_insert_index(C, region, new_panel_space_y);

  if (new_insert_index != g_drag_state.insert_index) {
    g_drag_state.insert_index = new_insert_index;
    ED_region_tag_redraw(region);
  }

  g_drag_state.panel_space_y = new_panel_space_y;
  g_drag_state.active = true;
  g_drag_state.last_poll_time = BLI_time_now_seconds();

  /* Find ghost panel or use modifier panels for positioning. */
  const Panel *ghost_panel = nullptr;
  for (const Panel &panel : region->panels) {
    if (panel.type && STREQ(panel.type->idname, "MOD_PT_Ghost")) {
      ghost_panel = &panel;
      break;
    }
  }

  /* Collect modifier panel positions. */
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

    const int real_ofsy = panel.ofsy;
    const int real_size_y = ui::panel_size_y(&panel);
    panels.append({float(real_ofsy + real_size_y), float(real_ofsy), modifier_index});
  }

  const int mod_count = BLI_listbase_count(&object->modifiers);
  const int insert_index = std::clamp(g_drag_state.insert_index, 0, mod_count);
  const float line_margin = 4.0f * U.pixelsize;

  /* Calculate line Y position. */
  float line_tot_y = 0;

  if (ghost_panel) {
    const int real_ofsy = ghost_panel->ofsy;
    const int real_size_y = ui::panel_size_y(ghost_panel);
    line_tot_y = (insert_index == 0) ? float(real_ofsy) + line_margin :
                                       float(real_ofsy + real_size_y) - line_margin;
  }
  else if (!panels.is_empty()) {
    std::sort(panels.begin(), panels.end(), [](const auto &a, const auto &b) {
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
      for (const auto &panel : panels) {
        if (panel.modifier_index == insert_index - 1) {
          prev_panel = &panel;
          break;
        }
      }
      line_tot_y = prev_panel ? prev_panel->bottom + line_margin :
                                panels.first().top + line_margin;
    }
  }
  else {
    return;
  }

  /* Check if line is visible. */
  if (line_tot_y < v2d->cur.ymin - 1.0f || line_tot_y > v2d->cur.ymax + 1.0f) {
    return;
  }

  /* Draw the indicator line. */
  const float pixel_y = ui::view2d_view_to_region_y(v2d, line_tot_y);

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

static void modifier_drop_draw_in_view(bContext *C,
                                       wmWindow * /*win*/,
                                       wmDrag *drag,
                                       const int xy[2])
{
  draw_drop_indicator(C, drag, xy);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Drop Callbacks
 * \{ */

static bool modifier_drop_poll(bContext *C, wmDrag *drag, const wmEvent *event)
{
  /* Check if DnD GN modifiers feature is enabled in preferences. */
  if (!USE_DND_GN_MODIFIERS()) {
    return false;
  }

  SpaceProperties *sbuts = CTX_wm_space_properties(C);
  if (!sbuts || sbuts->mainb != BCONTEXT_MODIFIER) {
    return false;
  }

  ARegion *region = CTX_wm_region(C);
  if (!region) {
    return false;
  }

  if (!drag_is_geometry_nodes_modifier(drag)) {
    /* Don't clear global state here - there might be another drag (e.g., WM_DRAG_ASSET_LIST)
     * that shares the polling. Only return false without affecting state. */
    return false;
  }

  g_drag_state.region = region;

  if (event) {
    g_drag_state.screen_xy[0] = event->xy[0];
    g_drag_state.screen_xy[1] = event->xy[1];
    g_drag_state.active = true;
    g_drag_state.last_poll_time = BLI_time_now_seconds();

    /* Calculate insert index based on cursor position. */
    const int panel_space_y = window_y_to_panel_space_y(region, event->xy[1]);
    g_drag_state.insert_index = calculate_insert_index(C, region, panel_space_y);
    g_drag_state.panel_space_y = panel_space_y;

    ED_region_tag_redraw(region);
  }

  return true;
}

static void modifier_drop_copy(bContext *C, wmDrag *drag, wmDropBox *drop)
{
  ID *id = WM_drag_get_local_ID_or_import_from_asset(C, drag, ID_NT);
  if (id) {
    WM_operator_properties_id_lookup_set_from_id(drop->ptr, id);
    RNA_boolean_set(drop->ptr, "show_datablock_in_modifier", (drag->type != WM_DRAG_ASSET));
    RNA_int_set(drop->ptr, "insert_index", g_drag_state.insert_index);
  }

  g_drag_state.active = false;
  g_drag_state.region = nullptr;

  ARegion *region = CTX_wm_region(C);
  if (region) {
    ED_region_tag_redraw(region);
  }
}

static std::string modifier_drop_tooltip(bContext * /*C*/,
                                         wmDrag *drag,
                                         const int /*xy*/[2],
                                         wmDropBox * /*drop*/)
{
  const std::string name = WM_drag_get_item_name(drag);
  return TIP_("Add \"") + name + TIP_("\" modifier to active object");
}

/**
 * Called when poll returns true the first time.
 * Sets up the draw_data pointer to link the global state with this dropbox.
 */
static void modifier_drop_on_enter(wmDropBox *drop, wmDrag * /*drag*/)
{
  DragState *state = static_cast<DragState *>(drop->draw_data);
  if (state && state->active) {
    /* Already active, nothing to do. */
    return;
  }
  drop->draw_data = &g_drag_state;
  g_drag_state.active = true;
}

/**
 * Called when poll returns false or when the drag event ends.
 * Cleans up the draw_data pointer and clears the global state.
 */
static void modifier_drop_on_exit(wmDropBox *drop, wmDrag * /*drag*/)
{
  if (g_drag_state.region) {
    ED_region_tag_redraw(g_drag_state.region);
  }
  if (drop->draw_data == &g_drag_state) {
    drop->draw_data = nullptr;
  }
  drag_state_clear();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public API
 * \{ */

bool ED_buttons_drop_active(const char *drop_type_idname, int *r_index)
{
  if (!STREQ(drop_type_idname, "GEOMETRY_NODES_MODIFIER")) {
    return false;
  }

  /* Feature must be enabled in preferences. */
  if (!USE_DND_GN_MODIFIERS()) {
    return false;
  }

  if (!g_drag_state.active) {
    return false;
  }

  if (drag_is_timed_out()) {
    if (g_drag_state.region) {
      ED_region_tag_redraw(g_drag_state.region);
    }
    drag_state_clear();
    return false;
  }

  if (r_index) {
    *r_index = g_drag_state.insert_index;
  }
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Dropbox Registration
 * \{ */

void buttons_dropboxes()
{
  ListBaseT<wmDropBox> *lb = WM_dropboxmap_find(
      "Property Editor", SPACE_PROPERTIES, RGN_TYPE_WINDOW);

  wmDropBox *drop = WM_dropbox_add(lb,
                                   "OBJECT_OT_modifier_add_node_group",
                                   modifier_drop_poll,
                                   modifier_drop_copy,
                                   WM_drag_free_imported_drag_ID,
                                   modifier_drop_tooltip);

  drop->draw_in_view = modifier_drop_draw_in_view;
  drop->on_enter = modifier_drop_on_enter;
  drop->on_exit = modifier_drop_on_exit;
}

/** \} */

}  // namespace blender
