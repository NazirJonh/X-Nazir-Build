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

#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_icons.hh"
#include "BKE_idtype.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_preview_image.hh"
#include "BKE_screen.hh"

#include "BLI_set.hh"
#include "BLI_vector.hh"
#include "BLT_translation.hh"

#include "ED_asset.hh"
#include "ED_asset_library.hh"
#include "ED_asset_list.hh"
#include "ED_render.hh"
#include "ED_screen.hh"
#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "BLI_rect.h"
#include "UI_grid_view.hh"
#include "UI_interface.hh"
#include "UI_interface_icons.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "interface_grid_view.hh"
#include "interface_intern.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_asset_shelf.hh"
#include "ED_view3d.hh"

namespace blender::ui {

using ed::view3d::IMAGE_TEXTURE_SHELF_IDNAME;

/**
 * Upper bound on tiles built per redraw (16 rows × wide N-panel). The actual window is
 * #image_grid_build_item_window_size() from visible rows × columns.
 */
constexpr int IMAGE_GRID_MAX_ITEMS = 512;

/* -------------------------------------------------------------------- */
/** \name Helpers
 * \{ */

static bool image_grid_asset_preview_is_drawable(const PreviewImage &preview)
{
  return preview.w[ICON_SIZE_PREVIEW] > 0 && preview.h[ICON_SIZE_PREVIEW] > 0 &&
         preview.rect[ICON_SIZE_PREVIEW] != nullptr &&
         !BKE_previewimg_is_invalid(&preview, ICON_SIZE_PREVIEW) &&
         BKE_previewimg_is_finished(&preview, ICON_SIZE_PREVIEW);
}

/**
 * Return the preview icon attached by #BKE_icon_preview_ensure(), even while deferred loading is
 * in progress. Using the type icon instead would skip #icon_ensure_deferred() / #PreviewLoadJob.
 */
static int image_grid_asset_preview_icon_id(const asset_system::AssetRepresentation &asset)
{
  if (const PreviewImage *preview = asset.get_preview()) {
    if (preview->runtime->icon_id) {
      return preview->runtime->icon_id;
    }
  }
  return ed::asset::asset_preview_or_icon(asset);
}

/** Items to build for the current scroll window: all visible grid cells, capped for performance.
 */
static int image_grid_build_item_window_size(const ed::view3d::ImageGridUIState &state,
                                             const GridViewStyle &style,
                                             const int cols)
{
  const int safe_cols = max_ii(1, cols);
  const int tile_h = max_ii(1, style.tile_height);
  /* #ceil to match #build_image_grid: a partially visible bottom row must be built too. */
  const int effective_rows = clamp_i(
      int(divide_ceil_u(uint(state.viewport.grip_pixel_height), uint(tile_h))), 1, 16);
  /* One extra buffer row so sub-row scrolling can reveal a partial row at the bottom. */
  const int visible_slots = max_ii(1, (effective_rows + 1) * safe_cols);
  return min_ii(visible_slots, IMAGE_GRID_MAX_ITEMS);
}

static void image_grid_block_listener(const wmRegionListenerParams *params)
{
  const wmNotifier *wmn = params->notifier;
  switch (wmn->category) {
    case NC_ASSET:
      if (wmn->data == int(ND_ASSET_LIST_PREVIEW)) {
        /* Preview pixels updated: redraw only. Full UI refresh rebuilds all grid items. */
        ED_region_tag_redraw(params->region);
      }
      else if (ELEM(wmn->data, int(ND_ASSET_LIST), int(ND_ASSET_LIST_READING))) {
        ED_region_tag_redraw(params->region);
        ED_region_tag_refresh_ui(params->region);
      }
      break;
    case NC_ID:
      if (ELEM(wmn->action, NA_EDITED, NA_RENAME)) {
        ED_region_tag_redraw(params->region);
      }
      break;
    case NC_SPACE:
      if (wmn->data == ND_SPACE_VIEW3D) {
        /* View3D display settings changed (e.g. preview size) — rebuild the grid. */
        ED_region_tag_redraw(params->region);
        ED_region_tag_refresh_ui(params->region);
      }
      break;
    case NC_WM:
      if (wmn->data == ND_UNDO) {
        ED_region_tag_redraw(params->region);
        ED_region_tag_refresh_ui(params->region);
      }
      break;
    default:
      break;
  }
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

  void init_item_callbacks()
  {
    this->hide_label();
    /* Activate on release (KM_CLICK), not on press — enables drag-to-scroll without
     * triggering texture assignment mid-gesture (mobile/pen UX pattern). */
    this->select_on_click_set();
    this->always_reactivate_on_click();
    this->set_on_activate_fn([this](bContext &C, PreviewGridItem & /*item*/) {
      wmOperatorType *ot = WM_operatortype_find("VIEW3D_OT_image_grid_assign_texture", true);
      if (!ot) {
        return;
      }

      PointerRNA op_ptr = WM_operator_properties_create_ptr(ot);
      if (target_ptr_.owner_id) {
        RNA_int_set(&op_ptr, "brush_session_uid", int(target_ptr_.owner_id->session_uid));
      }
      RNA_boolean_set(&op_ptr, "use_mask_slot", ed::view3d::image_grid_slot_is_mask(target_ptr_));

      if (kind_ == ImageGridItemKind::BlendImage) {
        RNA_int_set(&op_ptr, "image_session_uid", int(image_->id.session_uid));
      }
      else if (ID *local_id = asset_->local_id()) {
        if (GS(local_id->name) == ID_IM) {
          RNA_int_set(&op_ptr, "image_session_uid", int(local_id->session_uid));
        }
      }
      else {
        RNA_enum_set(&op_ptr,
                     "asset_library_reference",
                     ed::asset::library_reference_to_enum_value(&library_ref_));
        RNA_string_set(&op_ptr, "asset_identifier", asset_->library_relative_identifier().c_str());
      }

      WM_operator_name_call_ptr(&C, ot, wm::OpCallContext::ExecDefault, &op_ptr, nullptr);
      WM_operator_properties_free(&op_ptr);
    });
    this->set_is_active_fn([this]() { return this->is_active_texture(); });
  }

  ID *get_id() const
  {
    if (kind_ == ImageGridItemKind::Asset) {
      return asset_->local_id();
    }
    return &image_->id;
  }

  const Image *item_image() const
  {
    if (kind_ == ImageGridItemKind::BlendImage) {
      return image_;
    }
    if (ID *id = asset_->local_id()) {
      if (GS(id->name) == ID_IM) {
        return reinterpret_cast<const Image *>(id);
      }
    }
    return nullptr;
  }

  static const Image *active_slot_image(const PointerRNA &active_ptr)
  {
    if (!active_ptr.data || !active_ptr.type || !RNA_struct_is_ID(active_ptr.type)) {
      return nullptr;
    }
    ID *active_id = static_cast<ID *>(active_ptr.data);
    if (GS(active_id->name) == ID_TE) {
      const Tex *tex = reinterpret_cast<const Tex *>(active_id);
      if (tex->type == TEX_IMAGE) {
        return tex->ima;
      }
      return nullptr;
    }
    if (GS(active_id->name) == ID_IM) {
      return reinterpret_cast<const Image *>(active_id);
    }
    return nullptr;
  }

  int get_badge_icon() const
  {
    const ID *id = this->get_id();
    if (!id) {
      return ICON_NONE;
    }
    if (id->lib != nullptr) {
      /* Linked from an external .blend — not saved in the current file. */
      return ICON_LINKED;
    }
    /* Only badge local assets that were explicitly marked via "Mark as Asset".
     * Plain images (BlendImage kind, no asset_data) do not get a badge. */
    if (id->asset_data != nullptr) {
      return ICON_ASSET_MANAGER;
    }
    return ICON_NONE;
  }

  bool is_active_texture() const
  {
    const PointerRNA active_ptr = RNA_property_pointer_get(&target_ptr_, target_prop_);
    const Image *active_image = active_slot_image(active_ptr);
    if (!active_image) {
      return false;
    }

    if (const Image *item_im = this->item_image()) {
      return item_im == active_image;
    }

    if (kind_ == ImageGridItemKind::Asset) {
      return ed::view3d::image_grid_asset_represents_image(*asset_, *active_image);
    }

    return false;
  }

  static int preview_icon_id_for_id(const bContext &C, ID &id)
  {
    if (!ED_preview_id_is_supported(&id)) {
      return ui::icon_from_id(&id);
    }

    const int icon_id = BKE_icon_id_ensure(&id);
    icon_render_id(&C, nullptr, &id, ICON_SIZE_PREVIEW, !G.background);

    /* Keep the preview icon id so #def_but_icon queues loading; draw shows a spinner via
     * #icon_is_preview_deferred_loading(). */
    if (icon_is_preview_deferred_loading(icon_id, true)) {
      return icon_id;
    }

    PreviewImage *preview = BKE_previewimg_id_get(&id);
    if (preview && !BKE_previewimg_is_invalid(preview, ICON_SIZE_PREVIEW) &&
        image_grid_asset_preview_is_drawable(*preview))
    {
      return BKE_icon_preview_ensure(&id, preview);
    }

    return icon_id ? icon_id : ui::icon_from_id(&id);
  }

  int get_preview_icon_id(const bContext &C) const
  {
    if (kind_ == ImageGridItemKind::Asset) {
      if (!ed::asset::list::is_loaded(&library_ref_)) {
        return ICON_PREVIEW_LOADING;
      }

      if (ID *local_id = asset_->local_id()) {
        return preview_icon_id_for_id(C, *local_id);
      }

      return image_grid_asset_preview_icon_id(*asset_);
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

    /* Match #AssetViewItem::build_grid_tile — deferred thumbnails via #PreviewLoadJob. */
    if (kind_ == ImageGridItemKind::Asset) {
      asset_->ensure_previewable(C);
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

    const int badge_icon = this->get_badge_icon();
    if (badge_icon != ICON_NONE) {
      /* Bottom-right badge: ICON_ASSET_MANAGER for assets native to the current file,
       * ICON_LINKED for assets linked from an external .blend.
       * A separator spacer pushes the icon row to the bottom of the tile. */
      Layout &badge_col = overlap.column(true);
      uiDefBut(badge_col.block(),
               ButtonType::Sepr,
               "",
               0,
               0,
               0,
               style.tile_height - int(UI_UNIT_Y),
               nullptr,
               0.0,
               0.0,
               "");
      Layout &badge_row = badge_col.row(false);
      badge_row.alignment_set(LayoutAlign::Right);
      Button *badge_but = uiItemL_ex(&badge_row, "", badge_icon, false, false);
      button_label_alpha_factor_set(badge_but, 0.6f);
      button_label_draw_icon_border_set(badge_but, true);
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

  void set_grid_item_operator_props(const bContext &C, PointerRNA &props) const
  {
    RNA_boolean_set(
        &props, "use_mask_slot", ed::view3d::image_grid_is_mask_slot_from_context(C) ? 1 : 0);
    if (kind_ == ImageGridItemKind::Asset) {
      RNA_enum_set(&props,
                   "asset_library_reference",
                   ed::asset::library_reference_to_enum_value(&library_ref_));
      RNA_string_set(
          &props, "asset_identifier", asset_->library_relative_identifier().c_str());
    }
    else if (image_) {
      RNA_int_set(&props, "image_session_uid", int(image_->id.session_uid));
    }
  }

  bool is_linked_item() const
  {
    if (const ID *id = this->get_id()) {
      return id->lib != nullptr;
    }
    return false;
  }

  bool can_relocate_to_disk_library() const
  {
    if (is_linked_item()) {
      return false;
    }
    if (kind_ == ImageGridItemKind::BlendImage) {
      return image_ && BKE_image_has_filepath(image_);
    }
    if (kind_ == ImageGridItemKind::Asset) {
      return !asset_->full_path().empty();
    }
    return false;
  }

  void build_context_menu(bContext &C, Layout &layout) const override
  {
    const ID *id = this->get_id();
    if (id) {
      PointerRNA id_ptr = RNA_id_pointer_create(const_cast<ID *>(id));
      layout.context_ptr_set("id", &id_ptr);
    }

    const bool linked = is_linked_item();

    if (id) {
      Layout &mark_col = layout.column(false);
      mark_col.enabled_set(!linked);
      if (!id->asset_data) {
        mark_col.op("ASSET_OT_mark_single", IFACE_("Mark as Asset"), ICON_ASSET_MANAGER);
      }
      else {
        mark_col.op("ASSET_OT_clear_single", IFACE_("Clear Asset"), ICON_NONE);
      }
    }

    if (!linked && id) {
      layout.separator();
      PointerRNA catalog_props = layout.op("VIEW3D_OT_image_grid_assign_catalog",
                                           IFACE_("Assign to Catalog"),
                                           ICON_NONE);
      set_grid_item_operator_props(C, catalog_props);
    }

    if (can_relocate_to_disk_library()) {
      PointerRNA copy_props = layout.op("VIEW3D_OT_image_grid_copy_to_library",
                                        IFACE_("Copy to Library"),
                                        ICON_NONE);
      set_grid_item_operator_props(C, copy_props);

      PointerRNA move_props = layout.op("VIEW3D_OT_image_grid_move_to_library",
                                        IFACE_("Move to Library"),
                                        ICON_NONE);
      set_grid_item_operator_props(C, move_props);
    }
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Grid View
 * \{ */

class ImageAssetGridView : public AbstractGridView {
  const bContext &context_;
  ed::view3d::ImageGridUIState &state_;
  AssetLibraryReference library_ref_;
  PointerRNA target_ptr_;
  PropertyRNA *target_prop_ = nullptr;
  /** Column estimate when #state_.cached_cols is not yet known (first redraw). */
  int cols_hint_ = 1;

 public:
  ImageAssetGridView(const bContext &context,
                     ed::view3d::ImageGridUIState &state,
                     const AssetLibraryReference &library_ref,
                     const PointerRNA &target_ptr,
                     PropertyRNA *target_prop,
                     const int cols_hint)
      : context_(context),
        state_(state),
        library_ref_(library_ref),
        target_ptr_(target_ptr),
        target_prop_(target_prop),
        cols_hint_(max_ii(1, cols_hint))
  {
  }

  void build_items() override
  {
    Main *bmain = CTX_data_main(&context_);

    ed::asset::list::storage_fetch(&library_ref_, &context_);

    const int cols = cols_hint_;
    const int first_index = state_.viewport.scroll_row * cols;
    const int item_window = image_grid_build_item_window_size(state_, this->get_style(), cols);
    const int last_index = first_index + item_window;

    state_.viewport.cached_item_count = ed::view3d::image_grid_foreach_filtered_item(
        *bmain,
        library_ref_,
        state_.filter.enabled_catalog_paths,
        [&](const ed::view3d::ImageGridFilteredItem &item, int filtered_index) -> bool {
          if (filtered_index >= first_index && filtered_index < last_index) {
            if (item.asset) {
              this->add_item<ImageAssetGridItem>(
                  *item.asset, target_ptr_, target_prop_, library_ref_);
            }
            else {
              this->add_item<ImageAssetGridItem>(
                  *item.image, target_ptr_, target_prop_, library_ref_);
            }
          }
          return true;
        });
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name View3D-backed GridStateAccess adapter (Stage 2a)
 * \{ */

/** Forwards #GridStateAccess reads/writes to #ImageGridUIState and #View3D. Callback factories
 * re-derive state from #bContext so no dangling references when lambdas fire after the frame. */
class View3DGridStateAccess : public GridStateAccess {
  ed::view3d::ImageGridUIState &state_;
  View3D &v3d_;
  std::string idname_;
  bool is_mask_slot_;
  /** True when the grid is drawn inside a popover (tool-header Texture button).
   * Popover and sidebar share the same #ImageGridUIState but keep independent grip heights so
   * resizing one does not affect the other. Popover height is session-only (not written to DNA). */
  bool is_popover_;

 public:
  View3DGridStateAccess(ed::view3d::ImageGridUIState &state,
                        View3D &v3d,
                        std::string idname,
                        const bool is_mask_slot,
                        const bool is_popover)
      : state_(state),
        v3d_(v3d),
        idname_(std::move(idname)),
        is_mask_slot_(is_mask_slot),
        is_popover_(is_popover)
  {
  }

  int &grip_height_ref_() const
  {
    return is_popover_ ? state_.viewport.grip_pixel_height_popover :
                         state_.viewport.grip_pixel_height;
  }

  int grip_pixel_height() const override { return grip_height_ref_(); }
  void grip_pixel_height_set(const int value) override { grip_height_ref_() = value; }
  int *grip_pixel_height_ptr() override { return &grip_height_ref_(); }

  int scroll_row() const override { return state_.viewport.scroll_row; }
  void scroll_row_set(const int value) override { state_.viewport.scroll_row = value; }
  int *scroll_row_ptr() override { return &state_.viewport.scroll_row; }

  int scroll_offset_px() const override { return state_.viewport.scroll_offset_px; }
  void scroll_offset_px_set(const int value) override
  {
    state_.viewport.scroll_offset_px = value;
  }

  int cached_item_count() const override { return state_.viewport.cached_item_count; }
  void cached_item_count_set(const int value) override
  {
    state_.viewport.cached_item_count = value;
  }
  int cached_cols() const override { return state_.viewport.cached_cols; }
  void cached_cols_set(const int value) override { state_.viewport.cached_cols = value; }

  void store_scroll_for_layout(const int cols, const int rows) override
  {
    ed::view3d::image_grid_viewport_store_scroll_for_layout(state_.viewport, cols, rows);
  }
  void focus_clear() override { ed::view3d::image_grid_focus_clear(state_.viewport); }

  int effective_rows_dna_fallback() const override
  {
    /* Popover height is not persisted to DNA; use a sensible session default. */
    if (is_popover_) {
      return 3;
    }
    return ed::view3d::image_grid_effective_rows(v3d_, is_mask_slot_);
  }

  std::function<void(bContext &)> make_scroll_widget_fn(const int store_cols,
                                                        const int store_rows) const override
  {
    const bool is_mask_slot = is_mask_slot_;
    return [is_mask_slot, store_cols, store_rows](bContext &C) {
      View3D *v3d = CTX_wm_view3d(&C);
      if (!v3d) {
        return;
      }
      ed::view3d::ImageGridUIState &st = ed::view3d::image_grid_state_get(*v3d, is_mask_slot);
      st.viewport.scroll_offset_px = 0;
      ed::view3d::image_grid_focus_clear(st.viewport);
      ed::view3d::image_grid_viewport_store_scroll_for_layout(st.viewport, store_cols, store_rows);
      if (ARegion *region = CTX_wm_region(&C)) {
        ED_region_tag_redraw(region);
        ED_region_tag_refresh_ui(region);
      }
    };
  }

  std::function<void(bContext &)> make_grip_change_fn() const override
  {
    return [](bContext &C) {
      WM_event_add_notifier(&C, NC_SPACE | ND_SPACE_VIEW3D, nullptr);
      if (ARegion *region = CTX_wm_region(&C)) {
        ED_region_tag_redraw(region);
        ED_region_tag_refresh_ui(region);
      }
    };
  }

  StringRef grid_idname() const override { return idname_; }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Template UI
 * \{ */

static void add_browse_image_button(Layout &layout,
                                    bContext &C,
                                    ed::view3d::ImageGridUIState &state,
                                    PointerRNA &target_ptr)
{
  ed::view3d::image_grid_prepare_browse_shelf(C, state, IMAGE_TEXTURE_SHELF_IDNAME);

  Layout &split = layout.split(0.55f, true);
  split.context_string_set("asset_shelf_idname", IMAGE_TEXTURE_SHELF_IDNAME);
  split.context_ptr_set("image_grid_target", &target_ptr);

  Layout &browse_row = split.row(true);
  browse_row.popover(&C, "ASSETSHELF_PT_popover_panel", IFACE_("Browse Image"), ICON_FILEBROWSER);

  Layout &actions_row = split.row(true);
  actions_row.op("VIEW3D_OT_image_grid_new", IFACE_("New"), ICON_ADD);
  actions_row.op("VIEW3D_OT_image_grid_open", IFACE_("Open"), ICON_FILEBROWSER);
}

/** Icon-only popover with menu arrow (same footprint as #ASSETSHELF_PT_display in the shelf
 * header). */
static void image_grid_header_popover(Layout &row,
                                      const bContext &C,
                                      const StringRefNull panel_id,
                                      const int icon,
                                      const bool is_mask_slot)
{
  Block *block = row.block();
  Layout &popover_row = row.row(false);
  popover_row.emboss_set(EmbossType::Emboss);
  popover_row.ui_units_x_set(1.6f);
  popover_row.context_int_set("image_grid_is_mask_slot", is_mask_slot ? 1 : 0);
  popover_row.popover(&C, panel_id, "", icon);
  /* #layout_add_but() marks compact icon buttons as fixed width (#UI_UNIT_X); widen for arrow. */
  Button *but = block->buttons_ptrs.last().get();
  but->rect.xmax = but->rect.xmin + short(1.6f * UI_UNIT_X);
}

static void draw_header_row(Layout &layout,
                            ed::view3d::ImageGridUIState &state,
                            const bContext &C,
                            const bool is_mask_slot)
{
  layout.context_int_set("image_grid_is_mask_slot", is_mask_slot ? 1 : 0);

  Layout &row = layout.row(true);
  /* Library selector: dropdown menu of asset libraries. */
  row.op_menu_enum(&C,
                   "VIEW3D_OT_image_grid_set_library",
                   "asset_library_reference",
                   ed::view3d::image_grid_library_ui_name(state.filter.lib_ref),
                   ICON_ASSET_MANAGER);
  /* Catalog selector: opens a popover with library selector + catalog tree. */
  image_grid_header_popover(
      row, C, "VIEW3D_PT_image_grid_catalog_selector", ICON_COLLAPSEMENU, is_mask_slot);
  /* Display settings: preview thumbnail size (shared between texture and mask grids). */
  image_grid_header_popover(row, C, "VIEW3D_PT_image_grid_display", ICON_IMGDISPLAY, is_mask_slot);
}

static void build_image_grid(Layout &layout,
                             const bContext &C,
                             ed::view3d::ImageGridUIState &state,
                             PointerRNA &ptr,
                             PropertyRNA *prop,
                             const bool is_mask_slot,
                             const bool is_popover)
{
  Block *block = layout.block();
  View3D *v3d = CTX_wm_view3d(&C);
  if (!v3d) {
    return;
  }

  ed::view3d::image_grid_clamp_scroll_row(state, *v3d, is_mask_slot);

  const int preview_size = ed::view3d::image_grid_preview_size_get(*v3d);
  const int tile_w = ui::preview_tile_size_x(preview_size);
  const int panel_width = max_ii(layout.width(), 0);
  /* Always derive columns from this panel's width. #cached_cols is shared across N-Panel and
   * texture popover grids and would otherwise make the popover reuse the sidebar column count. */
  const int cols_est = (panel_width > 0) ? max_ii(1, panel_width / max_ii(tile_w, 1)) :
                                           max_ii(1, state.viewport.cached_cols);

  /* Publish this context's column count immediately so the focus/clamp helpers below use the
   * column count for *this* panel, not the other grid's count from the previous frame. */
  state.viewport.cached_cols = cols_est;

  /* Pre-compute visible rows for image_grid_apply_focus_scroll.
   * Use the correct grip height for this context (popover has its own independent height). */
  const int grip_height = is_popover ? state.viewport.grip_pixel_height_popover :
                                       state.viewport.grip_pixel_height;
  const int tile_h_hint = ui::preview_tile_size_y_no_label(preview_size);
  const int effective_rows_hint =
      (grip_height >= tile_h_hint) ?
          clamp_i(int(divide_ceil_u(uint(grip_height), uint(tile_h_hint))), 1, 16) :
          (is_popover ? 3 : ed::view3d::image_grid_effective_rows(*v3d, is_mask_slot));

  /* Restore this (cols, rows) layout's saved scroll before applying focus so the "already
   * visible" check in image_grid_apply_focus_scroll sees this layout's own current position. */
  ed::view3d::image_grid_viewport_restore_scroll_for_layout(
      state.viewport, cols_est, effective_rows_hint);
  ed::view3d::image_grid_apply_focus_scroll(C, *v3d, state, cols_est, effective_rows_hint);

  auto view_unique = std::make_unique<ImageAssetGridView>(
      C, state, state.filter.lib_ref, ptr, prop, cols_est);
  view_unique->set_tile_size(tile_w, ui::preview_tile_size_y_no_label(preview_size));
  view_unique->set_cols_per_row_hint(cols_est);
  const char *grid_view_id = is_mask_slot ? "image_asset_grid_mask" : "image_asset_grid";
  AbstractGridView *grid_view = block_add_view(*block, grid_view_id, std::move(view_unique));

  /* Persist a whole-row approximation to DNA for reload reconstruction (nearest row). Done here
   * so the generic core does not need to know about View3D DNA. Removed in Stage 7.
   * Popover height is session-only — never written to DNA. */
  if (!is_popover) {
    const GridViewStyle &style = grid_view->get_style();
    const int tile_h = max_ii(1, style.tile_height);
    const int grip = state.viewport.grip_pixel_height;
    if (grip >= tile_h) {
      const short row_count = short(clamp_i(round_fl_to_int(float(grip) / float(tile_h)), 1, 16));
      if (is_mask_slot) {
        v3d->image_grid_mask_rows = row_count;
      }
      else {
        v3d->image_grid_rows = row_count;
      }
    }
  }

  View3DGridStateAccess state_access(state, *v3d, grid_view_id, is_mask_slot, is_popover);
  build_grid_view(
      C, layout, *grid_view, state_access, state.viewport.cached_item_count, cols_est, panel_width);
}

void template_asset_image_grid(Layout *layout,
                               bContext *C,
                               PointerRNA *ptr,
                               const char *propname,
                               const bool is_popover)
{
  View3D *v3d = CTX_wm_view3d(C);
  if (!v3d || !ptr || !propname) {
    return;
  }

  PropertyRNA *prop = RNA_struct_find_property(ptr, propname);
  if (!prop || RNA_property_type(prop) != PROP_POINTER) {
    return;
  }

  const bool is_mask_slot = ed::view3d::image_grid_slot_is_mask(*ptr);
  ed::view3d::ImageGridUIState &state = ed::view3d::image_grid_state_get(*v3d, is_mask_slot);

  ed::view3d::image_grid_catalog_sanitize_selection(state);
  ed::view3d::image_grid_pending_apply_if_ready(*C, *v3d);

  Block *block = layout->block();

  block_add_dynamic_listener(block, ed::asset::list::asset_reading_region_listen_fn);
  block_add_dynamic_listener(block, image_grid_block_listener);

  draw_header_row(*layout, state, *C, is_mask_slot);
  build_image_grid(*layout, *C, state, *ptr, prop, is_mask_slot, is_popover);
  add_browse_image_button(*layout, *C, state, *ptr);
}

/** \} */

}  // namespace blender::ui
