/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <condition_variable>
#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>

#include "BLI_function_ref.hh"
#include "BLI_vector.hh"

namespace blender::bke {

/**
 * A least-recently-used store of shared values with a byte budget and an optional entry cap.
 *
 * The values are built outside the store's mutex: a miss inserts an in-flight marker before it
 * unlocks, so callers on other threads that ask for the same key wait for that one build instead of
 * building a copy. Values of different keys build in parallel. #Value must provide `size_t bytes()`
 * so the budget is accounted incrementally, without walking the entries.
 */
template<typename Key, typename Value> class LruByteCache {
 public:
  using ValuePtr = std::shared_ptr<const Value>;

  explicit LruByteCache(const size_t max_bytes,
                        const int max_entries = std::numeric_limits<int>::max())
      : max_bytes_(max_bytes), max_entries_(max_entries)
  {
  }

  /** Look \a key up and make it the most recently used entry; null on a miss. */
  ValuePtr get(const Key &key)
  {
    std::lock_guard lock(mutex_);
    return this->lookup_locked(key);
  }

  /**
   * Return the value of \a key, building it with \a build_fn outside the lock on a miss.
   *
   * \param r_built: whether this call built the value rather than finding or waiting for it.
   * \param build_bytes: lets the store make room before the build, so its peak stays near the
   * budget. Zero only evicts on insert.
   */
  ValuePtr get_or_build(const Key &key,
                        FunctionRef<ValuePtr()> build_fn,
                        bool *r_built = nullptr,
                        const size_t build_bytes = 0)
  {
    std::unique_lock lock(mutex_);
    if (ValuePtr hit = this->lookup_locked(key)) {
      if (r_built != nullptr) {
        *r_built = false;
      }
      return hit;
    }

    std::shared_ptr<Flight> flight;
    for (const std::shared_ptr<Flight> &candidate : flights_) {
      if (candidate->key == key) {
        flight = candidate;
        break;
      }
    }
    if (flight != nullptr) {
      /* Another caller is building this key: wait for it and read its result below. */
      flight->cv.wait(lock, [&] { return flight->done; });
      if (r_built != nullptr) {
        *r_built = false;
      }
      /* The entry may have been evicted between the insert and this wake-up; the flight still
       * holds the value the builder returned. */
      ValuePtr hit = this->lookup_locked(key);
      return hit != nullptr ? hit : flight->value;
    }

    flight = std::make_shared<Flight>();
    flight->key = key;
    flights_.append(flight);
    if (build_bytes > 0) {
      this->evict_locked(build_bytes);
    }
    lock.unlock();

    ValuePtr value = build_fn();

    lock.lock();
    if (value != nullptr) {
      this->insert_locked(key, value);
    }
    flight->value = value;
    flight->done = true;
    for (const int i : flights_.index_range()) {
      if (flights_[i] == flight) {
        flights_.remove(i);
        break;
      }
    }
    flight->cv.notify_all();
    if (r_built != nullptr) {
      *r_built = true;
    }
    return value;
  }

  void insert(const Key &key, ValuePtr value)
  {
    std::lock_guard lock(mutex_);
    this->insert_locked(key, value);
  }

  void clear()
  {
    std::lock_guard lock(mutex_);
    entries_.clear_and_shrink();
    total_bytes_ = 0;
  }

  size_t total_bytes() const
  {
    std::lock_guard lock(mutex_);
    return total_bytes_;
  }

  int64_t entries_num() const
  {
    std::lock_guard lock(mutex_);
    return entries_.size();
  }

 private:
  struct Entry {
    Key key;
    ValuePtr value;
    size_t bytes = 0;
  };

  struct Flight {
    Key key;
    std::condition_variable cv;
    bool done = false;
    ValuePtr value;
  };

  /** Oldest first: the vector is the LRU order. */
  Vector<Entry> entries_;
  size_t total_bytes_ = 0;
  Vector<std::shared_ptr<Flight>> flights_;
  const size_t max_bytes_;
  const int max_entries_;
  mutable std::mutex mutex_;

  ValuePtr lookup_locked(const Key &key)
  {
    for (const int i : entries_.index_range()) {
      if (entries_[i].key == key) {
        Entry hit = std::move(entries_[i]);
        entries_.remove(i);
        entries_.append(std::move(hit));
        return entries_.last().value;
      }
    }
    return nullptr;
  }

  void insert_locked(const Key &key, ValuePtr value)
  {
    if (value == nullptr) {
      return;
    }
    const size_t bytes = value->bytes();
    this->evict_locked(bytes);
    Entry entry;
    entry.key = key;
    entry.bytes = bytes;
    entry.value = std::move(value);
    entries_.append(std::move(entry));
    total_bytes_ += bytes;
  }

  void evict_locked(const size_t incoming_bytes)
  {
    /* An entry alone above the budget still goes in, as the one entry the store holds. */
    while (!entries_.is_empty() && (int(entries_.size()) >= max_entries_ ||
                                    total_bytes_ + incoming_bytes > max_bytes_))
    {
      total_bytes_ -= entries_[0].bytes;
      entries_.remove(0);
    }
  }
};

}  // namespace blender::bke
