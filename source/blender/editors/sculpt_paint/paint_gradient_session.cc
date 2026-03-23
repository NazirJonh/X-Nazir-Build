/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "paint_gradient_session.hh"

#include "BLI_assert.h"

namespace blender::ed::sculpt_paint::gradient::session {

namespace {

class GradientBackendAdapter : public stroke::session::Backend {
 public:
  using GradientBackend = blender::ed::sculpt_paint::gradient::session::Backend;

  explicit GradientBackendAdapter(std::unique_ptr<GradientBackend> gradient_backend)
      : gradient_backend_(std::move(gradient_backend))
  {
    BLI_assert(gradient_backend_ != nullptr);
  }

  bool begin_session(const stroke::session::StaticContext &static_context,
                     const stroke::session::DynamicState &dynamic_state) override
  {
    const DynamicState *gradient_dynamic_state = static_cast<const DynamicState *>(
        dynamic_state.step_data);
    if (gradient_dynamic_state == nullptr) {
      return false;
    }
    return gradient_backend_->begin_session(static_context, *gradient_dynamic_state);
  }

  void restore_baseline(const stroke::session::DynamicState &dynamic_state) override
  {
    const DynamicState *gradient_dynamic_state = static_cast<const DynamicState *>(
        dynamic_state.step_data);
    if (gradient_dynamic_state == nullptr) {
      return;
    }
    gradient_backend_->restore_baseline(*gradient_dynamic_state);
  }

  void apply_step(const stroke::session::DynamicState &dynamic_state) override
  {
    const DynamicState *gradient_dynamic_state = static_cast<const DynamicState *>(
        dynamic_state.step_data);
    if (gradient_dynamic_state == nullptr) {
      return;
    }
    gradient_backend_->apply_preview(*gradient_dynamic_state);
  }

  bool last_step_had_updates() const override
  {
    return gradient_backend_->last_tick_had_updates();
  }

  void commit() override
  {
    gradient_backend_->commit();
  }

  void cancel() override
  {
    gradient_backend_->cancel();
  }

  void end_session() override
  {
    gradient_backend_->end_session();
  }

 private:
  std::unique_ptr<GradientBackend> gradient_backend_;
};

static stroke::session::DynamicState make_runtime_dynamic_state(const DynamicState &dynamic_state)
{
  stroke::session::DynamicState runtime_dynamic_state;
  runtime_dynamic_state.step_data = &dynamic_state;
  runtime_dynamic_state.tick_version = dynamic_state.tick_version;
  return runtime_dynamic_state;
}

}  // namespace

Handle::Handle(std::unique_ptr<Backend> backend, const StaticContext &static_context)
{
  BLI_assert(backend != nullptr);
  runtime_handle_ = std::make_unique<stroke::session::Handle>(
      std::make_unique<GradientBackendAdapter>(std::move(backend)), static_context);
}

Handle::~Handle() = default;

bool Handle::tick(const DynamicState &dynamic_state)
{
  if (runtime_handle_ == nullptr) {
    return false;
  }

  const stroke::session::DynamicState runtime_dynamic_state = make_runtime_dynamic_state(
      dynamic_state);
  return runtime_handle_->tick(runtime_dynamic_state);
}

bool Handle::last_tick_had_updates() const
{
  return (runtime_handle_ != nullptr) ? runtime_handle_->last_step_had_updates() : false;
}

void Handle::commit()
{
  if (runtime_handle_ == nullptr) {
    return;
  }
  runtime_handle_->commit();
}

void Handle::cancel()
{
  if (runtime_handle_ == nullptr) {
    return;
  }
  runtime_handle_->cancel();
}

bool Handle::is_active() const
{
  return (runtime_handle_ != nullptr) ? runtime_handle_->is_active() : false;
}

}  // namespace blender::ed::sculpt_paint::gradient::session

