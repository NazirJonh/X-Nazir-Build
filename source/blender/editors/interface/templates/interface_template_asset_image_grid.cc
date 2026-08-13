/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include "UI_interface_c.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_screen_types.h"
#include "DNA_texture_types.h"
#include "DNA_userdef_types.h"
#include "DNA_view2d_types.h"
#include "DNA_view3d_types.h"

#include "AS_asset_catalog_path.hh"
#include "AS_asset_library.hh"
#include "AS_asset_representation.hh"

#include "BLI_listbase.h"
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
#include "BLI_utildefines.h"
#include "BLI_vector.hh"
#include "BLT_translation.hh"

#include "MEM_guardedalloc.h"

#include "IMB_imbuf_types.hh"

#include <fmt/format.h>

#include "ED_asset.hh"
#include "ED_asset_image_utils.hh"
#include "ED_asset_library.hh"
#include "ED_asset_list.hh"
#include "ED_render.hh"
#include "ED_screen.hh"
#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "BLI_path_utils.hh"
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
#include "ED_image_grid.hh"

namespace blender::ui {

using ed::image_grid::IMAGE_TEXTURE_SHELF_IDNAME;

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
    case NC_BRUSH:
      if (ELEM(wmn->action, NA_SELECTED, NA_EDITED)) {
        /* NA_SELECTED: active brush changed — auto-focus fires via
         * #image_grid_auto_focus_on_brush_change on the next redraw.
         * NA_EDITED: brush properties changed — redraw only to refresh the active highlight;
         * auto-focus does not re-trigger because the brush session UID is unchanged. */
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
  /** True when this item lives in the Texture popover, not the N-Panel. Forwarded to the assign
   * operator so it marks the correct (cols, rows) layout as already focused. */
  bool is_popover_ = false;

 public:
  ImageAssetGridItem(asset_system::AssetRepresentation &asset,
                     const PointerRNA &target_ptr,
                     PropertyRNA *target_prop,
                     const AssetLibraryReference &library_ref,
                     const bool is_popover)
      : PreviewGridItem(asset.library_relative_identifier(), asset.get_name(), ICON_NONE),
        kind_(ImageGridItemKind::Asset),
        asset_(&asset),
        target_ptr_(target_ptr),
        target_prop_(target_prop),
        library_ref_(library_ref),
        is_popover_(is_popover)
  {
    this->init_item_callbacks();
  }

  ImageAssetGridItem(Image &image,
                     const PointerRNA &target_ptr,
                     PropertyRNA *target_prop,
                     const AssetLibraryReference &library_ref,
                     const bool is_popover)
      : PreviewGridItem(image.id.name + 2, image.id.name + 2, ICON_NONE),
        kind_(ImageGridItemKind::BlendImage),
        image_(&image),
        target_ptr_(target_ptr),
        target_prop_(target_prop),
        library_ref_(library_ref),
        is_popover_(is_popover)
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
      wmOperatorType *ot = WM_operatortype_find("IMAGE_GRID_OT_assign_texture", true);
      if (!ot) {
        return;
      }

      PointerRNA op_ptr = WM_operator_properties_create_ptr(ot);
      if (target_ptr_.owner_id) {
        RNA_int_set(&op_ptr, "brush_session_uid", int(target_ptr_.owner_id->session_uid));
      }
      RNA_boolean_set(&op_ptr, "use_mask_slot", ed::image_grid::image_grid_slot_is_mask(target_ptr_));
      RNA_boolean_set(&op_ptr, "is_popover", is_popover_);

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
      return ed::image_grid::image_grid_asset_represents_image(*asset_, *active_image);
    }

    return false;
  }

  static int preview_icon_id_for_id(const bContext &C, ID &id)
  {
    if (!ED_preview_id_render_is_supported(&id)) {
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
      wmOperatorType *mark_ot = WM_operatortype_find("IMAGE_GRID_OT_mark_asset", true);
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
        [](bContext &C, TooltipData &tip, Button * /*but*/, void *argN) {
          const ImageAssetGridItem *item = static_cast<const ImageAssetGridItem *>(argN);
          if (item->kind_ == ImageGridItemKind::Asset) {
            ed::asset::asset_tooltip(&C, *item->asset_, tip);
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
        &props, "use_mask_slot", ed::image_grid::image_grid_is_mask_slot_from_context(C) ? 1 : 0);
    if (kind_ == ImageGridItemKind::Asset) {
      RNA_enum_set(&props,
                   "asset_library_reference",
                   ed::asset::library_reference_to_enum_value(&library_ref_));
      RNA_string_set(&props, "asset_identifier", asset_->library_relative_identifier().c_str());
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
      PointerRNA catalog_props = layout.op(
          "IMAGE_GRID_OT_assign_catalog", IFACE_("Assign to Catalog"), ICON_NONE);
      set_grid_item_operator_props(C, catalog_props);
    }

    if (can_relocate_to_disk_library()) {
      PointerRNA copy_props = layout.op(
          "IMAGE_GRID_OT_copy_to_library", IFACE_("Copy to Library"), ICON_NONE);
      set_grid_item_operator_props(C, copy_props);

      PointerRNA move_props = layout.op(
          "IMAGE_GRID_OT_move_to_library", IFACE_("Move to Library"), ICON_NONE);
      set_grid_item_operator_props(C, move_props);
    }
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Grid Drop Target
 * \{ */

/** Number of image files carried by \a drag; 0 for non-path drags. */
static int image_grid_drag_image_file_count(const wmDrag &drag)
{
  if (drag.type != WM_DRAG_PATH) {
    return 0;
  }
  int num = 0;
  for (const std::string &path : WM_drag_get_paths(&drag)) {
    if (BLI_path_extension_check_array(path.c_str(), imb_ext_image)) {
      num++;
    }
  }
  return num;
}

/**
 * First image-typed item in a #WM_DRAG_ASSET_LIST drag, or nullptr. The Asset Browser starts both
 * a #WM_DRAG_ASSET (single item) and a #WM_DRAG_ASSET_LIST drag for the same interaction, and
 * #drop_target_apply_drop() applies whichever happens to be first in the shared drag list.
 */
static const wmDragAssetListItem *first_image_item_in_list(const wmDrag &drag)
{
  const ListBaseT<wmDragAssetListItem> *asset_drags = WM_drag_asset_list_get(&drag);
  if (!asset_drags) {
    return nullptr;
  }
  for (const wmDragAssetListItem &item : *asset_drags) {
    const ID_Type item_idtype = item.is_external ?
                                    item.asset_data.external_info->asset->get_id_type() :
                                    (item.asset_data.local_id ?
                                         GS(item.asset_data.local_id->name) :
                                         ID_Type(0));
    if (item_idtype == ID_IM) {
      return &item;
    }
  }
  return nullptr;
}

/**
 * View-level drop target for the brush texture image grid. Dropping a single image assigns it to
 * the slot the grid is bound to (same as dropping on the slot button). Dropping several image files
 * opens the batch-import menu (see #IMAGE_GRID_OT_drop_import), wired in a later step.
 */
class ImageGridDropTarget : public ui::DropTargetInterface {
  /** The brush texture slot the grid is bound to (primary or mask). */
  PointerRNA target_ptr_;

 public:
  explicit ImageGridDropTarget(const PointerRNA &target_ptr) : target_ptr_(target_ptr) {}

  bool can_drop(bContext & /*C*/, const wmDrag &drag, const char ** /*r_disabled_hint*/) const override
  {
    /* Local image data-block or image asset (imported on drop). */
    if (WM_drag_is_ID_type(&drag, ID_IM)) {
      return true;
    }
    /* The Asset Browser drags a #WM_DRAG_ASSET_LIST alongside the single-item #WM_DRAG_ASSET
     * above (multi-select support); both must be accepted since #drop_target_apply_drop()
     * requires every drag in the shared list to pass this check, applying whichever is first. */
    if (drag.type == WM_DRAG_ASSET_LIST) {
      return first_image_item_in_list(drag) != nullptr;
    }
    /* One or more image files from the file browser or the OS. */
    if (drag.type == WM_DRAG_PATH) {
      return image_grid_drag_image_file_count(drag) > 0;
    }
    return false;
  }

  std::string drop_tooltip(const ui::DragInfo &drag_info) const override
  {
    const wmDrag &drag = drag_info.drag_data;
    const int image_num = image_grid_drag_image_file_count(drag);
    if (image_num > 1) {
      return fmt::format(fmt::runtime(TIP_("Import {} images into the grid")), image_num);
    }
    const std::string image_name = WM_drag_get_item_name(const_cast<wmDrag *>(&drag));
    return fmt::format(fmt::runtime(TIP_("Assign {} to the brush texture slot")), image_name);
  }

  bool on_drop(bContext *C, const ui::DragInfo &drag_info) const override
  {
    /* Batch of image files -> import menu (temporary vs library + catalog). */
    if (image_grid_drag_image_file_count(drag_info.drag_data) > 1) {
      return this->drop_multiple_images(C, drag_info);
    }
    return this->drop_single_image(C, drag_info);
  }

 private:
  /**
   * Assign one dropped image to the bound slot via the same brush-localizing path as
   * #IMAGE_GRID_OT_open and the grid's own tile click
   * (#ed::image_grid::image_grid_assign_dropped_image), not #BRUSH_OT_texture_slot_assign_image: that
   * operator moves (or, for a pre-existing image, copies) the image into a linked brush's library
   * to keep the reference in the same undo domain - which for the common case of an asset-shelf
   * (linked) active brush left the dropped image showing as linked, and for a pre-existing image
   * additionally duplicated it. Localizing the brush instead keeps the dropped image itself local
   * and untouched.
   */
  bool drop_single_image(bContext *C, const ui::DragInfo &drag_info) const
  {
    const wmDrag &drag = drag_info.drag_data;
    Main *bmain = CTX_data_main(C);

    /* Resolve a local image, importing the asset first if the drag came from the asset browser. */
    Image *image = nullptr;
    if (ID *image_id = WM_drag_get_local_ID_or_import_from_asset(C, &drag, ID_IM)) {
      image = id_cast<Image *>(image_id);
    }
    else if (drag.type == WM_DRAG_ASSET) {
      /* #WM_drag_get_local_ID_or_import_from_asset() only handles blend-library assets; on-disk
       * image libraries have no blend-library import method and fall through here. */
      if (wmDragAsset *asset_drag = WM_drag_get_asset_data(&drag, ID_IM)) {
        image = ed::asset::resolve_image_from_asset(*bmain, *asset_drag->asset);
      }
    }
    else if (drag.type == WM_DRAG_ASSET_LIST) {
      if (const wmDragAssetListItem *item = first_image_item_in_list(drag)) {
        if (item->is_external) {
          image = ed::asset::resolve_image_from_asset(*bmain,
                                                      *item->asset_data.external_info->asset);
        }
        else {
          image = id_cast<Image *>(item->asset_data.local_id);
        }
      }
    }
    else if (drag.type == WM_DRAG_PATH) {
      if (const char *path = WM_drag_get_single_path(&drag)) {
        image = BKE_image_load_exists(bmain, path, nullptr);
        if (image) {
          /* #BKE_image_load_exists takes a loan on the user count regardless of whether the
           * block was newly created or already existed; the sole consumer
           * (#image_grid_assign_dropped_image, via #brush_texture_for_image) always counts the
           * real reference with #id_us_plus, so release the loan here unconditionally. */
          id_us_min(&image->id);
        }
      }
    }
    if (!image) {
      return false;
    }

    return ed::image_grid::image_grid_assign_dropped_image(*C, target_ptr_, *image);
  }

  /**
   * Marshal the dropped image files into #IMAGE_GRID_OT_drop_import and invoke it. The
   * operator's own #invoke shows the temporary/library menu (and, for library, a catalog dialog).
   */
  bool drop_multiple_images(bContext *C, const ui::DragInfo &drag_info) const
  {
    const wmDrag &drag = drag_info.drag_data;

    PointerRNA props = WM_operator_properties_create("IMAGE_GRID_OT_drop_import");

    const char *first_image = nullptr;
    for (const std::string &path : WM_drag_get_paths(&drag)) {
      if (BLI_path_extension_check_array(path.c_str(), imb_ext_image)) {
        first_image = path.c_str();
        break;
      }
    }
    if (first_image) {
      char dir[FILE_MAX];
      BLI_path_split_dir_part(first_image, dir, sizeof(dir));
      RNA_string_set(&props, "directory", dir);

      for (const std::string &path : WM_drag_get_paths(&drag)) {
        if (!BLI_path_extension_check_array(path.c_str(), imb_ext_image)) {
          continue;
        }
        char name[FILE_MAX];
        BLI_path_split_file_part(path.c_str(), name, sizeof(name));
        PointerRNA itemptr{};
        RNA_collection_add(&props, "files", &itemptr);
        RNA_string_set(&itemptr, "name", name);
      }
    }
    RNA_boolean_set(&props, "use_mask_slot", ed::image_grid::image_grid_slot_is_mask(target_ptr_));

    /* Invoke so the operator can present the mode menu / catalog dialog. */
    const wmOperatorStatus status = WM_operator_name_call(C,
                                                          "IMAGE_GRID_OT_drop_import",
                                                          wm::OpCallContext::InvokeDefault,
                                                          &props,
                                                          &drag_info.event);
    WM_operator_properties_free(&props);
    return (status & (OPERATOR_FINISHED | OPERATOR_INTERFACE)) != 0;
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Grid View
 * \{ */

class ImageAssetGridView : public AbstractGridView {
  const bContext &context_;
  ed::image_grid::ImageGridUIState &state_;
  AssetLibraryReference library_ref_;
  PointerRNA target_ptr_;
  PropertyRNA *target_prop_ = nullptr;
  /** Column estimate when #state_.cached_cols is not yet known (first redraw). */
  int cols_hint_ = 1;
  /** Popover keeps its own grip height; the build window must use it, not the sidebar's. */
  bool is_popover_ = false;

 public:
  ImageAssetGridView(const bContext &context,
                     ed::image_grid::ImageGridUIState &state,
                     const AssetLibraryReference &library_ref,
                     const PointerRNA &target_ptr,
                     PropertyRNA *target_prop,
                     const int cols_hint,
                     const bool is_popover)
      : context_(context),
        state_(state),
        library_ref_(library_ref),
        target_ptr_(target_ptr),
        target_prop_(target_prop),
        cols_hint_(max_ii(1, cols_hint)),
        is_popover_(is_popover)
  {
  }

  void build_items() override
  {
    Main *bmain = CTX_data_main(&context_);

    ed::asset::list::storage_fetch(&library_ref_, &context_);

    const int cols = cols_hint_;
    const int tile_h = max_ii(this->get_style().tile_height, 1);
    /* Scroll/grip are the session's pixel truth (Stage 4); the visible window starts at the whole
     * row #scroll_px falls in (the sub-row offset only shifts the draw, not item selection). */
    const int scroll_px = session_ ? session_->scroll_px : 0;
    const int first_index = (scroll_px / tile_h) * cols;
    const int grip_height = session_ ? session_->grip_pixel_height : 0;
    const int item_window = grid_build_window_size(
        grip_height, this->get_style().tile_height, cols, IMAGE_GRID_MAX_ITEMS);
    const int last_index = first_index + item_window;

    const int filtered_count = ed::image_grid::image_grid_foreach_filtered_item(
        *bmain,
        state_,
        [&](const ed::image_grid::ImageGridFilteredItem &item, int filtered_index) -> bool {
          if (filtered_index >= first_index && filtered_index < last_index) {
            if (item.asset) {
              this->add_item<ImageAssetGridItem>(
                  *item.asset, target_ptr_, target_prop_, library_ref_, is_popover_);
            }
            else {
              this->add_item<ImageAssetGridItem>(
                  *item.image, target_ptr_, target_prop_, library_ref_, is_popover_);
            }
          }
          return true;
        });

    /* While the asset list is mid-(re)fetch the iteration above transiently yields zero (or a
     * partial count of) items even though the library is still populated. Committing that
     * transient value to #cached_item_count collapses #grid_max_scroll_px, which clamps
     * #scroll_px to the top and momentarily shrinks the grid (the shrink in turn lets the wheel
     * leak into the region's View2D pan, collapsing the panel to a single visible row). Keep the
     * last known good count until the list is ready again. */
    if (ed::asset::list::is_loaded(&library_ref_) && session_) {
      session_->cached_item_count = filtered_count;
    }
  }

  std::unique_ptr<DropTargetInterface> create_drop_target() override
  {
    return std::make_unique<ImageGridDropTarget>(target_ptr_);
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Owner-backed GridStateAccess adapter (Stage 2a)
 * \{ */

/** Forwards #GridStateAccess reads/writes to this grid variant's #GridSessionState (the single
 * source of truth for scroll/grip since Stage 4). The owner keeps only the DNA row fallback and
 * focus; callback factories re-derive those from #bContext so no dangling references. */
class ImageGridStateAccess : public GridStateAccess {
  GridSessionState &session_;
  ed::image_grid::ImageGridOwner owner_;
  std::string idname_;
  bool is_mask_slot_;
  /** True when drawn inside a popover; only affects the DNA fallback (popover height is not
   * persisted). Sidebar and popover use separate sessions (distinct #idname_), so grip/scroll are
   * independent without an is-popover branch on every access. */
  bool is_popover_;

 public:
  ImageGridStateAccess(GridSessionState &session,
                       const ed::image_grid::ImageGridOwner owner,
                       std::string idname,
                       const bool is_mask_slot,
                       const bool is_popover)
      : session_(session),
        owner_(owner),
        idname_(std::move(idname)),
        is_mask_slot_(is_mask_slot),
        is_popover_(is_popover)
  {
  }

  int grip_pixel_height() const override
  {
    return session_.grip_pixel_height;
  }
  void grip_pixel_height_set(const int value) override
  {
    session_.grip_pixel_height = value;
  }
  int *grip_pixel_height_ptr() override
  {
    return &session_.grip_pixel_height;
  }

  int scroll_px() const override
  {
    return session_.scroll_px;
  }
  void scroll_px_set(const int value) override
  {
    session_.scroll_px = value;
  }
  int *scroll_px_ptr() override
  {
    /* Pixel-scale scrollbar binds directly to the session's pixel scroll position. */
    return &session_.scroll_px;
  }

  int cached_item_count() const override
  {
    return session_.cached_item_count;
  }

  int cached_cols() const override
  {
    return session_.cols;
  }
  void cached_cols_set(const int value) override
  {
    session_.cols = value;
  }

  void store_scroll_for_cols(const int cols) override
  {
    session_.scroll_px_by_cols.add_overwrite(cols, session_.scroll_px);
  }

  void geometry_store(const ARegion *region,
                      const int tile_h,
                      const int cols,
                      const int viewport_px) override
  {
    /* Column count changed (width or preview size): restore the scroll pinned for the new layout. */
    if (session_.cols != 0 && session_.cols != cols) {
      if (const int *pinned = session_.scroll_px_by_cols.lookup_ptr(cols)) {
        session_.scroll_px = *pinned;
      }
    }
    session_.tile_h = tile_h;
    session_.cols = cols;
    session_.viewport_px = viewport_px;
    session_.region = region;
  }

  int effective_rows_dna_fallback() const override
  {
    /* Popover height is not persisted to DNA; use a sensible session default. */
    if (is_popover_) {
      return 3;
    }
    return ed::image_grid::image_grid_effective_rows(owner_, is_mask_slot_);
  }

  std::function<void(bContext &)> make_scroll_widget_fn(const int /*store_cols*/,
                                                        const int /*store_rows*/) const override
  {
    /* The pixel-scale widget already wrote the new #scroll_px directly. Dismiss any pending focus so
     * a manual scroll is not overridden on the next redraw, then redraw. */
    const bool is_mask_slot = is_mask_slot_;
    return [is_mask_slot](bContext &C) {
      if (const std::optional<ed::image_grid::ImageGridOwner> owner =
              ed::image_grid::image_grid_owner_from_context(C))
      {
        ed::image_grid::image_grid_focus_clear(
            ed::image_grid::image_grid_state_get(*owner, is_mask_slot).viewport);
      }
      if (ARegion *region = CTX_wm_region(&C)) {
        ED_region_tag_redraw(region);
        ED_region_tag_refresh_ui(region);
      }
    };
  }

  std::function<void(bContext &)> make_grip_change_fn() const override
  {
    const bool is_popover = is_popover_;
    return [is_popover](bContext &C) {
      WM_event_add_notifier(&C, NC_SPACE | ND_SPACE_VIEW3D, nullptr);
      if (ARegion *region = CTX_wm_region(&C)) {
        /* Keep the sidebar panels region's scroll offset for this value-apply pass. The build-time
         * level-trigger (#build_image_grid) covers the frames the grip is the active button; this
         * covers the release-apply frame, where the button may already be deactivating as the grid
         * settles to its final (shorter) height. The flag is consumed per layout pass by area.cc.
         * The popover has its own temporary region, which never snaps. */
        if (!is_popover && region->runtime) {
          region->runtime->keep_scroll_offset_on_resize = true;
        }
        ED_region_tag_redraw(region);
        ED_region_tag_refresh_ui(region);
      }
    };
  }

  StringRef grid_idname() const override
  {
    return idname_;
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Template UI
 * \{ */

static void add_browse_image_button(Layout &layout,
                                    bContext &C,
                                    ed::image_grid::ImageGridUIState &state,
                                    PointerRNA &target_ptr)
{
  ed::image_grid::image_grid_prepare_browse_shelf(C, state, IMAGE_TEXTURE_SHELF_IDNAME);

  Layout &split = layout.split(0.55f, true);
  split.context_string_set("asset_shelf_idname", IMAGE_TEXTURE_SHELF_IDNAME);
  split.context_ptr_set("image_grid_target", &target_ptr);

  Layout &browse_row = split.row(true);
  browse_row.popover(&C, "ASSETSHELF_PT_popover_panel", IFACE_("Browse Image"), ICON_FILEBROWSER);

  Layout &actions_row = split.row(true);
  actions_row.op("IMAGE_GRID_OT_new", IFACE_("New"), ICON_ADD);
  actions_row.op("IMAGE_GRID_OT_open", IFACE_("Open"), ICON_FILEBROWSER);
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
  const int64_t buttons_num_before = block->buttons_ptrs.size();
  popover_row.popover(&C, panel_id, "", icon);
  /* #Layout::popover() adds no button (and warns) when `panel_id` isn't a registered panel type. */
  if (block->buttons_ptrs.size() == buttons_num_before) {
    return;
  }
  /* #layout_add_but() marks compact icon buttons as fixed width (#UI_UNIT_X); widen for arrow. */
  Button *but = block->buttons_ptrs.last().get();
  but->rect.xmax = but->rect.xmin + short(1.6f * UI_UNIT_X);
}

/* Draw the asset-library choices as a plain vertical menu. The operator's enum dropdown
 * (#Layout::op_menu_enum) lays folder headings out as side-by-side columns; a menu reads top to
 * bottom. Folder headings become labels; each library is an operator row that sets the library and
 * closes the menu. The slot (texture vs mask) is re-applied to the menu's own context because the
 * operator resolves it from there (#image_grid_is_mask_slot_from_context). */
static void image_grid_library_selector_menu_draw(bContext * /*C*/, Layout *layout, void *arg)
{
  const bool is_mask_slot = POINTER_AS_INT(arg) != 0;
  layout->context_int_set("image_grid_is_mask_slot", is_mask_slot ? 1 : 0);

  Layout &col = layout->column(false);

  /* Recent / Favorites first — same order as the asset-shelf catalog tree. Not real libraries;
   * they switch #ImageGridFilter::catalog_mode via #IMAGE_GRID_OT_set_membership. */
  {
    PointerRNA recent_ptr = col.op(
        "IMAGE_GRID_OT_set_membership", IFACE_("Recent"), ICON_RECOVER_LAST);
    RNA_enum_set(&recent_ptr,
                 "mode",
                 int(ed::image_grid::ImageGridCatalogMode::Recent));
    PointerRNA favorites_ptr = col.op(
        "IMAGE_GRID_OT_set_membership", IFACE_("Favorites"), ICON_SOLO_ON);
    RNA_enum_set(&favorites_ptr,
                 "mode",
                 int(ed::image_grid::ImageGridCatalogMode::Favorites));
    col.separator();
  }

  /* Same item source as the operator's own enum (#rna_image_grid_library_itemf): image libraries
   * only, grouped by folder. */
  const EnumPropertyItem *items = ed::asset::library_reference_to_rna_enum_itemf(
      /*include_readonly=*/true,
      /*include_current_file=*/true,
      /*include_remote_libraries=*/false,
      /*include_separate_online_essentials=*/false,
      /*exclude_image_libraries=*/false,
      /*only_image_libraries=*/true);
  if (!items) {
    return;
  }

  /* Start "separated" so a leading folder heading gets no divider above it. */
  bool prev_was_separator = true;
  for (const EnumPropertyItem *item = items; item->identifier; item++) {
    /* Empty identifier: a folder heading (has a name) or a plain separator (no name). */
    if (!item->identifier[0]) {
      if (item->name && item->name[0]) {
        /* Divider before each folder name, unless one was just drawn (avoids doubling the
         * built-in separator before the custom section). */
        if (!prev_was_separator) {
          col.separator();
        }
        col.label(item->name, item->icon);
        prev_was_separator = false;
      }
      else {
        col.separator();
        prev_was_separator = true;
      }
      continue;
    }
    PointerRNA op_ptr = col.op("IMAGE_GRID_OT_set_library", item->name, item->icon);
    RNA_enum_set(&op_ptr, "asset_library_reference", item->value);
    prev_was_separator = false;
  }

  MEM_delete(items);
}

/* Ctrl-Wheel cycling for the library-selector button (#button_supports_cycling /
 * #do_but_BLOCK): steps through the same library list the menu draws, in the same order, and
 * applies the change through #image_grid_set_library (shared with #IMAGE_GRID_OT_set_library so
 * behavior matches picking an entry from the menu). */
static bool image_grid_library_selector_menu_step(bContext *C, int direction, void *arg)
{
  const bool is_mask_slot = POINTER_AS_INT(arg) != 0;

  const std::optional<ed::image_grid::ImageGridOwner> owner_opt =
      ed::image_grid::image_grid_owner_from_context(*C);
  if (!owner_opt) {
    return false;
  }
  const ed::image_grid::ImageGridOwner owner = *owner_opt;
  const ed::image_grid::ImageGridUIState &state = ed::image_grid::image_grid_state_get(owner,
                                                                                   is_mask_slot);

  const EnumPropertyItem *items = ed::asset::library_reference_to_rna_enum_itemf(
      /*include_readonly=*/true,
      /*include_current_file=*/true,
      /*include_remote_libraries=*/false,
      /*include_separate_online_essentials=*/false,
      /*exclude_image_libraries=*/false,
      /*only_image_libraries=*/true);
  if (!items) {
    return false;
  }

  /* Real library entries only, same order as the menu (folder headings/separators skipped). */
  Vector<int> values;
  for (const EnumPropertyItem *item = items; item->identifier; item++) {
    if (item->identifier[0]) {
      values.append(item->value);
    }
  }
  MEM_delete(items);

  if (values.is_empty()) {
    return false;
  }

  const int current_value = ed::asset::library_reference_to_enum_value(&state.filter.lib_ref);
  int current_index = 0;
  for (const int i : values.index_range()) {
    if (values[i] == current_value) {
      current_index = i;
      break;
    }
  }
  const int next_index = mod_i(current_index + direction, int(values.size()));
  const AssetLibraryReference new_ref = ed::asset::library_reference_from_enum_value(
      values[next_index]);

  return ed::image_grid::image_grid_set_library(*C, owner, is_mask_slot, new_ref);
}

static void draw_header_row(Layout &layout,
                            ed::image_grid::ImageGridUIState &state,
                            const bContext &C,
                            const bool is_mask_slot)
{
  layout.context_int_set("image_grid_is_mask_slot", is_mask_slot ? 1 : 0);

  Layout &row = layout.row(true);
  /* Library selector: a vertical menu of asset libraries (the operator's enum dropdown lays folder
   * headings out as side-by-side columns; this reads top to bottom). */
  {
    Block *block = row.block();
    const int64_t buttons_num_before = block->buttons_ptrs.size();
    row.menu_fn(ed::image_grid::image_grid_library_selector_label(state),
                ICON_ASSET_MANAGER,
                image_grid_library_selector_menu_draw,
                POINTER_FROM_INT(is_mask_slot ? 1 : 0));
    /* Wire Ctrl-Wheel cycling (#button_supports_cycling / #do_but_BLOCK): a plain #menu_fn button
     * has no RNA enum property to step through, so it is skipped unless a #menu_step_func is set
     * explicitly. #button_type_set_menu_from_pulldown matches the type this button would get in a
     * Panel/Toolbar layout (#Layout::item_menu) and avoids the `Menu` assert in
     * #button_menu_step_poll. #Layout::item_menu already performs that conversion itself when the
     * layout root is Panel/Toolbar (the N-panel sidebar case), leaving the button as
     * #ButtonType::Menu already; only convert here for other roots (e.g. the popover) where it is
     * still #ButtonType::Pulldown, otherwise #button_type_set_menu_from_pulldown's own precondition
     * assert fails on the already-converted button. */
    if (block->buttons_ptrs.size() != buttons_num_before) {
      Button *but = block->buttons_ptrs.last().get();
      button_func_menu_step_set(but, image_grid_library_selector_menu_step);
      if (but->type == ButtonType::Pulldown) {
        button_type_set_menu_from_pulldown(but);
      }
    }
  }
  /* Catalog selector: opens a popover with the catalog tree of the active library. */
  image_grid_header_popover(
      row, C, "VIEW3D_PT_image_grid_catalog_selector", ICON_COLLAPSEMENU, is_mask_slot);
    /* Name-match filter: master toggle + Map Types popover (no Tags). */
  {
    Layout &nm_row = row.row(true);
    const bool enabled = state.filter.name_match.enabled;
    PointerRNA props = nm_row.op("VIEW3D_OT_image_grid_name_match_enabled_toggle",
                                 "",
                                 enabled ? ICON_FILTER_FILLED : ICON_FILTER);
    UNUSED_VARS(props);
    /* Outliner-style disclosure (#ICON_DOWNARROW_HLT). Keep the popover button square so
     * #popover_widget_type uses MenuIconRadio (icon only). Widening like catalog/display
     * (#image_grid_header_popover at 1.6u) would add the geometric menu arrow on top and
     * produce a double chevron. */
    Layout &popover_row = nm_row.row(false);
    popover_row.emboss_set(EmbossType::Emboss);
    popover_row.context_int_set("image_grid_is_mask_slot", is_mask_slot ? 1 : 0);
    popover_row.popover(&C, "VIEW3D_PT_image_grid_name_match_filter", "", ICON_DOWNARROW_HLT);
  }
  /* Display settings: preview thumbnail size (shared between texture and mask grids). */
  image_grid_header_popover(row, C, "VIEW3D_PT_image_grid_display", ICON_IMGDISPLAY, is_mask_slot);
}

static void build_image_grid(Layout &layout,
                             const bContext &C,
                             ed::image_grid::ImageGridUIState &state,
                             PointerRNA &ptr,
                             PropertyRNA *prop,
                             const bool is_mask_slot,
                             const bool is_popover)
{
  Block *block = layout.block();
  const std::optional<ed::image_grid::ImageGridOwner> owner = ed::image_grid::image_grid_owner_from_context(C);
  if (!owner) {
    return;
  }

  /* Scroll/grip live in the shared session registry (Stage 4). Ensure this variant's session up
   * front so the column fallback and focus below read/write the pixel truth directly; the view
   * attaches to the same session via #use_session_scroll after it is built. */
  const std::string grid_id = ed::image_grid::image_grid_session_id(*owner, is_mask_slot, is_popover);
  GridSessionState &session = grid_session_state_ensure(grid_id);

  /* While the N-Panel sidebar grid's resize-grip is the region's active button, keep the panels
   * region's scroll offset every layout pass. When the grid shrinks while the region is scrolled,
   * the shorter content would raise #View2D::tot.ymin and snap the view up, drifting the header.
   * Level-triggered by the actual active button rather than a decaying frame counter, so it holds
   * for the whole gesture — including pauses while previews load — and releases immediately after
   * (no phantom space). area.cc consumes the flag per layout pass, so re-set it here each pass. The
   * popover lives in its own temporary region — unaffected. */
  if (!is_popover) {
    if (ARegion *region = CTX_wm_region(&C)) {
      const Button *active_but = region_find_active_but(region);
      if (region->runtime && active_but && active_but->type == ButtonType::Grip &&
          active_but->poin == reinterpret_cast<char *>(&session.grip_pixel_height))
      {
        region->runtime->keep_scroll_offset_on_resize = true;
      }
    }
  }

  /* Wrap the tile grid, scrollbar and resize grip in a themed box so the grid reads as a
   * distinct region (header row and browse/new/open buttons stay outside). #Layout::box() adds
   * #box_padding_px() of padding on every side beyond its content's width, so the width fed to
   * the pixel-exact column math below is shrunk by that amount up front — otherwise the box's
   * drawn outline would overflow the panel by that padding. */
  Layout &grid_box = layout.box();

  const int preview_size = ed::image_grid::image_grid_preview_size_get(*owner);
  const int tile_w = ui::preview_tile_size_x(preview_size);
  const int panel_width = max_ii(layout.width() - 2 * grid_box.box_padding_px(), 0);

  /* Visible row count for this viewport; needed both to reserve the scrollbar gutter below and for
   * the focus scroll further down. */
  const int grip_height = session.grip_pixel_height;
  const int tile_h_hint = ui::preview_tile_size_y_no_label(preview_size);
  const int effective_rows_hint =
      (grip_height >= tile_h_hint) ?
          clamp_i(int(divide_ceil_u(uint(grip_height), uint(tile_h_hint))), 1, 16) :
          (is_popover ? 3 : ed::image_grid::image_grid_effective_rows(*owner, is_mask_slot));

  /* Column count for this panel's width; falls back to the session's last count when the width is
   * not yet known. Sidebar and popover use separate sessions, so neither reuses the other's.
   *
   * When the content overflows the viewport the core draws the overlay scrollbar over the grid's
   * right edge. The tiles are fixed-size and pinned to this column count (#set_cols_per_row_hint),
   * so they always fill the full width — reserve a #V2D_SCROLL_WIDTH gutter (one fewer column when
   * it no longer fits) so the scrollbar sits beside the tiles instead of on top of them. Decided
   * from the full-width column count: narrowing only adds rows, so the overflow can't disappear. */
  const int cols_full = (panel_width > 0) ? max_ii(1, panel_width / max_ii(tile_w, 1)) :
                                            max_ii(1, session.cols);
  const int content_rows_full = (session.cached_item_count > 0) ?
                                    ((session.cached_item_count - 1) / cols_full + 1) :
                                    0;
  const bool reserve_scrollbar = content_rows_full > effective_rows_hint &&
                                 panel_width > int(V2D_SCROLL_WIDTH);
  const int cols_est = reserve_scrollbar ?
                           max_ii(1, (panel_width - int(V2D_SCROLL_WIDTH)) / max_ii(tile_w, 1)) :
                           cols_full;

  /* Publish this context's column count immediately so the focus computation below uses this
   * panel's count, not the previous frame's. */
  session.cols = cols_est;

  /* Focus: when a pending "scroll active texture into view" request applies to this layout the
   * helper returns the target row (-1 = nothing to do) and we set the session's pixel position. */
  const int focus_row = ed::image_grid::image_grid_apply_focus_scroll(
      C, state, cols_est, effective_rows_hint);
  if (focus_row >= 0) {
    session.scroll_px = focus_row * max_ii(1, tile_h_hint);
  }

  auto view_unique = std::make_unique<ImageAssetGridView>(
      C, state, state.filter.lib_ref, ptr, prop, cols_est, is_popover);
  view_unique->set_tile_size(tile_w, ui::preview_tile_size_y_no_label(preview_size));
  view_unique->set_cols_per_row_hint(cols_est);
  const char *grid_view_id = is_mask_slot ? "image_asset_grid_mask" : "image_asset_grid";
  AbstractGridView *grid_view = block_add_view(*block, grid_view_id, std::move(view_unique));
  /* Attach to the shared session so scroll survives the per-refresh rebuild and the unified input
   * handler drives this grid; keyed per variant so all four positions stay independent. */
  grid_view->use_session_scroll(grid_id);

  /* In a popover, picking a texture must not dismiss the panel. The tiles are select-on-click view
   * items, and #force_activate_view_item_but closes the popup on the tap unless the view opts to
   * stay open. Keep it open so the user can pick several textures and keep the Texture panel
   * visible (the sidebar is not a popup, so this is a no-op there). */
  if (is_popover) {
    grid_view->set_popup_keep_open();
  }

  /* Persist a whole-row approximation to DNA for reload reconstruction (nearest row). Done here
   * so the generic core does not need to know about View3D DNA. Removed in Stage 7.
   * Popover height is session-only — never written to DNA. */
  if (!is_popover) {
    const GridViewStyle &style = grid_view->get_style();
    const int tile_h = max_ii(1, style.tile_height);
    const int grip = session.grip_pixel_height;
    if (grip >= tile_h) {
      const short row_count = short(clamp_i(round_fl_to_int(float(grip) / float(tile_h)), 1, 16));
      owner->slot_dna(is_mask_slot).rows = row_count;
    }
  }

  ImageGridStateAccess state_access(session, *owner, grid_id, is_mask_slot, is_popover);
  build_grid_view(C,
                  grid_box,
                  *grid_view,
                  state_access,
                  session.cached_item_count,
                  cols_est,
                  panel_width);
}

void template_asset_image_grid(
    Layout *layout, bContext *C, PointerRNA *ptr, const char *propname, const bool is_popover)
{
  const std::optional<ed::image_grid::ImageGridOwner> owner = ed::image_grid::image_grid_owner_from_context(*C);
  if (!owner || !ptr || !propname) {
    return;
  }
  PropertyRNA *prop = RNA_struct_find_property(ptr, propname);
  if (!prop || RNA_property_type(prop) != PROP_POINTER) {
    return;
  }

  /* Expose the texture-slot pointer to the whole template (tiles included), not just the
   * New/Open/Browse row below, so widget drop-target lookups (e.g. #brush_texture_slot_drop_target_get)
   * can identify this slot regardless of where in the grid the pointer lands. */
  layout->context_ptr_set("image_grid_target", ptr);

  const bool is_mask_slot = ed::image_grid::image_grid_slot_is_mask(*ptr);
  ed::image_grid::ImageGridUIState &state = ed::image_grid::image_grid_state_get(*owner, is_mask_slot);

  ed::image_grid::image_grid_catalog_sanitize_selection(state);
  ed::image_grid::image_grid_pending_apply_if_ready(*C);
  /* Called inside the template redraw; NC_BRUSH already triggered this pass, so
   * #image_grid_notify_change must not be called here to avoid a recursive refresh. */
  ed::image_grid::image_grid_auto_focus_on_brush_change(*C, is_mask_slot);

  Block *block = layout->block();

  block_add_dynamic_listener(block, ed::asset::list::asset_reading_region_listen_fn);
  block_add_dynamic_listener(block, image_grid_block_listener);

  if (ed::image_grid::image_grid_library_is_missing(*owner, is_mask_slot)) {
    /* The header row is drawn first on purpose: it carries the library selector, which is how
     * the user recovers from a missing library. */
    draw_header_row(*layout, state, *C, is_mask_slot);
    layout->label(fmt::format(fmt::runtime(IFACE_("Library \"{}\" not found")),
                              state.filter.lib_ref.custom_library_name)
                      .c_str(),
                  ICON_ERROR);
    layout->label(IFACE_("Pick another library, or restore it in the Preferences"), ICON_NONE);
    return;
  }

  draw_header_row(*layout, state, *C, is_mask_slot);
  build_image_grid(*layout, *C, state, *ptr, prop, is_mask_slot, is_popover);
  add_browse_image_button(*layout, *C, state, *ptr);
}

/** \} */

}  // namespace blender::ui
