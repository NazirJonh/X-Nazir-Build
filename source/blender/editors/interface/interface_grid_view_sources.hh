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
 * Grid source backed by an asset library and an optional set of catalog-path filters.
 * An empty \a enabled_catalogs means "show all". Activation invokes \a activate_operator
 * with the standard asset-reference properties set on the operator.
 */
class AssetGridDataSource : public GridDataSource {
  AssetLibraryReference library_ref_;
  Set<std::string> enabled_catalogs_;
  std::string activate_operator_;

 public:
  AssetGridDataSource(const AssetLibraryReference &library_ref,
                      Set<std::string> enabled_catalogs,
                      std::string activate_operator);

  int item_count(const bContext &C) const override;
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
