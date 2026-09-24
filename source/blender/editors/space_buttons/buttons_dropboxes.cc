/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spbuttons
 *
 * Drag and drop support for the Property Editor.
 * Currently supports dropping Geometry Nodes modifier node groups (local or assets) at a chosen
 * position in the modifier stack.
 */

#include <cfloat>

#include <fmt/format.h>

#include "AS_asset_representation.hh"

#include "BKE_asset.hh"
#include "BKE_context.hh"
#include "BKE_idprop.hh"
#include "BKE_object.hh"
#include "BKE_screen.hh"

#include "BLI_listbase.h"
#include "BLI_rect.h"
#include "BLI_string_utf8.h"

#include "BLT_translation.hh"

#include "DNA_modifier_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_userdef_types.h"

#include "ED_buttons.hh"
#include "ED_object.hh"
#include "ED_screen.hh"

#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "buttons_intern.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Drop State
 * \{ */

/**
 * The insert position has to be known while the modifier panels are laid out (to add the
 * placeholder panel) and when the drop operator properties are filled in, neither of which gets
 * the drop-box. Only one drag can hover a region at a time, so a single file-level state is
 * enough; it is set by the poll and reset in #wmDropBox.on_exit, which the window manager calls
 * whenever the drop-box stops being active, including a finished or canceled drag.
 */
struct ModifierDropState {
  /** Hovered region. Only compared against, never dereferenced, so it may outlive the region. */
  const ARegion *region = nullptr;
  /** Modifier stack index the drop inserts at. */
  int insert_index = -1;
};

static ModifierDropState g_drop_state;

static void drop_state_set(const ModifierDropState &state)
{
  if (state.region == g_drop_state.region && state.insert_index == g_drop_state.insert_index) {
    return;
  }
  if (g_drop_state.region != nullptr) {
    /* Redraw every Property Editor instead of the stored region, which may have been freed. This
     * also removes the placeholder from a region the drag left without leaving the drop-box. */
    WM_main_add_notifier(NC_SPACE | ND_SPACE_PROPERTIES, nullptr);
  }
  g_drop_state = state;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Modifier Panel Queries
 * \{ */

static const ModifierData *panel_modifier_get(const Panel &panel)
{
  if (panel.type == nullptr || !(panel.type->flag & PANEL_TYPE_INSTANCED)) {
    return nullptr;
  }
  /* Also filters out the placeholder panel, which has no custom data. */
  const PointerRNA *ptr = ui::panel_custom_data_get(&panel);
  if (ptr == nullptr || ptr->data == nullptr || !RNA_struct_is_a(ptr->type, RNA_Modifier)) {
    return nullptr;
  }
  return static_cast<const ModifierData *>(ptr->data);
}

/** Vertical bounds of a panel in view space, taking collapsed panels into account. */
static void panel_bounds_y(const Panel &panel, float *r_bottom, float *r_top)
{
  /* Collapsed panels keep the #Panel.ofsy and #Panel.sizey of their open state. */
  const int ofsy = ui::panel_is_closed(&panel) ? panel.ofsy + panel.sizey : panel.ofsy;
  *r_bottom = float(ofsy);
  *r_top = float(ofsy + ui::panel_size_y(&panel));
}

/**
 * Insert before the top-most modifier panel whose center is below the cursor, or append when
 * there is none. The placeholder panel only adds space between two panels, so the result stays
 * stable while the cursor is over it.
 */
static int insert_index_at_y(const ARegion &region, const Object &object, const float view_y)
{
  int insert_index = object.modifiers.count();
  float best_center = -FLT_MAX;
  for (const Panel &panel : region.panels) {
    const ModifierData *md = panel_modifier_get(panel);
    if (md == nullptr) {
      continue;
    }
    const int md_index = BLI_findindex(&object.modifiers, md);
    if (md_index == -1) {
      continue;
    }
    float bottom, top;
    panel_bounds_y(panel, &bottom, &top);
    const float center = (bottom + top) * 0.5f;
    if (center < view_y && center > best_center) {
      best_center = center;
      insert_index = md_index;
    }
  }
  return insert_index;
}

static const Panel *ghost_panel_find(const ARegion &region)
{
  for (const Panel &panel : region.panels) {
    if (panel.type && STREQ(panel.type->idname, MODIFIER_DROP_GHOST_PANEL_IDNAME)) {
      return &panel;
    }
  }
  return nullptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Drag Type Detection
 * \{ */

/** Local geometry node groups, or node group assets flagged to be used as modifiers. */
static bool drag_is_geometry_nodes_modifier(const wmDrag &drag)
{
  if (drag.type == WM_DRAG_ID) {
    const bNodeTree *node_tree = reinterpret_cast<const bNodeTree *>(
        WM_drag_get_local_ID(&drag, ID_NT));
    return node_tree && node_tree->type == NTREE_GEOMETRY;
  }

  if (drag.type == WM_DRAG_ASSET) {
    const wmDragAsset *asset_data = WM_drag_get_asset_data(&drag, ID_NT);
    if (!asset_data) {
      return false;
    }
    /* Same filter as the "Add Modifier" asset menus, see `add_modifier_assets.cc`. */
    const AssetMetaData &metadata = asset_data->asset->get_metadata();
    const IDProperty *tree_type = BKE_asset_metadata_idprop_find(&metadata, "type");
    if (!tree_type || IDP_int_get(tree_type) != NTREE_GEOMETRY) {
      return false;
    }
    const IDProperty *traits_flag = BKE_asset_metadata_idprop_find(
        &metadata, "geometry_node_asset_traits_flag");
    return traits_flag && (IDP_int_get(traits_flag) & GEO_NODE_ASSET_MODIFIER);
  }

  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Drop-Box Callbacks
 * \{ */

static bool modifier_drop_poll(bContext *C, wmDrag *drag, const wmEvent *event)
{
  const SpaceProperties *sbuts = CTX_wm_space_properties(C);
  if (!sbuts || sbuts->mainb != BCONTEXT_MODIFIER) {
    return false;
  }
  const ARegion *region = CTX_wm_region(C);
  if (!region || region->regiontype != RGN_TYPE_WINDOW) {
    return false;
  }
  if (!drag_is_geometry_nodes_modifier(*drag)) {
    return false;
  }
  /* Same object and conditions as the operator, so no placeholder is shown for a drop that would
   * fail (linked data, object types without modifier support, ...). */
  const Object *object = ed::object::context_active_object(C);
  if (!ED_operator_object_active_editable_ex(C, object) ||
      !BKE_object_support_modifier_type_check(object, eModifierType_Nodes))
  {
    return false;
  }

  if (event) {
    const float view_y = ui::view2d_region_to_view_y(
        &region->v2d, float(event->xy[1] - region->winrct.ymin));
    const int insert_index = insert_index_at_y(*region, *object, view_y);
    if (region != g_drop_state.region || insert_index != g_drop_state.insert_index) {
      drop_state_set({region, insert_index});
      /* Rebuilds the modifier panels with the placeholder at its new position. */
      ED_region_tag_redraw(const_cast<ARegion *>(region));
    }
  }
  return true;
}

static void modifier_drop_copy(bContext *C, wmDrag *drag, wmDropBox *drop)
{
  ID *id = WM_drag_get_local_ID_or_import_from_asset(C, drag, ID_NT);
  if (!id) {
    return;
  }
  WM_operator_properties_id_lookup_set_from_id(drop->ptr, id);
  /* A local node group was picked explicitly, keep it changeable from the modifier. */
  RNA_boolean_set(drop->ptr, "show_datablock_selector", drag->type == WM_DRAG_ID);
  RNA_int_set(drop->ptr, "insert_index", g_drop_state.insert_index);
}

static void modifier_drop_on_exit(wmDropBox * /*drop*/, wmDrag * /*drag*/)
{
  drop_state_set({});
}

static std::string modifier_drop_tooltip(bContext * /*C*/,
                                         wmDrag *drag,
                                         const int /*xy*/[2],
                                         wmDropBox * /*drop*/)
{
  return fmt::format(fmt::runtime(TIP_("Add modifier \"{}\"")), WM_drag_get_item_name(drag));
}

/** Scroll the stack while the cursor is near the top or bottom of the region. */
static void modifier_drop_scroll_at_borders(bContext *C, wmDropBox &drop, const wmEvent *event)
{
  if (!ELEM(event->type, MOUSEMOVE, TIMER)) {
    return;
  }
  ARegion *region = CTX_wm_region(C);
  wmWindowManager *wm = CTX_wm_manager(C);
  wmWindow *win = CTX_wm_window(C);
  if (region == nullptr) {
    return;
  }

  const float margin = float(UI_UNIT_Y);
  float scroll_y = 0.0f;
  if (float(event->xy[1]) > float(region->winrct.ymax) - margin) {
    scroll_y = 0.5f * UI_UNIT_Y;
  }
  else if (float(event->xy[1]) < float(region->winrct.ymin) + margin) {
    scroll_y = -0.5f * UI_UNIT_Y;
  }

  if (scroll_y == 0.0f) {
    if (drop.timer) {
      WM_event_timer_remove(wm, win, drop.timer);
      drop.timer = nullptr;
    }
    return;
  }
  if (drop.timer == nullptr) {
    drop.timer = WM_event_timer_add(wm, win, TIMER, 0.05);
    return;
  }
  if (event->type != TIMER) {
    return;
  }

  View2D *v2d = &region->v2d;
  BLI_rctf_translate(&v2d->cur, 0.0f, scroll_y);
  ui::view2d_curRect_validate(v2d);
  ED_region_tag_redraw(region);
}

/** Accent line through the placeholder panel, which the plain panel doesn't make obvious enough. */
static void modifier_drop_draw_in_view(bContext *C,
                                       wmWindow * /*win*/,
                                       wmDrag * /*drag*/,
                                       const int /*xy*/[2])
{
  const ARegion *region = CTX_wm_region(C);
  if (region == nullptr || region != g_drop_state.region) {
    return;
  }
  const Panel *ghost_panel = ghost_panel_find(*region);
  if (ghost_panel == nullptr) {
    return;
  }
  float bottom, top;
  panel_bounds_y(*ghost_panel, &bottom, &top);
  const float line_y = ui::view2d_view_to_region_y(&region->v2d, top);
  const float half_width = U.pixelsize;

  GPU_matrix_push();
  GPU_matrix_push_projection();
  wmOrtho2_region_pixelspace(region);

  float color[4];
  ui::theme::get_color_4fv(TH_TEXT_HI, color);
  color[3] = 0.8f;

  GPU_blend(GPU_BLEND_ALPHA);
  const uint pos = GPU_vertformat_attr_add(
      immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
  immUniformColor4fv(color);
  immRectf(pos,
           UI_PANEL_MARGIN_X,
           line_y - half_width,
           float(BLI_rcti_size_x(&region->winrct)) - UI_PANEL_MARGIN_X,
           line_y + half_width);
  immUnbindProgram();
  GPU_blend(GPU_BLEND_NONE);

  GPU_matrix_pop_projection();
  GPU_matrix_pop();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public API
 * \{ */

std::optional<int> ED_buttons_modifier_drop_insert_index(const ARegion *region)
{
  if (region == nullptr || region != g_drop_state.region) {
    return std::nullopt;
  }
  return g_drop_state.insert_index;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Registration
 * \{ */

static void modifier_drop_ghost_panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &row = panel->layout->row(false);
  row.alignment_set(ui::LayoutAlign::Center);
  row.label(IFACE_("Drop Modifier Here"), ICON_ADD);
}

void buttons_modifier_drop_ghost_panel_register(ARegionType *art)
{
  PanelType *pt = MEM_new_zeroed<PanelType>(__func__);
  STRNCPY_UTF8(pt->idname, MODIFIER_DROP_GHOST_PANEL_IDNAME);
  STRNCPY_UTF8(pt->label, N_("Insert Modifier"));
  STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->flag = PANEL_TYPE_NO_HEADER | PANEL_TYPE_INSTANCED;
  pt->draw = modifier_drop_ghost_panel_draw;
  BLI_addtail(&art->paneltypes, pt);
}

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
  drop->on_exit = modifier_drop_on_exit;
  drop->on_event_while_hover = modifier_drop_scroll_at_borders;
  drop->draw_in_view = modifier_drop_draw_in_view;
}

/** \} */

}  // namespace blender
