/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#pragma once

#include <cstdint>
#include <memory>

namespace blender::ed::sculpt_paint::stroke::session {

struct StaticContext {
  /** Opaque pointer owned by caller/backend-specific integration. */
  const void *user_data = nullptr;
};

struct DynamicState {
  /** Opaque pointer to mode/tool specific dynamic step payload. */
  const void *step_data = nullptr;
  int64_t tick_version = 0;
};

class Backend {
 public:
  virtual ~Backend() = default;

  virtual bool begin_session(const StaticContext &static_context, const DynamicState &dynamic_state) = 0;
  virtual void restore_baseline(const DynamicState &dynamic_state) = 0;
  virtual void apply_step(const DynamicState &dynamic_state) = 0;
  virtual bool last_step_had_updates() const
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
  bool last_step_had_updates() const;
  void commit();
  void cancel();

  bool is_active() const;

 private:
  enum class State {
    Uninitialized,
    Active,
    Ended,
  };

  bool begin_if_needed(const DynamicState &dynamic_state);
  void end();

  std::unique_ptr<Backend> backend_;
  StaticContext static_context_;
  State state_ = State::Uninitialized;
  bool last_step_had_updates_ = true;
};

}  // namespace blender::ed::sculpt_paint::stroke::session

