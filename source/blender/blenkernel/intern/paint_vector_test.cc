/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <cstring>

#include "BKE_appdir.hh"
#include "BKE_attribute.hh"
#include "BKE_curve_patch.hh"
#include "BKE_curves.hh"
#include "BKE_gtest_base.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_paint.hh"
#include "BKE_paint_vector.hh"

#include "BLI_fileops.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"


#include "BLO_readfile.hh"
#include "BLO_writefile.hh"

#include "DNA_paint_vector_types.h"
#include "DNA_scene_types.h"

namespace blender {

namespace {

/** Two items with points, a spline and the full owned style profiles, so copy / save must handle
 * every owning pointer an item can have. */
PaintVector *make_two_item_vector(Main *bmain, const char *name)
{
  PaintVector *pv = BKE_paint_vector_add(bmain, name);
  /* Unused IDs are not saved; the test opens a fresh file. */
  id_fake_user_set(&pv->id);

  PaintVectorItem &a = BKE_paint_vector_item_add(*pv);
  a.type = PAINT_VECTOR_ITEM_SHAPE;
  a.shape_type = PAINT_SHAPE_LINE;
  BKE_paint_vector_item_style_init(a);
  a.style.stroke_width = 12.0f;
  MutableSpan<PaintVectorPoint> points = BKE_paint_vector_item_points_alloc(a, 2);
  points[0].co[0] = 1.0f;
  points[1].co[0] = 2.0f;
  MutableSpan<PaintVectorSpline> splines = BKE_paint_vector_item_splines_alloc(a, 1);
  splines[0].point_offset = 0;
  splines[0].point_num = 2;

  PaintVectorItem &b = BKE_paint_vector_item_add(*pv);
  b.type = PAINT_VECTOR_ITEM_SHAPE;
  b.shape_type = PAINT_SHAPE_ELLIPSE;
  BKE_paint_vector_item_style_init(b);
  b.style.fill_type = PAINT_SHAPE_FILL_TEXTURE;

  pv->active_item = 1;
  return pv;
}

class PaintVectorTest : public bke::BlenderGTestBase {
 public:
  Main *bmain = nullptr;

  void SetUp() override
  {
    bmain = BKE_main_new();
  }
  void TearDown() override
  {
    BKE_main_free(bmain);
  }
};

TEST_F(PaintVectorTest, CopyIsDeepAndIndependent)
{
  PaintVector *pv = make_two_item_vector(bmain, "PVCopy");

  ID *copy_id = BKE_id_copy(bmain, &pv->id);
  ASSERT_NE(copy_id, nullptr);
  PaintVector *copy = id_cast<PaintVector *>(copy_id);

  ASSERT_EQ(copy->items_num, 2);
  EXPECT_EQ(copy->active_item, 1);
  /* Deep copy: no owning pointer is shared. */
  EXPECT_NE(copy->items, pv->items);
  EXPECT_NE(copy->items[0].points, pv->items[0].points);
  EXPECT_NE(copy->items[0].splines, pv->items[0].splines);
  EXPECT_NE(copy->items[0].style.stroke_profile, pv->items[0].style.stroke_profile);
  EXPECT_NE(copy->items[0].style.stroke_ramp, pv->items[0].style.stroke_ramp);

  /* Mutating the copy leaves the original untouched. */
  copy->items[0].style.stroke_width = 99.0f;
  copy->items[0].points[0].co[0] = -1.0f;
  EXPECT_FLOAT_EQ(pv->items[0].style.stroke_width, 12.0f);
  EXPECT_FLOAT_EQ(pv->items[0].points[0].co[0], 1.0f);

  BKE_id_free(bmain, copy_id);
}

TEST_F(PaintVectorTest, NeedsRebakeTracksRevision)
{
  PaintVector *pv = make_two_item_vector(bmain, "PVRev");

  /* A freshly built document has not been baked, but nothing changed since "revision 0". */
  EXPECT_FALSE(BKE_paint_vector_needs_rebake(*pv));

  BKE_paint_vector_tag_changed(*pv);
  EXPECT_TRUE(BKE_paint_vector_needs_rebake(*pv));
  BKE_paint_vector_tag_baked(*pv);
  EXPECT_FALSE(BKE_paint_vector_needs_rebake(*pv));

  /* Every further change is seen again after a bake. */
  BKE_paint_vector_tag_changed(*pv);
  BKE_paint_vector_tag_changed(*pv);
  EXPECT_TRUE(BKE_paint_vector_needs_rebake(*pv));
  BKE_paint_vector_tag_baked(*pv);
  EXPECT_FALSE(BKE_paint_vector_needs_rebake(*pv));
}

TEST_F(PaintVectorTest, WasBakedFlagIsExplicit)
{
  /* Until Stack Layers, re-editing an already baked PaintVector writes only the document; the
   * decision is the explicit DNA flag, not the revision counters. */
  PaintVector *pv = make_two_item_vector(bmain, "PVBaked");
  EXPECT_FALSE(BKE_paint_vector_was_baked(*pv));
  BKE_paint_vector_tag_changed(*pv);
  EXPECT_FALSE(BKE_paint_vector_was_baked(*pv));
  BKE_paint_vector_tag_baked(*pv);
  EXPECT_TRUE(BKE_paint_vector_was_baked(*pv));
  BKE_paint_vector_tag_changed(*pv);
  EXPECT_TRUE(BKE_paint_vector_was_baked(*pv));
}

TEST_F(PaintVectorTest, StyleSnapshotDeepCopyRoundTrip)
{
  PaintShapeSettings original = {};
  BKE_paint_shape_settings_init(&original);
  original.stroke_width = 5.0f;
  original.fill_type = PAINT_SHAPE_FILL_TEXTURE;
  original.stroke_channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].color[0] = 0.25f;

  /* This is exactly the save/restore path of the tool settings around a `vector_edit` session. */
  PaintShapeSettings copy = {};
  BKE_paint_vector_style_copy(copy, original);
  EXPECT_FLOAT_EQ(copy.stroke_width, 5.0f);
  EXPECT_EQ(copy.fill_type, PAINT_SHAPE_FILL_TEXTURE);
  EXPECT_FLOAT_EQ(copy.stroke_channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].color[0], 0.25f);
  EXPECT_NE(copy.stroke_profile, nullptr);
  EXPECT_NE(copy.fill_profile, nullptr);
  EXPECT_NE(copy.stroke_ramp, nullptr);
  EXPECT_NE(copy.stroke_profile, original.stroke_profile);
  EXPECT_NE(copy.stroke_ramp, original.stroke_ramp);

  /* The copy is independent: the session edits its own block, leaving the original untouched. */
  copy.stroke_width = 99.0f;
  copy.stroke_channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].color[0] = 0.9f;
  EXPECT_FLOAT_EQ(original.stroke_width, 5.0f);
  EXPECT_FLOAT_EQ(original.stroke_channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].color[0], 0.25f);

  BKE_paint_vector_style_free(copy);
  BKE_paint_vector_style_free(original);
}

TEST_F(PaintVectorTest, MaterialLinkRoundTrip)
{
  Material *ma = BKE_material_add(bmain, "PVMat");
  PaintVector *pv = BKE_paint_vector_add(bmain, "PVLink");

  EXPECT_EQ(BKE_paint_vector_material_count(*ma), 0);
  EXPECT_EQ(BKE_paint_vector_material_get(*ma, "PVLink"), nullptr);

  BKE_paint_vector_material_link(*ma, *pv);
  /* The "create + assign" pattern drops the user `BKE_id_new` added, leaving the material link as
   * the single owner. */
  id_us_min(&pv->id);
  EXPECT_EQ(pv->id.us, 1);
  EXPECT_EQ(BKE_paint_vector_material_count(*ma), 1);
  EXPECT_EQ(BKE_paint_vector_material_get(*ma, "PVLink"), pv);
  EXPECT_EQ(BKE_paint_vector_material_get_index(*ma, 0), pv);

  /* Linking again is idempotent, matched by ID pointer even after a rename. */
  BKE_paint_vector_material_link(*ma, *pv);
  EXPECT_EQ(BKE_paint_vector_material_count(*ma), 1);
  BLI_strncpy(pv->id.name + 2, "PVRenamed", sizeof(pv->id.name) - 2);
  EXPECT_EQ(BKE_paint_vector_material_get(*ma, "PVRenamed"), pv);
  EXPECT_EQ(BKE_paint_vector_material_get(*ma, "PVLink"), nullptr);
  BKE_paint_vector_material_link(*ma, *pv);
  EXPECT_EQ(BKE_paint_vector_material_count(*ma), 1);

  BKE_paint_vector_material_unlink(*ma, *pv);
  EXPECT_EQ(BKE_paint_vector_material_count(*ma), 0);

  BKE_id_free(bmain, &pv->id);
  BKE_id_free(bmain, ma);
}

TEST_F(PaintVectorTest, SaveReloadKeepsAllItems)
{
  PaintVector *pv = make_two_item_vector(bmain, "PVSaveTest");
  pv->items[0].style.stroke_width = 13.0f;

  char filepath[FILE_MAX];
  BLI_path_join(filepath, sizeof(filepath), BKE_tempdir_session(), "paint_vector_test.blend");
  BlendFileWriteParams write_params{};
  ASSERT_TRUE(BLO_write_file(bmain, filepath, 0, &write_params, nullptr));

  BlendFileReadReport read_report{};
  BlendFileData *bfd = BLO_read_from_file(filepath, BLO_READ_SKIP_NONE, &read_report);
  ASSERT_NE(bfd, nullptr);

  PaintVector *loaded = nullptr;
  for (PaintVector &cur : bfd->main->paint_vectors) {
    if (STREQ(cur.id.name + 2, "PVSaveTest")) {
      loaded = &cur;
      break;
    }
  }
  ASSERT_NE(loaded, nullptr);

  /* The failing case of the old writer: >1 item read back as more than one. */
  ASSERT_EQ(loaded->items_num, 2);
  EXPECT_EQ(loaded->active_item, 1);
  EXPECT_EQ(loaded->items[0].points_num, 2);
  EXPECT_EQ(loaded->items[0].splines_num, 1);
  EXPECT_FLOAT_EQ(loaded->items[0].points[0].co[0], 1.0f);
  EXPECT_FLOAT_EQ(loaded->items[0].style.stroke_width, 13.0f);
  EXPECT_NE(loaded->items[0].style.stroke_profile, nullptr);
  EXPECT_EQ(loaded->items[1].shape_type, PAINT_SHAPE_ELLIPSE);

  BLO_blendfiledata_free(bfd);
  BLI_delete(filepath, false, false);
}

/* -------------------------------------------------------------------- */
/** \name Curve Patch serialization
 * \{ */

/** A 3-point bezier curve with the required attributes (positions, handles + types, radius) and,
 * when \a with_optional is set, the full optional `paintcurve_*` attribute set of a Curve Patch
 * control curve. Handle types stay free / vector / align so no auto-handle pass is involved. */
static bke::CurvesGeometry make_curve_patch_curve(const bool with_optional)
{
  constexpr int point_num = 3;
  bke::CurvesGeometry geom(point_num, 1);
  geom.offsets_for_write()[0] = 0;
  geom.offsets_for_write()[1] = point_num;
  geom.fill_curve_types(CURVE_TYPE_BEZIER);
  geom.resolution_for_write().fill(12);
  geom.cyclic_for_write()[0] = true;

  const MutableSpan<float3> positions = geom.positions_for_write();
  positions[0] = float3(0.0f, 0.0f, 0.0f);
  positions[1] = float3(1.0f, 0.5f, 0.25f);
  positions[2] = float3(2.0f, 0.0f, 0.0f);
  const MutableSpan<float3> handles_left = geom.handle_positions_left_for_write();
  const MutableSpan<float3> handles_right = geom.handle_positions_right_for_write();
  handles_left[0] = float3(-0.5f, -0.25f, 0.0f);
  handles_right[0] = float3(0.5f, 0.25f, 0.0f);
  handles_left[1] = float3(0.5f, 0.5f, 0.25f);
  handles_right[1] = float3(1.5f, 0.5f, 0.25f);
  handles_left[2] = float3(1.5f, 0.25f, 0.0f);
  handles_right[2] = float3(2.5f, -0.25f, 0.0f);
  const MutableSpan<int8_t> types_left = geom.handle_types_left_for_write();
  const MutableSpan<int8_t> types_right = geom.handle_types_right_for_write();
  types_left[0] = BEZIER_HANDLE_FREE;
  types_right[0] = BEZIER_HANDLE_FREE;
  types_left[1] = BEZIER_HANDLE_ALIGN;
  types_right[1] = BEZIER_HANDLE_ALIGN;
  types_left[2] = BEZIER_HANDLE_VECTOR;
  types_right[2] = BEZIER_HANDLE_VECTOR;
  const MutableSpan<float> radii = geom.radius_for_write();
  radii[0] = 0.25f;
  radii[1] = 0.5f;
  radii[2] = 0.75f;

  if (with_optional) {
    bke::MutableAttributeAccessor attrs = geom.attributes_for_write();
    bke::SpanAttributeWriter<float3> normals = attrs.lookup_or_add_for_write_span<float3>(
        bke::CURVE_PATCH_ATTR_SURFACE_NORMAL, bke::AttrDomain::Point);
    normals.span[0] = float3(0.0f, 0.0f, 1.0f);
    normals.span[1] = float3(0.1f, 0.0f, 0.9f);
    normals.span[2] = float3(0.0f, 0.2f, 0.8f);
    normals.finish();
    bke::SpanAttributeWriter<float> strength = attrs.lookup_or_add_for_write_span<float>(
        bke::CURVE_PATCH_ATTR_STAMP_STRENGTH, bke::AttrDomain::Point);
    strength.span[0] = 0.8f;
    strength.span[1] = 0.6f;
    strength.span[2] = 1.0f;
    strength.finish();
    bke::SpanAttributeWriter<float> angle = attrs.lookup_or_add_for_write_span<float>(
        bke::CURVE_PATCH_ATTR_STAMP_ANGLE, bke::AttrDomain::Point);
    angle.span[0] = 0.25f;
    angle.span[1] = -0.5f;
    angle.span[2] = 1.5f;
    angle.finish();
    bke::SpanAttributeWriter<int> texture = attrs.lookup_or_add_for_write_span<int>(
        bke::CURVE_PATCH_ATTR_STAMP_TEXTURE, bke::AttrDomain::Point);
    texture.span[0] = -1;
    texture.span[1] = 2;
    texture.span[2] = 0;
    texture.finish();
    bke::SpanAttributeWriter<int> seed = attrs.lookup_or_add_for_write_span<int>(
        bke::CURVE_PATCH_ATTR_STAMP_SEED, bke::AttrDomain::Point);
    seed.span[0] = 11;
    seed.span[1] = 22;
    seed.span[2] = 33;
    seed.finish();
    bke::SpanAttributeWriter<float> opacity = attrs.lookup_or_add_for_write_span<float>(
        bke::CURVE_PATCH_ATTR_OPACITY, bke::AttrDomain::Point);
    opacity.span[0] = 1.0f;
    opacity.span[1] = 0.35f;
    opacity.span[2] = 0.0f;
    opacity.finish();
    bke::SpanAttributeWriter<float> falloff = attrs.lookup_or_add_for_write_span<float>(
        bke::CURVE_PATCH_ATTR_OPACITY_FALLOFF, bke::AttrDomain::Point);
    falloff.span[0] = 0.5f;
    falloff.span[1] = 2.5f;
    falloff.span[2] = 1.0f;
    falloff.finish();
  }

  geom.tag_topology_changed();
  geom.tag_positions_changed();
  return geom;
}

/** A frozen parameter set with every field off its default, for the round-trip tests. */
static bke::CurvePatchParams make_curve_patch_params()
{
  bke::CurvePatchParams params;
  params.radius = 0.35f;
  params.radius_per_size = 0.01f;
  params.swap_axis = true;
  params.length_mode = bke::CurvePatchLengthMode::Repeat;
  params.length_repeat = 3;
  params.radius_interp = bke::CurvePatchRadiusInterp::Spline;
  params.end_falloff_mode = bke::CurvePatchEndFalloff::Smooth;
  params.end_falloff_percent = 15;
  params.start_point_shape = bke::CurvePatchPointShape::Round;
  params.end_point_shape = bke::CurvePatchPointShape::Triangle;
  params.stamp_mode = bke::CurvePatchStampMode::Stamps;
  params.stamp_layout = bke::CurvePatchStampLayout::Points;
  params.stamp_projection = bke::CurvePatchStampProjection::Planar;
  params.stamp_use_random_angle = true;
  params.stamp_size_random = 0.2f;
  params.stamp_strength_random = 0.3f;
  params.stamp_seed = 42;
  params.spacing_frac = 0.15f;
  params.jitter_amount = 0.05f;
  params.base_angle = 0.75f;
  params.random_angle = 1.25f;
  params.plane_normal = float3(0.2f, 0.3f, 0.9f);
  params.final_quality = true;
  return params;
}

TEST_F(PaintVectorTest, CurvePatchParamsConvertRoundTrip)
{
  const PaintVectorCurveParams dna = BKE_paint_vector_curve_params_encode(make_curve_patch_params());

  const bke::CurvePatchParams decoded = BKE_paint_vector_curve_params_decode(dna);
  EXPECT_TRUE(decoded == make_curve_patch_params());
  EXPECT_EQ(decoded.radius_interp, bke::CurvePatchRadiusInterp::Spline);

  const PaintVectorCurveParams reencoded = BKE_paint_vector_curve_params_encode(decoded);
  EXPECT_EQ(std::memcmp(&dna, &reencoded, sizeof(PaintVectorCurveParams)), 0);
}

TEST_F(PaintVectorTest, CurvePatchParamsDecodeDefaults)
{
  PaintVectorCurveParams zeroed{};
  const bke::CurvePatchParams decoded = BKE_paint_vector_curve_params_decode(zeroed);
  /* The DNA zero state decodes to the source struct's defaults (radius 0, ribbon/fill/curve
   * stamping, no falloff). */
  EXPECT_FLOAT_EQ(decoded.radius, 0.0f);
  EXPECT_FALSE(decoded.swap_axis);
  EXPECT_EQ(decoded.radius_interp, bke::CurvePatchRadiusInterp::Linear);
  EXPECT_EQ(decoded.length_mode, bke::CurvePatchLengthMode::Default);
  EXPECT_EQ(decoded.length_repeat, 0);
  EXPECT_EQ(decoded.end_falloff_mode, bke::CurvePatchEndFalloff::None);
  EXPECT_EQ(decoded.stamp_mode, bke::CurvePatchStampMode::Ribbon);
  EXPECT_EQ(decoded.stamp_layout, bke::CurvePatchStampLayout::Fill);
  EXPECT_EQ(decoded.stamp_projection, bke::CurvePatchStampProjection::Curve);
  EXPECT_EQ(decoded.stamp_seed, 0u);
  EXPECT_FLOAT_EQ(decoded.plane_normal.z, 0.0f);
  EXPECT_FALSE(decoded.final_quality);
}

/** A container holding a Shape item and a full Curve Patch item, saved and reloaded. */
TEST_F(PaintVectorTest, SaveReloadKeepsCurvePatchItems)
{
  PaintVector *pv = BKE_paint_vector_add(bmain, "PVCurvePatchSave");
  id_fake_user_set(&pv->id);

  PaintVectorItem &shape = BKE_paint_vector_item_add(*pv);
  shape.type = PAINT_VECTOR_ITEM_SHAPE;
  shape.shape_type = PAINT_SHAPE_RECT;
  BKE_paint_vector_item_style_init(shape);

  const bke::CurvesGeometry reference_curve = make_curve_patch_curve(true);
  const bke::CurvePatchParams reference_params = make_curve_patch_params();
  PaintVectorItem &curve = BKE_paint_vector_item_add(*pv);
  curve.type = PAINT_VECTOR_ITEM_CURVE_PATCH;
  curve.curve_patch_geometry.wrap() = reference_curve;
  curve.curve_patch_params = BKE_paint_vector_curve_params_encode(reference_params);
  BLI_strncpy(curve.curve_name, "Curve A", sizeof(curve.curve_name));
  curve.surface_binding.topology_hash = 0xC0FFEE11u;
  curve.surface_binding.verts_num = 42;
  curve.surface_binding.corner_tris_num = 80;
  curve.surface_binding.state = PAINT_VECTOR_BIND_BOUND;
  pv->active_item = 1;

  char filepath[FILE_MAX];
  BLI_path_join(filepath, sizeof(filepath), BKE_tempdir_session(), "paint_vector_cp_test.blend");
  BlendFileWriteParams write_params{};
  ASSERT_TRUE(BLO_write_file(bmain, filepath, 0, &write_params, nullptr));

  /* Saving must not disturb the live document: the write prepares a temporary copy, so the item
   * stays readable for the next tool entry (this used to leave a null geometry runtime and crash
   * the first Curve Patch edit after any undo push). */
  ASSERT_TRUE(BKE_paint_vector_item_curve_geometry_is_valid(curve));
  const bke::CurvesGeometry &live_geom = curve.curve_patch_geometry.wrap();
  ASSERT_EQ(live_geom.points_num(), 3);
  EXPECT_EQ(live_geom.positions()[1], float3(1.0f, 0.5f, 0.25f));

  BlendFileReadReport read_report{};
  BlendFileData *bfd = BLO_read_from_file(filepath, BLO_READ_SKIP_NONE, &read_report);
  ASSERT_NE(bfd, nullptr);

  PaintVector *loaded = nullptr;
  for (PaintVector &cur : bfd->main->paint_vectors) {
    if (STREQ(cur.id.name + 2, "PVCurvePatchSave")) {
      loaded = &cur;
      break;
    }
  }
  ASSERT_NE(loaded, nullptr);
  ASSERT_EQ(loaded->items_num, 2);

  /* The Shape item survives next to the curve, untouched. */
  EXPECT_EQ(loaded->items[0].type, PAINT_VECTOR_ITEM_SHAPE);
  EXPECT_EQ(loaded->items[0].shape_type, PAINT_SHAPE_RECT);

  PaintVectorItem &loaded_curve = loaded->items[1];
  ASSERT_EQ(loaded_curve.type, PAINT_VECTOR_ITEM_CURVE_PATCH);
  EXPECT_STREQ(loaded_curve.curve_name, "Curve A");
  EXPECT_EQ(loaded->active_item, 1);
  /* The surface binding signature is plain item data and survives the round trip. */
  EXPECT_EQ(loaded_curve.surface_binding.topology_hash, 0xC0FFEE11u);
  EXPECT_EQ(loaded_curve.surface_binding.verts_num, 42);
  EXPECT_EQ(loaded_curve.surface_binding.corner_tris_num, 80);
  EXPECT_EQ(loaded_curve.surface_binding.state, PAINT_VECTOR_BIND_BOUND);

  PaintVectorCurveParams reencoded = BKE_paint_vector_curve_params_encode(reference_params);
  EXPECT_EQ(std::memcmp(&loaded_curve.curve_patch_params,
                        &reencoded,
                        sizeof(PaintVectorCurveParams)),
            0);

  const bke::CurvesGeometry &geom = loaded_curve.curve_patch_geometry.wrap();
  ASSERT_EQ(geom.curves_num(), 1);
  ASSERT_EQ(geom.points_num(), 3);
  EXPECT_EQ(geom.curve_types()[0], CURVE_TYPE_BEZIER);
  EXPECT_TRUE(geom.cyclic()[0]);
  EXPECT_EQ(geom.resolution()[0], 12);

  const Span<float3> positions = geom.positions();
  EXPECT_EQ(positions[0], float3(0.0f, 0.0f, 0.0f));
  EXPECT_EQ(positions[1], float3(1.0f, 0.5f, 0.25f));
  const std::optional<Span<float3>> handles_left = geom.handle_positions_left();
  const std::optional<Span<float3>> handles_right = geom.handle_positions_right();
  ASSERT_TRUE(handles_left.has_value());
  ASSERT_TRUE(handles_right.has_value());
  EXPECT_EQ((*handles_left)[1], float3(0.5f, 0.5f, 0.25f));
  EXPECT_EQ((*handles_right)[2], float3(2.5f, -0.25f, 0.0f));
  EXPECT_EQ(geom.handle_types_left()[0], BEZIER_HANDLE_FREE);
  EXPECT_EQ(geom.handle_types_right()[2], BEZIER_HANDLE_VECTOR);

  const VArray<float> radii = geom.radius();
  EXPECT_FLOAT_EQ(radii[0], 0.25f);
  EXPECT_FLOAT_EQ(radii[2], 0.75f);

  const bke::AttributeAccessor attrs = geom.attributes();
  const bke::AttributeReader<float3> normals = attrs.lookup_or_default<float3>(
      bke::CURVE_PATCH_ATTR_SURFACE_NORMAL, bke::AttrDomain::Point, float3(0.0f, 0.0f, 1.0f));
  EXPECT_EQ(normals.varray[1], float3(0.1f, 0.0f, 0.9f));
  const bke::AttributeReader<float> strength = attrs.lookup_or_default<float>(
      bke::CURVE_PATCH_ATTR_STAMP_STRENGTH, bke::AttrDomain::Point, 1.0f);
  EXPECT_FLOAT_EQ(strength.varray[0], 0.8f);
  const bke::AttributeReader<float> angle = attrs.lookup_or_default<float>(
      bke::CURVE_PATCH_ATTR_STAMP_ANGLE, bke::AttrDomain::Point, 0.0f);
  EXPECT_FLOAT_EQ(angle.varray[2], 1.5f);
  const bke::AttributeReader<int> texture = attrs.lookup_or_default<int>(
      bke::CURVE_PATCH_ATTR_STAMP_TEXTURE, bke::AttrDomain::Point, -1);
  EXPECT_EQ(texture.varray[1], 2);
  const bke::AttributeReader<int> seed = attrs.lookup_or_default<int>(
      bke::CURVE_PATCH_ATTR_STAMP_SEED, bke::AttrDomain::Point, 0);
  EXPECT_EQ(seed.varray[2], 33);
  /* Per-point opacity is a regular point attribute on the embedded geometry, so the same blend
   * write/read that carries the stamp attributes has to carry it too. */
  const bke::AttributeReader<float> opacity = attrs.lookup_or_default<float>(
      bke::CURVE_PATCH_ATTR_OPACITY, bke::AttrDomain::Point, 1.0f);
  EXPECT_FLOAT_EQ(opacity.varray[1], 0.35f);
  EXPECT_FLOAT_EQ(opacity.varray[2], 0.0f);
  /* Per-point opacity falloff lives on the point domain and must survive the same round trip. */
  const bke::AttributeReader<float> falloff = attrs.lookup_or_default<float>(
      bke::CURVE_PATCH_ATTR_OPACITY_FALLOFF, bke::AttrDomain::Point, 1.0f);
  EXPECT_FLOAT_EQ(falloff.varray[1], 2.5f);

  BLO_blendfiledata_free(bfd);
  BLI_delete(filepath, false, false);
}

/** Optional attributes stay absent on read (their defaults apply lazily), and an empty geometry
 * is a valid persisted Curve Patch item. */
TEST_F(PaintVectorTest, CurvePatchDefaultsAndEmptyGeometry)
{
  PaintVector *pv = BKE_paint_vector_add(bmain, "PVCurvePatchDefaults");
  id_fake_user_set(&pv->id);

  const bke::CurvesGeometry reference_curve = make_curve_patch_curve(false);
  PaintVectorItem &bare = BKE_paint_vector_item_add(*pv);
  bare.type = PAINT_VECTOR_ITEM_CURVE_PATCH;
  bare.curve_patch_geometry.wrap() = reference_curve;
  BLI_strncpy(bare.curve_name, "Bare", sizeof(bare.curve_name));

  PaintVectorItem &empty = BKE_paint_vector_item_add(*pv);
  empty.type = PAINT_VECTOR_ITEM_CURVE_PATCH;
  BLI_strncpy(empty.curve_name, "Empty", sizeof(empty.curve_name));

  char filepath[FILE_MAX];
  BLI_path_join(filepath, sizeof(filepath), BKE_tempdir_session(), "paint_vector_cp_def.blend");
  BlendFileWriteParams write_params{};
  ASSERT_TRUE(BLO_write_file(bmain, filepath, 0, &write_params, nullptr));

  BlendFileReadReport read_report{};
  BlendFileData *bfd = BLO_read_from_file(filepath, BLO_READ_SKIP_NONE, &read_report);
  ASSERT_NE(bfd, nullptr);

  PaintVector *loaded = nullptr;
  for (PaintVector &cur : bfd->main->paint_vectors) {
    if (STREQ(cur.id.name + 2, "PVCurvePatchDefaults")) {
      loaded = &cur;
      break;
    }
  }
  ASSERT_NE(loaded, nullptr);
  ASSERT_EQ(loaded->items_num, 2);

  const bke::CurvesGeometry &geom = loaded->items[0].curve_patch_geometry.wrap();
  ASSERT_EQ(geom.points_num(), 3);
  const bke::AttributeAccessor attrs = geom.attributes();
  EXPECT_FALSE(attrs.contains(bke::CURVE_PATCH_ATTR_SURFACE_NORMAL));
  EXPECT_FALSE(attrs.contains(bke::CURVE_PATCH_ATTR_STAMP_STRENGTH));
  EXPECT_FALSE(attrs.contains(bke::CURVE_PATCH_ATTR_STAMP_ANGLE));
  EXPECT_FALSE(attrs.contains(bke::CURVE_PATCH_ATTR_STAMP_TEXTURE));
  EXPECT_FALSE(attrs.contains(bke::CURVE_PATCH_ATTR_STAMP_SEED));
  /* Zeroed DNA params decode to the defaults a fresh build would use. */
  const bke::CurvePatchParams decoded = BKE_paint_vector_curve_params_decode(
      loaded->items[0].curve_patch_params);
  EXPECT_FLOAT_EQ(decoded.radius, 0.0f);

  /* The empty item stays empty and valid. */
  const bke::CurvesGeometry &empty_geom = loaded->items[1].curve_patch_geometry.wrap();
  EXPECT_EQ(empty_geom.curves_num(), 0);
  EXPECT_TRUE(empty_geom.is_empty());

  BLO_blendfiledata_free(bfd);
  BLI_delete(filepath, false, false);
}

/** \} */

}  // namespace

}  // namespace blender
