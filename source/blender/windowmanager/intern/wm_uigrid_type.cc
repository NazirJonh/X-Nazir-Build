/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * UI Grid Type Registry.
 */

#include <cstdio>
#include <cstring>

#include "BLI_utildefines.h"
#include "BLI_vector_set.hh"

#include "BKE_main.hh"
#include "BKE_screen.hh"

#include "MEM_guardedalloc.h"

#include "WM_api.hh"

namespace blender {

static auto &get_grid_type_map()
{
  struct IDNameGetter {
    StringRef operator()(const uiGridType *value) const
    {
      return StringRef(value->idname);
    }
  };
  static CustomIDVectorSet<uiGridType *, IDNameGetter> map;
  return map;
}

uiGridType *WM_uigridtype_find(const StringRef idname, const bool quiet)
{
  if (!idname.is_empty()) {
    if (uiGridType *const *ugt = get_grid_type_map().lookup_key_ptr_as(idname)) {
      return *ugt;
    }
  }

  if (!quiet) {
    printf("search for unknown uigridtype %s\n", std::string(idname).c_str());
  }
  return nullptr;
}

bool WM_uigridtype_add(uiGridType *ugt)
{
  return get_grid_type_map().add(ugt);
}

void WM_uigridtype_remove_ptr(Main * /*bmain*/, uiGridType *ugt)
{
  get_grid_type_map().remove(ugt);
  MEM_delete(ugt);
}

void WM_uigridtype_init()
{
  get_grid_type_map().clear();
}

void WM_uigridtype_free()
{
  for (uiGridType *ugt : get_grid_type_map()) {
    if (ugt->rna_ext.free) {
      ugt->rna_ext.free(ugt->rna_ext.data);
    }
    MEM_delete(ugt);
  }
  get_grid_type_map().clear();
}

}  // namespace blender
