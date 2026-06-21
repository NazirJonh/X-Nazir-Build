/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "multi_object_undo.hh"

#include "DNA_object_types.h"
#include "DNA_scene_types.h"

namespace blender::ed::sculpt_paint::multi_object::undo {

bool Manager::push_begin(const Scene & /*scene*/,
                         const Vector<Object *> &objects,
                         const char *name)
{
  if (is_active_) {
    return false;
  }

  objects_.clear();
  objects_started_.clear();
  operation_name_ = name;

  for (Object *ob : objects) {
    if (ob == nullptr || ob->type != OB_CURVES) {
      continue;
    }
    objects_.append(ob);
    objects_started_.add(ob, true);
  }

  is_active_ = true;
  return true;
}

bool Manager::push_end(const Vector<Object *> & /*objects*/)
{
  if (!is_active_) {
    return false;
  }

  objects_.clear();
  objects_started_.clear();
  operation_name_ = nullptr;
  is_active_ = false;
  return true;
}

void Manager::cancel()
{
  objects_.clear();
  objects_started_.clear();
  operation_name_ = nullptr;
  is_active_ = false;
}

}  // namespace blender::ed::sculpt_paint::multi_object::undo
