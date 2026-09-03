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

#include "interface_templates_intern.hh"

namespace blender::ui {

using ed::image_grid::IMAGE_TEXTURE_SHELF_IDNAME;

constexpr GridViewHostParams IMAGE_GRID_HOST{};

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
 * Preview icon of \a id, safe to hand to a button.
 *
 * Unlike #id_icon_get this never returns a dynamic icon id that has no icon behind it: an ID whose
 * preview cannot be rendered falls back to its type icon, and a preview that is still loading keeps
 * its (registered) deferred icon so the button can draw the spinner.
 */
static int image_grid_preview_icon_id_for_id(const bContext &C, ID &id)
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

/**
 * Return the preview icon attached by #BKE_icon_preview_ensure(), even while deferred loading is
 * in progress. Using the type icon instead would skip #icon_ensure_deferred() / #PreviewLoadJob.
 */
static int image_grid_asset_preview_icon_id(const asset_system::AssetRepresentation &asset)
{
  if (const PreviewImage *preview = asset.get_preview()) {
    /* Checked for existence, not just for being set: the cached id survives the deletion of the
     * icon it names (see #ed::asset::asset_preview_icon_id). */
    if (preview->runtime->icon_id && BKE_icon_exists(preview->runtime->icon_id)) {
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
      if (ELEM(wmn->data, ND_SPACE_VIEW3D, ND_SPACE_IMAGE)) {
        /* Host-space display settings changed (e.g. preview size / rows) — rebuild the grid. */
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


  int get_preview_icon_id(const bContext &C) const
  {
    if (kind_ == ImageGridItemKind::Asset) {
      if (!ed::asset::list::is_loaded(&library_ref_)) {
        return ICON_PREVIEW_LOADING;
      }

      if (ID *local_id = asset_->local_id()) {
        return image_grid_preview_icon_id_for_id(C, *local_id);
      }

      return image_grid_asset_preview_icon_id(*asset_);
    }

    return image_grid_preview_icon_id_for_id(C, image_->id);
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
        &props, "use_mask_slot", ed::image_grid::image_grid_slot_from_context(C) == ed::image_grid::ImageGridSlot::Mask);
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
    /* Local image ID, or an image-typed #WM_DRAG_ASSET. #WM_drag_is_ID_type covers both
     * #WM_DRAG_ID and #WM_DRAG_ASSET (via #WM_drag_get_asset_data with ID_IM); assets of any
     * other type are rejected. */
    if (WM_drag_is_ID_type(&drag, ID_IM)) {
      return true;
    }
    /* Multi-select from the Asset Browser is a separate #WM_DRAG_ASSET_LIST drag. Every drag in
     * the shared list must pass this check; accept the list when it contains at least one image. */
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
/** \name Image-grid data source
 * \{ */

/**
 * Product tiles (#ImageAssetGridItem: asset or local #Image) over
 * #image_grid_foreach_filtered_item. One walk fills the window and returns the filtered count.
 */
class ImageGridDataSource : public GridDataSource {
  ed::image_grid::ImageGridUIState &state_;
  AssetLibraryReference library_ref_;
  PointerRNA target_ptr_;
  PropertyRNA *target_prop_ = nullptr;
  bool is_popover_ = false;

 public:
  ImageGridDataSource(ed::image_grid::ImageGridUIState &state,
                      const AssetLibraryReference &library_ref,
                      const PointerRNA &target_ptr,
                      PropertyRNA *target_prop,
                      const bool is_popover)
      : state_(state),
        library_ref_(library_ref),
        target_ptr_(target_ptr),
        target_prop_(target_prop),
        is_popover_(is_popover)
  {
  }

  bool item_count_ready(const bContext & /*C*/) const override
  {
    return ed::asset::list::is_loaded(&library_ref_);
  }

  int item_count(const bContext &C) const override
  {
    Main *bmain = CTX_data_main(&C);
    return ed::image_grid::image_grid_foreach_filtered_item(
        *bmain, state_, [](const ed::image_grid::ImageGridFilteredItem &, int) { return true; });
  }

  void build_window(const bContext &C, AbstractGridView &view, const IndexRange window) override
  {
    this->build_window_and_count(C, view, window);
  }

  int build_window_and_count(const bContext &C,
                             AbstractGridView &view,
                             const IndexRange window) override
  {
    Main *bmain = CTX_data_main(&C);
    ed::asset::list::storage_fetch(&library_ref_, &C);
    return ed::image_grid::image_grid_foreach_filtered_item(
        *bmain,
        state_,
        [&](const ed::image_grid::ImageGridFilteredItem &item, const int filtered_index) -> bool {
          if (window.contains(filtered_index)) {
            if (item.asset) {
              view.add_item<ImageAssetGridItem>(
                  *item.asset, target_ptr_, target_prop_, library_ref_, is_popover_);
            }
            else {
              view.add_item<ImageAssetGridItem>(
                  *item.image, target_ptr_, target_prop_, library_ref_, is_popover_);
            }
          }
          return true;
        });
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Grid View
 * \{ */

class ImageAssetGridView : public AbstractGridView {
  const bContext &context_;
  std::unique_ptr<GridDataSource> source_;
  PointerRNA target_ptr_;
  /** Column estimate when the session has not yet recorded cols (first redraw). */
  int cols_hint_ = 1;

 public:
  ImageAssetGridView(const bContext &context,
                     ed::image_grid::ImageGridUIState &state,
                     const AssetLibraryReference &library_ref,
                     const PointerRNA &target_ptr,
                     PropertyRNA *target_prop,
                     const int cols_hint,
                     const bool is_popover)
      : context_(context),
        source_(std::make_unique<ImageGridDataSource>(
            state, library_ref, target_ptr, target_prop, is_popover)),
        target_ptr_(target_ptr),
        cols_hint_(max_ii(1, cols_hint))
  {
  }

  void build_items() override
  {
    grid_view_fill_from_source(*this, *source_, context_, session_, cols_hint_, IMAGE_GRID_HOST);
  }

  std::unique_ptr<DropTargetInterface> create_drop_target() override
  {
    return std::make_unique<ImageGridDropTarget>(target_ptr_);
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Owner-backed GridStateAccess adapter
 * \{ */

/** Forwards #GridStateAccess reads/writes to this grid variant's #GridSessionState (the single
 * source of truth for scroll/grip since Stage 4). The owner keeps only the DNA row fallback and
 * focus; callback factories re-derive those from #bContext so no dangling references. */
class ImageGridStateAccess : public GridStateAccess {
  GridSessionState &session_;
  ed::image_grid::ImageGridOwner owner_;
  std::string idname_;
  ed::image_grid::ImageGridSlot grid_slot_;
  /** True when drawn inside a popover; only affects the DNA fallback (popover height is not
   * persisted). Sidebar and popover use separate sessions (distinct #idname_), so grip/scroll are
   * independent without an is-popover branch on every access. */
  bool is_popover_;

 public:
  ImageGridStateAccess(GridSessionState &session,
                       const ed::image_grid::ImageGridOwner owner,
                       std::string idname,
                       const ed::image_grid::ImageGridSlot grid_slot,
                       const bool is_popover)
      : session_(session),
        owner_(owner),
        idname_(std::move(idname)),
        grid_slot_(grid_slot),
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
    return ed::image_grid::image_grid_effective_rows(owner_, grid_slot_);
  }

  std::function<void(bContext &)> make_scroll_widget_fn(const int /*store_cols*/,
                                                        const int /*store_rows*/) const override
  {
    /* The pixel-scale widget already wrote the new #scroll_px directly. Dismiss any pending focus so
     * a manual scroll is not overridden on the next redraw, then redraw. */
    const ed::image_grid::ImageGridSlot grid_slot = grid_slot_;
    return [grid_slot](bContext &C) {
      if (const std::optional<ed::image_grid::ImageGridOwner> owner =
              ed::image_grid::image_grid_owner_from_context(C))
      {
        ed::image_grid::image_grid_focus_clear(
            ed::image_grid::image_grid_state_get(*owner, grid_slot).viewport);
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
      WM_event_add_notifier(&C, NC_ASSET | ND_ASSET_LIST, nullptr);
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

/**
 * The Texture panel's slot holds a #Tex, and #tooltip_from_id knows no preview image for that ID
 * type -- only for the #Image itself. So resolve the image an IMAGE texture points at and show
 * that tooltip, giving this row the same hover preview the PBR paint source picker has, where the
 * slot holds an Image directly. The image is looked up while the tooltip is built, since the
 * texture may point at a different one by then.
 */
static void image_grid_slot_tooltip_set(Button *but, ID *id)
{
  if (but == nullptr || id == nullptr) {
    return;
  }
  if (GS(id->name) != ID_TE) {
    id_preview_tooltip_set(but, id);
    return;
  }
  /* The ID outlives the tooltip (the button is rebuilt whenever the slot changes), so the raw
   * pointer needs no ownership handling. */
  button_func_tooltip_custom_set(
      but,
      [](bContext & /*C*/, TooltipData &tip, Button * /*but*/, void *arg) {
        Tex *texture = static_cast<Tex *>(arg);
        ID *preview_id = (texture->type == TEX_IMAGE && texture->ima != nullptr) ?
                             &texture->ima->id :
                             &texture->id;
        tooltip_from_id(tip, preview_id);
      },
      id,
      nullptr);
}

static void add_browse_image_button(Layout &layout,
                                    bContext &C,
                                    ed::image_grid::ImageGridUIState &state,
                                    PointerRNA &target_ptr)
{
  ed::image_grid::image_grid_prepare_browse_shelf(C, state, IMAGE_TEXTURE_SHELF_IDNAME);

  PropertyRNA *prop = RNA_struct_find_property(&target_ptr, "texture");
  PointerRNA idptr = PointerRNA_NULL;
  if (prop && RNA_property_type(prop) == PROP_POINTER) {
    idptr = RNA_property_pointer_get(&target_ptr, prop);
  }
  ID *id = static_cast<ID *>(idptr.data);

  /* Same row the PBR paint source picker uses (see #template_id_browser): two units tall in both
   * states, so the assigned texture has room for its thumbnail and the empty slot offers a drop
   * area that is easier to hit while dragging. The browse target here is the asset shelf popover,
   * not the ID browser -- the slot holds a Texture, which the shelf is the picker for. */
  Layout &row = layout.row(true);
  row.scale_y_set(2.0f);
  row.context_string_set("asset_shelf_idname", IMAGE_TEXTURE_SHELF_IDNAME);
  /* Makes every button of the row a texture-slot drop target, see #determine_texture_slot_type. */
  row.context_ptr_set("image_grid_target", &target_ptr);

  Block *block = row.block();
  if (id == nullptr) {
    row.popover(&C, "ASSETSHELF_PT_popover_panel", IFACE_("Drop image"), ICON_IMAGE_DATA);
    /* The icon is the button's own, left-aligned one, so the centered label cannot run into it.
     * #BUT_NO_MENU_TRIA drops the dropdown arrow, which is sized from the button height and would
     * be oversized on this deliberately two-unit-tall button. */
    button_drawflag_enable(block->last_but(), BUT_ICON_LEFT | BUT_NO_MENU_TRIA);
  }
  else {
    /* Rendering the preview is deferred, so this may still return the plain type icon at first.
     * #id_icon_get is deliberately not used: for an ID whose preview cannot be rendered it hands
     * back a dynamic icon id with no icon behind it, which the draw code rejects ("no icon for
     * icon ID"). */
    const int preview_icon = image_grid_preview_icon_id_for_id(C, *id);
    row.popover(&C,
                "ASSETSHELF_PT_popover_panel",
                "",
                preview_icon ? preview_icon : ICON_TEXTURE);
    Button *but = block->last_but();
    if (preview_icon) {
      /* Draw the assigned texture as a thumbnail instead of a small icon. The button is widened
       * to stay roughly square, the row's own scale gives it its height. */
      def_but_icon(but, preview_icon, UI_HAS_ICON | BUT_ICON_PREVIEW);
      but->rect.xmax = but->rect.xmin + UI_UNIT_X * 2;
    }
    button_drawflag_enable(but, BUT_NO_MENU_TRIA);
    /* The thumbnail is too small to judge the texture by, so hovering it shows the large preview. */
    image_grid_slot_tooltip_set(but, id);

    /* The name is the widest target in the row -- the part a user naturally clicks to change the
     * assignment -- so it opens the shelf too rather than being a rename field. */
    row.popover(&C, "ASSETSHELF_PT_popover_panel", id->name + 2, ICON_NONE);
    but = block->last_but();
    button_drawflag_enable(but, BUT_TEXT_LEFT | BUT_NO_MENU_TRIA);
    image_grid_slot_tooltip_set(but, id);
  }

  /* Icon-only: the drop button beside them already says what the row is for, and spelling out New
   * and Open would crowd it at this row height. */
  Layout &actions_row = row.row(true);
  actions_row.op("IMAGE_GRID_OT_new", "", ICON_ADD);
  actions_row.op("IMAGE_GRID_OT_open", "", ICON_FILEBROWSER);
  if (id) {
    /* Only meaningful once something is assigned; clicking a tile never clears the slot. Unaligned
     * so it keeps its rounded corners while the parent row's zero spacing still holds it flush
     * against Open (`item_align` passes the alignment group down only into aligned sub-rows). */
    Layout &clear_row = row.row(false);
    clear_row.op("IMAGE_GRID_OT_clear", "", ICON_X);
  }

  /* Same disclosure-triangle pattern as the PBR paint source picker's "Show Source Grid": an
   * icon-only, unembossed toggle at the row's right edge. Drawn last so it keeps that position
   * whether or not Clear is there. */
  Layout &toggle_row = row.row(false);
  toggle_row.emboss_set(EmbossType::None);
  toggle_row.op("IMAGE_GRID_OT_show_grid_toggle",
                "",
                state.show_grid ? ICON_DOWNARROW_HLT : ICON_RIGHTARROW);
}

/** Icon-only popover with menu arrow (same footprint as #ASSETSHELF_PT_display in the shelf
 * header). */
static void image_grid_header_popover(Layout &row,
                                      const bContext &C,
                                      const StringRefNull panel_id,
                                      const int icon,
                                      const ed::image_grid::ImageGridSlot grid_slot)
{
  Block *block = row.block();
  Layout &popover_row = row.row(false);
  popover_row.emboss_set(EmbossType::Emboss);
  popover_row.ui_units_x_set(1.6f);
  popover_row.context_int_set(ed::image_grid::IMAGE_GRID_CONTEXT_SLOT_KEY, int(grid_slot));
  const int64_t buttons_num_before = block->buttons_ptrs.size();
  popover_row.popover(&C, panel_id, "", icon);
  /* #Layout::popover() adds no button (and warns) when `panel_id` isn't a registered panel type. */
  if (block->buttons_ptrs.size() == buttons_num_before) {
    return;
  }
  /* #layout_add_but() marks compact icon buttons as fixed width (#UI_UNIT_X); widen for arrow. */
  Button *but = block->last_but();
  but->rect.xmax = but->rect.xmin + short(1.6f * UI_UNIT_X);
}

/* This grid keeps its membership mode in #ImageGridUIState, the reusable selector speaks
 * #grid_settings::CatalogMode; translate between the two in one place. */
static grid_settings::CatalogMode image_grid_catalog_mode_to_grid_settings(
    const ed::image_grid::ImageGridCatalogMode mode)
{
  switch (mode) {
    case ed::image_grid::ImageGridCatalogMode::Recent:
      return grid_settings::CatalogMode::Recent;
    case ed::image_grid::ImageGridCatalogMode::Favorites:
      return grid_settings::CatalogMode::Favorites;
    default:
      return grid_settings::CatalogMode::All;
  }
}

static ed::image_grid::ImageGridCatalogMode image_grid_catalog_mode_from_grid_settings(
    const grid_settings::CatalogMode mode)
{
  BLI_assert(ELEM(mode, grid_settings::CatalogMode::Recent, grid_settings::CatalogMode::Favorites));
  return (mode == grid_settings::CatalogMode::Recent) ?
             ed::image_grid::ImageGridCatalogMode::Recent :
             ed::image_grid::ImageGridCatalogMode::Favorites;
}

/* Draw the asset-library choices as a plain vertical menu, through the same drawer the reusable
 * grid selector uses (#library_selector_menu_draw_items): identical heading, Recent/Favorites rows,
 * folder grouping and active highlight. Only the item source (image libraries) and what a pick does
 * are this grid's own -- its state lives in #ImageGridUIState, not in an RNA property, so the picks
 * are applied through callbacks (the same functions #IMAGE_GRID_OT_set_library and
 * #IMAGE_GRID_OT_set_membership call). The slot (texture vs mask) stays in the menu's own context
 * for anything else drawn from here that resolves it (#image_grid_slot_from_context). */
static void image_grid_library_selector_menu_draw(bContext *C, Layout *layout, void *arg)
{
  const ed::image_grid::ImageGridSlot grid_slot = ed::image_grid::image_grid_slot_from_int(
      POINTER_AS_INT(arg));
  layout->context_int_set(ed::image_grid::IMAGE_GRID_CONTEXT_SLOT_KEY, int(grid_slot));

  const std::optional<ed::image_grid::ImageGridOwner> owner_opt =
      ed::image_grid::image_grid_owner_from_context(*C);
  if (!owner_opt) {
    return;
  }
  const ed::image_grid::ImageGridUIState &state = ed::image_grid::image_grid_state_get(*owner_opt,
                                                                                       grid_slot);

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

  LibrarySelectorMenuParams params;
  params.items = items;
  params.title = IFACE_("Asset Library");
  params.current_library_value = ed::asset::library_reference_to_enum_value(&state.filter.lib_ref);
  params.current_mode = image_grid_catalog_mode_to_grid_settings(state.filter.catalog_mode);
  params.show_membership = true;
  /* The owner resolved here, while the panel is being drawn, is the fallback: a click is handled
   * from the popup, where the context may no longer name the owning space. */
  const ed::image_grid::ImageGridOwner owner_at_draw = *owner_opt;
  params.apply_library = [grid_slot, owner_at_draw](bContext &C, const int library_enum_value) {
    const ed::image_grid::ImageGridOwner owner =
        ed::image_grid::image_grid_owner_from_context(C).value_or(owner_at_draw);
    const AssetLibraryReference new_ref = ed::asset::library_reference_from_enum_value(
        library_enum_value);
    ed::image_grid::image_grid_set_library(C, owner, grid_slot, new_ref);
  };
  params.apply_membership = [grid_slot, owner_at_draw](bContext &C,
                                                       const grid_settings::CatalogMode mode) {
    const ed::image_grid::ImageGridOwner owner =
        ed::image_grid::image_grid_owner_from_context(C).value_or(owner_at_draw);
    ed::image_grid::image_grid_set_membership(
        C, owner, grid_slot, image_grid_catalog_mode_from_grid_settings(mode));
  };

  library_selector_menu_draw_items(*C, *layout, params);

  MEM_delete(items);
}

/* Ctrl-Wheel cycling for the library-selector button (#button_supports_cycling /
 * #do_but_BLOCK): steps through the same entries the menu draws, in the same order (Recent,
 * Favorites, then the libraries -- see #library_selector_step), and applies the change through
 * #image_grid_set_library / #image_grid_set_membership (shared with #IMAGE_GRID_OT_set_library and
 * #IMAGE_GRID_OT_set_membership so behavior matches picking an entry from the menu).
 *
 * The button is a plain #Layout::menu_fn button, so the return value is unused (nothing applies it
 * to an RNA property); the work happens here. */
static int image_grid_library_selector_menu_step(bContext *C, int direction, Button *but)
{
  const ed::image_grid::ImageGridSlot grid_slot = ed::image_grid::image_grid_slot_from_int(
      POINTER_AS_INT(but->poin));

  const std::optional<ed::image_grid::ImageGridOwner> owner_opt =
      ed::image_grid::image_grid_owner_from_context(*C);
  if (!owner_opt) {
    return 0;
  }
  const ed::image_grid::ImageGridOwner owner = *owner_opt;
  const ed::image_grid::ImageGridUIState &state = ed::image_grid::image_grid_state_get(owner,
                                                                                   grid_slot);

  const EnumPropertyItem *items = ed::asset::library_reference_to_rna_enum_itemf(
      /*include_readonly=*/true,
      /*include_current_file=*/true,
      /*include_remote_libraries=*/false,
      /*include_separate_online_essentials=*/false,
      /*exclude_image_libraries=*/false,
      /*only_image_libraries=*/true);
  if (!items) {
    return 0;
  }

  /* Real library entries only, same order as the menu (folder headings/separators skipped). */
  Vector<int> library_values;
  for (const EnumPropertyItem *item = items; item->identifier; item++) {
    if (item->identifier[0]) {
      library_values.append(item->value);
    }
  }
  MEM_delete(items);

  const std::optional<LibraryStepResult> result = library_selector_step(
      library_values,
      /*include_membership=*/true,
      image_grid_catalog_mode_to_grid_settings(state.filter.catalog_mode),
      ed::asset::library_reference_to_enum_value(&state.filter.lib_ref),
      direction);
  if (!result) {
    return 0;
  }

  if (result->is_membership) {
    ed::image_grid::image_grid_set_membership(
        *C, owner, grid_slot, image_grid_catalog_mode_from_grid_settings(result->mode));
    return 0;
  }

  const AssetLibraryReference new_ref = ed::asset::library_reference_from_enum_value(
      result->library_enum_value);
  ed::image_grid::image_grid_set_library(*C, owner, grid_slot, new_ref);
  return 0;
}

static void draw_header_row(Layout &layout,
                            ed::image_grid::ImageGridUIState &state,
                            const bContext &C,
                            const ed::image_grid::ImageGridSlot grid_slot)
{
  layout.context_int_set(ed::image_grid::IMAGE_GRID_CONTEXT_SLOT_KEY, int(grid_slot));

  Layout &row = layout.row(true);
  /* Library selector: a vertical menu of asset libraries (the operator's enum dropdown lays folder
   * headings out as side-by-side columns; this reads top to bottom). */
  {
    Block *block = row.block();
    const int64_t buttons_num_before = block->buttons_ptrs.size();
    row.menu_fn(ed::image_grid::image_grid_library_selector_label(state),
                ICON_ASSET_MANAGER,
                image_grid_library_selector_menu_draw,
                POINTER_FROM_INT(int(grid_slot)));
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
      Button *but = block->last_but();
      button_func_menu_step_set(but, image_grid_library_selector_menu_step);
      if (but->type == ButtonType::Pulldown) {
        button_type_set_menu_from_pulldown(but);
      }
    }
  }
  /* Catalog selector: opens a popover with the catalog tree of the active library. */
  image_grid_header_popover(
      row, C, ed::image_grid::IMAGE_GRID_PT_CATALOG_SELECTOR, ICON_COLLAPSEMENU, grid_slot);
    /* Name-match filter: master toggle + Map Types popover (no Tags). */
  {
    Layout &nm_row = row.row(true);
    const bool enabled = state.filter.name_match.enabled;
    PointerRNA props = nm_row.op("IMAGE_GRID_OT_name_match_enabled_toggle",
                                 "",
                                 enabled ? ICON_FILTER_FILLED : ICON_FILTER);
    UNUSED_VARS(props);
    /* Outliner-style disclosure (#ICON_DOWNARROW_HLT). Keep the popover button square so
     * #popover_widget_type uses MenuIconRadio (icon only). Widening like catalog/display
     * (#image_grid_header_popover at 1.6u) would add the geometric menu arrow on top and
     * produce a double chevron. */
    Layout &popover_row = nm_row.row(false);
    popover_row.emboss_set(EmbossType::Emboss);
    popover_row.enabled_set(true);
    popover_row.context_int_set(ed::image_grid::IMAGE_GRID_CONTEXT_SLOT_KEY, int(grid_slot));
    popover_row.popover(&C, ed::image_grid::IMAGE_GRID_PT_NAME_MATCH_FILTER, "", ICON_DOWNARROW_HLT);
  }
  /* Display settings: preview thumbnail size (shared between texture and mask grids). */
  image_grid_header_popover(row, C, ed::image_grid::IMAGE_GRID_PT_DISPLAY, ICON_IMGDISPLAY, grid_slot);
}

/**
 * While the N-Panel sidebar grid's resize-grip is the region's active button, keep the panels
 * region's scroll offset every layout pass. When the grid shrinks while the region is scrolled,
 * the shorter content would raise #View2D::tot.ymin and snap the view up, drifting the header.
 * Level-triggered by the actual active button rather than a decaying frame counter, so it holds
 * for the whole gesture -- including pauses while previews load -- and releases immediately after
 * (no phantom space). `area.cc` consumes the flag per layout pass, so it is re-set here each pass.
 *
 * \note The popover lives in its own temporary region and is unaffected, so it never calls this.
 */
static void image_grid_hold_region_scroll_during_grip_drag(const bContext &C,
                                                           const GridSessionState &session)
{
  ARegion *region = CTX_wm_region(&C);
  if (region == nullptr || region->runtime == nullptr) {
    return;
  }
  const Button *active_but = region_find_active_but(region);
  if (active_but && active_but->type == ButtonType::Grip &&
      active_but->poin == reinterpret_cast<const char *>(&session.grip_pixel_height))
  {
    region->runtime->keep_scroll_offset_on_resize = true;
  }
}

namespace {

/** Tile and column geometry #build_image_grid derives from the panel width and preview size. */
struct ImageGridTileLayout {
  int tile_w = 1;
  int tile_h = 1;
  int panel_width = 0;
  int cols_est = 1;
  /** Visible row count for this viewport, needed before the view exists. */
  int effective_rows_hint = 1;
};

}  // namespace

/**
 * Resolve the tile size and column count for this panel's width.
 *
 * When the content overflows the viewport the core draws the overlay scrollbar over the grid's
 * right edge. The tiles are fixed-size and pinned to the column count (#set_cols_per_row_hint), so
 * they always fill the full width -- a #V2D_SCROLL_WIDTH gutter is reserved (one fewer column when
 * it no longer fits) so the scrollbar sits beside the tiles instead of on top of them. Decided
 * from the full-width column count: narrowing only adds rows, so the overflow cannot disappear.
 */
static ImageGridTileLayout image_grid_tile_layout(
    const GridSessionState &session,
    const ed::image_grid::ImageGridOwner owner,
    const ed::image_grid::ImageGridSlot grid_slot,
    const int panel_width,
    const bool is_popover)
{
  ImageGridTileLayout tiles;
  tiles.panel_width = panel_width;

  const int preview_size = ed::image_grid::image_grid_preview_size_get(owner);
  tiles.tile_w = ui::preview_tile_size_x(preview_size);
  tiles.tile_h = ui::preview_tile_size_y_no_label(preview_size);

  const int grip_height = session.grip_pixel_height;
  tiles.effective_rows_hint =
      (grip_height >= tiles.tile_h) ?
          clamp_i(int(divide_ceil_u(uint(grip_height), uint(tiles.tile_h))),
                  1,
                  IMAGE_GRID_HOST.max_rows) :
          (is_popover ? 3 : ed::image_grid::image_grid_effective_rows(owner, grid_slot));

  /* Falls back to the session's last column count when the width is not yet known. Sidebar and
   * popover use separate sessions, so neither reuses the other's. */
  const int cols_full = (panel_width > 0) ? max_ii(1, panel_width / max_ii(tiles.tile_w, 1)) :
                                            max_ii(1, session.cols);
  const int content_rows_full = (session.cached_item_count > 0) ?
                                    ((session.cached_item_count - 1) / cols_full + 1) :
                                    0;
  const bool reserve_scrollbar = content_rows_full > tiles.effective_rows_hint &&
                                 panel_width > int(V2D_SCROLL_WIDTH);
  tiles.cols_est = reserve_scrollbar ? max_ii(1,
                                              (panel_width - int(V2D_SCROLL_WIDTH)) /
                                                  max_ii(tiles.tile_w, 1)) :
                                       cols_full;
  return tiles;
}

/**
 * Persist a whole-row approximation of the grip height to DNA, so a reloaded file reconstructs a
 * similar viewport. Done here rather than in the generic core, which must not know about DNA.
 * Popover height is session-only and never written.
 */
static void image_grid_persist_rows_to_dna(const GridSessionState &session,
                                           const AbstractGridView &grid_view,
                                           const ed::image_grid::ImageGridOwner owner,
                                           const ed::image_grid::ImageGridSlot grid_slot)
{
  const int tile_h = max_ii(1, grid_view.get_style().tile_height);
  const int grip = session.grip_pixel_height;
  if (grip < tile_h) {
    return;
  }
  owner.slot_dna(grid_slot).rows = short(
      clamp_i(round_fl_to_int(float(grip) / float(tile_h)), 1, IMAGE_GRID_HOST.max_rows));
}

static void build_image_grid(Layout &layout,
                             const bContext &C,
                             ed::image_grid::ImageGridUIState &state,
                             PointerRNA &ptr,
                             PropertyRNA *prop,
                             const ed::image_grid::ImageGridSlot grid_slot,
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
  const std::string grid_id = ed::image_grid::image_grid_session_id(*owner, grid_slot, is_popover);
  GridSessionState &session = grid_session_state_ensure(grid_id);

  if (!is_popover) {
    image_grid_hold_region_scroll_during_grip_drag(C, session);
  }

  /* Wrap the tile grid, scrollbar and resize grip in a themed box so the grid reads as a
   * distinct region (header row and browse/new/open buttons stay outside). #Layout::box() adds
   * #box_padding_px() of padding on every side beyond its content's width, so the width fed to
   * the pixel-exact column math below is shrunk by that amount up front — otherwise the box's
   * drawn outline would overflow the panel by that padding. */
  Layout &grid_box = layout.box();

  const ImageGridTileLayout tiles = image_grid_tile_layout(
      session,
      *owner,
      grid_slot,
      max_ii(layout.width() - 2 * grid_box.box_padding_px(), 0),
      is_popover);

  /* Publish this context's column count immediately so the focus computation below uses this
   * panel's count, not the previous frame's. */
  session.cols = tiles.cols_est;

  /* Focus: when a pending "scroll active texture into view" request applies to this layout the
   * helper returns the target row (-1 = nothing to do) and we set the session's pixel position. */
  const int focus_row = ed::image_grid::image_grid_apply_focus_scroll(
      C, state, tiles.cols_est, tiles.effective_rows_hint);
  if (focus_row >= 0) {
    session.scroll_px = focus_row * max_ii(1, tiles.tile_h);
  }

  auto view_unique = std::make_unique<ImageAssetGridView>(
      C, state, state.filter.lib_ref, ptr, prop, tiles.cols_est, is_popover);
  view_unique->set_tile_size(tiles.tile_w, tiles.tile_h);
  view_unique->set_cols_per_row_hint(tiles.cols_est);
  const char *grid_view_id = grid_slot == ed::image_grid::ImageGridSlot::Mask ? "image_asset_grid_mask" : "image_asset_grid";
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

  if (!is_popover) {
    image_grid_persist_rows_to_dna(session, *grid_view, *owner, grid_slot);
  }

  ImageGridStateAccess state_access(session, *owner, grid_id, grid_slot, is_popover);
  /* Filter row backed by the runtime state rather than RNA: this host has no #GridViewSettings.
   * Both pointers live in the owner's runtime slot, so they outlive this block. */
  GridViewHostParams host_params = IMAGE_GRID_HOST;
  host_params.filter_show = &state.show_filter;
  host_params.filter_search_buf = state.filter.search;
  host_params.filter_search_maxncpy = sizeof(state.filter.search);

  build_grid_view(C,
                  grid_box,
                  *grid_view,
                  state_access,
                  session.cached_item_count,
                  tiles.cols_est,
                  tiles.panel_width,
                  host_params);
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
   * can identify this slot regardless of where in the grid the pointer lands. Host space and
   * slot go on the same store so popovers/operators do not depend on #CTX_wm_view3d /
   * #CTX_wm_space_image. */
  const ed::image_grid::ImageGridSlot grid_slot = ed::image_grid::image_grid_slot_from_texture_ptr(*ptr);
  layout->context_ptr_set("image_grid_target", ptr);
  PointerRNA owner_ptr = owner->owner_rna();
  layout->context_ptr_set(ed::image_grid::IMAGE_GRID_CONTEXT_OWNER_KEY, &owner_ptr);
  layout->context_int_set(ed::image_grid::IMAGE_GRID_CONTEXT_SLOT_KEY, int(grid_slot));

  ed::image_grid::ImageGridUIState &state = ed::image_grid::image_grid_state_get(*owner, grid_slot);

  ed::image_grid::image_grid_catalog_sanitize_selection(state);
  /* Called inside the template redraw; NC_BRUSH already triggered this pass, so
   * #image_grid_notify_change must not be called here to avoid a recursive refresh. */
  ed::image_grid::image_grid_auto_focus_on_brush_change(*C, grid_slot);

  Block *block = layout->block();

  block_add_dynamic_listener(block, ed::asset::list::asset_reading_region_listen_fn);
  block_add_dynamic_listener(block, image_grid_block_listener);

  if (ed::image_grid::image_grid_library_is_missing(*owner, grid_slot)) {
    /* The assigned slot still works with no library, so its row stays; the header row follows it
     * because it carries the library selector, which is how the user recovers from this state. */
    add_browse_image_button(*layout, *C, state, *ptr);
    if (!state.show_grid) {
      return;
    }
    layout->separator();
    draw_header_row(*layout, state, *C, grid_slot);
    layout->label(fmt::format(fmt::runtime(IFACE_("Library \"{}\" not found")),
                              state.filter.lib_ref.custom_library_name)
                      .c_str(),
                  ICON_ERROR);
    layout->label(IFACE_("Pick another library, or restore it in the Preferences"), ICON_NONE);
    return;
  }

  /* First: it shows what is currently assigned, which is the subject of the whole template. The
   * library and catalog selectors below it only narrow down what the grid offers. */
  add_browse_image_button(*layout, *C, state, *ptr);
  if (!state.show_grid) {
    return;
  }
  /* Sets the assigned slot apart from the library and catalog selectors, which browse rather than
   * assign. */
  layout->separator();
  draw_header_row(*layout, state, *C, grid_slot);
  build_image_grid(*layout, *C, state, *ptr, prop, grid_slot, is_popover);
}

/** \} */

}  // namespace blender::ui
