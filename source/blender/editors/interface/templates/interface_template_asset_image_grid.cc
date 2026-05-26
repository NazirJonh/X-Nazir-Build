/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include "UI_interface_c.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_texture_types.h"
#include "DNA_userdef_types.h"
#include "DNA_view2d_types.h"
#include "DNA_view3d_types.h"

#include "AS_asset_catalog_path.hh"
#include "AS_asset_library.hh"
#include "AS_asset_representation.hh"

#include "BLI_math_base.h"

#include "IMB_imbuf.hh"

#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_icons.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_preview_image.hh"
#include "BKE_texture.h"
#include "BKE_global.hh"
#include "BKE_idtype.hh"
#include "BKE_screen.hh"

#include "BLI_set.hh"
#include "BLT_translation.hh"

#include "ED_asset.hh"
#include "ED_asset_library.hh"
#include "ED_asset_list.hh"
#include "ED_render.hh"
#include "ED_screen.hh"
#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "UI_grid_view.hh"
#include "UI_interface.hh"
#include "UI_interface_icons.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "UI_view2d.hh"
#include "BLI_rect.h"

#include "interface_intern.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_view3d.hh"

namespace blender::ui {

/** Safety cap for N-panel performance; not the old MVP `rows * cols` display limit. */
constexpr int IMAGE_GRID_MAX_ITEMS = 128;

/* -------------------------------------------------------------------- */
/** \name Helpers
 * \{ */

static std::optional<asset_system::AssetCatalogFilter> catalog_filter_from_path(
    const asset_system::AssetLibrary &library, const StringRefNull catalog_path)
{
  if (catalog_path.is_empty()) {
    return std::nullopt;
  }

  const char *const path_cstr = catalog_path.c_str();
  asset_system::AssetCatalog *active_catalog = library.catalog_service().find_catalog_by_path(
      path_cstr);
  if (!active_catalog) {
    return std::nullopt;
  }

  return library.catalog_service().create_catalog_filter(active_catalog->catalog_id);
}

/** Textures assignable from the grid (exclude render result, viewer, generated). */
static bool image_grid_is_assignable_texture(const Image &image)
{
  if (ELEM(image.source, IMA_SRC_VIEWER, IMA_SRC_GENERATED)) {
    return false;
  }
  if (ELEM(image.type, IMA_TYPE_R_RESULT, IMA_TYPE_COMPOSITE)) {
    return false;
  }
  return true;
}


static void image_grid_block_listener(const wmRegionListenerParams *params)
{
  const wmNotifier *wmn = params->notifier;
  switch (wmn->category) {
    case NC_ASSET:
      if (ELEM(wmn->data,
               int(ND_ASSET_LIST),
               int(ND_ASSET_LIST_READING),
               int(ND_ASSET_LIST_PREVIEW)))
      {
        ED_region_tag_redraw(params->region);
        ED_region_tag_refresh_ui(params->region);
      }
      break;
    case NC_ID:
      if (ELEM(wmn->action, NA_EDITED, NA_RENAME)) {
        ED_region_tag_redraw(params->region);
      }
      break;
    default:
      break;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Brush texture assignment
 * \{ */

/**
 * BrushTextureSlot.texture is a #Tex (ID_TE), while the grid lists #Image (ID_IM) assets.
 * Find or create an image texture datablock for assignment.
 */
static Tex *brush_texture_for_image(Main &bmain, Image &image, const ID *owner_id)
{
  ID *id;
  FOREACH_MAIN_ID_BEGIN (&bmain, id) {
    if (GS(id->name) != ID_TE) {
      continue;
    }
    Tex *tex = reinterpret_cast<Tex *>(id);
    if (tex->type == TEX_IMAGE && tex->ima == &image) {
      return tex;
    }
  }
  FOREACH_MAIN_ID_END;

  Tex *tex = BKE_texture_add(&bmain, image.id.name + 2);
  BKE_texture_type_set(tex, TEX_IMAGE);
  id_us_plus(&image.id);
  tex->ima = &image;

  if (owner_id) {
    BKE_id_move_to_same_lib(bmain, tex->id, *owner_id);
  }

  return tex;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Grid Item
 * \{ */

enum class ImageGridItemKind { Asset, BlendImage };

class ImageAssetGridItem : public PreviewGridItem {
  ImageGridItemKind kind_;
  asset_system::AssetRepresentation *asset_ = nullptr;
  Image *image_ = nullptr;
  mutable PointerRNA target_ptr_;
  PropertyRNA *target_prop_ = nullptr;
  AssetLibraryReference library_ref_;
  /** Preview icon for on-disk image assets without a local ID (not in #G_MAIN). */
  mutable int external_preview_icon_id_ = 0;

 public:
  ImageAssetGridItem(asset_system::AssetRepresentation &asset,
                     const PointerRNA &target_ptr,
                     PropertyRNA *target_prop,
                     const AssetLibraryReference &library_ref)
      : PreviewGridItem(asset.library_relative_identifier(), asset.get_name(), ICON_NONE),
        kind_(ImageGridItemKind::Asset),
        asset_(&asset),
        target_ptr_(target_ptr),
        target_prop_(target_prop),
        library_ref_(library_ref)
  {
    this->init_item_callbacks();
  }

  ImageAssetGridItem(Image &image,
                     const PointerRNA &target_ptr,
                     PropertyRNA *target_prop,
                     const AssetLibraryReference &library_ref)
      : PreviewGridItem(image.id.name + 2, image.id.name + 2, ICON_NONE),
        kind_(ImageGridItemKind::BlendImage),
        image_(&image),
        target_ptr_(target_ptr),
        target_prop_(target_prop),
        library_ref_(library_ref)
  {
    this->init_item_callbacks();
  }

  ~ImageAssetGridItem() override
  {
    if (external_preview_icon_id_) {
      BKE_icon_delete(external_preview_icon_id_);
      external_preview_icon_id_ = 0;
    }
  }

  void init_item_callbacks()
  {
    this->hide_label();
    this->always_reactivate_on_click();
    this->set_on_activate_fn([this](bContext &C, PreviewGridItem & /*item*/) {
      Image *image = nullptr;
      if (kind_ == ImageGridItemKind::BlendImage) {
        image = image_;
      }
      else if (ID *local_id = asset_->local_id()) {
        image = id_cast<Image *>(local_id);
      }
      else {
        image = BKE_image_load(CTX_data_main(&C), asset_->full_path().c_str());
      }
      if (!image || GS(image->id.name) != ID_IM) {
        return;
      }

      Main *bmain = CTX_data_main(&C);
      Tex *tex = brush_texture_for_image(*bmain, *image, target_ptr_.owner_id);

      PointerRNA tex_ptr = RNA_id_pointer_create(&tex->id);
      RNA_property_pointer_set(&target_ptr_, target_prop_, tex_ptr, nullptr);
      RNA_property_update(&C, &target_ptr_, target_prop_);
    });
    this->set_is_active_fn([this]() { return this->is_active_texture(); });
  }

  int ensure_external_preview_icon_id() const
  {
    if (external_preview_icon_id_) {
      return external_preview_icon_id_;
    }
    ImBuf *ibuf = IMB_load_image_from_filepath(
        asset_->full_path().c_str(), ImBufFlags::ByteData, nullptr);
    if (!ibuf) {
      return 0;
    }
    external_preview_icon_id_ = BKE_icon_imbuf_create(ibuf);
    IMB_freeImBuf(ibuf);
    return external_preview_icon_id_;
  }

  ID *get_id() const
  {
    if (kind_ == ImageGridItemKind::Asset) {
      return asset_->local_id();
    }
    return &image_->id;
  }

  bool is_active_texture() const
  {
    const PointerRNA active_ptr = RNA_property_pointer_get(&target_ptr_, target_prop_);
    if (!active_ptr.data) {
      return false;
    }

    const ID *image_id = this->get_id();
    if (!image_id) {
      return false;
    }

    if (active_ptr.type && RNA_struct_is_ID(active_ptr.type)) {
      ID *active_id = static_cast<ID *>(active_ptr.data);
      if (GS(active_id->name) == ID_TE) {
        const Tex *tex = reinterpret_cast<const Tex *>(active_id);
        return tex->type == TEX_IMAGE && tex->ima == reinterpret_cast<const Image *>(image_id);
      }
    }

    return active_ptr.data == image_id;
  }

  static int preview_icon_id_for_id(const bContext &C, ID &id)
  {
    if (!ED_preview_id_is_supported(&id)) {
      return ui::icon_from_id(&id);
    }

    const int icon_id = BKE_icon_id_ensure(&id);
    icon_render_id(&C, nullptr, &id, ICON_SIZE_PREVIEW, !G.background);

    if (icon_is_preview_deferred_loading(icon_id, true)) {
      return ICON_PREVIEW_LOADING;
    }

    PreviewImage *preview = BKE_previewimg_id_get(&id);
    if (preview && !BKE_previewimg_is_invalid(preview, ICON_SIZE_PREVIEW)) {
      return BKE_icon_preview_ensure(&id, preview);
    }

    return icon_id ? icon_id : ui::icon_from_id(&id);
  }

  int get_preview_icon_id(const bContext &C) const
  {
    if (kind_ == ImageGridItemKind::Asset) {
      asset_->ensure_previewable(C);
      if (!ed::asset::list::is_loaded(&library_ref_)) {
        return ICON_PREVIEW_LOADING;
      }

      const int asset_icon = ed::asset::asset_preview_or_icon(*asset_);
      if (asset_icon != ICON_NONE && asset_icon != ui::icon_from_idcode(ID_IM)) {
        return asset_icon;
      }

      if (ID *local_id = asset_->local_id()) {
        return preview_icon_id_for_id(C, *local_id);
      }
      const int external_icon = this->ensure_external_preview_icon_id();
      if (external_icon) {
        return external_icon;
      }
      return asset_icon;
    }

    return preview_icon_id_for_id(C, image_->id);
  }

  void build_grid_tile(const bContext &C, Layout &layout) const override
  {
    Button *view_but = reinterpret_cast<Button *>(this->view_item_button());
    if (!view_but) {
      return;
    }

    Block *block = layout.block();

    if (ID *id = this->get_id()) {
      PointerRNA id_ptr = RNA_id_pointer_create(id);
      button_context_ptr_set(block, view_but, "id", &id_ptr);
    }

    const int preview_id = this->get_preview_icon_id(C);
    const GridViewStyle &style = this->get_view().get_style();

    button_view_item_draw_size_set(
        view_but, style.tile_width + 2 * U.pixelsize, style.tile_height + 2 * U.pixelsize);

    /* Match #AssetViewItem::build_grid_tile overlap sizing (fixed tile footprint). */
    Layout &overlap = layout.overlap();
    overlap.fixed_size_set(true);
    overlap.ui_units_x_set(style.tile_width / float(UI_UNIT_X));
    overlap.ui_units_y_set(style.tile_height / float(UI_UNIT_Y));

    PreviewGridItem::build_grid_tile_button(overlap.column(true), preview_id);

    Layout &overlay_row = overlap.row(true);
    overlay_row.alignment_set(LayoutAlign::Right);

    if (kind_ == ImageGridItemKind::BlendImage) {
      wmOperatorType *mark_ot = WM_operatortype_find("VIEW3D_OT_image_grid_mark_asset", true);
      if (mark_ot) {
        /* Icon-only overlay like asset shelf online indicator — not a full #Layout::op button. */
        Button *mark_but = uiItemL_ex(&overlay_row, "", ICON_SOLO_OFF, false, false);
        button_operator_set(mark_but, mark_ot, wm::OpCallContext::ExecDefault, nullptr);
        button_label_alpha_factor_set(mark_but, 0.6f);
        button_label_draw_icon_border_set(mark_but, true);
      }
    }

    button_func_tooltip_custom_set(
        view_but,
        [](bContext & /*C*/, TooltipData &tip, Button * /*but*/, void *argN) {
          const ImageAssetGridItem *item = static_cast<const ImageAssetGridItem *>(argN);
          if (item->kind_ == ImageGridItemKind::Asset) {
            ed::asset::asset_tooltip(*item->asset_, tip);
          }
          else {
            tooltip_text_field_add(
                tip, item->image_->id.name + 2, {}, TIP_STYLE_HEADER, TIP_LC_MAIN);
          }
        },
        const_cast<ImageAssetGridItem *>(this),
        nullptr);
  }

  void build_context_menu(bContext & /*C*/, Layout &layout) const override
  {
    if (kind_ == ImageGridItemKind::BlendImage) {
      layout.op("VIEW3D_OT_image_grid_mark_asset",
                IFACE_("Mark as Asset"),
                ICON_ASSET_MANAGER);
    }
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Grid View
 * \{ */

class ImageAssetGridView : public AbstractGridView {
  const bContext &context_;
  AssetLibraryReference library_ref_;
  std::optional<asset_system::AssetCatalogFilter> catalog_filter_;
  PointerRNA target_ptr_;
  PropertyRNA *target_prop_ = nullptr;

 public:
  ImageAssetGridView(const bContext &context,
                     const AssetLibraryReference &library_ref,
                     std::optional<asset_system::AssetCatalogFilter> catalog_filter,
                     const PointerRNA &target_ptr,
                     PropertyRNA *target_prop)
      : context_(context),
        library_ref_(library_ref),
        catalog_filter_(std::move(catalog_filter)),
        target_ptr_(target_ptr),
        target_prop_(target_prop)
  {
  }

  void build_items() override
  {
    Main *bmain = CTX_data_main(&context_);
    Set<ID *> seen_ids;

    ed::asset::list::storage_fetch(&library_ref_, &context_);

    auto add_asset = [&](asset_system::AssetRepresentation &asset) -> bool {
      if (this->get_item_count() >= IMAGE_GRID_MAX_ITEMS) {
        return false;
      }
      if (asset.get_id_type() != ID_IM) {
        return true;
      }
      if (catalog_filter_ && !catalog_filter_->contains(asset.get_metadata().catalog_id)) {
        return true;
      }

      if (ID *id = asset.local_id()) {
        if (GS(id->name) == ID_IM) {
          const Image *image = reinterpret_cast<const Image *>(id);
          if (!image_grid_is_assignable_texture(*image)) {
            return true;
          }
        }
        if (seen_ids.contains(id)) {
          return true;
        }
        seen_ids.add_new(id);
      }

      this->add_item<ImageAssetGridItem>(asset, target_ptr_, target_prop_, library_ref_);
      return true;
    };

    if (ed::asset::list::library_get_once_available(library_ref_)) {
      ed::asset::list::iterate(library_ref_, [&](asset_system::AssetRepresentation &asset) {
        return add_asset(asset);
      });
    }

    if (library_ref_.type == ASSET_LIBRARY_LOCAL &&
        this->get_item_count() < IMAGE_GRID_MAX_ITEMS)
    {
      ID *id;
      FOREACH_MAIN_ID_BEGIN (bmain, id) {
        if (this->get_item_count() >= IMAGE_GRID_MAX_ITEMS) {
          break;
        }
        if (GS(id->name) != ID_IM) {
          continue;
        }
        if (id->asset_data) {
          continue;
        }
        if (seen_ids.contains(id)) {
          continue;
        }

        Image *image = id_cast<Image *>(id);
        if (!image_grid_is_assignable_texture(*image)) {
          continue;
        }
        seen_ids.add_new(id);

        this->add_item<ImageAssetGridItem>(*image, target_ptr_, target_prop_, library_ref_);
      }
      FOREACH_MAIN_ID_END;
    }
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Template UI
 * \{ */

static void add_browse_more_button(Layout &layout,
                                   bContext & /*C*/,
                                   const AssetLibraryReference &library_ref)
{
  Layout &row = layout.row(false);
  PointerRNA op_ptr = row.op("VIEW3D_OT_image_grid_browse_assets",
                             IFACE_("Browse More"),
                             ICON_FILEBROWSER,
                             wm::OpCallContext::ExecDefault,
                             UI_ITEM_NONE);
  RNA_enum_set(&op_ptr,
               "asset_library_reference",
               ed::asset::library_reference_to_enum_value(&library_ref));
}

static void draw_header_row(Layout &layout,
                            ed::view3d::ImageGridUIState &state,
                            const bContext &C)
{
  Layout &row = layout.row(true);
  /* Library selector: opens enum search to change the active library. */
  row.op("VIEW3D_OT_image_grid_set_library",
         ed::view3d::image_grid_library_ui_name(state.lib_ref),
         ICON_ASSET_MANAGER,
         wm::OpCallContext::InvokeDefault,
         UI_ITEM_NONE);
  /* Catalog selector: opens a popover with library selector + catalog tree. */
  row.popover(&C, "VIEW3D_PT_image_grid_catalog_selector", std::nullopt, ICON_COLLAPSEMENU);
}


static void build_image_grid(Layout &layout,
                             const bContext &C,
                             ed::view3d::ImageGridUIState &state,
                             PointerRNA &ptr,
                             PropertyRNA *prop)
{
  Block *block = layout.block();
  View3D *v3d = CTX_wm_view3d(&C);
  if (!v3d) {
    return;
  }

  ed::view3d::image_grid_clamp_scroll_row(state, *v3d);

  const std::optional<asset_system::AssetCatalogFilter> cat_filter =
      [&]() -> std::optional<asset_system::AssetCatalogFilter> {
    const asset_system::AssetLibrary *library =
        ed::asset::list::library_get_once_available(state.lib_ref);
    if (!library) {
      return std::nullopt;
    }
    return catalog_filter_from_path(*library, state.active_catalog_path);
  }();

  auto view_unique = std::make_unique<ImageAssetGridView>(
      C, state.lib_ref, std::move(cat_filter), ptr, prop);
  view_unique->set_tile_size(3 * UI_UNIT_X, 3 * UI_UNIT_Y);
  AbstractGridView *grid_view = block_add_view(*block, "image_asset_grid", std::move(view_unique));

  const GridViewStyle &style = grid_view->get_style();
  const int tile_h = style.tile_height;

  if (state.grip_pixel_height < tile_h) {
    state.grip_pixel_height = ed::view3d::image_grid_effective_rows(*v3d) * tile_h;
  }
  state.grip_pixel_height = clamp_i(state.grip_pixel_height, tile_h, 16 * tile_h);
  const int effective_rows = clamp_i(
      round_fl_to_int(float(state.grip_pixel_height) / float(tile_h)), 1, 16);
  state.grip_pixel_height = effective_rows * tile_h;
  v3d->image_grid_rows = short(effective_rows);
  ed::view3d::image_grid_clamp_scroll_row(state, *v3d);

  Layout &outer_row = layout.row(false);
  Layout &grid_layout = outer_row.column(true);
  grid_layout.fixed_size_set(true);

  const int panel_width = max_ii(layout.width(), 0);
  const int tile_w = style.tile_width;
  const int visible_height = effective_rows * tile_h;
  if (panel_width > 0) {
    grid_layout.ui_units_x_set(float(panel_width) / float(UI_UNIT_X));
  }
  grid_layout.ui_units_y_set(float(visible_height) / float(UI_UNIT_Y));

  Layout &grid_stack = grid_layout.overlap();
  grid_stack.fixed_size_set(true);
  if (panel_width > 0) {
    grid_stack.ui_units_x_set(float(panel_width) / float(UI_UNIT_X));
  }
  grid_stack.ui_units_y_set(float(visible_height) / float(UI_UNIT_Y));

  const int grid_width = (panel_width > 0) ? panel_width : max_ii(tile_w, 1);
  const int cols_est = (state.cached_cols > 0) ?
                           state.cached_cols :
                           max_ii(1, grid_width / max_ii(tile_w, 1));
  const int total_rows = (state.cached_item_count > 0 && cols_est > 0) ?
                             int(ceilf(float(state.cached_item_count) / float(cols_est))) :
                             effective_rows;
  const int total_height = max_ii(visible_height, total_rows * tile_h);
  const int scroll_px = state.scroll_row * tile_h;

  View2D local_v2d{};
  local_v2d.flag |= V2D_IS_INIT;
  /* #rctf member order: xmin, xmax, ymin, ymax (ymax = 0 at top, ymin negative downward). */
  local_v2d.tot.xmin = 0.0f;
  local_v2d.tot.xmax = float(grid_width);
  local_v2d.tot.ymin = float(-total_height);
  local_v2d.tot.ymax = 0.0f;
  local_v2d.cur.xmin = 0.0f;
  local_v2d.cur.xmax = float(grid_width);
  local_v2d.cur.ymin = float(-(scroll_px + visible_height));
  local_v2d.cur.ymax = float(-scroll_px);
  BLI_rcti_init(&local_v2d.mask, 0, grid_width, -visible_height, 0);

  GridViewBuilder builder(*block);
  builder.build_grid_view(C, *grid_view, grid_stack, "", &local_v2d);

  state.cached_item_count = grid_view->get_item_count_filtered();
  state.cached_cols = grid_view->cols_per_row();
  ed::view3d::image_grid_clamp_scroll_row(state, *v3d);

  const int max_scroll_row = ed::view3d::image_grid_max_scroll_row(state, *v3d);
  if (max_scroll_row > 0) {
    /* Overlay scrollbar (does not steal grid width). Narrow fixed column so #widget_scroll
     * stays vertical (#BLI_rcti_size_x < #BLI_rcti_size_y). */
    Layout &scroll_anchor = grid_stack.row(false);
    scroll_anchor.alignment_set(LayoutAlign::Right);
    Layout &scroll_col = scroll_anchor.column(false);
    scroll_col.fixed_size_set(true);
    scroll_col.ui_units_x_set(float(V2D_SCROLL_WIDTH) / float(UI_UNIT_X));
    scroll_col.ui_units_y_set(float(visible_height) / float(UI_UNIT_Y));

    block_layout_set_current(block, &scroll_col);
    /* Default block emboss — #EmbossType::None maps scroll to #WidgetStyle::Icon (invisible). */
    Button *but = uiDefButV(block,
                            ButtonType::Scroll,
                            "",
                            0,
                            0,
                            short(V2D_SCROLL_WIDTH),
                            visible_height,
                            &state.scroll_row,
                            0.0f,
                            float(max_scroll_row),
                            "");
    auto *but_scroll = reinterpret_cast<ButtonScrollBar *>(but);
    but_scroll->visual_height = float(effective_rows);
    /* Opaque track over preview tiles (theme #wcol_scroll inner alpha is often 0). */
    uchar scroll_track_bg[4];
    theme::get_color_4ubv(TH_BACK, scroll_track_bg);
    scroll_track_bg[3] = 255;
    button_color_set(but, scroll_track_bg);
    button_flag_disable(but, BUT_UNDO);
    button_func_set(but, [](bContext &C) {
      if (ARegion *region = CTX_wm_region(&C)) {
        ED_region_tag_redraw(region);
        ED_region_tag_refresh_ui(region);
      }
    });
    block_layout_set_current(block, &layout);
  }

  Layout &grip_row = layout.row(false);
  grip_row.scale_x_set(1.0f);
  block_layout_set_current(block, &grip_row);
  /* Grip in pixels (softmin/max 0) — same model as #AbstractTreeView::custom_height_. */
  Button *grip_but = uiDefIconButV(block,
                                   ButtonType::Grip,
                                   ICON_GRIP,
                                   0,
                                   0,
                                   short(max_ii(panel_width, int(UI_UNIT_X * 10))),
                                   short(UI_UNIT_Y * 0.5f),
                                   &state.grip_pixel_height,
                                   0.0f,
                                   0.0f,
                                   "");
  button_flag_disable(grip_but, BUT_UNDO);
  button_func_set(grip_but, [](bContext &C) {
    WM_event_add_notifier(&C, NC_SPACE | ND_SPACE_VIEW3D, nullptr);
    if (ARegion *region = CTX_wm_region(&C)) {
      ED_region_tag_redraw(region);
      ED_region_tag_refresh_ui(region);
    }
  });
  block_layout_set_current(block, &layout);
}

void template_asset_image_grid(Layout *layout,
                               bContext *C,
                               PointerRNA *ptr,
                               const char *propname)
{
  View3D *v3d = CTX_wm_view3d(C);
  if (!v3d || !ptr || !propname) {
    return;
  }

  PropertyRNA *prop = RNA_struct_find_property(ptr, propname);
  if (!prop || RNA_property_type(prop) != PROP_POINTER) {
    return;
  }

  ed::view3d::ImageGridUIState &state = ed::view3d::image_grid_state_get(*v3d);
  Block *block = layout->block();

  block_add_dynamic_listener(block, ed::asset::list::asset_reading_region_listen_fn);
  block_add_dynamic_listener(block, image_grid_block_listener);

  draw_header_row(*layout, state, *C);
  build_image_grid(*layout, *C, state, *ptr, prop);
  add_browse_more_button(*layout, *C, state.lib_ref);
}

/** \} */

}  // namespace blender::ui
