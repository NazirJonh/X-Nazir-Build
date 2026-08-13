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

#include <fmt/format.h>

#include "AS_asset_representation.hh"

#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_preview_image.hh"
#include "BKE_screen.hh"
#include "BKE_wm_runtime.hh"
#include "BKE_name_matching.hh"

#include "BLI_listbase.h"
#include "BLI_rect.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"

#include "BLT_translation.hh"

#include "DNA_asset_types.h"
#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_space_types.h"
#include "DNA_view3d_types.h"
#include "DNA_windowmanager_types.h"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "ED_asset.hh"
#include "ED_asset_import.hh"
#include "ED_asset_list.hh"
#include "ED_asset_menu_utils.hh"
#include "ED_screen.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "UI_grid_view.hh"
#include "UI_interface.hh"
#include "UI_interface_c.hh"
#include "UI_interface_icons.hh"
#include "BLI_vector.hh"

#include "interface_grid_view_settings_utils.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "interface_intern.hh"
#include "interface_templates_intern.hh"

/* #shelf_asset_lists_record_recent / #ShelfAssetRef for the Recent list write-on-activate below.
 * See #interface_template_id_browser_asset.cc for the read side and why this internal header is
 * reused across modules the same way. */
#include "../../asset/intern/asset_shelf_asset_lists.hh"

namespace blender::ui {

/**
 * Height of the scrollable image grid viewport (in #UI_UNIT_Y).
 * Keep header rows + this value within the popover #PopupBlockHandle::max_size_y (~16 units).
 */
static constexpr float ID_BROWSER_GRID_VIEWPORT_UNITS_Y = 18.0f;
/** Popover content width (in #UI_UNIT_X). List rows and grid columns use this. */
static constexpr float ID_BROWSER_POPOVER_UNITS_X = 15.0f;
/**
 * Minimum width (in #UI_UNIT_X) of a single list-mode column. The popover packs its row width into
 * as many equal columns as fit at this minimum, so a widened popover lays items out in horizontal
 * columns instead of one tall single column.
 */
static constexpr float ID_BROWSER_LIST_MIN_COL_UNITS_X = 14.0f;

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

/**
 * Raw pointer to the space's `image_filter_mode` DNA field. Each of the three space types
 * re-declares its own field (rather than sharing one through #SpaceLink), so the concrete type is
 * needed to address it; used for a direct bit-toggle button (#uiDefButBit) instead of going
 * through RNA, since the "All" toggle flips a single bit without disturbing the others.
 */
static char *image_filter_mode_pointer(SpaceLink *sl)
{
  switch (sl->spacetype) {
    case SPACE_IMAGE:
      return &reinterpret_cast<SpaceImage *>(sl)->image_filter_mode;
    case SPACE_NODE:
      return &reinterpret_cast<SpaceNode *>(sl)->image_filter_mode;
    case SPACE_VIEW3D:
      return &reinterpret_cast<View3D *>(sl)->image_filter_mode;
    default:
      return nullptr;
  }
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

/** Compact image-filter-mode radio on row 2; fixed intrinsic width so labels do not stretch. */
static void id_browser_image_filter_mode_button(Layout &parent,
                                                PointerRNA *space_ptr,
                                                const char *identifier,
                                                const char *label,
                                                const int icon,
                                                const bool active)
{
  Layout &item = parent.row(false);
  /* Non-Expand alignment is required for #text_icon_width_ex() to size the button from its
   * actual text/icon instead of falling back to a fixed 10 `UI_UNIT_X` width (see
   * `layout_vary_direction()` in `interface_layout.cc`). */
  item.alignment_set(LayoutAlign::Left);
  item.fixed_size_set(true);
  item.active_set(active);
  item.prop_enum(space_ptr, "image_filter_mode", identifier, IFACE_(label), icon);
}

/**
 * "Slot" filter button, drawn as a direct bit toggle on the `image_filter_mode` DNA field instead
 * of going through #id_browser_image_filter_mode_button's exact-value #prop_enum. That keeps this
 * button's pressed state tied to whether #TEMPLATE_ID_FILTER_SLOT_TYPE is set at all, regardless of
 * the independent #TEMPLATE_ID_FILTER_CURRENT_MATERIAL bit the "All" toggle flips (row 2) — so
 * toggling that control never makes this button (and the slot-type row it gates) look deselected.
 */
static void id_browser_slot_type_filter_button(Layout &parent, char *filter_mode_poin)
{
  Layout &item = parent.row(false);
  item.fixed_size_set(true);
  Block *block = item.block();
  /* #ButtonType::ButToggle, not #ButtonType::IconToggle: the latter auto-advances the drawn icon
   * by one (#button_icon, #but->iconadd) whenever the button is pressed and has no `rnaprop` —
   * meant for consecutive on/off icon pairs, which this single fixed icon is not. */
  uiDefIconButBit<char>(block,
                        ButtonType::ButToggle,
                        TEMPLATE_ID_FILTER_SLOT_TYPE,
                        ICON_NODE_TEXTURE,
                        0,
                        0,
                        short(UI_UNIT_X),
                        short(UI_UNIT_Y),
                        filter_mode_poin,
                        0.0f,
                        0.0f,
                        TIP_("Show images used by a specific paint slot type"));
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

/**
 * The ID browser can list two kinds of items: local data-blocks (#LocalID) or assets from an
 * asset library (#Asset, browsed but not necessarily imported yet). Kept as one item class (rather
 * than two) so list-mode layout, tooltips and #select_on_click_set() are not duplicated; see
 * #ImageAssetGridItem (interface_template_asset_image_grid.cc) for the same pattern.
 */
enum class IDBrowserItemKind { LocalID, Asset };

class IDBrowserGridItem : public PreviewGridItem {
  IDBrowserItemKind kind_;
  ID *id_ = nullptr;
  asset_system::AssetRepresentation *asset_ = nullptr;
  /** Only meaningful for #IDBrowserItemKind::Asset: needed to gate the preview on
   * #ed::asset::list::is_loaded(). */
  AssetLibraryReference asset_library_ref_ = {};
  bool list_mode_;

  void install_id_preview_tooltip() const
  {
    Button *item_but = this->view_item_button();
    if (item_but == nullptr) {
      return;
    }
    if (kind_ == IDBrowserItemKind::Asset) {
      button_func_tooltip_custom_set(
          item_but,
          [](bContext &C, TooltipData &tip, Button * /*but*/, void *arg) {
            asset_system::AssetRepresentation &asset =
                *static_cast<asset_system::AssetRepresentation *>(arg);
            ed::asset::asset_tooltip(&C, asset, tip);

            /* #asset_tooltip is text-only; append the same preview image #tooltip_from_id shows
             * for a local ID below, using whatever preview the grid tile itself already triggered
             * (see #build_grid_tile's #ensure_previewable call). */
            const PreviewImage *preview = asset.get_preview();
            if (preview == nullptr || !BKE_previewimg_is_finished(preview, ICON_SIZE_PREVIEW)) {
              return;
            }
            ImBuf *ibuf = BKE_previewimg_to_imbuf(preview, ICON_SIZE_PREVIEW);
            if (ibuf == nullptr) {
              return;
            }
            TooltipImage image_data;
            image_data.ibuf = ibuf;
            image_data.width = short(ibuf->x);
            image_data.height = short(ibuf->y);
            image_data.border = true;
            image_data.background = TooltipImageBackground::Checkerboard_Themed;
            image_data.premultiplied = true;
            tooltip_text_field_add(tip, {}, {}, TIP_STYLE_SPACER, TIP_LC_NORMAL);
            tooltip_text_field_add(tip, {}, {}, TIP_STYLE_SPACER, TIP_LC_NORMAL);
            tooltip_image_field_add(tip, image_data);
            IMB_freeImBuf(ibuf);
          },
          asset_,
          nullptr);
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

  /**
   * Mirrors #image_grid_asset_preview_icon_id (interface_template_asset_image_grid.cc): the
   * preview attached by #BKE_icon_preview_ensure(), even while deferred loading is in progress.
   * While the library has not finished loading, show a spinner instead of a stale/default icon.
   */
  int asset_preview_icon_id() const
  {
    if (!ed::asset::list::is_loaded(&asset_library_ref_)) {
      return ICON_PREVIEW_LOADING;
    }
    if (const PreviewImage *preview = asset_->get_preview()) {
      if (preview->runtime->icon_id) {
        return preview->runtime->icon_id;
      }
    }
    return ed::asset::asset_preview_or_icon(*asset_);
  }

 public:
  IDBrowserGridItem(
      StringRef identifier, StringRef label, int preview_icon_id, ID *id, const bool list_mode)
      : PreviewGridItem(identifier, label, preview_icon_id),
        kind_(IDBrowserItemKind::LocalID),
        id_(id),
        list_mode_(list_mode)
  {
    /* Activate on release (KM_CLICK), not on press — otherwise the item is selected (and the
     * popup closed) on the initial touch-down, before the drag-scroll handler's MOUSEMOVE
     * threshold check can recognize the gesture as a scroll. */
    this->select_on_click_set();
  }

  IDBrowserGridItem(StringRef identifier,
                    StringRef label,
                    asset_system::AssetRepresentation &asset,
                    const AssetLibraryReference &library_ref,
                    const bool list_mode)
      : PreviewGridItem(identifier, label, ICON_NONE),
        kind_(IDBrowserItemKind::Asset),
        asset_(&asset),
        asset_library_ref_(library_ref),
        list_mode_(list_mode)
  {
    this->select_on_click_set();
  }

  StringRef get_rename_string() const override
  {
    /* Used by grid-view search filtering; base class returns null. */
    return label;
  }

  /**
   * Overlay favorite star, top-right of the tile (mirrors #AssetViewItem::build_grid_tile's star
   * in `asset_shelf_asset_view.cc`, minus the online/download indicators this browser has no use
   * for). Reuses #ASSETSHELF_OT_asset_favorite_toggle: its poll/exec resolve the shelf idname from
   * context ("asset_shelf_idname", set on \a overlap below), and the ID Browser's synthetic
   * per-idcode idname (#id_browser_shelf_idname) is accepted by #shelf_supports_asset_lists() the
   * same way a real asset-shelf idname is.
   */
  void build_favorite_star(Layout &overlap) const
  {
    const short idcode = short(asset_->get_id_type());
    const std::string shelf_idname = id_browser_shelf_idname(idcode);
    const bool is_favorite = ed::asset::shelf::shelf_asset_lists_is_favorite(
        shelf_idname, asset_->make_weak_reference());
    /* A favorite always shows its star so the state is visible at a glance; a non-favorite only
     * reveals it on hover, keeping the grid uncluttered otherwise. */
    if (!is_favorite && !this->is_hovered()) {
      return;
    }

    Layout &overlay_row = overlap.column(false).row(true);
    overlay_row.alignment_set(LayoutAlign::Right);
    overlay_row.context_string_set("asset_shelf_idname", shelf_idname);

    Block *overlay_block = overlay_row.block();
    block_layout_set_current(overlay_block, &overlay_row);
    Button *favorite_but = uiDefIconButO(overlay_block,
                                         ButtonType::But,
                                         "ASSETSHELF_OT_asset_favorite_toggle",
                                         wm::OpCallContext::ExecDefault,
                                         is_favorite ? ICON_SOLO_ON : ICON_SOLO_OFF,
                                         0,
                                         0,
                                         short(ICON_DEFAULT_WIDTH_SCALE),
                                         short(ICON_DEFAULT_HEIGHT_SCALE),
                                         std::nullopt);
    PointerRNA *favorite_opptr = button_operator_ptr_ensure(favorite_but);
    ed::asset::operator_asset_reference_props_set(*asset_, *favorite_opptr);
    /* The star sits on top of the preview image, which can be any color: draw it as a white icon
     * over a dark circle, like the equivalent asset-shelf star. */
    button_pushbutton_draw_as_overlay_set(favorite_but, true);
    button_pushbutton_overlay_alpha_factor_set(favorite_but, this->is_hovered() ? 1.0f : 0.8f);

  }

  void build_grid_tile(const bContext &C, Layout &layout) const override
  {
    if (kind_ == IDBrowserItemKind::Asset) {
      /* Deferred thumbnail loading (#PreviewLoadJob); without this an external asset has no
       * preview at all. Matches #AssetViewItem::build_grid_tile. */
      asset_->ensure_previewable(C);
    }

    if (!list_mode_) {
      if (kind_ == IDBrowserItemKind::Asset) {
        /* Overlap layout so the favorite star can sit on top of the preview button. */
        Layout &overlap = layout.overlap();
        this->build_grid_tile_button(overlap.column(true), this->asset_preview_icon_id());
        this->build_favorite_star(overlap);
      }
      else {
        PreviewGridItem::build_grid_tile(C, layout);
      }
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
    const int icon_id = (kind_ == IDBrowserItemKind::Asset) ? this->asset_preview_icon_id() :
                                                              preview_icon_id;
    def_but_icon(icon_but, icon_id, UI_HAS_ICON | BUT_ICON_PREVIEW);
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

static bool id_browser_asset_passes_name_match(const NameMatchResolvedFilter &resolved,
                                               const asset_system::AssetRepresentation &asset)
{
  Vector<StringRef> metadata_tags;
  for (const AssetTag &tag : asset.get_metadata().tags) {
    metadata_tags.append(tag.name);
  }
  return BKE_name_match_resolved_asset_passes(resolved, asset.get_name(), metadata_tags);
}

static NameMatchFilterState id_browser_name_match_state_from_wm(wmWindowManager &wm)
{
  PointerRNA wm_ptr = RNA_id_pointer_create(&wm.id);
  PointerRNA settings_ptr = RNA_pointer_get(&wm_ptr, "id_browser_grid_view_settings");
  if (settings_ptr.data == nullptr) {
    return {};
  }
  return grid_settings::name_match_filter_get(settings_ptr);
}

class IDBrowserView : public AbstractGridView {
  PointerRNA target_ptr_;
  PropertyRNA *target_prop_;
  Main *bmain_;
  const bContext *context_;
  /** Data-block list for the target property's ID type (#which_libbase). Null when #source_ is
   * #ID_BROWSER_SOURCE_ASSET_LIBRARY: that source iterates the asset library instead, see
   * #build_items(). */
  ListBaseT<ID> *idlb_;
  IDBrowserFilter filter_;
  bool list_mode_;
  /** #eIDBrowserSource, stored as `int` since #wmWindowManager::id_browser_source is a `char`
   * bitfield-sized DNA enum and comparing against it directly is simplest. */
  int source_;
  AssetLibraryReference asset_library_ref_;
  grid_settings::CatalogMode catalog_mode_;
  short idcode_;
  NameMatchFilterState name_match_;

 public:
  IDBrowserView(PointerRNA target_ptr,
                PropertyRNA *target_prop,
                Main *bmain,
                const bContext *C,
                ListBaseT<ID> *idlb,
                const IDBrowserFilter &filter,
                const bool list_mode,
                const int source,
                const AssetLibraryReference &asset_library_ref,
                const grid_settings::CatalogMode catalog_mode,
                const short idcode,
                NameMatchFilterState name_match)
      : target_ptr_(target_ptr),
        target_prop_(target_prop),
        bmain_(bmain),
        context_(C),
        idlb_(idlb),
        filter_(filter),
        list_mode_(list_mode),
        source_(source),
        asset_library_ref_(asset_library_ref),
        catalog_mode_(catalog_mode),
        idcode_(idcode),
        name_match_(std::move(name_match))
  {
  }

  void build_items() override
  {
    const PointerRNA active_ptr = RNA_property_pointer_get(&target_ptr_, target_prop_);
    const ID *active_id = active_ptr.data ? static_cast<ID *>(active_ptr.data) : nullptr;

    const NameMatchResolvedFilter name_match_resolved = BKE_name_match_filter_resolve(
        name_match_, U);

    if (source_ == ID_BROWSER_SOURCE_ASSET_LIBRARY) {
      /* The script-defined filter takes an `ID &`, so it can only be applied to assets that
       * already have a local ID. Assets that are not imported yet are shown — hiding them
       * would silently hide exactly what this source exists to show. The built-in paint-slot
       * filter (#IDBrowserFilter::paint_mode/material/slot_type) is not applied here: it
       * needs a local #Image, which an unimported asset does not have. */
      auto add_if_passes = [&](asset_system::AssetRepresentation &asset) -> bool {
        if (filter_.custom != nullptr) {
          if (ID *local_id = asset.local_id()) {
            if (!id_filter_type_poll(*filter_.custom, *context_, *local_id)) {
              return true;
            }
          }
        }
        if (!id_browser_asset_passes_name_match(name_match_resolved, asset)) {
          return true;
        }
        this->add_asset_item(asset, active_id);
        return true;
      };

      if (ELEM(catalog_mode_, grid_settings::CatalogMode::Recent, grid_settings::CatalogMode::Favorites))
      {
        id_browser_foreach_membership_asset(*context_, catalog_mode_, idcode_, add_if_passes);
      }
      else {
        id_browser_foreach_asset(*context_, asset_library_ref_, idcode_, add_if_passes);
      }
      return;
    }

    for (ID &id : *idlb_) {
      if (!filter_.passes(*context_, *bmain_, id)) {
        continue;
      }
      const StringRef name = id.name + 2;
      if (!BKE_name_match_resolved_asset_passes(name_match_resolved, name, {})) {
        continue;
      }
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

 private:
  /** Add one asset-sourced grid item and wire up its activation (import + assign) and active-state
   * callbacks. Split out of #build_items() only because it is invoked from inside the
   * #id_browser_foreach_asset callback. */
  void add_asset_item(asset_system::AssetRepresentation &asset, const ID *active_id)
  {
    const StringRefNull identifier = asset.library_relative_identifier();
    const StringRefNull name = asset.get_name();
    IDBrowserGridItem &item = this->add_item<IDBrowserGridItem>(
        identifier, name, asset, asset_library_ref_, list_mode_);

    PointerRNA target_ptr = target_ptr_;
    PropertyRNA *target_prop = target_prop_;
    asset_system::AssetRepresentation *asset_ptr = &asset;
    const short idcode = idcode_;
    item.set_on_activate_fn(
        [target_ptr, target_prop, asset_ptr, idcode](bContext &C, PreviewGridItem & /*item*/) {
          Main *bmain = CTX_data_main(&C);
          /* Returns the existing local ID, or links/appends per the library's import method
           * (falling back to "Append & Reuse"). Same path as an asset drag-and-drop. A real
           * #ReportList is required: import can fail (asset deleted / file moved), and without
           * reports the click would silently look like a no-op. */
          ID *id = ed::asset::asset_local_id_ensure_imported(*bmain,
                                                             *asset_ptr,
                                                             /*flags*/ 0,
                                                             /*import_method*/ std::nullopt,
                                                             /*instantiate_context*/ std::nullopt,
                                                             CTX_wm_reports(&C));
          /* #asset_local_id_ensure_imported only handles assets stored inside a .blend library; it
           * returns null when #AssetRepresentation::full_library_path is empty, which is the case
           * for an image browsed straight from disk (a loose file in an on-disk asset library, not
           * wrapped in a .blend). Mirror the third resolution step of
           * #image_grid_resolve_image_from_asset (image_grid_ops.cc) and load it directly. */
          if (id == nullptr && idcode == ID_IM) {
            if (Image *image = BKE_image_load_exists(
                    bmain, asset_ptr->full_path().c_str(), nullptr))
            {
              /* #BKE_image_load_exists takes a loan on the user count regardless of whether the
               * block was newly created or already existed. Only release it when the target
               * property actually counts the reference below: #template_id_browser is reachable
               * from Python with an arbitrary property (#RNA_UI_api's template_id_browser), and
               * some ID pointer properties are deliberately not reference-counted (e.g.
               * `SpaceImageEditor.image`, `SpaceProperties.pin_id`). Releasing unconditionally
               * would under-count a non-refcounted assignment, making the image purgeable while
               * still referenced. */
              if (RNA_property_flag(target_prop) & PROP_ID_REFCOUNT) {
                id_us_min(&image->id);
              }
              id = &image->id;
            }
          }
          if (id == nullptr || GS(id->name) != idcode) {
            return;
          }
          /* Record every selection (not just from Recent/Favorites mode) as the most-recently-used
           * asset for this ID type, same as the Image Grid's shelf-driven Recent list. */
          const std::string record_shelf_idname = id_browser_shelf_idname(idcode);
          ed::asset::shelf::shelf_asset_lists_record_recent(record_shelf_idname,
                                                            asset_ptr->make_weak_reference());
          PointerRNA value = RNA_id_pointer_create(id);
          PointerRNA ptr = target_ptr;
          RNA_property_pointer_set(&ptr, target_prop, value, nullptr);
          RNA_property_update(&C, &ptr, target_prop);
        });
    item.set_is_active_fn([active_id, asset_ptr]() {
      const ID *local_id = asset_ptr->local_id();
      return local_id != nullptr && local_id == active_id;
    });
  }
};

static void build_id_grid(const bContext &C,
                          Layout &layout,
                          const float grid_viewport_units,
                          const int cols_hint = 0)
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
  if (idcode == 0) {
    /* Not an ID pointer property: neither source has anything to list. This used to be caught by
     * #which_libbase returning null, but that early return only runs for the blend-data source
     * below, so the asset source would otherwise fall through to an empty grid and a pointless
     * #storage_fetch. */
    return;
  }

  const int source = wm->id_browser_source;
  /* Blend-data source needs the ID list; the asset source does not (it iterates the library). */
  ListBaseT<ID> *idlb = nullptr;
  if (source == ID_BROWSER_SOURCE_BLEND_DATA) {
    idlb = which_libbase(bmain, idcode);
    if (idlb == nullptr) {
      return;
    }
  }

  if (source == ID_BROWSER_SOURCE_ASSET_LIBRARY && id_browser_library_is_missing(*wm)) {
    const AssetLibraryReference lib_ref = id_browser_library_ref_get(*wm);
    layout.label(fmt::format(fmt::runtime(IFACE_("Library \"{}\" not found")),
                             lib_ref.custom_library_name)
                     .c_str(),
                 ICON_ERROR);
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

  NameMatchFilterState name_match = id_browser_name_match_state_from_wm(*wm);

  grid_settings::CatalogMode catalog_mode = grid_settings::CatalogMode::All;
  if (source == ID_BROWSER_SOURCE_ASSET_LIBRARY) {
    PointerRNA settings_ptr = id_browser_grid_settings_ptr(*wm);
    if (settings_ptr.data != nullptr) {
      catalog_mode = grid_settings::catalog_mode_get(settings_ptr);
    }
  }

  std::unique_ptr<IDBrowserView> view = std::make_unique<IDBrowserView>(
      target_ptr,
      target_prop,
      bmain,
      &C,
      idlb,
      filter,
      list_mode,
      source,
      id_browser_library_ref_get(*wm),
      catalog_mode,
      idcode,
      std::move(name_match));

  if (list_mode) {
    /* Pack the row width into as many equal columns as fit (each at least
     * #ID_BROWSER_LIST_MIN_COL_UNITS_X wide), so a widened popover lays items out in horizontal
     * columns instead of one tall single column. The tile width divides the popover exactly so the
     * columns fill it with no gap on the right, and the column count is forced as a hint so float
     * rounding at the boundary cannot drop one. */
    const float units_x = layout.ui_units_x() > 0.0f ? layout.ui_units_x() :
                                                       ID_BROWSER_POPOVER_UNITS_X;
    const int list_cols = std::max(1, int(units_x / ID_BROWSER_LIST_MIN_COL_UNITS_X));
    const int tile_w = std::max(1, int(units_x * UI_UNIT_X) / list_cols);
    view->set_tile_size(tile_w, UI_UNIT_X);
    view->set_cols_per_row_hint(list_cols);
  }
  else {
    view->set_tile_size(UI_UNIT_X * 3, UI_UNIT_Y * 3);
    if (cols_hint > 0) {
      /* The popover snaps its width to whole columns; force the column count so float rounding at
       * the boundary cannot drop it to one fewer column and reopen the gap on the right. */
      view->set_cols_per_row_hint(cols_hint);
    }
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
  /* Fixed-viewport popover: the scroll position must survive the per-refresh view rebuild and
   * popover reopen; the session registry provides both. */
  grid_view->use_session_scroll(id_browser_grid_session_key);

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

/**
 * Add the interactive 2D resize grip. Placed at the bottom-right for a popover that opened
 * downward, or at the top-right (with the vertical axis flipped, so dragging up grows) when it
 * opened upward — e.g. a Shader-editor node button near the bottom of the area. Drives the
 * width/height stored on the window manager; the values persist with the file automatically, so
 * the callback only flags it modified.
 */
static void id_browser_add_resize_grip(Layout &layout, wmWindowManager &wm, const bool flip_up)
{
  Layout &grip_row = layout.row(false);
  grip_row.alignment_set(LayoutAlign::Right);
  Block *block = layout.block();
  block_layout_set_current(block, &grip_row);
  Button *grip = uiDefIconButV(block,
                               ButtonType::Grip,
                               ICON_GRIP,
                               0,
                               0,
                               short(UI_UNIT_X),
                               short(UI_UNIT_Y * 0.7f),
                               &wm.id_browser_popup_width_units,
                               0.0f,
                               0.0f,
                               std::nullopt);
  button_grip_2d_set(grip, &wm.id_browser_popup_height_units, flip_up);
  button_flag_disable(grip, BUT_UNDO);
  button_func_set(grip, [](bContext & /*C*/) { WM_file_tag_modified(); });
  block_layout_set_current(block, &layout);
}

/**
 * Redraw/refresh the popover when its asset content changes.
 *
 * #ed::asset::list::asset_reading_region_listen_fn only reacts to #ND_ASSET_LIST_READING,
 * #ND_ASSET_LIST_PREVIEW and #ND_ASSET_CATALOGS (the asynchronous library-loading notifiers). The
 * #GRIDVIEW_PT_catalog_selector and #UI_OT_id_browser_set_library instead send plain #ND_ASSET_LIST
 * on every synchronous filter change (see #id_browser_set_library_exec,
 * #GridCatalogSelectorTree::update_enabled_catalogs_from_items, and #GridCatalogSelectorTree::AllItem
 * activate). The catalog selector is a separate nested popup region and tags *its own* region for
 * redraw, but #ND_ASSET_LIST is what reaches this block's region (#ED_region_do_listen ->
 * #block_listen -> this listener) so the ID browser grid behind the popover rebuilds without
 * waiting for an unrelated event. Mirrors
 * #image_grid_block_listener (interface_template_asset_image_grid.cc), which the equivalent
 * View3D grid template already listens with.
 */
static void id_browser_asset_block_listen(const wmRegionListenerParams *params)
{
  const wmNotifier *wmn = params->notifier;
  if (wmn->category != NC_ASSET) {
    return;
  }
  if (ELEM(wmn->data,
           ND_ASSET_LIST,
           ND_ASSET_LIST_READING,
           ND_ASSET_LIST_PREVIEW,
           ND_ASSET_CATALOGS))
  {
    ED_region_tag_redraw(params->region);
    ED_region_tag_refresh_ui(params->region);
  }
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

  /* Interactive popover size, remembered on the window manager (per-`.blend`). Materialize a stored
   * 0 ("use the default") so the corner resize grip drags from the size actually shown. */
  if (wm->id_browser_popup_width_units <= 0) {
    wm->id_browser_popup_width_units = short(ID_BROWSER_POPOVER_UNITS_X);
  }
  if (wm->id_browser_popup_height_units <= 0) {
    wm->id_browser_popup_height_units = short(ID_BROWSER_GRID_VIEWPORT_UNITS_Y);
  }
  const wmWindow *win = CTX_wm_window(C);
  const int win_max_x = win ? std::max(10, (WM_window_native_pixel_x(win) / UI_UNIT_X) - 2) : 300;
  int popover_units_x = std::clamp(int(wm->id_browser_popup_width_units), 10, win_max_x);
  /* Grid mode: snap the width to a whole number of tile columns (tile = 3 #UI_UNIT_X) so previews
   * fill the row with no gap on the right. List mode is a single full-width column — no snap. The
   * column count is forwarded to the grid so float rounding cannot drop a column. */
  const bool view_list_mode = RNA_enum_get(&wm_ptr, "id_browser_view_mode") ==
                              IMAGE_BROWSER_VIEW_LIST;
  int grid_cols = 1;
  if (!view_list_mode) {
    grid_cols = std::max(1, popover_units_x / 3);
    popover_units_x = grid_cols * 3;
  }

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
  const bool asset_source = wm->id_browser_source == ID_BROWSER_SOURCE_ASSET_LIBRARY;
  /* The paint filters need both an image target and a space to back their state, and they only
   * apply to the blend-data source (an asset that is not imported yet has no local #Image to
   * test). */
  const bool show_paint_filters = is_image && space_ptr.data != nullptr && !asset_source;

  Layout &layout = *panel->layout;
  layout.ui_units_x_set(float(popover_units_x));

  if (asset_source || show_paint_filters) {
    /* Asset library async load / catalog changes, and GridViewSettings name-match updates
     * (NC_ASSET | ND_ASSET_LIST). Without this listener Blend Data mode would not rebuild the
     * grid when map types are toggled from the nested popover. See #id_browser_asset_block_listen. */
    block_add_dynamic_listener(layout.block(), id_browser_asset_block_listen);
  }

  /* Resolved open direction from the previous frame (the block is positioned, and its direction
   * copied to the handle, after this draw runs). Unknown (0) on the very first frame, so the grip
   * defaults to the bottom and corrects on the next refresh. When the popover opened upward, put
   * the grip on the top (growth) edge so resizing grows it up-and-right, away from the button. */
  const Block *block = layout.block();
  const bool flip_up = block->handle && (block->handle->direction & UI_DIR_UP);
  if (flip_up) {
    id_browser_add_resize_grip(layout, *wm, true);
  }

  Layout &header = layout.column(true);
  header.fixed_size_set(true);

  const bool has_material = CTX_data_pointer_get(C, "id_browser_material").data != nullptr;

  /* #Layout::separator uses 6px*UI_SCALE_FAC steps; convert 0.5 #UI_UNIT_Y to that factor. */
  const float half_unit_gap_factor = (0.5f * UI_UNIT_Y) / (6.0f * UI_SCALE_FAC);

  /* Row 1: source toggle on the left, view-mode toggle pushed to the right of the same row.
   * #separator_spacer is unsupported in popups, so a Right-aligned, fixed-size group is used
   * instead: the resolver's "ignore min flag" override (see #LayoutRow::resolve_impl) treats a
   * Right/Center-aligned fixed-size child of an Expand row as free space to consume, which is what
   * pushes it to the row's right edge. */
  Layout &filter_row = header.row(false);
  filter_row.alignment_set(LayoutAlign::Expand);

  Layout &source_toggle = filter_row.row(true);
  source_toggle.fixed_size_set(true);
  source_toggle.prop_enum(&wm_ptr, "id_browser_source", "BLEND_DATA", "", ICON_NONE);
  source_toggle.prop_enum(&wm_ptr, "id_browser_source", "ASSET_LIBRARY", "", ICON_NONE);

  Layout &view_mode_row = filter_row.row(true);
  view_mode_row.alignment_set(LayoutAlign::Right);
  view_mode_row.fixed_size_set(true);
  view_mode_row.prop_enum(&wm_ptr, "id_browser_view_mode", "GRID", "", ICON_NONE);
  view_mode_row.prop_enum(&wm_ptr, "id_browser_view_mode", "LIST", "", ICON_NONE);

  /* Same gap as between the source-options row and the search row below, so the header reads as
   * evenly spaced groups instead of one packed block. */
  header.separator(half_unit_gap_factor);

  /* Row 2, source-dependent: the asset source needs a library picker, catalog filter and the
   * name-match filter; the blend-data source keeps the paint filter-mode buttons, the slot-type
   * dropdown and its "All" material-restriction toggle (shown when "Slot" is active), with the
   * name-match filter last. */
  if (asset_source || show_paint_filters) {
    PointerRNA settings_ptr = RNA_pointer_get(&wm_ptr, "id_browser_grid_view_settings");

    if (asset_source) {
      Layout &source_options_row = header.row(false);
      source_options_row.alignment_set(LayoutAlign::Expand);

      Layout &source_options = source_options_row.row(true);
      source_options.context_ptr_set("id_browser_ptr", &target_ptr);
      if (prop_name) {
        source_options.context_string_set("id_browser_prop", *prop_name);
      }
      source_options.context_ptr_set("grid_view_settings", &settings_ptr);
      /* Only the ID Browser's vertical library menu shows Recent/Favorites (see
       * #grid_library_selector_menu_draw); other #template_grid_library_selector callers leave
       * this context key unset and stay unaffected. */
      source_options.context_int_set("grid_library_selector_show_recent_favorites", 1);

      /* Thin WM RNA property: side-effecting set + image-library itemf. */
      template_grid_library_selector(&source_options,
                                     const_cast<bContext *>(C),
                                     &wm_ptr,
                                     "id_browser_asset_library_reference",
                                     /*embed_in_parent_row=*/true,
                                     /*show_refresh=*/true);

      /* The closed dropdown otherwise shows "All Libraries" while Recent/Favorites is active:
       * #id_browser_set_membership stores the mode under #ASSET_LIBRARY_ALL in
       * #UserDef.catalog_memory (read here via #catalog_mode_get), not on the enum property the
       * button above is bound to -- from that property's point of view the library is simply
       * #ASSET_LIBRARY_ALL (see #id_browser_set_membership's comment on why). Override the
       * button's own label/icon after the fact so the closed state still communicates which
       * pseudo-catalog is showing. */
      if (settings_ptr.data != nullptr) {
        const grid_settings::CatalogMode catalog_mode = grid_settings::catalog_mode_get(
            settings_ptr);
        if (ELEM(catalog_mode, grid_settings::CatalogMode::Recent, grid_settings::CatalogMode::Favorites))
        {
          PropertyRNA *lib_prop = RNA_struct_find_property(&wm_ptr,
                                                            "id_browser_asset_library_reference");
          for (const std::unique_ptr<Button> &but_ptr : source_options.block()->buttons_ptrs) {
            Button *but = but_ptr.get();
            if (but->rnaprop != lib_prop || but->type != ButtonType::Menu) {
              continue;
            }
            const bool is_recent = catalog_mode == grid_settings::CatalogMode::Recent;
            but->str = is_recent ? IFACE_("Recent") : IFACE_("Favorites");
            but->drawstr = but->str;
            but->icon = is_recent ? ICON_RECOVER_LAST : ICON_SOLO_ON;
            /* Without this, #but_update_ex (interface.cc, reached via #button_update) would
             * immediately regenerate #str/drawstr from the bound enum's actual current value
             * (#ASSET_LIBRARY_ALL) on the next refresh pass of this #BLOCK_LOOP popup, reverting
             * the override above while leaving the icon alone (its own regeneration is
             * conditional on the enum item having a non-#ICON_NONE icon, which library items
             * never do). */
            button_drawflag_enable(but, BUT_MENU_KEEP_LABEL);
            break;
          }
        }
      }

      Layout &catalog_btn = source_options.row(true);
      catalog_btn.fixed_size_set(true);
      /* The library selector is already present in the ID Browser header. Keep the shared
       * catalog popover focused on catalogs instead of showing a duplicate library row. */
      catalog_btn.context_int_set("grid_catalog_selector_show_library_row", 0);
      template_grid_catalog_selector(&catalog_btn,
                                     const_cast<bContext *>(C),
                                     &settings_ptr,
                                     /*embed_in_parent_row=*/true);

      if (settings_ptr.data != nullptr) {
        Layout &name_match_row = source_options_row.row(true);
        name_match_row.alignment_set(LayoutAlign::Right);
        name_match_row.fixed_size_set(true);
        template_grid_name_match_filter(
            &name_match_row, const_cast<bContext *>(C), &settings_ptr);
      }
    }
    else {
      /* Full-width row so the name-match filter lands at the row's end. */
      Layout &source_options_row = header.row(false);
      source_options_row.alignment_set(LayoutAlign::Expand);

      /* #show_paint_filters guarantees `sl` (and therefore `space_ptr`) is valid here. */
      char *filter_mode_poin = image_filter_mode_pointer(sl);

      Layout &paint_filters = source_options_row.row(true);
      paint_filters.fixed_size_set(true);
      id_browser_image_filter_mode_button(
          paint_filters, &space_ptr, "ALL", "All", ICON_NONE, true);
      id_browser_image_filter_mode_button(
          paint_filters, &space_ptr, "CURRENT_MATERIAL", "", ICON_MATERIAL, has_material);
      id_browser_slot_type_filter_button(paint_filters, filter_mode_poin);

      const int mode = RNA_enum_get(&space_ptr, "image_filter_mode");
      const bool show_slot_type = (mode & TEMPLATE_ID_FILTER_SLOT_TYPE) != 0;
      if (show_slot_type) {
        /* #LayoutAlign::Center on a fixed-size child of an Expand+fixed parent is treated as
         * free space by #LayoutRow::resolve_impl (same trick as the view-mode group on row 1).
         * With name-match kept fixed (not Right) below, this group alone absorbs the gap between
         * the paint filters and the name-match icon and centers the menu+"All" as the popover
         * width changes. `align=true` keeps menu and toggle on one embossed height. */
        Layout &slot_type_row = source_options_row.row(true);
        slot_type_row.alignment_set(LayoutAlign::Center);
        slot_type_row.fixed_size_set(true);

        /* Fixed width instead of letting the dropdown stretch to fill the row: its content
         * ("Base Color", "Roughness", ...) does not need the full remaining space. */
        Layout &slot_type_dropdown = slot_type_row.row(true);
        slot_type_dropdown.fixed_size_set(true);
        slot_type_dropdown.ui_units_x_set(5.0f);
        slot_type_dropdown.prop(
            &space_ptr, "image_filter_slot_type", eUI_Item_Flag(0), "", ICON_NONE);

        /* "All" is a parameter nested inside the "Slot" filter: pressed (the default, since the
         * DNA field starts zeroed) means #TEMPLATE_ID_FILTER_CURRENT_MATERIAL is off and every
         * material's images are shown; releasing it restricts to the active material's slots.
         * #ButtonType::ToggleN is pressed when the underlying bit is *off* (see
         * #button_is_pushed_ex), matching that default, and matches the embossed menu height.
         * Flips the bit directly on the DNA field (bypassing RNA/#prop_enum) so it never touches
         * #TEMPLATE_ID_FILTER_SLOT_TYPE — toggling it can therefore never deselect
         * #id_browser_slot_type_filter_button or hide this row. */
        Layout &material_toggle = slot_type_row.row(true);
        material_toggle.fixed_size_set(true);
        material_toggle.active_set(has_material);
        Block *toggle_block = material_toggle.block();
        uiDefButBit<char>(toggle_block,
                          ButtonType::ToggleN,
                          TEMPLATE_ID_FILTER_CURRENT_MATERIAL,
                          IFACE_("All"),
                          0,
                          0,
                          short(2.5f * UI_UNIT_X),
                          short(UI_UNIT_Y),
                          filter_mode_poin,
                          0.0f,
                          0.0f,
                          TIP_("Show images used by slots of this type in any material, not just "
                               "the active one"));
      }

      if (settings_ptr.data != nullptr) {
        Layout &name_match_row = source_options_row.row(true);
        name_match_row.fixed_size_set(true);
        /* When the slot-type group is present it already absorbs the free space (Center above).
         * Right-aligning name-match too would split that space and break centering — only push
         * name-match to the trailing edge when it is the sole free child of this row. */
        if (!show_slot_type) {
          name_match_row.alignment_set(LayoutAlign::Right);
        }
        template_grid_name_match_filter(
            &name_match_row, const_cast<bContext *>(C), &settings_ptr);
      }
    }
  }

  header.separator(half_unit_gap_factor);

  {
    Layout &search_row = header.row(true);
    Block *search_block = search_row.block();
    Button *search_but = uiDefBut(search_block,
                                  ButtonType::Text,
                                  "",
                                  0,
                                  0,
                                  UI_UNIT_X * (popover_units_x - 2),
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

  /* Header/gap height consumed before the grid, kept in sync with the layout built above: the
   * source + view-mode + name-match row, the 0.5-unit separator, the source-options row (paint
   * filters, including any inline slot-type selector, or library/catalog), the 0.5-unit separator,
   * the search row, and the 0.5-unit gaps above and below the grid. */
  const float non_grid_units = 1.0f + 0.5f + 1.0f + 0.5f + 1.0f + 0.5f + 0.5f;
  const bool list_mode = RNA_enum_get(&wm_ptr, "id_browser_view_mode") ==
                         IMAGE_BROWSER_VIEW_LIST;
  const float tile_units = list_mode ? float(UI_UNIT_X) / float(UI_UNIT_Y) : 3.0f;

  /* User-set popover height (grid-viewport units) remembered on the window manager, clamped to the
   * window. Fed as the default so #popup_grid_fixed_viewport_units still shrinks it further only
   * when a zoomed popover would overflow the window, keeping the fixed header on screen. */
  const int win_max_y = win ? std::max(3, (WM_window_native_pixel_y(win) / UI_UNIT_Y) - 4) : 120;
  const float default_grid_units = float(
      std::clamp(int(wm->id_browser_popup_height_units), 3, win_max_y));
  const float grid_units = popup_grid_fixed_viewport_units(
      C, layout.block(), non_grid_units, tile_units, default_grid_units);

  Layout &grid_area = layout.column(true);
  grid_area.ui_units_x_set(float(popover_units_x));
  grid_area.ui_units_y_set(grid_units);
  grid_area.fixed_size_set(true);

  build_id_grid(*C, grid_area, grid_units, view_list_mode ? 0 : grid_cols);

  /* Matching gap under the grid for the persistent scroll-down arrow. */
  layout.separator(half_unit_gap_factor);

  /* Downward popover: grip on the bottom (growth) edge. The upward case is placed at the top above.
   */
  if (!flip_up) {
    id_browser_add_resize_grip(layout, *wm, false);
  }
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
