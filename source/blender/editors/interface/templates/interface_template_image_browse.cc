/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Popover-based Image ID browser with paint-slot filters and a grid/list view.
 */

#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_main.hh"
#include "BKE_screen.hh"

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"

#include "BLT_translation.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_space_types.h"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "UI_grid_view.hh"
#include "UI_interface.hh"
#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "WM_api.hh"

#include "interface_intern.hh"
#include "interface_templates_intern.hh"

namespace blender::ui {

/* Search string for the popover's semi-modal filter field. Session-only; need not persist. */
static char g_image_browser_search[256] = "";

/** Minimum height of the scrollable image grid area inside the popover (in #UI_UNIT_Y). */
static constexpr float IMAGE_BROWSER_GRID_VIEWPORT_UNITS_Y = 15.0f;
/** Popover content width (in #UI_UNIT_X). List rows and grid columns use this. */
static constexpr float IMAGE_BROWSER_POPOVER_UNITS_X = 15.0f;

/* -------------------------------------------------------------------- */
/** \name Popover registration
 * \{ */

static void image_browser_popover_draw(const bContext *C, Panel *panel);
static bool image_browser_popover_poll(const bContext *C, PanelType *panel_type);
static void build_image_grid(const bContext &C, Layout &layout);

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
    label_col.label(label, ICON_NONE);

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

static void build_image_grid(const bContext &C, Layout &layout)
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

  view->set_min_viewport_height(int(UI_UNIT_Y * IMAGE_BROWSER_GRID_VIEWPORT_UNITS_Y));

  Block *block = layout.block();
  AbstractGridView *grid_view = block_add_view(*block, "image browser view", std::move(view));

  std::optional<StringRef> filter_str;
  if (g_image_browser_search[0] != '\0') {
    char search[sizeof(g_image_browser_search) + 2];
    BLI_strncpy_ensure_pad(search, g_image_browser_search, '*', sizeof(search));
    filter_str = search;
  }

  GridViewBuilder builder(*block);
  builder.build_grid_view(C, *grid_view, layout, filter_str);
  grid_view->scroll_active_into_view(const_cast<bContext *>(&C));
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

  const bool has_material = CTX_data_pointer_get(C, "image_browser_material").data != nullptr;

  Layout &filter_row = layout.row(true);
  filter_row.prop_enum(&space_ptr, "image_filter_mode", "ALL", "", ICON_NONE);
  Layout &mat_sub = filter_row.row(true);
  mat_sub.active_set(has_material);
  mat_sub.prop_enum(&space_ptr, "image_filter_mode", "CURRENT_MATERIAL", "", ICON_NONE);
  filter_row.prop_enum(&space_ptr, "image_filter_mode", "SLOT_TYPE", "", ICON_NONE);
  Layout &both_sub = filter_row.row(true);
  both_sub.active_set(has_material);
  both_sub.prop_enum(
      &space_ptr, "image_filter_mode", "CURRENT_MATERIAL_AND_SLOT_TYPE", "", ICON_NONE);

  const int mode = RNA_enum_get(&space_ptr, "image_filter_mode");
  if (mode & TEMPLATE_ID_FILTER_SLOT_TYPE) {
    layout.prop(&space_ptr, "image_filter_slot_type", eUI_Item_Flag(0), "", ICON_NONE);
  }

  Layout &tools = layout.row(true);
  tools.prop(&space_ptr, "image_browser_view_mode", ITEM_R_EXPAND, "", ICON_NONE);

  Block *tools_block = tools.block();
  Button *search_but = uiDefBut(tools_block,
                                ButtonType::Text,
                                "",
                                0,
                                0,
                                UI_UNIT_X * 10,
                                UI_UNIT_Y,
                                g_image_browser_search,
                                0.0f,
                                float(sizeof(g_image_browser_search)),
                                TIP_("Filter by name"));
  button_flag2_enable(search_but, BUT2_FORCE_SEMI_MODAL_ACTIVE);
  /* Live filter while typing (same as asset shelf search_filter with PROP_TEXTEDIT_UPDATE). */
  button_flag_enable(search_but, BUT_TEXTEDIT_UPDATE);

  build_image_grid(*C, layout);
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
