/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#pragma once

#include <cstdint>
#include <memory>

#include "paint_stroke_session.hh"
#include "paint_gradient_types.hh"

namespace blender::ed::sculpt_paint::gradient::session {

using StaticContext = stroke::session::StaticContext;

struct DynamicState {
  Params gradient_params;
  int64_t tick_version = 0;
};

class Backend {
 public:
  virtual ~Backend() = default;

  virtual bool begin_session(const StaticContext &static_context, const DynamicState &dynamic_state) = 0;
  virtual void restore_baseline(const DynamicState &dynamic_state) = 0;
  virtual void apply_preview(const DynamicState &dynamic_state) = 0;
  virtual bool last_tick_had_updates() const
  {
    return true;
  }
  virtual void commit() = 0;
  virtual void cancel() = 0;
  virtual void end_session() = 0;
};

class Handle {
 public:
  Handle(std::unique_ptr<Backend> backend, const StaticContext &static_context);
  ~Handle();

  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;

  bool tick(const DynamicState &dynamic_state);
  bool last_tick_had_updates() const;
  void commit();
  void cancel();

  bool is_active() const;

 private:
  std::unique_ptr<stroke::session::Handle> runtime_handle_;
};

}  // namespace blender::ed::sculpt_paint::gradient::session

