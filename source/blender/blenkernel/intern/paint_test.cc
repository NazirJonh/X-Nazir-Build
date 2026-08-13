/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_gtest_base.hh"
#include "BKE_main.hh"
#include "BKE_object.hh"
#include "BKE_paint.hh"

#include "DNA_object_types.h"

#include "testing/testing.h"

#include <cstdint>

namespace blender {

class SculptSessionFreeTest : public bke::BlenderGTestBase {
 public:
  Main *bmain = nullptr;
  Object *ob = nullptr;

  void SetUp() override
  {
    bmain = BKE_main_new();
    ob = BKE_object_add_only_object(bmain, OB_MESH, "Object");
    ob->mode = OB_MODE_SCULPT;
    BKE_object_sculpt_data_create(ob);
  }

  void TearDown() override
  {
    BKE_main_free(bmain);
  }
};

static bool g_curve_patch_teardown_called = false;

static void stub_curve_patch_teardown(Object &ob)
{
  g_curve_patch_teardown_called = true;
  /* Production discard restores the mesh then MEM_delete's the session. This stub only clears the
   * pointer: the test plants a sentinel, not a real CurvePatchSession. */
  ob.runtime->sculpt_session->curve_patch_session = nullptr;
}

TEST_F(SculptSessionFreeTest, frees_live_curve_patch_without_mode_exit)
{
  /* I2: BKE_object_free -> BKE_sculptsession_free never went through sculpt mode-exit, so a live
   * CurvePatchSession leaked and left uncommitted relief. The editor registers a teardown
   * callback; this test is that BKE actually invokes it. */
  g_curve_patch_teardown_called = false;
  SculptSession &ss = *ob->runtime->sculpt_session;
  ss.curve_patch_session = reinterpret_cast<ed::sculpt_paint::CurvePatchSession *>(uintptr_t(1));
  ss.free_curve_patch_session = stub_curve_patch_teardown;

  BKE_sculptsession_free(ob);

  EXPECT_TRUE(g_curve_patch_teardown_called);
  EXPECT_EQ(ob->runtime->sculpt_session, nullptr);
}

TEST_F(SculptSessionFreeTest, already_discarded_curve_patch_is_not_torn_down_again)
{
  /* Mode-exit already called curve_patch_discard_on_session_end; BKE_sculptsession_free follows.
   * The leftover callback must not run on a null session. */
  g_curve_patch_teardown_called = false;
  SculptSession &ss = *ob->runtime->sculpt_session;
  ss.curve_patch_session = nullptr;
  ss.free_curve_patch_session = stub_curve_patch_teardown;

  BKE_sculptsession_free(ob);

  EXPECT_FALSE(g_curve_patch_teardown_called);
  EXPECT_EQ(ob->runtime->sculpt_session, nullptr);
}

}  // namespace blender
