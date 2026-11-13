/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "testing/testing.h"

#include "paint_lru_cache.hh"

namespace blender::bke::tests {

namespace {

struct TestValue {
  int id = 0;
  size_t bytes_ = 0;

  size_t bytes() const
  {
    return bytes_;
  }
};

}  // namespace

TEST(LruByteCache, evicts_the_least_recently_used_entries_to_fit_the_budget)
{
  LruByteCache<int, TestValue> cache(16, 8);
  const auto build = [](const int id) {
    return std::make_shared<TestValue>(TestValue{id, 8});
  };

  cache.get_or_build(1, [&]() { return build(1); });
  cache.get_or_build(2, [&]() { return build(2); });
  EXPECT_EQ(cache.entries_num(), 2);
  EXPECT_EQ(cache.total_bytes(), size_t(16));

  /* Reaching the first key makes it the youngest, so the third insert evicts the second. */
  EXPECT_NE(cache.get(1), nullptr);
  cache.get_or_build(3, [&]() { return build(3); });
  EXPECT_EQ(cache.entries_num(), 2);
  EXPECT_EQ(cache.total_bytes(), size_t(16));
  EXPECT_NE(cache.get(1), nullptr);
  EXPECT_EQ(cache.get(2), nullptr);
  EXPECT_NE(cache.get(3), nullptr);
}

TEST(LruByteCache, a_repeated_key_reuses_the_value_without_building_again)
{
  LruByteCache<int, TestValue> cache(1024, 4);
  int builds = 0;
  const auto build = [&]() {
    builds++;
    return std::make_shared<TestValue>(TestValue{7, 8});
  };

  const auto first = cache.get_or_build(7, build);
  const auto second = cache.get_or_build(7, build);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first, second);
  EXPECT_EQ(builds, 1);
  EXPECT_EQ(cache.entries_num(), 1);
}

TEST(LruByteCache, concurrent_callers_of_one_key_build_it_once)
{
  LruByteCache<int, TestValue> cache(1024, 4);
  std::atomic<int> builds{0};
  constexpr int threads_num = 8;
  std::thread threads[threads_num];
  std::shared_ptr<const TestValue> results[threads_num];

  for (int i = 0; i < threads_num; i++) {
    threads[i] = std::thread([&, i]() {
      results[i] = cache.get_or_build(42, [&]() {
        builds++;
        /* Hold the build long enough for the other callers to find the in-flight marker. */
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return std::make_shared<TestValue>(TestValue{42, 8});
      });
    });
  }
  for (std::thread &thread : threads) {
    thread.join();
  }

  EXPECT_EQ(builds.load(), 1);
  for (int i = 0; i < threads_num; i++) {
    ASSERT_NE(results[i], nullptr);
    EXPECT_EQ(results[0], results[i]);
  }
  EXPECT_EQ(cache.entries_num(), 1);
}

TEST(LruByteCache, clear_drops_every_entry_and_the_byte_total)
{
  LruByteCache<int, TestValue> cache(1024, 4);
  cache.get_or_build(1, []() { return std::make_shared<TestValue>(TestValue{1, 8}); });
  cache.get_or_build(2, []() { return std::make_shared<TestValue>(TestValue{2, 16}); });
  EXPECT_EQ(cache.total_bytes(), size_t(24));

  cache.clear();
  EXPECT_EQ(cache.entries_num(), 0);
  EXPECT_EQ(cache.total_bytes(), size_t(0));
  EXPECT_EQ(cache.get(1), nullptr);
}

}  // namespace blender::bke::tests
