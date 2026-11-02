/* SPDX-FileCopyrightText: 2014 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "MEM_guardedalloc.h"

#include <stdlib.h>

/* Declared in BLI_cpp_type.hh (blenlib). The full header is not included here on purpose: the
 * minimal bf_testing_main target does not carry blenlib's transitive include directories. Every
 * test executable links #bf_blenlib, so the symbol resolves at link time. */
namespace blender {
void register_cpp_types();
}

DEFINE_string(test_assets_dir, "", "tests/files directory containing the test assets.");
DEFINE_string(test_release_dir, "", "bin/{blender version} directory of the current build.");

namespace blender::tests {

const std::string &flags_test_asset_dir()
{
  if (FLAGS_test_assets_dir.empty()) {
    ADD_FAILURE() << "Pass the flag --test-assets-dir and point to the tests/files directory.";
  }
  return FLAGS_test_assets_dir;
}

const std::string &flags_test_release_dir()
{
  if (FLAGS_test_release_dir.empty()) {
    ADD_FAILURE()
        << "Pass the flag --test-release-dir and point to the bin/{blender version} directory.";
  }
  return FLAGS_test_release_dir;
}

bool should_ignore_blocklist(bool is_all_gpu_vendors)
{
  static bool has_env = getenv("BLENDER_TEST_IGNORE_BLOCKLIST");
  static bool has_env_vendor = getenv("BLENDER_TEST_IGNORE_VENDOR_BLOCKLIST");
  return has_env || (!is_all_gpu_vendors && has_env_vendor);
}

}  // namespace blender::tests

int main(int argc, char **argv)
{
  MEM_use_guarded_allocator();
  MEM_init_memleak_detection();
  MEM_enable_fail_on_memleak();
  /* Register the basic #CPPType instances (float2, ColorGeometry4f, ...). Without them every
   * #CPPType is an all-zero object and #CPPType::to_static_type dispatches to the wrong type,
   * which corrupts memory wherever typed spans are written through the dispatch. Test fixtures
   * that need the Blender-level types call #BKE_cpp_types_init themselves; this covers the
   * blenlib-level types for every test binary. The registration is guarded and idempotent. */
  blender::register_cpp_types();
  testing::InitGoogleTest(&argc, argv);
  BLENDER_GFLAGS_NAMESPACE::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);

  return RUN_ALL_TESTS();
}
