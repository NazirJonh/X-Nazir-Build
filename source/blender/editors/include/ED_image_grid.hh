/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 *
 * Public editor API for the compact brush-texture asset grid
 * (#template_asset_image_grid). #ImageGridOwner stands in for a specific
 * space's persisted DNA + runtime cache so the rest of the grid subsystem
 * never touches #View3D or #SpaceImage directly.
 *
 * This header does not include #ED_view3d.hh.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "BLI_function_ref.hh"
#include "BLI_set.hh"
#include "BLI_vector.hh"

#include "BKE_name_matching.hh"

#include "DNA_asset_types.h"
#include "DNA_image_grid_types.h"

struct View3D;
struct SpaceImage;
struct bContext;
struct AssetShelf;
struct Image;
struct PointerRNA;
struct Main;
struct ARegion;
struct wmEvent;

namespace blender {
struct BlendDataReader;
struct BlendWriter;
namespace asset_system {
class AssetLibrary;
class AssetRepresentation;
}  // namespace asset_system
namespace ui {
struct Layout;
}  // namespace ui
}  // namespace blender

namespace blender::ed::image_grid {

/**
 * Non-owning accessor over one space's persisted image-grid DNA and runtime
 * cache slot. Constructed via #ImageGridOwner::from(); carries no context or
 * lifetime ownership, so it is passed by value like a reference.
 */
class ImageGridOwner {
 public:
  ImageGridSlotDNA &slot_dna(bool is_mask_slot) const;
  short &preview_size_dna() const;
  /** Opaque lazy-cache anchor, equivalent to #View3D_Runtime::image_grid_state. */
  void *&runtime_state_slot() const;
  /** Stable pointer used to derive session/registry keys (currently the
   * owning space's address). */
  const void *identity() const;

  /** Concrete-space downcasts for code that must construct a space-specific #PointerRNA (RNA
   * property panels); returns null when the owner wraps a different kind. */
  View3D *as_view3d() const;
  SpaceImage *as_space_image() const;

  static ImageGridOwner from(View3D &v3d);
  static ImageGridOwner from(SpaceImage &sima);

 private:
  enum class Kind { View3D, SpaceImage };

  ImageGridOwner(Kind kind, void *space) : kind_(kind), space_(space) {}

  Kind kind_;
  void *space_;
};

std::optional<ImageGridOwner> image_grid_owner_from_context(const bContext &C);

/**
 * How the image grid interprets its library/catalog filter relative to the shared
 * image-texture asset shelf. Recent/Favorites are membership modes — they never
 * store sentinel strings in #ImageGridFilter::enabled_catalog_paths.
 *
 * Integer values match #eImageGridCatalogMode in DNA. UserDef #eAssetCatalogMemoryMode
 * is mapped only at the catalog-memory boundary (SINGLE/SET are storage details, not UI).
 */
enum class ImageGridCatalogMode : short {
  All = IMAGE_GRID_CATALOG_MODE_ALL,
  CatalogPath = IMAGE_GRID_CATALOG_MODE_CATALOG_PATH,
  Recent = IMAGE_GRID_CATALOG_MODE_RECENT,
  Favorites = IMAGE_GRID_CATALOG_MODE_FAVORITES,
};

static_assert(int(ImageGridCatalogMode::All) == IMAGE_GRID_CATALOG_MODE_ALL);
static_assert(int(ImageGridCatalogMode::CatalogPath) == IMAGE_GRID_CATALOG_MODE_CATALOG_PATH);
static_assert(int(ImageGridCatalogMode::Recent) == IMAGE_GRID_CATALOG_MODE_RECENT);
static_assert(int(ImageGridCatalogMode::Favorites) == IMAGE_GRID_CATALOG_MODE_FAVORITES);

/**
 * Domain key for #bUserAssetCatalogMemory entries owned by the image grid.
 * Live catalog UUID sets and Recent/Favorites live here, not in slot DNA lists.
 */
constexpr const char *image_grid_catalog_memory_domain = "image_grid";

/** Asset shelf type used by the image grid browse popover and shelf sync. */
constexpr const char *IMAGE_TEXTURE_SHELF_IDNAME = "VIEW3D_AST_image_texture";

/** Library + catalog filter state (which library and which catalog paths are enabled). */
struct ImageGridFilter {
  AssetLibraryReference lib_ref{};
  /**
   * Set of catalog paths currently enabled for display in the grid.
   * An empty set means "show all" (no catalog filter) when #catalog_mode is #All.
   * Must stay empty for #Recent / #Favorites (membership lists, not catalog paths).
   */
  blender::Set<std::string> enabled_catalog_paths;
  /**
   * Library section keys the user has expanded in the All Libraries catalog selector.
   * Keyed by #image_grid_library_key(). Absence means collapsed (default). Session-only —
   * not written to DNA or Preferences.
   */
  blender::Set<std::string> expanded_library_section_keys;
  /**
   * Follow mode for the image-texture shelf. #Recent / #Favorites use #ASSET_LIBRARY_ALL and
   * ordered membership from #shelf_asset_lists_recent / #shelf_asset_lists_favorites.
   */
  ImageGridCatalogMode catalog_mode = ImageGridCatalogMode::All;
  /** Name Matching include-filter (persisted on #ImageGridSlotDNA). */
  blender::NameMatchFilterState name_match;
};

/**
 * Focus / grip state for the image grid. Scroll position, grip height, column count and item count
 * now live in the shared session registry (one #GridSessionState per grid variant, keyed by
 * #image_grid_session_id); only host-specific focus bookkeeping remains here.
 */
struct ImageGridViewport {
  /**
   * Number of focus-tracking buckets: one per column count (clamped to 1..16). The N-Panel and
   * Texture popover grids share one #ImageGridUIState but usually differ in width, so keying the
   * focus-applied flag by column count keeps each host from clearing the other's pending focus.
   */
  static constexpr int layout_bucket_num = 16 * 16;

  /**
   * When non-empty, the grid should scroll to this asset's filtered index (session-only).
   * Cleared only when the user manually scrolls or the asset is absent from the loaded library.
   * Each grid host (N-Panel, Texture Popover) applies this independently for its own column count,
   * so both can scroll to the correct row even when their widths differ.
   */
  std::string focus_asset_identifier;

  /**
   * Per-column-count flag recording whether #focus_asset_identifier was already applied for that
   * layout. Lets each host apply focus once without clearing the identifier for the other. Reset on
   * a new focus request or manual scroll.
   */
  bool focus_applied_by_layout[layout_bucket_num] = {};

  /** Session UID of the brush whose texture was last auto-focused on brush activation (0 if
   * none). Compared on each grid redraw so auto-focus fires exactly once per brush switch. */
  uint32_t last_auto_focus_brush_uid = 0;
};

/**
 * Deferred sync from asset shelf browse popover (applied after popover closes).
 * See #image_grid_pending_schedule_from_asset() / #image_grid_pending_apply_if_ready().
 */
struct ImageGridPendingSync {
  bool apply_after_popover = false;
  AssetLibraryReference lib_ref{};
  bool use_all_catalogs = false;
  std::string catalog_path;
  /** Applied into #ImageGridFilter::catalog_mode when the pending sync runs. */
  ImageGridCatalogMode catalog_mode = ImageGridCatalogMode::All;
  std::string focus_asset_identifier;
  /** Index in the full filtered asset list; computed on apply if still -1. */
  int focus_filtered_index = -1;
};

/**
 * Per-owner persistent UI state for the compact image asset grid template.
 * Stored externally (not in DNA) so it survives redraw without being serialized.
 */
struct ImageGridUIState {
  ImageGridFilter filter;
  ImageGridViewport viewport;
  ImageGridPendingSync pending;

  /**
   * Last brush texture weak reference for the image browse popover (session-only).
   * Updated in #image_grid_prepare_browse_shelf(); used when the popover redraws without
   * `image_grid_target` in context.
   */
  bool shelf_active_asset_valid = false;
  AssetWeakReference shelf_active_asset{};
};

ImageGridUIState &image_grid_state_get(ImageGridOwner owner, bool is_mask_slot = false);
bool image_grid_library_is_missing(ImageGridOwner owner, bool is_mask_slot);
void image_grid_state_remove(ImageGridOwner owner);
void image_grid_foreach_live_library_ref(ImageGridOwner owner,
                                         blender::FunctionRef<void(AssetLibraryReference &)> fn);
/**
 * Run \a fn on the active name-match map-type ID set of every runtime state this owner already has
 * (never creates one), so a map type removed or renamed in the Preferences does not stay selected
 * in a grid that is currently open.
 */
void image_grid_foreach_live_name_match_ids(
    ImageGridOwner owner, blender::FunctionRef<void(blender::Set<std::string> &)> fn);
std::string image_grid_session_id(ImageGridOwner owner, bool is_mask_slot, bool is_popover);
void image_grid_reset_scroll(ImageGridOwner owner, bool is_mask_slot);

int image_grid_effective_rows(ImageGridOwner owner, bool is_mask_slot);
int image_grid_preview_size_get(ImageGridOwner owner);
void image_grid_state_persist(ImageGridOwner owner,
                              ImageGridUIState &state,
                              bool is_mask_slot = false);

void image_grid_slot_dna_free(ImageGridSlotDNA &slot);
void image_grid_slot_dna_duplicate(ImageGridSlotDNA &dst, const ImageGridSlotDNA &src);
void image_grid_slot_dna_blend_read(blender::BlendDataReader *reader, ImageGridSlotDNA &slot);
void image_grid_slot_dna_blend_write(blender::BlendWriter *writer, const ImageGridSlotDNA &slot);

bool image_grid_is_mask_slot_from_context(const bContext &C);
/** True when #ImageGridUIState::filter's library no longer exists in the Preferences (§5). */
void image_grid_state_reset_catalog(ImageGridUIState &state);
/**
 * Store #enabled_catalog_paths into #BKE_asset_catalog_memory_set_set / #_set_all for the current
 * library (domain #"image_grid").
 */
void image_grid_catalog_commit_active(ImageGridUIState &state);
/**
 * Exit Recent/Favorites membership (or clear a normal catalog filter) to "show all" for the
 * current library. Skips #image_grid_catalog_commit_active when leaving membership so empty paths
 * do not wipe saved per-library catalog filters for #ASSET_LIBRARY_ALL.
 */
void image_grid_filter_set_show_all(ImageGridUIState &state);
/**
 * Clear every library's saved catalog filter at once -- the global "All" item at the top of the
 * catalog-selector tree in All-Libraries mode. Distinct from #image_grid_filter_set_show_all(),
 * which only clears the currently active library's filter (meaningless when #ImageGridFilter::
 * lib_ref is #ASSET_LIBRARY_ALL, since that library has no per-library filter of its own).
 */
void image_grid_filter_set_show_all_for_all_libraries(ImageGridUIState &state);
/**
 * Enter Recent or Favorites membership: set #lib_ref to #ASSET_LIBRARY_ALL, clear catalog paths,
 * and set #catalog_mode. Commits the previous library's catalog filter first when leaving a
 * normal library view (not when switching between Recent/Favorites).
 */
void image_grid_filter_set_membership(ImageGridUIState &state,
                                      ImageGridCatalogMode membership_mode);
/** Save the old library filter, switch to \a new_lib_ref, restore its saved filter (or all). */
void image_grid_catalog_swap_library(ImageGridUIState &state,
                                     const AssetLibraryReference &old_lib_ref,
                                     const AssetLibraryReference &new_lib_ref);
/**
 * Position-independent identifier for \a lib_ref, stable across a Preferences reorder (same key
 * #BKE_preferences_asset_library_identifier_from_ref returns for catalog-memory lookup).
 */
std::string image_grid_library_key(const AssetLibraryReference &lib_ref);
/**
 * Loaded libraries that contribute to #ASSET_LIBRARY_ALL filtering / All-mode catalog sections.
 * Ordered like the image-grid library selector (#library_reference_to_rna_enum_itemf with the same
 * flags as #rna_image_grid_library_itemf): Current File, Essentials, then Preferences image
 * libraries in listbase/folder order. Membership still comes from #AssetLibrary::foreach_loaded
 * (same remote-library experimental gate; skip libraries with no #library_reference()). Loaded
 * libraries that are not in that selector (e.g. a still-cached non-image library) are appended
 * after the selector-ordered entries so filtering stays complete.
 */
blender::Vector<asset_system::AssetLibrary *> image_grid_all_mode_libraries();
/**
 * Kick off (or refresh) an #ed::asset::list::storage_fetch() for every real library that
 * #image_grid_all_mode_libraries() would enumerate, regardless of whether it is loaded yet.
 * Needed because #ASSET_LIBRARY_ALL's own #storage_fetch() only warms the unrelated built-in
 * merged file-list, not each real library's own #AssetList.
 */
void image_grid_fetch_all_mode_libraries(const bContext &C);
void image_grid_notify_change(bContext &C, bool is_mask_slot = false);

/** Copy grid library/catalog filter into popup asset shelf before opening browse UI. */
void image_grid_sync_shelf_from_state(AssetShelf &shelf, const ImageGridUIState &state);

/** Prepare popup shelf for image browse; returns null if shelf type missing or poll fails. */
AssetShelf *image_grid_prepare_browse_shelf(const bContext &C,
                                            ImageGridUIState &state,
                                            const char *shelf_idname);

void image_grid_pending_clear(ImageGridUIState &state);

bool image_grid_asset_is_visible_in_state(const ImageGridUIState &state,
                                          const AssetLibraryReference &asset_lib_ref,
                                          const std::optional<std::string> &asset_catalog_path);

/** Catalog filter implied by popup asset shelf settings (nullopt = All or a pseudo-catalog). */
std::optional<std::string> image_grid_catalog_path_from_shelf(const AssetShelf &shelf);

/** Shelf active-catalog mode for image-grid follow (All / path / Recent / Favorites). */
ImageGridCatalogMode image_grid_shelf_catalog_mode(const AssetShelf &shelf);

/**
 * True when the grid's library + catalog/membership filter already matches \a shelf.
 * Recent/Favorites compare follow mode + #ASSET_LIBRARY_ALL, not raw #lib_ref equality alone.
 */
bool image_grid_filter_matches_shelf(const ImageGridUIState &state, const AssetShelf &shelf);

/** Drop unknown catalog paths so an empty set means All is selected. */
void image_grid_catalog_sanitize_selection(ImageGridUIState &state);

/**
 * Reverse of #image_grid_library_key(): reconstruct the full reference an identifier names.
 * Returns a default (#ASSET_LIBRARY_LOCAL) reference when \a key names a custom library no longer
 * present in the Preferences; callers must compare against \a key itself (not the returned type)
 * to distinguish that case from a legitimate "local" lookup.
 */
AssetLibraryReference image_grid_library_ref_from_key(const std::string &key);

void image_grid_pending_schedule_from_asset(
    ImageGridUIState &state,
    const AssetLibraryReference &lib_ref,
    const std::optional<std::string> &catalog_path,
    const std::string &asset_identifier,
    ImageGridCatalogMode catalog_mode = ImageGridCatalogMode::All);

std::optional<std::string> image_grid_catalog_path_for_asset(
    const asset_system::AssetRepresentation &asset, const AssetLibraryReference &lib_ref);

void image_grid_request_scroll_to_asset(ImageGridUIState &state,
                                        const std::string &asset_identifier);

/**
 * When the active paint brush changes, request the grid to scroll to the image assigned to its
 * texture slot (main or mask depending on \a is_mask_slot). No-op when the brush is unchanged,
 * has no image texture assigned, the image is absent from the current library, or the library has
 * not finished loading yet (an #NC_ASSET notifier will retrigger a redraw when it does).
 * Call once per template redraw, before #image_grid_apply_focus_scroll runs.
 */
void image_grid_auto_focus_on_brush_change(bContext &C, bool is_mask_slot);

/** Clear a pending scroll-to-asset request and all per-layout applied flags. */
void image_grid_focus_clear(ImageGridViewport &viewport);
/**
 * Mark the (cols, rows) layout as already focus-scrolled so it keeps its current scroll instead of
 * re-centering. Used for the grid the user just clicked in (the asset is already where they
 * clicked), while other layouts still center on it.
 */
void image_grid_focus_mark_applied(ImageGridViewport &viewport, int cols, int rows);

/**
 * Weak reference to the image texture currently assigned to the brush slot in
 * context `image_grid_target`, for asset shelf popover highlighting.
 */
std::optional<AssetWeakReference> image_grid_shelf_active_asset_weak_ref(
    const bContext &C, const AssetLibraryReference &library_ref);

/** Register popover shelf resolver; safe to call repeatedly. */
void image_grid_shelf_sync_register();

/**
 * Compute the target first-visible row that brings #focus_asset_identifier into view, centered
 * vertically (shifted back by half of \a effective_rows_hint so the active asset lands in the
 * middle of the popover). The caller turns the row into the session's pixel scroll position.
 *
 * The identifier is NOT cleared after a successful scroll; instead
 * #ImageGridViewport::focus_applied_by_layout records which column counts already applied it.
 * Subsequent draws with the same column count skip the lookup entirely, while a draw with a
 * different width (e.g. Texture Popover vs. N-Panel sidebar) re-applies for its own layout. If the
 * asset is already visible in the current window the scroll is left unchanged (so the grid the user
 * clicked in does not jump). The identifier is cleared when the user manually scrolls or the asset
 * is absent from the fully-loaded library.
 *
 * \param effective_rows_hint: Number of visible grid rows, pre-computed by the caller from the
 * session grip height and tile_h *before* the slot DNA rows field is written for the current
 * frame. This avoids the first-frame case where rows is still 0 (DNA default),
 * which would otherwise give center_offset = 0 and produce no vertical centering.
 *
 * \return the target scroll row for the focused asset, or -1 when there is nothing to apply (no
 * request, already applied, not-yet-loaded, or absent).
 */
int image_grid_apply_focus_scroll(const bContext &C,
                                  ImageGridUIState &state,
                                  int cols,
                                  int effective_rows_hint);

/**
 * Apply pending shelf selection when the browse popover is closed.
 * Safe to call every image grid redraw.
 */
void image_grid_pending_apply_if_ready(bContext &C);

/** Return the short display name for an asset library reference (used in image grid UI). */
const char *image_grid_library_ui_name(const AssetLibraryReference &lib_ref);
/**
 * Library-selector button label: "Recent" / "Favorites" while in membership mode, otherwise the
 * active library name from #image_grid_library_ui_name.
 */
const char *image_grid_library_selector_label(const ImageGridUIState &state);
/**
 * Numpad-period (KP_DEL) over a brush texture image grid: scroll-center both the N-Panel and the
 * Texture popover grids on the slot's currently assigned texture, even when it scrolled out of
 * view.
 */
int handle_image_grid_focus_active_event(bContext *C, const wmEvent *event, ARegion *region);

/** True when \a texture_slot_ptr refers to #Brush.mask_mtex (not #Brush.mtex). */
bool image_grid_slot_is_mask(const PointerRNA &texture_slot_ptr);

/**
 * Assign \a image to the brush texture slot identified by \a target_ptr (the same slot the grid
 * is bound to), then switch the grid to the current-file library and scroll-focus the image.
 * Mirrors #IMAGE_GRID_OT_open / #IMAGE_GRID_OT_new (localize a linked brush rather
 * than moving/copying the image into its library), so a dropped image never ends up linked.
 * Returns false when \a target_ptr does not resolve to a brush.
 */
bool image_grid_assign_dropped_image(bContext &C, const PointerRNA &target_ptr, Image &image);

/**
 * Build an `image_grid_target` #PointerRNA for the active paint brush texture slot.
 * Used when the browse popover is opened without an N-panel button context (e.g. hotkey).
 */
bool image_grid_brush_target_pointer_get(const bContext &C,
                                         PointerRNA *r_target_ptr,
                                         bool is_mask_slot = false);

/**
 * Set layout context members required by the image-texture asset shelf browse popover.
 */
void image_grid_popover_layout_context_set(ui::Layout &layout,
                                           bContext &C,
                                           bool is_mask_slot = false);

/**
 * True when \a asset and \a image are the same texture source: identical local ID, or matching
 * normalized file path for on-disk assets.
 */
bool image_grid_asset_represents_image(const asset_system::AssetRepresentation &asset,
                                       const Image &image);

/**
 * Discriminated item yielded by #image_grid_foreach_filtered_item.
 * Exactly one of the two pointers is non-null per item.
 */
struct ImageGridFilteredItem {
  /** Non-null for image assets from the library list. */
  asset_system::AssetRepresentation *asset = nullptr;
  /** Non-null for non-asset blend-file images (LOCAL library only). */
  Image *image = nullptr;
};

/**
 * True when \a image can be assigned as a brush texture from the grid.
 * Excludes render results, composites, viewer nodes, and generated images.
 */
bool image_grid_is_assignable_texture(const Image &image);

/**
 * Iterate all visible image items for \a lib_ref and \a enabled_catalog_paths in display order.
 *
 * Handles: ID type filtering, catalog filter, assignability check, asset deduplication,
 * and the LOCAL blend-file image extension (non-asset images from #Main).
 *
 * The callback receives each item and its zero-based filtered index.
 * Return false from \a fn to stop early; the total count continues to accumulate.
 *
 * \return Total filtered item count (consistent ordering guarantee for focus-scroll).
 */
int image_grid_foreach_filtered_item(
    Main &bmain,
    const AssetLibraryReference &lib_ref,
    const blender::Set<std::string> &enabled_catalog_paths,
    blender::FunctionRef<bool(const ImageGridFilteredItem &item, int filtered_index)> fn);

/**
 * Like the overload above, but honors #ImageGridFilter::catalog_mode: Recent/Favorites iterate
 * #ASSET_LIBRARY_ALL and emit only ordered membership from the image-texture shelf lists.
 */
int image_grid_foreach_filtered_item(
    Main &bmain,
    const ImageGridUIState &state,
    blender::FunctionRef<bool(const ImageGridFilteredItem &item, int filtered_index)> fn);

/**
 * Switch the grid's active library to \a new_ref: exits Recent/Favorites membership if needed,
 * restores the target library's saved catalog filter, resets scroll, persists state, and notifies
 * listeners. Returns false (no-op) when \a new_ref is already active and not in membership.
 * Shared by #IMAGE_GRID_OT_set_library and Ctrl-Wheel cycling on the header library selector.
 */
bool image_grid_set_library(bContext &C,
                            ImageGridOwner owner,
                            bool is_mask_slot,
                            const AssetLibraryReference &new_ref);

}  // namespace blender::ed::image_grid
