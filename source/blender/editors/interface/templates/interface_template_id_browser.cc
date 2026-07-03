/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Popover-based ID browser with a grid/list view, name search and pluggable content filters.
 *
 * Built for texture-paint image selection (paint-slot / material filters, see
 * #image_id_passes_paint_filter) but generic over the browsed ID type: items come from the
 * data-block list of the target pointer property, and scripts can narrow them further with a
 * registered #IDFilterType (see #template_id_browser `filter_type`).
 */

#include <algorithm>

#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_main.hh"
#include "BKE_screen.hh"
#include "BKE_wm_runtime.hh"

#include "BLI_listbase.h"
#include "BLI_rect.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"

#include "BLT_translation.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"

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

/**
 * Height of the scrollable image grid viewport (in #UI_UNIT_Y).
 * Keep header rows + this value within the popover #PopupBlockHandle::max_size_y (~16 units).
 */
static constexpr float ID_BROWSER_GRID_VIEWPORT_UNITS_Y = 18.0f;
/** Popover content width (in #UI_UNIT_X). List rows and grid columns use this. */
static constexpr float ID_BROWSER_POPOVER_UNITS_X = 15.0f;

/* -------------------------------------------------------------------- */
/** \name Popover registration
 * \{ */

static void id_browser_popover_draw(const bContext *C, Panel *panel);
static bool id_browser_popover_poll(const bContext *C, PanelType *panel_type);
static void build_id_grid(const bContext &C, Layout &layout, float grid_viewport_units);

static void id_browser_popover_register()
{
  if (WM_paneltype_find("UI_PT_id_browser", true)) {
    return;
  }
  PanelType *pt = MEM_new_zeroed<PanelType>(__func__);
  STRNCPY_UTF8(pt->idname, "UI_PT_id_browser");
  STRNCPY_UTF8(pt->label, N_("Image Browser"));
  STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->description = N_("Browse and assign an image with paint-slot filters");
  pt->draw = id_browser_popover_draw;
  pt->poll = id_browser_popover_poll;
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

/**
 * Composable predicate deciding which data-blocks the browser shows. The built-in image paint-slot
 * filter and an optional script-defined #IDFilterType are combined (an empty filter passes
 * everything). Generic over ID type, so the same popover serves any pointer property.
 */
struct IDBrowserFilter {
  /** Built-in paint-slot filter mask (#TEMPLATE_ID_FILTER_*); only consulted for #ID_IM. */
  int paint_mode = TEMPLATE_ID_FILTER_ALL;
  const Material *material = nullptr;
  char slot_type = 0;
  /** Optional script-defined filter, resolved from the popover's `filter_type` context. */
  const IDFilterType *custom = nullptr;

  bool passes(const bContext &C, Main &bmain, const ID &id) const
  {
    if (GS(id.name) == ID_IM) {
      if (!image_id_passes_paint_filter(
              bmain, reinterpret_cast<const Image &>(id), paint_mode, material, slot_type))
      {
        return false;
      }
    }
    if (custom != nullptr && !id_filter_type_poll(*custom, C, const_cast<ID &>(id))) {
      return false;
    }
    return true;
  }
};

class IDBrowserGridItem : public PreviewGridItem {
  ID *id_;
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
        id_,
        nullptr);
  }

 public:
  IDBrowserGridItem(
      StringRef identifier, StringRef label, int preview_icon_id, ID *id, const bool list_mode)
      : PreviewGridItem(identifier, label, preview_icon_id), id_(id), list_mode_(list_mode)
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

class IDBrowserView : public AbstractGridView {
  PointerRNA target_ptr_;
  PropertyRNA *target_prop_;
  Main *bmain_;
  const bContext *context_;
  /** Data-block list for the target property's ID type (#which_libbase). */
  ListBaseT<ID> *idlb_;
  IDBrowserFilter filter_;
  bool list_mode_;

 public:
  IDBrowserView(PointerRNA target_ptr,
                PropertyRNA *target_prop,
                Main *bmain,
                const bContext *C,
                ListBaseT<ID> *idlb,
                const IDBrowserFilter &filter,
                const bool list_mode)
      : target_ptr_(target_ptr),
        target_prop_(target_prop),
        bmain_(bmain),
        context_(C),
        idlb_(idlb),
        filter_(filter),
        list_mode_(list_mode)
  {
  }

  void build_items() override
  {
    const PointerRNA active_ptr = RNA_property_pointer_get(&target_ptr_, target_prop_);
    const ID *active_id = active_ptr.data ? static_cast<ID *>(active_ptr.data) : nullptr;

    for (ID &id : *idlb_) {
      if (!filter_.passes(*context_, *bmain_, id)) {
        continue;
      }
      const StringRef name = id.name + 2;
      const int preview_icon = id_icon_get(context_, &id, !list_mode_);
      IDBrowserGridItem &item = this->add_item<IDBrowserGridItem>(
          name, name, preview_icon, &id, list_mode_);

      PointerRNA target_ptr = target_ptr_;
      PropertyRNA *target_prop = target_prop_;
      ID *id_ptr = &id;
      item.set_on_activate_fn(
          [target_ptr, target_prop, id_ptr](bContext &C, PreviewGridItem & /*item*/) {
            PointerRNA value = RNA_id_pointer_create(id_ptr);
            PointerRNA ptr = target_ptr;
            RNA_property_pointer_set(&ptr, target_prop, value, nullptr);
            RNA_property_update(&C, &ptr, target_prop);
          });
      item.set_is_active_fn(
          [active_id, id_ptr]() { return active_id != nullptr && id_ptr == active_id; });
    }
  }
};

static void build_id_grid(const bContext &C, Layout &layout, const float grid_viewport_units)
{
  PointerRNA target_ptr = CTX_data_pointer_get(&C, "id_browser_ptr");
  const std::optional<StringRefNull> prop_name = CTX_data_string_get(&C, "id_browser_prop");
  if (target_ptr.data == nullptr || !prop_name) {
    return;
  }
  PropertyRNA *target_prop = RNA_struct_find_property(&target_ptr, prop_name->c_str());
  if (!target_prop || RNA_property_type(target_prop) != PROP_POINTER) {
    return;
  }

  wmWindowManager *wm = CTX_wm_manager(&C);
  if (wm == nullptr) {
    return;
  }
  PointerRNA wm_ptr = RNA_id_pointer_create(&wm->id);

  /* The browsed items are the data-blocks of the target property's ID type. */
  Main *bmain = CTX_data_main(&C);
  const StructRNA *ptr_type = RNA_property_pointer_type(&target_ptr, target_prop);
  const short idcode = ptr_type ? RNA_type_to_ID_code(ptr_type) : 0;
  ListBaseT<ID> *idlb = (idcode != 0) ? which_libbase(bmain, idcode) : nullptr;
  if (idlb == nullptr) {
    return;
  }

  IDBrowserFilter filter;
  /* Built-in paint-slot filter: only for image targets, and only when an Image/Node editor backs
   * its state. Read straight from the space data so a refresh sees the current toggle values. */
  StructRNA *space_srna = nullptr;
  SpaceLink *sl = (idcode == ID_IM) ? image_browser_active_space(&C, &space_srna) : nullptr;
  if (sl != nullptr) {
    bScreen *screen = CTX_wm_screen(&C);
    if (screen != nullptr) {
      PointerRNA space_ptr = RNA_pointer_create_discrete(&screen->id, space_srna, sl);
      int mode = RNA_enum_get(&space_ptr, "image_filter_mode");
      const PointerRNA mat_ptr = CTX_data_pointer_get(&C, "id_browser_material");
      const Material *material = static_cast<const Material *>(mat_ptr.data);
      if (material == nullptr && (mode & TEMPLATE_ID_FILTER_CURRENT_MATERIAL)) {
        mode = TEMPLATE_ID_FILTER_ALL;
      }
      filter.paint_mode = mode;
      filter.slot_type = char(RNA_enum_get(&space_ptr, "image_filter_slot_type"));
      filter.material = material;
    }
  }
  /* Optional script-defined filter, referenced by name (see #template_id_browser `filter_type`). */
  if (const std::optional<StringRefNull> filter_type_idname = CTX_data_string_get(
          &C, "id_browser_filter_type"))
  {
    filter.custom = id_filter_type_find(*filter_type_idname);
  }

  const bool list_mode = RNA_enum_get(&wm_ptr, "id_browser_view_mode") ==
                         IMAGE_BROWSER_VIEW_LIST;

  std::unique_ptr<IDBrowserView> view = std::make_unique<IDBrowserView>(
      target_ptr, target_prop, bmain, &C, idlb, filter, list_mode);

  if (list_mode) {
    /* One item per row; width matches the popover so selection covers the full row. */
    const float units_x = layout.ui_units_x() > 0.0f ? layout.ui_units_x() :
                                                       ID_BROWSER_POPOVER_UNITS_X;
    view->set_tile_size(int(units_x * UI_UNIT_X), UI_UNIT_X);
  }
  else {
    view->set_tile_size(UI_UNIT_X * 3, UI_UNIT_Y * 3);
  }

  view->set_min_viewport_height(int(UI_UNIT_Y * grid_viewport_units));
  view->set_fixed_viewport_layout(true);

  Block *block = layout.block();

  std::optional<StringRef> filter_str;
  char search_pattern[sizeof(wm->runtime->id_browser_search) + 2];
  if (wm->runtime->id_browser_search[0] != '\0') {
    BLI_strncpy_ensure_pad(
        search_pattern, wm->runtime->id_browser_search, '*', sizeof(search_pattern));
    filter_str = search_pattern;
  }

  AbstractGridView *grid_view = block_add_view(*block, "id browser view", std::move(view));

  /* True only on the popover's initial build. On a refresh (#ED_region_tag_refresh_ui fires on
   * every scroll step) the region already has the old block, so #Block::oldblock is non-null.
   * On first open the region is fresh and #uiblocks is empty → #oldblock is null. */
  const bool first_open = block->oldblock == nullptr;

  /* Scroll the currently assigned data-block into view only when the popover first opens. The grid
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

static bool id_browser_popover_poll(const bContext * /*C*/, PanelType * /*panel_type*/)
{
  /* Available from any editor: the popover is only ever invoked from #template_id_browser, which
   * supplies its target via context, and its UI state lives on the window manager (not on a
   * specific space). The optional paint-slot filters are shown only when an Image/Node editor
   * provides the relevant space (see #id_browser_popover_draw). */
  return true;
}

static void id_browser_popover_draw(const bContext *C, Panel *panel)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  if (wm == nullptr) {
    return;
  }
  /* The popover's own UI state (view mode, search) lives on the window manager, so it works in any
   * editor. */
  PointerRNA wm_ptr = RNA_id_pointer_create(&wm->id);

  /* The built-in paint-slot filters need an Image/Node editor's space to store their state; they
   * are optional. When absent (the popover is used elsewhere) the search, view toggle and any
   * script-defined filter still work. */
  StructRNA *space_srna = nullptr;
  SpaceLink *sl = image_browser_active_space(C, &space_srna);
  bScreen *screen = CTX_wm_screen(C);
  PointerRNA space_ptr = {};
  if (sl != nullptr && screen != nullptr) {
    space_ptr = RNA_pointer_create_discrete(&screen->id, space_srna, sl);
  }

  /* Resolve the target ID type: the paint filters apply to images only. */
  PointerRNA target_ptr = CTX_data_pointer_get(C, "id_browser_ptr");
  const std::optional<StringRefNull> prop_name = CTX_data_string_get(C, "id_browser_prop");
  PropertyRNA *target_prop = (target_ptr.data && prop_name) ?
                                 RNA_struct_find_property(&target_ptr, prop_name->c_str()) :
                                 nullptr;
  bool is_image = false;
  if (target_prop && RNA_property_type(target_prop) == PROP_POINTER) {
    const StructRNA *ptr_type = RNA_property_pointer_type(&target_ptr, target_prop);
    is_image = ptr_type && RNA_type_to_ID_code(ptr_type) == ID_IM;
  }
  /* The paint filters need both an image target and a space to back their state. */
  const bool show_paint_filters = is_image && space_ptr.data != nullptr;

  Layout &layout = *panel->layout;
  layout.ui_units_x_set(ID_BROWSER_POPOVER_UNITS_X);

  Layout &header = layout.column(true);
  header.fixed_size_set(true);

  const bool has_material = CTX_data_pointer_get(C, "id_browser_material").data != nullptr;

  /* Paint filters on the left, view-mode toggle pushed to the right of the same row (saves one
   * header row vs. a dedicated view-mode row). #separator_spacer is unsupported in popups, so split
   * the row and right-align the view-mode group. The paint filters only make sense for images. */
  Layout &filter_row = header.split(0.6f, false);
  Layout &filters = filter_row.row(true);
  if (show_paint_filters) {
    filters.prop_enum(&space_ptr, "image_filter_mode", "ALL", "", ICON_NONE);
    Layout &mat_sub = filters.row(true);
    mat_sub.active_set(has_material);
    mat_sub.prop_enum(&space_ptr, "image_filter_mode", "CURRENT_MATERIAL", "", ICON_NONE);
    filters.prop_enum(&space_ptr, "image_filter_mode", "SLOT_TYPE", "", ICON_NONE);
    Layout &both_sub = filters.row(true);
    both_sub.active_set(has_material);
    both_sub.prop_enum(
        &space_ptr, "image_filter_mode", "CURRENT_MATERIAL_AND_SLOT_TYPE", "", ICON_NONE);
  }

  Layout &view_mode_row = filter_row.row(true);
  view_mode_row.alignment_set(LayoutAlign::Right);
  view_mode_row.prop_enum(&wm_ptr, "id_browser_view_mode", "GRID", "", ICON_NONE);
  view_mode_row.prop_enum(&wm_ptr, "id_browser_view_mode", "LIST", "", ICON_NONE);

  const int mode = show_paint_filters ? RNA_enum_get(&space_ptr, "image_filter_mode") :
                                        TEMPLATE_ID_FILTER_ALL;
  const bool slot_row = (mode & TEMPLATE_ID_FILTER_SLOT_TYPE) != 0;
  if (slot_row) {
    header.prop(&space_ptr, "image_filter_slot_type", eUI_Item_Flag(0), "", ICON_NONE);
  }

  /* #Layout::separator uses 6px*UI_SCALE_FAC steps; convert 0.5 #UI_UNIT_Y to that factor. */
  const float half_unit_gap_factor = (0.5f * UI_UNIT_Y) / (6.0f * UI_SCALE_FAC);
  header.separator(half_unit_gap_factor);

  {
    Layout &search_row = header.row(true);
    Block *search_block = search_row.block();
    Button *search_but = uiDefBut(search_block,
                                  ButtonType::Text,
                                  "",
                                  0,
                                  0,
                                  UI_UNIT_X * (ID_BROWSER_POPOVER_UNITS_X - 2.0f),
                                  UI_UNIT_Y,
                                  wm->runtime->id_browser_search,
                                  0.0f,
                                  float(sizeof(wm->runtime->id_browser_search)),
                                  TIP_("Filter by name"));
    button_flag2_enable(search_but, BUT2_FORCE_SEMI_MODAL_ACTIVE);
    /* Live filter while typing (same as asset shelf search_filter with PROP_TEXTEDIT_UPDATE). */
    button_flag_enable(search_but, BUT_TEXTEDIT_UPDATE);
    /* Magnifier on the left, matching standard search fields (e.g. tree-view filter). */
    def_but_icon(search_but, ICON_VIEWZOOM, UI_HAS_ICON);
    button_placeholder_set(search_but, IFACE_("Search"));
  }

  /* Empty gap (~0.5 #UI_UNIT_Y) between the search field and the grid; the persistent scroll-up
   * arrow (#AbstractGridView::draw_overlays) is drawn here, clear of the top tiles. */
  layout.separator(half_unit_gap_factor);

  /* Header/gap height consumed before the grid, kept in sync with the layout built above:
   * filters row, optional slot-type row, the 0.5-unit separator, the search row, and the
   * 0.5-unit gaps above and below the grid. */
  const float non_grid_units = 1.0f + (slot_row ? 1.0f : 0.0f) + 0.5f + 1.0f + 0.5f + 0.5f;
  const bool list_mode = RNA_enum_get(&wm_ptr, "id_browser_view_mode") ==
                         IMAGE_BROWSER_VIEW_LIST;
  const float tile_units = list_mode ? float(UI_UNIT_X) / float(UI_UNIT_Y) : 3.0f;

  /* Shrink the grid when a zoomed popover would otherwise overflow the window (see
   * #id_browser_grid_viewport_units); keeps the fixed header on screen while scrolling. */
  const float grid_units = popup_grid_fixed_viewport_units(
      C, layout.block(), non_grid_units, tile_units, ID_BROWSER_GRID_VIEWPORT_UNITS_Y);

  Layout &grid_area = layout.column(true);
  grid_area.ui_units_x_set(ID_BROWSER_POPOVER_UNITS_X);
  grid_area.ui_units_y_set(grid_units);
  grid_area.fixed_size_set(true);

  build_id_grid(*C, grid_area, grid_units);

  /* Matching gap under the grid for the persistent scroll-down arrow. */
  layout.separator(half_unit_gap_factor);
}

void id_browser_add_popover_button(Layout &row,
                                      const bContext *C,
                                      PointerRNA *ptr,
                                      const char *propname,
                                      Material *material,
                                      const char *filter_type)
{
  id_browser_popover_register();

  row.context_ptr_set("id_browser_ptr", ptr);
  row.context_string_set("id_browser_prop", propname);
  if (material) {
    PointerRNA mat_ptr = RNA_id_pointer_create(&material->id);
    row.context_ptr_set("id_browser_material", &mat_ptr);
  }
  if (filter_type && filter_type[0] != '\0') {
    /* Read back in #build_id_grid via #id_filter_type_find. */
    row.context_string_set("id_browser_filter_type", filter_type);
  }

  PropertyRNA *prop = RNA_struct_find_property(ptr, propname);
  const StructRNA *type = RNA_property_pointer_type(ptr, prop);
  const int icon = type ? RNA_struct_ui_icon(type) : ICON_IMAGE_DATA;

  row.popover(C, "UI_PT_id_browser", "", icon, PopupAttachDirection::VerticalAlignLeft);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Entry point
 * \{ */

void template_id_browser(Layout *layout,
                         const bContext *C,
                         PointerRNA *ptr,
                         const char *propname,
                         Material *material,
                         const char *newop,
                         const char *openop,
                         const char *unlinkop,
                         const char *filter_type)
{
  PropertyRNA *prop = RNA_struct_find_property(ptr, propname);
  if (!prop || RNA_property_type(prop) != PROP_POINTER) {
    RNA_warning("Image browse property not found or not a pointer: %s", propname);
    return;
  }

  Layout &row = layout->row(true);
  id_browser_add_popover_button(row, C, ptr, propname, material, filter_type);
  template_id_image_row_append_standard(C, row, ptr, prop, newop, openop, unlinkop);
}

/** \} */

}  // namespace blender::ui
