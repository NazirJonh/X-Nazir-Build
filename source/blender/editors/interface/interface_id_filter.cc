/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Registry of script-defined ID filter types (#IDFilterType). ID-browsing templates
 * (#template_ID_with_filter_context, #template_id_browser) consult a registered filter, by
 * #IDFilterType::idname, to let Python decide which IDs are offered. The lifetime/registration
 * model mirrors the asset-shelf type registry (#ed::asset::shelf::type_register).
 */

#include <memory>
#include <utility>

#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include "UI_interface_c.hh"

namespace blender::ui {

/** Process-wide registry; owns every registered #IDFilterType. */
static Vector<std::unique_ptr<IDFilterType>> &id_filter_types()
{
  static Vector<std::unique_ptr<IDFilterType>> types;
  return types;
}

void id_filter_type_register(std::unique_ptr<IDFilterType> type)
{
  id_filter_types().append(std::move(type));
}

void id_filter_type_unregister(const IDFilterType &type)
{
  Vector<std::unique_ptr<IDFilterType>> &types = id_filter_types();
  types.remove_if(
      [&](const std::unique_ptr<IDFilterType> &iter_type) { return iter_type.get() == &type; });
}

IDFilterType *id_filter_type_find(const StringRef idname)
{
  if (idname.is_empty()) {
    return nullptr;
  }
  for (const std::unique_ptr<IDFilterType> &type : id_filter_types()) {
    if (type->idname == idname) {
      return type.get();
    }
  }
  return nullptr;
}

bool id_filter_type_poll(const IDFilterType &type, const bContext &C, ID &id)
{
  /* A type without a `filter_id` method imposes no restriction. */
  if (type.filter_id == nullptr) {
    return true;
  }
  return type.filter_id(&type, &C, &id);
}

}  // namespace blender::ui
