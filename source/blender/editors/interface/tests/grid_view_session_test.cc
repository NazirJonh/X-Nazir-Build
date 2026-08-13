/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "interface_grid_view.hh"

#include "UI_grid_view.hh"

#include "testing/testing.h"

namespace blender::ui::tests {

TEST(grid_view_session, remove_prefix_drops_unreferenced_python_keys)
{
  /* Unique prefix so this does not collide with live editor sessions in the same process. */
  const char *owned = "pygrid:TEST_GT_wave5:regionA:grid";
  const char *other_type = "pygrid:TEST_GT_other:regionA:grid";
  const char *asset = "pygrid-asset:regionA:grid";

  grid_session_state_ensure(owned);
  grid_session_state_ensure(other_type);
  grid_session_state_ensure(asset);

  grid_view_session_remove_prefix("pygrid:TEST_GT_wave5:");

  EXPECT_EQ(grid_session_state_lookup(owned), nullptr);
  EXPECT_NE(grid_session_state_lookup(other_type), nullptr);
  EXPECT_NE(grid_session_state_lookup(asset), nullptr);

  grid_view_session_remove(other_type);
  grid_view_session_remove(asset);
}

TEST(grid_view_session, remove_prefix_keeps_referenced_sessions)
{
  const char *key = "pygrid:TEST_GT_wave5_live:regionA:grid";
  GridSessionState &session = grid_session_state_ensure(key);
  grid_session_acquire(session);

  grid_view_session_remove_prefix("pygrid:TEST_GT_wave5_live:");
  EXPECT_NE(grid_session_state_lookup(key), nullptr);

  grid_session_release(session);
  grid_view_session_remove_prefix("pygrid:TEST_GT_wave5_live:");
  EXPECT_EQ(grid_session_state_lookup(key), nullptr);
}

}  // namespace blender::ui::tests
