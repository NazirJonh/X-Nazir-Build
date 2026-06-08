/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Popover-based Image ID browser with paint-slot filters and a grid/list view.
 */

#include <algorithm>

#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_main.hh"
#include "BKE_screen.hh"

#include "BLI_listbase.h"
#include "BLI_rect.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"

#include "BLT_translation.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_space_types.h"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "ED_screen.hh"

#include "UI_grid_view.hh"
#include "UI_interface.hh"
#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "interface_intern.hh"
#include "interface_templates_intern.hh"

namespace blender::ui {

/* Search string for the popover's semi-modal filter field. Session-only; need not persist. */
static char g_image_browser_search[256] = "";

/**
 * Height of the scrollable image grid viewport (in #UI_UNIT_Y).
 * Keep header rows + this value within the popover #PopupBlockHandle::max_size_y (~16 units).
 */
static constexpr float IMAGE_BROWSER_GRID_VIEWPORT_UNITS_Y = 18.0f;
/** Popover content width (in #UI_UNIT_X). List rows and grid columns use this. */
static constexpr float IMAGE_BROWSER_POPOVER_UNITS_X = 15.0f;

/* -------------------------------------------------------------------- */
/** \name Popover registration
 * \{ */

static void image_browser_popover_draw(const bContext *C, Panel *panel);
static bool image_browser_popover_poll(const bContext *C, PanelType *panel_type);
static void build_image_grid(const bContext &C, Layout &layout, float grid_viewport_units);

static void image_browser_popover_register()
{
  if (WM_paneltype_find("UI_PT_image_browser", true)) {
    return;
  }
  PanelType *pt = MEM_new_zeroed<PanelType>(__func__);
  STRNCPY_UTF8(pt->idname, "UI_PT_image_browser");
  STRNCPY_UTF8(pt->label, N_("Image Browser"));
  STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->description = N_("Browse and assign an image with paint-slot filters");
  pt->draw = image_browser_popover_draw;
  pt->poll = image_browser_popover_poll;
  WM_paneltype_add(pt);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Helpers
 * \{ */

static SpaceLink *image_browser_active_space(const bContext *C, StructRNA **r_srna)
{
  SpaceLink *sl = CTX_wm_space_data(C);
  *r_srna = nullptr;
  if (sl == nullptr) {
    return nullptr;
  }
  if (sl->spacetype == SPACE_IMAGE) {
    *r_srna = RNA_SpaceImageEditor;
    return sl;
  }
  if (sl->spacetype == SPACE_NODE) {
    *r_srna = RNA_SpaceNodeEditor;
    return sl;
  }
  if (sl->spacetype == SPACE_VIEW3D) {
    *r_srna = RNA_SpaceView3D;
    return sl;
  }
  return nullptr;
}

bool image_id_passes_paint_filter(Main &bmain,
                                  const Image &image,
                                  const int filter_mode,
                                  const Material *material,
                                  char slot_type)
{
  if (ELEM(image.type, IMA_TYPE_R_RESULT, IMA_TYPE_COMPOSITE)) {
    return false;
  }
  const bool filter_material = (filter_mode & TEMPLATE_ID_FILTER_CURRENT_MATERIAL) != 0;
  const bool filter_slot = (filter_mode & TEMPLATE_ID_FILTER_SLOT_TYPE) != 0;

  if (filter_material && filter_slot) {
    return material &&
           BKE_image_paint_slot_info_is_used_in_material(&bmain, &image, material, slot_type);
  }
  if (filter_material &&
      (!material || !BKE_image_paint_slot_info_is_used_in_material(&bmain, &image, material, 0)))
  {
    return false;
  }
  if (filter_slot && slot_type != NODE_TEX_IMAGE_SLOT_NONE &&
      !BKE_image_paint_slot_info_has_slot_type(&bmain, &image, slot_type))
  {
    return false;
  }
  return true;
}

class ImageBrowserGridItem : public PreviewGridItem {
  Image *ima_;
  bool list_mode_;

  void install_id_preview_tooltip() const
  {
    Button *item_but = this->view_item_button();
    if (item_but == nullptr) {
      return;
    }
    button_func_tooltip_custom_set(
        item_but,
        [](bContext & /*C*/, TooltipData &tip, Button * /*but*/, void *arg) {
          tooltip_from_id(tip, static_cast<ID *>(arg));
        },
        &ima_->id,
        nullptr);
  }

 public:
  ImageBrowserGridItem(
      StringRef identifier, StringRef label, int preview_icon_id, Image *ima, const bool list_mode)
      : PreviewGridItem(identifier, label, preview_icon_id), ima_(ima), list_mode_(list_mode)
  {
  }

  StringRef get_rename_string() const override
  {
    /* Used by grid-view search filtering; base class returns null. */
    return label;
  }

  void build_grid_tile(const bContext &C, Layout &layout) const override
  {
    if (!list_mode_) {
      PreviewGridItem::build_grid_tile(C, layout);
      this->install_id_preview_tooltip();
      return;
    }

    const GridViewStyle &style = this->get_view().get_style();

    Layout &row = layout.row(true);
    row.alignment_set(LayoutAlign::Expand);

    Layout &icon_col = row.row(true);
    icon_col.fixed_size_set(true);
    icon_col.ui_units_x_set(1.0f);
    icon_col.ui_units_y_set(1.0f);

    Button *icon_but = uiDefBut(icon_col.block(),
                                ButtonType::PreviewTile,
                                "",
                                0,
                                0,
                                UI_UNIT_X,
                                UI_UNIT_X,
                                nullptr,
                                0,
                                0,
                                "");
    def_but_icon(icon_but, preview_icon_id, UI_HAS_ICON | BUT_ICON_PREVIEW);
    icon_but->emboss = EmbossType::None;

    Layout &label_col = row.row(true);
    label_col.alignment_set(LayoutAlign::Expand);
    /* Use uiItemL_ex (returns Button*) so we can set BUT_LIST_ITEM. Without that flag,
     * #widget_state_label uses TH_TEXT (grey) instead of the wcol_list_item theme that
     * #PreviewTile (grid mode) uses, and #layout_list_set_labels_active skips the button. */
    Button *label_but = uiItemL_ex(&label_col, label, ICON_NONE, false, false);
    if (label_but) {
      button_flag_enable(label_but, BUT_LIST_ITEM);
    }

    if (this->is_active()) {
      layout_list_set_labels_active(&row);
    }

    this->install_id_preview_tooltip();

    if (Button *item_but = this->view_item_button()) {
      /* Match highlight area to the full list row (see #AbstractTreeViewItem::add_treerow_button).
       */
      button_view_item_draw_size_set(item_but, style.tile_width, style.tile_height);
    }
  }
};

class ImageBrowserView : public AbstractGridView {
  PointerRNA target_ptr_;
  PropertyRNA *target_prop_;
  Main *bmain_;
  const bContext *context_;
  int mode_;
  const Material *material_;
  char slot_type_;
  bool list_mode_;

 public:
  ImageBrowserView(PointerRNA target_ptr,
                   PropertyRNA *target_prop,
                   Main *bmain,
                   const bContext *C,
                   int mode,
                   const Material *material,
                   char slot_type,
                   const bool list_mode)
      : target_ptr_(target_ptr),
        target_prop_(target_prop),
        bmain_(bmain),
        context_(C),
        mode_(mode),
        material_(material),
        slot_type_(slot_type),
        list_mode_(list_mode)
  {
  }

  void build_items() override
  {
    const PointerRNA active_ptr = RNA_property_pointer_get(&target_ptr_, target_prop_);
    const ID *active_id = active_ptr.data ? static_cast<ID *>(active_ptr.data) : nullptr;

    for (Image &ima : bmain_->images) {
      if (!image_id_passes_paint_filter(*bmain_, ima, mode_, material_, slot_type_)) {
        continue;
      }
      const StringRef name = ima.id.name + 2;
      const int preview_icon = id_icon_get(context_, &ima.id, !list_mode_);
      ImageBrowserGridItem &item = this->add_item<ImageBrowserGridItem>(
          name, name, preview_icon, &ima, list_mode_);

      PointerRNA target_ptr = target_ptr_;
      PropertyRNA *target_prop = target_prop_;
      Image *ima_ptr = &ima;
      item.set_on_activate_fn(
          [target_ptr, target_prop, ima_ptr](bContext &C, PreviewGridItem & /*item*/) {
            PointerRNA value = RNA_id_pointer_create(&ima_ptr->id);
            PointerRNA ptr = target_ptr;
            RNA_property_pointer_set(&ptr, target_prop, value, nullptr);
            RNA_property_update(&C, &ptr, target_prop);
          });
      item.set_is_active_fn(
          [active_id, ima_ptr]() { return active_id != nullptr && &ima_ptr->id == active_id; });
    }
  }
};

/**
 * Header height in block space (filters+view-mode row, optional slot row, search row, plus the
 * ~0.5 #UI_UNIT_Y gap above the grid). Used by #popup_block_grid_view2d_scroll to keep wheel
 * scrolling out of the fixed header. Filters and the view-mode toggle now share one row, so this
 * is reduced by one row versus the separate-rows layout; includes the half-unit gaps before
 * search and before the grid.
 */
static constexpr float IMAGE_BROWSER_HEADER_UNITS_Y = 6.0f;

static AbstractGridView *image_browser_grid_view_from_block(Block &block)
{
  return dynamic_cast<AbstractGridView *>(
      block_view_find_by_idname(block, "image browser view"));
}

bool popup_image_browser_autoscroll_at_pointer(Block *block, const int my)
{
  if (block == nullptr) {
    return false;
  }
  const AbstractGridView *grid_view = image_browser_grid_view_from_block(*block);
  if (grid_view == nullptr) {
    return false;
  }
  return grid_view->fixed_viewport_scroll_at_y(*block, float(my)).has_value();
}

void popup_image_browser_redraw_for_scroll_overlay(ARegion *region, Block *block)
{
  if (region == nullptr || block == nullptr) {
    return;
  }
  const AbstractGridView *grid_view = image_browser_grid_view_from_block(*block);
  if (grid_view == nullptr || !grid_view->use_fixed_viewport_layout() ||
      grid_view->is_fully_visible())
  {
    return;
  }
  ED_region_tag_redraw(region);
}

bool popup_image_browser_scrolltimer_step(bContext * /*C*/,
                                          PopupBlockHandle *menu,
                                          Block *block,
                                          const int my)
{
  if (block == nullptr || menu == nullptr || menu->region == nullptr) {
    return false;
  }
  AbstractGridView *grid_view = image_browser_grid_view_from_block(*block);
  if (grid_view == nullptr || !grid_view->use_fixed_viewport_layout()) {
    return false;
  }

  const std::optional<ViewScrollDirection> scroll_dir = grid_view->fixed_viewport_scroll_at_y(
      *block, float(my));
  if (!scroll_dir) {
    return false;
  }

  /* The scroll position is a row index stored in the grid view; the rebuild triggered below reads
   * it to pick the visible rows. */
  grid_view->scroll(*scroll_dir);
  ED_region_tag_refresh_ui(menu->region);
  return true;
}

bool popup_block_grid_view2d_scroll(bContext * /*C*/, ARegion *region, const wmEvent *event)
{
  if (region == nullptr) {
    return false;
  }

  Block *block = static_cast<Block *>(region->runtime->uiblocks.first);
  if (block == nullptr || (block->flag & BLOCK_POPOVER) == 0) {
    return false;
  }

  AbstractGridView *grid_view = image_browser_grid_view_from_block(*block);
  if (grid_view == nullptr || !grid_view->use_fixed_viewport_layout() ||
      grid_view->is_fully_visible())
  {
    return false;
  }

  /* Don't steal wheel events while hovering the fixed header rows; only the grid scrolls. */
  float mx = float(event->xy[0]);
  float my = float(event->xy[1]);
  window_to_block_fl(region, block, &mx, &my);
  const float header_bottom = block->rect.ymax - UI_UNIT_Y * IMAGE_BROWSER_HEADER_UNITS_Y;
  if (my > header_bottom) {
    return false;
  }

  /* Discretize trackpad pan into whole wheel steps (same accumulation as #view_scroll_invoke);
   * the row-snapped fixed viewport can only move in whole tile rows. */
  int type = event->type;
  bool invert = false;
  if (type == MOUSEPAN) {
    int dummy_val;
    pan_to_scroll(event, &type, &dummy_val);
    /* #pan_to_scroll gives the absolute direction. */
    if (event->flag & WM_EVENT_SCROLL_INVERT) {
      invert = true;
    }
  }

  std::optional<ViewScrollDirection> direction;
  if (type == WHEELUPMOUSE) {
    direction = invert ? ViewScrollDirection::DOWN : ViewScrollDirection::UP;
  }
  else if (type == WHEELDOWNMOUSE) {
    direction = invert ? ViewScrollDirection::UP : ViewScrollDirection::DOWN;
  }
  else {
    return false;
  }

  /* The scroll position is a row index stored in the grid view (#scroll_value_), not the region
   * #View2D. The popup pipeline re-initializes the region #View2D on every refresh, so it can't
   * hold a stable scroll position. The rebuild triggered below reads the row index to pick the
   * visible rows. */
  grid_view->scroll(*direction);
  ED_region_tag_refresh_ui(region);
  return true;
}

/**
 * Grid viewport height (in #UI_UNIT_Y) for the current popover.
 *
 * The popover layout is built at normal UI scale and only scaled to the spawning button's zoom
 * (#Block::aspect) when it is positioned. In a strongly zoomed node editor (aspect < 1) the
 * fixed-height popover would blow up past the window edge, turning the whole block menu-scrollable
 * and dragging the fixed header (filters/search) out of view. Shrink the grid so the final popover
 * still fits the available vertical space; the grid keeps its own internal row scrolling.
 *
 * \a non_grid_units: header rows and gaps consumed before/after the grid (kept in sync with the
 * layout in #image_browser_popover_draw). \a tile_units: one tile row height in #UI_UNIT_Y; the
 * result is snapped to whole rows so the viewport shows complete rows with no trailing gap.
 */
static float image_browser_grid_viewport_units(const bContext *C,
                                               const Block *block,
                                               const float non_grid_units,
                                               const float tile_units)
{
  const float default_units = IMAGE_BROWSER_GRID_VIEWPORT_UNITS_Y;
  if (block == nullptr || block->handle == nullptr) {
    return default_units;
  }
  const Button *but = block->handle->popup_create_vars.but;
  ARegion *butregion = block->handle->popup_create_vars.butregion;
  if (but == nullptr || but->block == nullptr || butregion == nullptr) {
    return default_units;
  }
  const float aspect = but->block->aspect;
  if (aspect >= 1.0f) {
    /* Not zoomed in; the full-height popover fits. */
    return default_units;
  }

  /* Vertical space on the roomier side, measured from the button *edge* the popover stacks away
   * from, not its center: the popover grows away from the button, so the center over-counts the
   * available space by half the button height. At high zoom the button is drawn large (scaling with
   * 1/aspect), making that a 1-2 unit error - exactly what overflows the tightly-packed List grid. */
  float bx = BLI_rctf_cent_x(&but->rect);
  float by_bottom = but->rect.ymin;
  float bx_top = bx;
  float by_top = but->rect.ymax;
  block_to_window_fl(butregion, but->block, &bx, &by_bottom);
  block_to_window_fl(butregion, but->block, &bx_top, &by_top);
  const auto win_size = WM_window_native_pixel_size(CTX_wm_window(C));
  float avail_px = std::max(by_bottom, float(win_size[1]) - by_top);

  /* Chrome the popover wraps around the content when positioned, all scaling with 1/aspect like the
   * layout: the arrow hint (~0.5 #widget_unit, #block_translate in #popup_block_create) plus the
   * block bounds padding on both sides (#block_margin = widget_unit/2 each, see
   * #scrollmin/#scrollmax) ≈ 1.5 widget_unit, plus a fixed screen-edge margin. Subtracting it keeps
   * the final block inside the window so it never turns menu-scrollable, which would otherwise drag
   * the fixed header off screen while the grid scrolls. #List rows snap to ~1 unit and fill the
   * budget tightly, so unlike the coarser ~3-unit #Grid rows they leave no slack to absorb an
   * under-estimate - keep this conservative. */
  avail_px -= 1.5f * float(UI_UNIT_Y) / aspect + float(UI_UNIT_Y) * 1.5f;

  /* popover_pixels = units * UI_UNIT_Y / aspect  →  units = pixels * aspect / UI_UNIT_Y. */
  const float budget_units = avail_px * aspect / float(UI_UNIT_Y);
  const float tile = std::max(tile_units, 0.001f);
  /* Whole tile rows that fit after the fixed header/gaps; never drop below a single row. */
  const int rows = std::max(1, int((budget_units - non_grid_units) / tile));
  return std::clamp(float(rows) * tile, tile, default_units);
}

static void build_image_grid(const bContext &C, Layout &layout, const float grid_viewport_units)
{
  PointerRNA target_ptr = CTX_data_pointer_get(&C, "image_browser_ptr");
  const std::optional<StringRefNull> prop_name = CTX_data_string_get(&C, "image_browser_prop");
  if (target_ptr.data == nullptr || !prop_name) {
    return;
  }
  PropertyRNA *target_prop = RNA_struct_find_property(&target_ptr, prop_name->c_str());
  if (!target_prop) {
    return;
  }

  StructRNA *srna = nullptr;
  SpaceLink *sl = image_browser_active_space(&C, &srna);
  if (sl == nullptr) {
    return;
  }
  bScreen *screen = CTX_wm_screen(&C);
  PointerRNA space_ptr = RNA_pointer_create_discrete(&screen->id, srna, sl);

  int mode = RNA_enum_get(&space_ptr, "image_filter_mode");
  char slot_type = char(RNA_enum_get(&space_ptr, "image_filter_slot_type"));
  const PointerRNA mat_ptr = CTX_data_pointer_get(&C, "image_browser_material");
  const Material *material = static_cast<const Material *>(mat_ptr.data);

  if (material == nullptr && (mode & TEMPLATE_ID_FILTER_CURRENT_MATERIAL)) {
    mode = TEMPLATE_ID_FILTER_ALL;
  }

  const int view_mode = RNA_enum_get(&space_ptr, "image_browser_view_mode");
  const bool list_mode = view_mode == IMAGE_BROWSER_VIEW_LIST;

  Main *bmain = CTX_data_main(&C);
  std::unique_ptr<ImageBrowserView> view = std::make_unique<ImageBrowserView>(
      target_ptr, target_prop, bmain, &C, mode, material, slot_type, list_mode);

  if (list_mode) {
    /* One item per row; width matches the popover so selection covers the full row. */
    const float units_x = layout.ui_units_x() > 0.0f ? layout.ui_units_x() :
                                                       IMAGE_BROWSER_POPOVER_UNITS_X;
    view->set_tile_size(int(units_x * UI_UNIT_X), UI_UNIT_X);
  }
  else {
    view->set_tile_size(UI_UNIT_X * 3, UI_UNIT_Y * 3);
  }

  view->set_min_viewport_height(int(UI_UNIT_Y * grid_viewport_units));
  view->set_fixed_viewport_layout(true);

  Block *block = layout.block();

  std::optional<StringRef> filter_str;
  if (g_image_browser_search[0] != '\0') {
    char search[sizeof(g_image_browser_search) + 2];
    BLI_strncpy_ensure_pad(search, g_image_browser_search, '*', sizeof(search));
    filter_str = search;
  }

  AbstractGridView *grid_view = block_add_view(*block, "image browser view", std::move(view));

  /* True only on the popover's initial build. On a refresh (#ED_region_tag_refresh_ui fires on
   * every scroll step) the region already has the old block, so #Block::oldblock is non-null.
   * On first open the region is fresh and #uiblocks is empty → #oldblock is null. */
  const bool first_open = block->oldblock == nullptr;

  /* Scroll the currently assigned image into view only when the popover first opens. The grid
   * view defers this until its build (when the column count is known) and applies it as a row
   * offset. A refresh fires on each scroll step; re-centering on every refresh would fight the
   * user's scrolling and clamp the view to the active item's row, making the last rows
   * unreachable. */
  if (first_open) {
    grid_view->scroll_active_into_view(const_cast<bContext *>(&C));
  }

  GridViewBuilder builder(*block);
  builder.build_grid_view(C, *grid_view, layout, filter_str);
}

static bool image_browser_popover_poll(const bContext *C, PanelType * /*panel_type*/)
{
  StructRNA *srna = nullptr;
  return image_browser_active_space(C, &srna) != nullptr;
}

static void image_browser_popover_draw(const bContext *C, Panel *panel)
{
  StructRNA *srna = nullptr;
  SpaceLink *sl = image_browser_active_space(C, &srna);
  if (sl == nullptr) {
    return;
  }
  bScreen *screen = CTX_wm_screen(C);
  PointerRNA space_ptr = RNA_pointer_create_discrete(&screen->id, srna, sl);

  Layout &layout = *panel->layout;
  layout.ui_units_x_set(IMAGE_BROWSER_POPOVER_UNITS_X);

  Layout &header = layout.column(true);
  header.fixed_size_set(true);

  const bool has_material = CTX_data_pointer_get(C, "image_browser_material").data != nullptr;

  /* Filters on the left, view-mode toggle pushed to the right of the same row (saves one header
   * row vs. a dedicated view-mode row). #separator_spacer is unsupported in popups, so split the
   * row and right-align the view-mode group. */
  Layout &filter_row = header.split(0.6f, false);
  Layout &filters = filter_row.row(true);
  filters.prop_enum(&space_ptr, "image_filter_mode", "ALL", "", ICON_NONE);
  Layout &mat_sub = filters.row(true);
  mat_sub.active_set(has_material);
  mat_sub.prop_enum(&space_ptr, "image_filter_mode", "CURRENT_MATERIAL", "", ICON_NONE);
  filters.prop_enum(&space_ptr, "image_filter_mode", "SLOT_TYPE", "", ICON_NONE);
  Layout &both_sub = filters.row(true);
  both_sub.active_set(has_material);
  both_sub.prop_enum(
      &space_ptr, "image_filter_mode", "CURRENT_MATERIAL_AND_SLOT_TYPE", "", ICON_NONE);

  Layout &view_mode_row = filter_row.row(true);
  view_mode_row.alignment_set(LayoutAlign::Right);
  view_mode_row.prop_enum(&space_ptr, "image_browser_view_mode", "GRID", "", ICON_NONE);
  view_mode_row.prop_enum(&space_ptr, "image_browser_view_mode", "LIST", "", ICON_NONE);

  const int mode = RNA_enum_get(&space_ptr, "image_filter_mode");
  if (mode & TEMPLATE_ID_FILTER_SLOT_TYPE) {
    header.prop(&space_ptr, "image_filter_slot_type", eUI_Item_Flag(0), "", ICON_NONE);
  }

  /* #Layout::separator uses 6px*UI_SCALE_FAC steps; convert 0.5 #UI_UNIT_Y to that factor. */
  const float half_unit_gap_factor = (0.5f * UI_UNIT_Y) / (6.0f * UI_SCALE_FAC);
  header.separator(half_unit_gap_factor);

  Layout &search_row = header.row(true);
  Block *search_block = search_row.block();
  Button *search_but = uiDefBut(search_block,
                                ButtonType::Text,
                                "",
                                0,
                                0,
                                UI_UNIT_X * (IMAGE_BROWSER_POPOVER_UNITS_X - 2.0f),
                                UI_UNIT_Y,
                                g_image_browser_search,
                                0.0f,
                                float(sizeof(g_image_browser_search)),
                                TIP_("Filter by name"));
  button_flag2_enable(search_but, BUT2_FORCE_SEMI_MODAL_ACTIVE);
  /* Live filter while typing (same as asset shelf search_filter with PROP_TEXTEDIT_UPDATE). */
  button_flag_enable(search_but, BUT_TEXTEDIT_UPDATE);
  /* Magnifier on the left, matching standard search fields (e.g. tree-view filter). */
  def_but_icon(search_but, ICON_VIEWZOOM, UI_HAS_ICON);
  button_placeholder_set(search_but, IFACE_("Search"));

  /* Empty gap (~0.5 #UI_UNIT_Y) between the search field and the grid; the persistent scroll-up
   * arrow (#AbstractGridView::draw_overlays) is drawn here, clear of the top tiles. */
  layout.separator(half_unit_gap_factor);

  /* Header/gap height consumed before the grid, kept in sync with the layout built above:
   * filters row, optional slot-type row, the 0.5-unit separator, the search row, and the
   * 0.5-unit gaps above and below the grid. */
  const bool slot_row = (mode & TEMPLATE_ID_FILTER_SLOT_TYPE) != 0;
  const float non_grid_units = 1.0f + (slot_row ? 1.0f : 0.0f) + 0.5f + 1.0f + 0.5f + 0.5f;
  const bool list_mode = RNA_enum_get(&space_ptr, "image_browser_view_mode") ==
                         IMAGE_BROWSER_VIEW_LIST;
  const float tile_units = list_mode ? float(UI_UNIT_X) / float(UI_UNIT_Y) : 3.0f;

  /* Shrink the grid when a zoomed popover would otherwise overflow the window (see
   * #image_browser_grid_viewport_units); keeps the fixed header on screen while scrolling. */
  const float grid_units = image_browser_grid_viewport_units(
      C, layout.block(), non_grid_units, tile_units);

  Layout &grid_area = layout.column(true);
  grid_area.ui_units_x_set(IMAGE_BROWSER_POPOVER_UNITS_X);
  grid_area.ui_units_y_set(grid_units);
  grid_area.fixed_size_set(true);

  build_image_grid(*C, grid_area, grid_units);

  /* Matching gap under the grid for the persistent scroll-down arrow. */
  layout.separator(half_unit_gap_factor);
}

void image_browser_add_popover_button(
    Layout &row, const bContext *C, PointerRNA *ptr, const char *propname, Material *material)
{
  image_browser_popover_register();

  row.context_ptr_set("image_browser_ptr", ptr);
  row.context_string_set("image_browser_prop", propname);
  if (material) {
    PointerRNA mat_ptr = RNA_id_pointer_create(&material->id);
    row.context_ptr_set("image_browser_material", &mat_ptr);
  }

  PropertyRNA *prop = RNA_struct_find_property(ptr, propname);
  const StructRNA *type = RNA_property_pointer_type(ptr, prop);
  const int icon = type ? RNA_struct_ui_icon(type) : ICON_IMAGE_DATA;

  row.popover(C, "UI_PT_image_browser", "", icon, PopupAttachDirection::VerticalAlignLeft);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Entry point
 * \{ */

void uiTemplateImageBrowse(Layout *layout,
                           const bContext *C,
                           PointerRNA *ptr,
                           const char *propname,
                           Material *material,
                           const char *newop,
                           const char *openop,
                           const char *unlinkop)
{
  PropertyRNA *prop = RNA_struct_find_property(ptr, propname);
  if (!prop || RNA_property_type(prop) != PROP_POINTER) {
    RNA_warning("Image browse property not found or not a pointer: %s", propname);
    return;
  }

  Layout &row = layout->row(true);
  image_browser_add_popover_button(row, C, ptr, propname, material);
  template_id_image_row_append_standard(C, row, ptr, prop, newop, openop, unlinkop);
}

/** \} */

}  // namespace blender::ui
