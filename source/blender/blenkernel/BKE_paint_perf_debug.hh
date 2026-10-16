/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * Optional printf diagnostics for the material composite / Combined preview cost.
 *
 * Off unless the environment variable `BLENDER_PBR_PERF` is set. Its value is the minimum duration in
 * milliseconds a scope must take to be printed: `0` prints everything, `2` only scopes of 2 ms or
 * more. Read once, so changing it needs a restart, not a rebuild.
 */

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#include "BLI_time.h"

namespace blender::bke::paint_perf {

/** Minimum duration in milliseconds to print, or a negative value while disabled. */
inline double min_ms()
{
  static const double value = []() {
    const char *env = std::getenv("BLENDER_PBR_PERF");
    return env != nullptr ? std::atof(env) : -1.0;
  }();
  return value;
}

inline bool enabled()
{
  return min_ms() >= 0.0;
}

/** Prints its label, duration and the note when it leaves scope. */
class ScopedTimer {
  const char *label_;
  double start_;
  char note_[320];

 public:
  explicit ScopedTimer(const char *label)
      : label_(label), start_(enabled() ? BLI_time_now_seconds() : 0.0)
  {
    note_[0] = '\0';
  }

  /** Replaces the note shown beside the duration. */
  void note(const char *format, ...)
  {
    if (!enabled()) {
      return;
    }
    va_list args;
    va_start(args, format);
    vsnprintf(note_, sizeof(note_), format, args);
    va_end(args);
  }

  /** Milliseconds since construction; zero while disabled. */
  double elapsed_ms() const
  {
    return enabled() ? (BLI_time_now_seconds() - start_) * 1000.0 : 0.0;
  }

  ~ScopedTimer()
  {
    if (!enabled()) {
      return;
    }
    const double ms = this->elapsed_ms();
    if (ms < min_ms()) {
      return;
    }
    printf("[PBR-PERF] %-34s %9.3f ms%s%s\n", label_, ms, note_[0] != '\0' ? "  | " : "", note_);
    fflush(stdout);
  }

  ScopedTimer(const ScopedTimer &) = delete;
  ScopedTimer &operator=(const ScopedTimer &) = delete;
};

}  // namespace blender::bke::paint_perf
