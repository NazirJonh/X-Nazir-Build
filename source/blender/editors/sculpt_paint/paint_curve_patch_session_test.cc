/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cstdint>
#include <memory>

#include "testing/testing.h"

#include "BKE_gtest_base.hh"
#include "BKE_object_types.hh"

#include "DNA_object_types.h"

#include "ED_curve_patch.hh"

#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.hh"
#include "BLI_math_vector.hh"
#include "BLI_set.hh"

#include "paint_curve_patch_layer.hh"
#include "paint_curve_patch_session.hh"
#include "paint_curve_patch_targets.hh"

#include "mesh/sculpt_intern.hh"

namespace blender::ed::sculpt_paint::tests {

/** Fixture: registers the CPP types and attribute providers CurvesGeometry needs. */
class paint_curve_patch_session : public bke::BlenderGTestBase {};

/* -------------------------------------------------------------------- */
/** \name Multi-object curve transform
 * \{ */

/** A two-point bezier control curve with distinct positions, handle positions and radii. */
static bke::CurvesGeometry test_control_curve()
{
  const std::array<float3, 2> positions = {float3(1.0f, 2.0f, 3.0f), float3(4.0f, 5.0f, 6.0f)};
  const std::array<float, 2> radii = {0.5f, 1.0f};
  bke::CurvesGeometry curve = curve_patch_control_curve_from_points(
      positions, radii, /*cyclic=*/false);
  /* The auto handles computed at construction are distinct from the pivots; move one so the
   * transform's handle coverage is actually exercised. */
  if (curve.attributes().contains("handle_left")) {
    curve.handle_positions_left_for_write()[0] += float3(0.25f, 0.0f, 0.0f);
  }
  return curve;
}

TEST_F(paint_curve_patch_session, curve_transform_moves_positions_and_handles)
{
  bke::CurvesGeometry curve = test_control_curve();
  ASSERT_TRUE(curve.attributes().contains("handle_left"));
  const Span<float3> before = curve.positions();
  const float3 pivot_before = before[0];
  const float3 handle_before = curve.handle_positions_left().value()[0];

  const float4x4 translate = math::from_location<float4x4>(float3(10.0f, 0.0f, 0.0f));
  curve_patch_control_curve_transform(translate, curve);

  EXPECT_FLOAT_EQ(curve.positions()[0].x, pivot_before.x + 10.0f);
  EXPECT_FLOAT_EQ(curve.positions()[0].y, pivot_before.y);
  EXPECT_FLOAT_EQ(curve.positions()[0].z, pivot_before.z);
  EXPECT_FLOAT_EQ(curve.positions()[1].x, before[1].x + 10.0f);
  /* The handle moved by the SAME delta as its pivot: a transform that skipped the handle
   * attributes would leave the bezier distorted (the failure the anchor-seed bug produced). */
  const float3 handle_after = curve.handle_positions_left().value()[0];
  EXPECT_FLOAT_EQ(handle_after.x, handle_before.x + 10.0f);
  EXPECT_FLOAT_EQ(handle_after.y, handle_before.y);
  EXPECT_FLOAT_EQ(handle_after.z, handle_before.z);
  /* Radii are dimensionless multipliers and pass through. */
  EXPECT_FLOAT_EQ(curve.radius()[0], 0.5f);
  EXPECT_FLOAT_EQ(curve.radius()[1], 1.0f);
}

TEST_F(paint_curve_patch_session, curve_transform_composes_exactly)
{
  bke::CurvesGeometry curve = test_control_curve();
  const Vector<float3> positions_before(curve.positions().begin(), curve.positions().end());

  /* The exact composition a secondary target's mirror performs:
   * world_to_object(target) @ object_to_world(primary). */
  const float4x4 primary_to_world =
      math::from_location<float4x4>(float3(3.0f, -2.0f, 7.0f)) *
      math::from_rotation<float4x4>(math::EulerXYZ(0.3f, 0.0f, -0.6f));
  const float4x4 world_to_target =
      math::from_rotation<float4x4>(math::EulerXYZ(-0.1f, 0.2f, 0.5f)) *
      math::from_location<float4x4>(float3(-3.0f, 2.0f, -7.0f));
  curve_patch_control_curve_transform(primary_to_world, curve);
  curve_patch_control_curve_transform(world_to_target, curve);

  const float3 expected = math::transform_point(
      world_to_target, math::transform_point(primary_to_world, positions_before[0]));
  EXPECT_NEAR(curve.positions()[0].x, expected.x, 1e-5f);
  EXPECT_NEAR(curve.positions()[0].y, expected.y, 1e-5f);
  EXPECT_NEAR(curve.positions()[0].z, expected.z, 1e-5f);
}

TEST_F(paint_curve_patch_session, curve_transform_identity_keeps_curve)
{
  bke::CurvesGeometry curve = test_control_curve();
  const Vector<float3> positions_before(curve.positions().begin(), curve.positions().end());

  curve_patch_control_curve_transform(float4x4::identity(), curve);

  for (const int i : positions_before.index_range()) {
    EXPECT_FLOAT_EQ(curve.positions()[i].x, positions_before[i].x);
    EXPECT_FLOAT_EQ(curve.positions()[i].y, positions_before[i].y);
    EXPECT_FLOAT_EQ(curve.positions()[i].z, positions_before[i].z);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/* NOTE: the target-structure tests below use fabricated `Object *` and `StrokeCache *`
 * identities. The invariant that makes that safe: NONE of the paths under test --
 * #CurvePatchSession::target_for / #items_for, #is_usable, #has_valid_target,
 * #curve_patch_session_has_commit_content (through #target_state, which only binds references the
 * test effect ignores), #curve_patch_session_remove_target, #curve_patch_target_mesh_claim (which
 * reads `ob.data` of a REAL stack object, never a fabricated one) -- may DEREFERENCE a fabricated
 * pointer; comparing, storing or binding a reference to it is fine. If a refactor makes one of
 * these paths read through the object or the cache, replace the sentinels with `MEM_new` /
 * `MEM_delete` stand-ins first.
 */

constexpr uintptr_t fake_object_a = uintptr_t(1);
constexpr uintptr_t fake_object_b = uintptr_t(2);

TEST_F(paint_curve_patch_session, primary_target_views_the_document)
{
  CurvePatchSession session;
  session.doc.patches.resize(2);
  const int64_t primary_index = session.targets.append_and_get_index_as();
  CurvePatchObjectTarget &primary = session.targets[primary_index];
  primary.ob = reinterpret_cast<Object *>(fake_object_a);
  primary.is_primary = true;
  session.owner = primary.ob;

  MutableSpan<CurvePatchItem> items = session.items_for(primary);
  ASSERT_EQ(items.size(), 2);
  /* The primary's items ARE the document's: mutating one mutates the document (no mirror copy). */
  items[0].curve_id = 42;
  EXPECT_EQ(session.doc.patches[0].curve_id, 42);
  EXPECT_EQ(session.target_for(*primary.ob), &primary);
}

TEST_F(paint_curve_patch_session, secondary_target_reads_its_mirror_index_parallel)
{
  CurvePatchSession session;
  session.doc.patches.resize(3);
  for (const int i : session.doc.patches.index_range()) {
    session.doc.patches[i].curve_id = 100 + i;
    session.doc.patches[i].name = "curve-" + std::to_string(i);
  }
  const int64_t primary_index = session.targets.append_and_get_index_as();
  session.targets[primary_index].ob = reinterpret_cast<Object *>(fake_object_a);
  session.targets[primary_index].is_primary = true;
  session.owner = session.targets[primary_index].ob;

  const int64_t secondary_index = session.targets.append_and_get_index_as();
  CurvePatchObjectTarget &secondary = session.targets[secondary_index];
  secondary.ob = reinterpret_cast<Object *>(fake_object_b);
  secondary.is_primary = false;
  secondary.mirror_items.resize(3);
  for (const int i : secondary.mirror_items.index_range()) {
    secondary.mirror_items[i].curve_id = session.doc.patches[i].curve_id;
    secondary.mirror_items[i].name = session.doc.patches[i].name;
  }
  CurvePatchObjectTarget &primary = session.targets[primary_index];

  Span<CurvePatchItem> items = session.items_for(secondary);
  ASSERT_EQ(items.size(), 3);
  for (const int i : items.index_range()) {
    /* Index-parallel to the document, identity carried. */
    EXPECT_EQ(items[i].curve_id, session.doc.patches[i].curve_id);
    EXPECT_EQ(items[i].name, session.doc.patches[i].name);
    /* And NOT the document's own storage: the secondary stamps its mirror. */
    EXPECT_NE(&items[i], &session.doc.patches[i]);
  }
  EXPECT_EQ(session.target_for(*secondary.ob), &secondary);
  EXPECT_EQ(session.target_for(*primary.ob), &primary);
}

TEST_F(paint_curve_patch_session, target_for_misses_unknown_object)
{
  CurvePatchSession session;
  const int64_t primary_index = session.targets.append_and_get_index_as();
  CurvePatchObjectTarget &primary = session.targets[primary_index];
  primary.ob = reinterpret_cast<Object *>(fake_object_a);
  primary.is_primary = true;
  session.owner = primary.ob;

  EXPECT_EQ(session.target_for(*reinterpret_cast<Object *>(fake_object_b)), nullptr);
}

/** Minimal effect used to exercise the target predicates without a sculpt session. */
class test_effect : public CurvePatchEffect {
 public:
  explicit test_effect(const bool has_content) : has_content_(has_content) {}

  UpdateType update_type() const override
  {
    return UpdateType::Position;
  }
  int64_t element_num(Object & /*ob*/) const override
  {
    return 0;
  }
  void restore(CurvePatchTargetState & /*target*/) override {}
  void begin_restamp(const Depsgraph & /*depsgraph*/,
                     CurvePatchTargetState & /*target*/) override
  {
  }
  void apply_pass(const Depsgraph & /*depsgraph*/,
                  const Brush & /*brush*/,
                  CurvePatchTargetState & /*target*/,
                  const CurvePatchItem & /*item*/) override
  {
  }
  void end_restamp(CurvePatchTargetState & /*target*/) override {}
  bool commit_has_content(const CurvePatchTargetState & /*target*/) const override
  {
    return has_content_;
  }
  void commit_push_nodes(const Depsgraph & /*depsgraph*/,
                         CurvePatchTargetState & /*target*/) override
  {
  }
  int64_t snapshot_size() const override
  {
    return 0;
  }

 private:
  bool has_content_;
};

/** A non-null sentinel is enough: none of these tests dereferences the cache. */
static StrokeCache *fake_cache()
{
  return reinterpret_cast<StrokeCache *>(uintptr_t(3));
}

TEST_F(paint_curve_patch_session, usable_targets_are_skipped_individually)
{
  CurvePatchSession session;
  const int64_t primary_index = session.targets.append_and_get_index_as();
  session.targets[primary_index].ob = reinterpret_cast<Object *>(fake_object_a);
  session.targets[primary_index].cache = fake_cache();
  session.targets[primary_index].effect = std::make_unique<test_effect>(false);
  session.targets[primary_index].is_primary = true;
  session.owner = session.targets[primary_index].ob;

  const int64_t secondary_index = session.targets.append_and_get_index_as();
  session.targets[secondary_index].ob = reinterpret_cast<Object *>(fake_object_b);
  session.targets[secondary_index].cache = fake_cache();
  session.targets[secondary_index].effect = std::make_unique<test_effect>(false);
  session.targets[secondary_index].is_primary = false;

  CurvePatchObjectTarget &primary = session.targets[primary_index];
  CurvePatchObjectTarget &secondary = session.targets[secondary_index];
  EXPECT_TRUE(session.has_valid_target());
  EXPECT_TRUE(primary.is_usable());
  EXPECT_TRUE(secondary.is_usable());

  /* A broken secondary must not invalidate the primary. */
  secondary.apply.invalidated = true;
  EXPECT_TRUE(session.has_valid_target());
  EXPECT_TRUE(primary.is_usable());
  EXPECT_FALSE(secondary.is_usable());

  primary.apply.invalidated = true;
  EXPECT_FALSE(session.has_valid_target());
}

TEST_F(paint_curve_patch_session, commit_without_content_opens_no_step)
{
  CurvePatchSession session;
  CurvePatchObjectTarget &primary = session.targets[session.targets.append_and_get_index_as()];
  primary.ob = reinterpret_cast<Object *>(fake_object_a);
  primary.cache = fake_cache();
  primary.effect = std::make_unique<test_effect>(false);
  primary.is_primary = true;
  session.owner = primary.ob;

  /* This is the predicate `curve_patch_finish_commit()` checks before touching the undo stack.
   * False means it returns without calling `push_end_all_ex()` at all. */
  EXPECT_FALSE(curve_patch_session_has_commit_content(session));
  primary.effect = std::make_unique<test_effect>(true);
  EXPECT_TRUE(curve_patch_session_has_commit_content(session));
}

TEST_F(paint_curve_patch_session, shared_mesh_dedup_uses_the_data_id)
{
  Set<const ID *> taken_meshes;
  Object first = {};
  Object second = {};
  Object third = {};
  first.data = reinterpret_cast<ID *>(uintptr_t(11));
  second.data = reinterpret_cast<ID *>(uintptr_t(11));
  third.data = reinterpret_cast<ID *>(uintptr_t(12));

  EXPECT_TRUE(curve_patch_target_mesh_claim(taken_meshes, first));
  EXPECT_FALSE(curve_patch_target_mesh_claim(taken_meshes, second));
  EXPECT_TRUE(curve_patch_target_mesh_claim(taken_meshes, third));
}

TEST_F(paint_curve_patch_session, detaching_removes_only_that_secondary)
{
  Object *primary_ob = reinterpret_cast<Object *>(fake_object_a);
  Object *secondary_ob = reinterpret_cast<Object *>(fake_object_b);
  Object *other_ob = reinterpret_cast<Object *>(uintptr_t(5));

  CurvePatchSession session;
  const int64_t primary_index = session.targets.append_and_get_index_as();
  session.targets[primary_index].ob = primary_ob;
  session.targets[primary_index].is_primary = true;
  const int64_t secondary_index = session.targets.append_and_get_index_as();
  session.targets[secondary_index].ob = secondary_ob;
  const int64_t other_index = session.targets.append_and_get_index_as();
  session.targets[other_index].ob = other_ob;

  EXPECT_TRUE(curve_patch_session_remove_target(session, *secondary_ob));
  EXPECT_EQ(session.targets.size(), 2);
  EXPECT_EQ(session.target_for(*secondary_ob), nullptr);
  const CurvePatchObjectTarget *remaining_primary = session.target_for(*primary_ob);
  const CurvePatchObjectTarget *remaining_other = session.target_for(*other_ob);
  ASSERT_NE(remaining_primary, nullptr);
  ASSERT_NE(remaining_other, nullptr);
  EXPECT_EQ(remaining_primary->ob, primary_ob);
  EXPECT_EQ(remaining_other->ob, other_ob);
  EXPECT_FALSE(curve_patch_session_remove_target(session, *secondary_ob));
}

TEST_F(paint_curve_patch_session, mirror_round_trip_uses_world_scale)
{
  bke::ObjectRuntime primary_runtime;
  bke::ObjectRuntime target_runtime;
  Object primary_ob = {};
  Object target_ob = {};
  primary_ob.runtime = &primary_runtime;
  target_ob.runtime = &target_runtime;

  /* Parent transforms are already baked into the world matrices. The different uniform scales
   * make the radius conversion observable. */
  primary_runtime.object_to_world =
      math::from_location<float4x4>(float3(3.0f, -2.0f, 1.0f)) *
      math::from_rotation<float4x4>(math::EulerXYZ(0.2f, -0.1f, 0.4f)) *
      math::from_scale<float4x4>(float3(2.0f));
  target_runtime.object_to_world =
      math::from_location<float4x4>(float3(-4.0f, 1.0f, 2.0f)) *
      math::from_rotation<float4x4>(math::EulerXYZ(-0.3f, 0.25f, -0.2f)) *
      math::from_scale<float4x4>(float3(4.0f));
  primary_runtime.world_to_object = math::invert(primary_runtime.object_to_world);
  target_runtime.world_to_object = math::invert(target_runtime.object_to_world);

  CurvePatchSession session;
  session.owner = &primary_ob;
  session.doc.patches.resize(1);
  CurvePatchItem &src = session.doc.patches[0];
  src.control_curve = test_control_curve();
  src.params.radius = 2.0f;
  src.params.radius_per_size = 3.0f;
  src.params.jitter_amount = 0.5f;
  session.doc.revision = curve_patch_document_content_revision(session.doc);

  CurvePatchObjectTarget &target = session.targets[session.targets.append_and_get_index_as()];
  target.ob = &target_ob;
  target.is_primary = false;
  curve_patch_target_mirror_sync(session, target, nullptr);

  ASSERT_EQ(target.mirror_items.size(), 1);
  const CurvePatchItem &mirror = target.mirror_items[0];
  EXPECT_NEAR(mirror.params.radius, 1.0f, 1e-5f);
  EXPECT_NEAR(mirror.params.radius_per_size, 1.5f, 1e-5f);
  EXPECT_NEAR(mirror.params.jitter_amount, 0.25f, 1e-5f);

  const float3 expected = math::transform_point(
      target_runtime.world_to_object,
      math::transform_point(primary_runtime.object_to_world, src.control_curve.positions()[0]));
  EXPECT_NEAR(mirror.control_curve.positions()[0].x, expected.x, 1e-5f);
  EXPECT_NEAR(mirror.control_curve.positions()[0].y, expected.y, 1e-5f);
  EXPECT_NEAR(mirror.control_curve.positions()[0].z, expected.z, 1e-5f);
}

TEST_F(paint_curve_patch_session, document_revision_tracks_curve_and_params)
{
  CurvePatchSession session;
  session.doc.patches.resize(1);
  CurvePatchItem &item = session.doc.patches[0];
  item.control_curve = test_control_curve();

  const uint64_t initial = curve_patch_document_content_revision(session.doc);
  EXPECT_EQ(curve_patch_document_content_revision(session.doc), initial);

  item.params.radius += 0.5f;
  const uint64_t params_changed = curve_patch_document_content_revision(session.doc);
  EXPECT_NE(params_changed, initial);

  item.control_curve.positions_for_write()[0] += float3(0.25f, 0.0f, 0.0f);
  item.control_curve.tag_positions_changed();
  EXPECT_NE(curve_patch_document_content_revision(session.doc), params_changed);
}

/** \} */

TEST_F(paint_curve_patch_session, active_source_curve_maps_to_filtered_patch)
{
  CurvePatchSession session;
  session.doc.patches.resize(2);
  session.doc.patches[0].source_curve_index = 1;
  session.doc.patches[1].source_curve_index = 3;

  EXPECT_EQ(curve_patch_index_for_source_curve(session.doc, 1), 0);
  EXPECT_EQ(curve_patch_index_for_source_curve(session.doc, 3), 1);
}

TEST_F(paint_curve_patch_session, missing_source_curve_is_reported_as_unmapped)
{
  CurvePatchSession session;
  session.doc.patches.resize(2);
  session.doc.patches[0].source_curve_index = 2;
  session.doc.patches[1].source_curve_index = 4;

  EXPECT_EQ(curve_patch_index_for_source_curve(session.doc, 0), -1);
  EXPECT_EQ(curve_patch_index_for_source_curve(session.doc, 5), -1);
}

TEST_F(paint_curve_patch_session, missing_source_curve_has_no_fallback_for_empty_session)
{
  const CurvePatchSession session;
  EXPECT_EQ(curve_patch_index_for_source_curve(session.doc, 0), -1);
}

TEST_F(paint_curve_patch_session, overlay_data_is_empty_without_session)
{
  blender::CurvePatchOverlayData data;
  data.splines.append(nullptr);
  data.active_index = 4;

  blender::ED_curve_patch_overlay_data_get(static_cast<const CurvePatchSession *>(nullptr), data);

  EXPECT_TRUE(data.splines.is_empty());
  EXPECT_EQ(data.active_index, -1);
}

TEST_F(paint_curve_patch_session, overlay_data_lists_all_splines_and_marks_active)
{
  CurvePatchSession session;
  session.doc.patches.resize(2);
  session.doc.active_patch = 1;

  blender::CurvePatchOverlayData data;
  blender::ED_curve_patch_overlay_data_get(&session, data);

  ASSERT_EQ(data.splines.size(), 2);
  EXPECT_EQ(data.splines[0], &session.doc.patches[0].control_curve);
  EXPECT_EQ(data.splines[1], &session.doc.patches[1].control_curve);
  EXPECT_EQ(data.active_index, 1);
}

TEST_F(paint_curve_patch_session, overlay_data_has_no_active_on_half_built_session)
{
  CurvePatchSession session;
  session.doc.active_patch = 0;

  blender::CurvePatchOverlayData data;
  blender::ED_curve_patch_overlay_data_get(&session, data);

  EXPECT_TRUE(data.splines.is_empty());
  EXPECT_EQ(data.active_index, -1);
}

}  // namespace blender::ed::sculpt_paint::tests
