/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <utility>

#include "BKE_gtest_base.hh"
#include "BKE_lib_id.hh"
#include "BKE_sculpt_layers.hh"

#include "DNA_mesh_types.h"

#include "GEO_mesh_primitive_cuboid.hh"

#include "sculpt_undo.hh"

#include "testing/testing.h"

namespace blender::ed::sculpt_paint::undo::tests {

class SculptUndoTest : public bke::BlenderGTestBase {
 public:
  Mesh *cube_mesh;

  void SetUp() override
  {
    cube_mesh = geometry::create_cuboid_mesh(float3(1, 1, 1), 50, 50, 50);
  }

  void TearDown() override
  {
    BKE_id_free(nullptr, cube_mesh);
  }
};

TEST_F(SculptUndoTest, CompressRoundTrip)
{
  Mesh *mesh = this->cube_mesh;

  Vector<std::byte> buffer;
  Vector<std::byte> compressed;

  {
    compression::filter_compress<float3>(mesh->vert_positions(), buffer, compressed);
    Vector<float3> decompressed;
    compression::filter_decompress<float3>(compressed, buffer, decompressed);
    EXPECT_EQ(mesh->vert_positions().size(), decompressed.size());
    EXPECT_EQ_SPAN(mesh->vert_positions(), decompressed.as_span());
  }

  {
    compression::filter_compress<int>(mesh->corner_verts(), buffer, compressed);
    Vector<int> decompressed;
    compression::filter_decompress<int>(compressed, buffer, decompressed);
    EXPECT_EQ(mesh->corner_verts().size(), decompressed.size());
    EXPECT_EQ_SPAN(mesh->corner_verts(), decompressed.as_span());
  }
}

/* A mask big enough to have a block table but small enough to build in a test; the fill value only
 * has to be recognizable. */
static SculptLayerMask *test_mask_new()
{
  return bke::sculpt_layers::mask_new(64, SCULPT_LAYER_MASK_VERT_BLOCK, 128);
}

TEST_F(SculptUndoTest, PayloadCaptureTakesTheMaskOnlyWhenTheNodeIsRemoved)
{
  /* A folder node, so the capture's layer branch is not entered and the mask is the only thing under
   * test. Built on the stack rather than in a tree: the capture reads the shared node alone. */
  SculptLayerTreeNode node;
  node.type = SCULPT_LAYER_TREE_NODE_TYPE_GROUP;
  node.uid = 7;
  node.mask = test_mask_new();
  ASSERT_NE(node.mask, nullptr);
  const SculptLayerMask *original = node.mask;

  {
    /* The node stays in the tree, so it must keep its mask; taking it here would silently strip a
     * validated layer. */
    const SculptLayerUndoPayload payload = sculpt_layer_payload_capture(
        *this->cube_mesh, node, PayloadCapture::DataOnly);
    EXPECT_EQ(payload.mask, nullptr);
    EXPECT_EQ(node.mask, original);
  }
  EXPECT_EQ(node.mask, original);

  {
    /* The node is on its way out, so the payload takes the mask ahead of the free that removal
     * performs. The payload owns it from here and releases it at the end of this scope. */
    const SculptLayerUndoPayload payload = sculpt_layer_payload_capture(
        *this->cube_mesh, node, PayloadCapture::NodeRemoved);
    EXPECT_EQ(payload.mask, original);
    EXPECT_EQ(node.mask, nullptr);
  }
}

TEST_F(SculptUndoTest, PayloadInsertHandsTheMaskBackToTheTree)
{
  Mesh &mesh = *this->cube_mesh;
  /* The insert resolves the recorded parent through the tree, so the root folder has to be there. */
  bke::sculpt_layers::root_group_ensure(mesh);

  SculptLayerMask *mask = test_mask_new();
  ASSERT_NE(mask, nullptr);

  /* A folder payload, as in the capture test above: no data buffer is involved and the mask is the
   * only thing under test. Parent and anchor are left at 0, which is the root and the head of its
   * child list. */
  SculptLayerUndoPayload payload;
  payload.type = SCULPT_LAYER_TREE_NODE_TYPE_GROUP;
  payload.uid = 11;
  payload.name = "Folder";
  payload.mask = mask;

  sculpt_layer_payload_insert(mesh, payload);

  /* The far half of the transfer #sculpt_layer_payload_capture performs: the tree owns the mask
   * again, and the payload owns nothing. Were the hand-back missed, a masked folder deleted and then
   * restored by an undo would come back silently unmasked, and the payload's destructor would free
   * the weights out from under nothing. */
  const SculptLayerTreeNode *node = bke::sculpt_layers::node_find_by_uid(mesh, 11);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->mask, mask);
  EXPECT_EQ(payload.mask, nullptr);
}

TEST_F(SculptUndoTest, PayloadMoveTransfersMaskOwnership)
{
  SculptLayerMask *mask = test_mask_new();
  ASSERT_NE(mask, nullptr);

  SculptLayerUndoPayload src;
  src.type = SCULPT_LAYER_TREE_NODE_TYPE_GROUP;
  src.color_tag = SCULPT_LAYER_COLOR_02;
  src.mask = mask;

  SculptLayerUndoPayload moved(std::move(src));
  EXPECT_EQ(moved.mask, mask);
  /* Nulled, or the two payloads would both free it. */
  EXPECT_EQ(src.mask, nullptr);
  /* Carried by the move like every other field. A folder that came back from an undo without its
   * color tag is the same class of silent loss the mask transfer exists to prevent. */
  EXPECT_EQ(moved.color_tag, SCULPT_LAYER_COLOR_02);

  SculptLayerUndoPayload assigned;
  assigned = std::move(moved);
  EXPECT_EQ(assigned.mask, mask);
  EXPECT_EQ(moved.mask, nullptr);
  EXPECT_EQ(assigned.color_tag, SCULPT_LAYER_COLOR_02);
}

TEST_F(SculptUndoTest, MaskSessionBoundaryFollowsTheRestoreDirection)
{
  /* Undo lands before the step, redo after it. */
  EXPECT_TRUE(mask_session_boundary(3, true, false).want_open);
  EXPECT_FALSE(mask_session_boundary(3, true, true).want_open);
  EXPECT_TRUE(mask_session_boundary(3, false, true).want_open);
  EXPECT_FALSE(mask_session_boundary(3, false, false).want_open);

  EXPECT_EQ(mask_session_boundary(3, true, false).node_uid, 3);

  /* A step that records no session change never asks for one to be opened, in either direction. */
  EXPECT_EQ(mask_session_boundary(0, true, false).node_uid, 0);
  EXPECT_FALSE(mask_session_boundary(0, true, false).want_open);
  EXPECT_FALSE(mask_session_boundary(0, false, true).want_open);
}

}  // namespace blender::ed::sculpt_paint::undo::tests
