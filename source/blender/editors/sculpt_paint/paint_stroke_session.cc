/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "paint_stroke_session.hh"

#include "BLI_assert.h"

namespace blender::ed::sculpt_paint::stroke::session {

Handle::Handle(std::unique_ptr<Backend> backend, const StaticContext &static_context)
    : backend_(std::move(backend)), static_context_(static_context)
{
  BLI_assert(backend_ != nullptr);
}

Handle::~Handle()
{
  if (state_ == State::Active) {
    this->cancel();
  }
  else if (state_ != State::Ended) {
    this->end();
  }
}

bool Handle::begin_if_needed(const DynamicState &dynamic_state)
{
  if (state_ == State::Active) {
    return true;
  }
  if (state_ != State::Uninitialized || backend_ == nullptr) {
    return false;
  }

  if (!backend_->begin_session(static_context_, dynamic_state)) {
    this->end();
    return false;
  }

  state_ = State::Active;
  return true;
}

bool Handle::tick(const DynamicState &dynamic_state)
{
  if (!this->begin_if_needed(dynamic_state)) {
    return false;
  }

  backend_->restore_baseline(dynamic_state);
  backend_->apply_step(dynamic_state);
  last_step_had_updates_ = backend_->last_step_had_updates();
  return true;
}

bool Handle::last_step_had_updates() const
{
  return last_step_had_updates_;
}

void Handle::commit()
{
  if (state_ != State::Active || backend_ == nullptr) {
    return;
  }
  backend_->commit();
  this->end();
}

void Handle::cancel()
{
  if (state_ != State::Active || backend_ == nullptr) {
    return;
  }
  backend_->cancel();
  this->end();
}

bool Handle::is_active() const
{
  return state_ == State::Active;
}

void Handle::end()
{
  if (backend_ != nullptr) {
    backend_->end_session();
  }
  state_ = State::Ended;
}

}  // namespace blender::ed::sculpt_paint::stroke::session

