/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_math_vector_types.hh"

#include "BKE_attribute.h"
#include "BKE_attribute.hh"
#include "BKE_gtest_base.hh"
#include "BKE_lib_id.hh"

#include "DNA_mesh_types.h"

#include "GEO_mesh_primitive_cuboid.hh"

#include "sculpt_color.hh"

#include "testing/testing.h"

namespace blender::ed::sculpt_paint::color::tests {

static void add_color_attribute(Mesh &mesh,
                                 const StringRef name,
                                 const bke::AttrDomain domain,
                                 const bke::AttrType type,
                                 const bool set_active)
{
  ASSERT_TRUE(
      mesh.attributes_for_write().add(name, domain, type, bke::AttributeInitDefaultValue()));
  if (set_active) {
    BKE_id_attributes_active_color_set(&mesh.id, name);
  }
}

class EnsureSharedColorAttributesTest : public bke::BlenderGTestBase {
 public:
  Mesh *mesh_a;
  Mesh *mesh_b;

  void SetUp() override
  {
    mesh_a = geometry::create_cuboid_mesh(float3(1, 1, 1), 2, 2, 2);
    mesh_b = geometry::create_cuboid_mesh(float3(1, 1, 1), 2, 2, 2);
  }

  void TearDown() override
  {
    BKE_id_free(nullptr, mesh_a);
    BKE_id_free(nullptr, mesh_b);
  }
};

TEST_F(EnsureSharedColorAttributesTest, CreatesMissingChannel)
{
  add_color_attribute(
      *mesh_a, "PaintColor", bke::AttrDomain::Point, bke::AttrType::ColorFloat, true);

  Mesh *others[1] = {mesh_b};
  ensure_shared_color_attributes(*mesh_a, Span(others));

  const std::optional<bke::AttributeMetaData> meta = mesh_b->attributes().lookup_meta_data(
      "PaintColor");
  ASSERT_TRUE(meta.has_value());
  EXPECT_EQ(meta->domain, bke::AttrDomain::Point);
  EXPECT_EQ(meta->data_type, bke::AttrType::ColorFloat);
  EXPECT_STREQ(mesh_b->active_color_attribute, "PaintColor");
  EXPECT_STREQ(mesh_b->default_color_attribute, "PaintColor");
}

TEST_F(EnsureSharedColorAttributesTest, ActivatesExistingChannelWithoutConversion)
{
  add_color_attribute(
      *mesh_a, "PaintColor", bke::AttrDomain::Point, bke::AttrType::ColorFloat, true);
  /* mesh_b already has the shared name, but Corner/Byte, and a DIFFERENT active channel. */
  add_color_attribute(
      *mesh_b, "PaintColor", bke::AttrDomain::Corner, bke::AttrType::ColorByte, false);
  add_color_attribute(
      *mesh_b, "SkinTone", bke::AttrDomain::Point, bke::AttrType::ColorFloat, true);

  Mesh *others[1] = {mesh_b};
  ensure_shared_color_attributes(*mesh_a, Span(others));

  /* Active switched to the shared name; existing structure untouched ("paint as is"). */
  EXPECT_STREQ(mesh_b->active_color_attribute, "PaintColor");
  const std::optional<bke::AttributeMetaData> meta = mesh_b->attributes().lookup_meta_data(
      "PaintColor");
  ASSERT_TRUE(meta.has_value());
  EXPECT_EQ(meta->domain, bke::AttrDomain::Corner);
  EXPECT_EQ(meta->data_type, bke::AttrType::ColorByte);
}

TEST_F(EnsureSharedColorAttributesTest, SkipsNameCollisionWithNonColorAttribute)
{
  add_color_attribute(
      *mesh_a, "PaintColor", bke::AttrDomain::Point, bke::AttrType::ColorFloat, true);
  /* Occupy the name on mesh_b with a non-color attribute. */
  ASSERT_TRUE(mesh_b->attributes_for_write().add(
      "PaintColor", bke::AttrDomain::Point, bke::AttrType::Float, bke::AttributeInitDefaultValue()));

  Mesh *others[1] = {mesh_b};
  ensure_shared_color_attributes(*mesh_a, Span(others));

  /* Untouched: no conversion, no rename, active color not set to the colliding name. */
  const std::optional<bke::AttributeMetaData> meta = mesh_b->attributes().lookup_meta_data(
      "PaintColor");
  ASSERT_TRUE(meta.has_value());
  EXPECT_EQ(meta->data_type, bke::AttrType::Float);
  EXPECT_TRUE(mesh_b->active_color_attribute == nullptr ||
              StringRef(mesh_b->active_color_attribute) != "PaintColor");
}

TEST_F(EnsureSharedColorAttributesTest, NoActiveChannelOnReferenceIsNoOp)
{
  Mesh *others[1] = {mesh_b};
  ensure_shared_color_attributes(*mesh_a, Span(others));
  EXPECT_FALSE(mesh_b->attributes().lookup_meta_data("PaintColor").has_value());
  EXPECT_EQ(mesh_b->active_color_attribute, nullptr);
}

}  // namespace blender::ed::sculpt_paint::color::tests
