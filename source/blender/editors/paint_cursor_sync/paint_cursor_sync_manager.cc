/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "MEM_guardedalloc.h"

#include "paint_cursor_sync_manager.hh"

namespace blender::editors {

PaintCursorSyncManager &PaintCursorSyncManager::get()
{
  static PaintCursorSyncManager instance;
  return instance;
}

void PaintCursorSyncManager::initialize()
{
  PaintCursorSyncManager &manager = get();
  manager.is_initialized_ = true;
}

void PaintCursorSyncManager::shutdown()
{
  PaintCursorSyncManager &manager = get();
  manager.sources_.clear();
  manager.targets_.clear();
  manager.is_initialized_ = false;
  manager.current_data_ = CursorSyncData();
  manager.target_filter_.reset();
  manager.image_filter_ = nullptr;
}

void PaintCursorSyncManager::register_source(PaintCursorSource *source)
{
  if (!source || !is_initialized_) {
    return;
  }
  if (!sources_.contains(source)) {
    sources_.append(source);
  }
}

void PaintCursorSyncManager::unregister_source(PaintCursorSource *source)
{
  if (!source) {
    return;
  }
  sources_.remove_if([source](PaintCursorSource *s) { return s == source; });
}

void PaintCursorSyncManager::register_target(PaintCursorTarget *target)
{
  if (!target || !is_initialized_) {
    return;
  }
  if (!targets_.contains(target)) {
    targets_.append(target);
  }
}

void PaintCursorSyncManager::unregister_target(PaintCursorTarget *target)
{
  if (!target) {
    return;
  }
  targets_.remove_if([target](PaintCursorTarget *t) { return t == target; });
}

void PaintCursorSyncManager::sync_from_source(PaintCursorSource *source)
{
  if (!source || !source->is_active()) {
    return;
  }

  current_data_ = source->get_sync_data();

  for (PaintCursorTarget *target : targets_) {
    if (target == nullptr) {
      continue;
    }
    if (!target->can_display()) {
      continue;
    }

    if (target_filter_.has_value() && target->get_space_type() != *target_filter_) {
      continue;
    }

    if (image_filter_ && !target->is_compatible_image(image_filter_)) {
      continue;
    }

    if (current_data_.source_image && !target->is_compatible_image(current_data_.source_image)) {
      continue;
    }

    target->update_cursor(current_data_);
  }
}

const CursorSyncData &PaintCursorSyncManager::get_current_data() const
{
  return current_data_;
}

bool PaintCursorSyncManager::has_valid_data() const
{
  return current_data_.is_valid;
}

void PaintCursorSyncManager::set_target_filter(std::optional<PaintCursorTarget::SpaceType> filter)
{
  target_filter_ = filter;
}

void PaintCursorSyncManager::set_image_filter(Image *image)
{
  image_filter_ = image;
}

}  // namespace blender::editors
