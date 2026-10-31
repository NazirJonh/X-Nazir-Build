/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * The main-thread <-> worker handshake of a mesh map bake, with no dependency on the window
 * manager, so it can be exercised with plain threads.
 *
 * One private render world is alive at a time: the main thread prepares it and publishes it, the
 * worker renders it and publishes the result, the main thread commits it and publishes the next
 * world (or signals that none is left). The worker never commits anything itself.
 *
 * Every worker wait is a loop with a short timeout that re-checks the caller's stop flag. That is
 * what keeps a forced kill safe: `WM_jobs_kill` sets the stop flag and joins the worker without ever
 * running the main-thread update callback, so a single unbounded wait would deadlock and a single
 * timed wait would give up while a slow main thread is still preparing the next world.
 */

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace blender {

class MeshMapBakeHandshake {
 public:
  /** Main: publish the world prepared for the current pair, or that no pair is left. */
  void main_publish_world(const bool prepared)
  {
    std::unique_lock lock(mutex_);
    world_ready_ = prepared;
    exit_ = !prepared;
    cond_.notify_all();
  }

  /** Main: the current pair's result was consumed; let the worker continue. */
  void main_finish_commit()
  {
    std::unique_lock lock(mutex_);
    commit_done_ = true;
    cond_.notify_all();
  }

  /** Main: stop the run and wake the worker. */
  void main_request_exit()
  {
    std::unique_lock lock(mutex_);
    exit_ = true;
    cond_.notify_all();
  }

  /** Main: take the worker's published result, if there is one. */
  bool main_try_take_result()
  {
    std::unique_lock lock(mutex_);
    if (!result_ready_) {
      return false;
    }
    result_ready_ = false;
    return true;
  }

  /**
   * Worker: block until a world is ready. Returns false when the worker must exit: the run is over
   * (`exit_`) or \a stop reports true while waiting. A timeout only loops; it never ends the run.
   *
   * \a stop is a callable, not a `const bool &`, so a caller can pass a live atomic or a
   * `worker_status->stop` field without snapshotting it.
   */
  template<typename StopFn> bool worker_await_world(StopFn stop)
  {
    std::unique_lock lock(mutex_);
    while (!world_ready_ && !exit_ && !stop()) {
      cond_.wait_for(lock, kTimeout);
    }
    if (!world_ready_ || stop()) {
      return false;
    }
    /* Consume the world: the main thread publishes one per pair, so an unconsumed world must not
     * make the worker render a phantom pair after the last one. */
    world_ready_ = false;
    return true;
  }

  /**
   * Worker: publish the rendered result and block until the main thread commits it. Returns false
   * when the worker must exit without the result being committed (\a stop or `exit_`).
   */
  template<typename StopFn> bool worker_publish_result(StopFn stop)
  {
    std::unique_lock lock(mutex_);
    result_ready_ = true;
    commit_done_ = false;
    cond_.notify_all();
    while (!commit_done_ && !exit_ && !stop()) {
      cond_.wait_for(lock, kTimeout);
    }
    return commit_done_ && !stop();
  }

 private:
  static constexpr std::chrono::milliseconds kTimeout{50};

  std::mutex mutex_;
  std::condition_variable cond_;
  bool world_ready_ = false;
  bool result_ready_ = false;
  bool commit_done_ = false;
  bool exit_ = false;
};

}  // namespace blender
