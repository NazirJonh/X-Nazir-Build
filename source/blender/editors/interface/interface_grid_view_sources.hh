/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edinterface
 *
 * Concrete #GridDataSource implementations:
 * - #AssetGridDataSource — asset library + catalog filter backed.
 * - #PyCallbackGridDataSource — bridges a registered Python #UIGrid type.
 */

#include "interface_grid_view.hh"

#include "BKE_name_matching.hh"

#include "AS_asset_library.hh"
#include "BLI_set.hh"
#include "RNA_types.hh"

namespace blender {
struct uiGridType;

namespace ui {

/* -------------------------------------------------------------------- */
/** \name AssetGridDataSource
 * \{ */

/**
 * Grid source backed by an asset library, an optional set of catalog-path filters, and an
 * optional set of ID-type filters. An empty \a enabled_catalogs or \a filter_id_types means "show
 * all" for that filter; any asset type (Image, Material, Object, a custom asset type, ...) is
 * shown unless \a filter_id_types narrows it down. Activation invokes \a activate_operator
 * with the standard asset-reference properties set on the operator. When \a drag_operator is
 * non-empty, it is invoked (with the same asset-reference properties) instead of the default
 * asset/ID drag-and-drop when the user starts dragging an item.
 */
class AssetGridDataSource : public GridDataSource {
  AssetLibraryReference library_ref_;
  Set<std::string> enabled_catalogs_;
  Set<short> filter_id_types_;
  NameMatchFilterState name_match_;
  std::string activate_operator_;
  std::string drag_operator_;

 public:
  AssetGridDataSource(const AssetLibraryReference &library_ref,
                      Set<std::string> enabled_catalogs,
                      Set<short> filter_id_types,
                      NameMatchFilterState name_match,
                      std::string activate_operator,
                      std::string drag_operator = "");

  int item_count(const bContext &C) const override;
  bool item_count_ready(const bContext &C) const override;
  void build_window(const bContext &C, AbstractGridView &view, IndexRange window) override;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name PyCallbackGridDataSource
 * \{ */

/**
 * Bridges a registered Python #UIGrid type to the generic #GridDataSource interface.
 */
class PyCallbackGridDataSource : public GridDataSource {
  uiGridType *grid_type_;
  PointerRNA dataptr_;
  std::string propname_;

 public:
  PyCallbackGridDataSource(uiGridType *grid_type, PointerRNA dataptr, StringRef propname);

  int item_count(const bContext &C) const override;
  void build_window(const bContext &C, AbstractGridView &view, IndexRange window) override;
};

/** \} */

} /* namespace ui */
} /* namespace blender */
