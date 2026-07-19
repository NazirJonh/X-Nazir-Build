/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cmath>

#include "testing/testing.h"

#include "BKE_ccg.hh"
#include "BKE_sculpt_layers.hh"

#include "BLI_array.hh"

#include "DNA_mesh_types.h"

namespace blender::bke::sculpt_layers::tests {

TEST(sculpt_layers_mask, new_is_uniform_everywhere)
{
  /* A freshly filled mask must cost nothing beyond its index tables: a uniform fill has no dense
   * block at all, which is the property the sparse layout exists for. */
  SculptLayerMask *mask = mask_new(10000, SCULPT_LAYER_MASK_VERT_BLOCK, 255);
  EXPECT_EQ(mask->totelem, 10000);
  EXPECT_EQ(mask->blocks_num, 3);
  EXPECT_EQ(mask->data_num, 0);
  for (const int block : IndexRange(mask->blocks_num)) {
    EXPECT_EQ(mask->block_kind[block], SCULPT_LAYER_MASK_BLOCK_UNIFORM);
    EXPECT_EQ(mask->block_value[block], 255);
    EXPECT_EQ(mask->block_offset[block], -1);
  }
  EXPECT_EQ(mask_value_at(*mask, 0), 255);
  EXPECT_EQ(mask_value_at(*mask, 9999), 255);
  mask_free(mask);
}

TEST(sculpt_layers_mask, blocks_num_rounds_up)
{
  EXPECT_EQ(mask_blocks_num(0, 4096), 0);
  EXPECT_EQ(mask_blocks_num(1, 4096), 1);
  EXPECT_EQ(mask_blocks_num(4096, 4096), 1);
  EXPECT_EQ(mask_blocks_num(4097, 4096), 2);
}

TEST(sculpt_layers_mask, copy_is_deep)
{
  /* #tree_copy shallow-copies the DNA struct, so a shared mask pointer would be freed twice and
   * would let one mesh's edits show up on another. This is the guard for that. */
  SculptLayerMask *src = mask_new(8192, SCULPT_LAYER_MASK_VERT_BLOCK, 128);
  SculptLayerMask *dst = mask_copy(*src);
  EXPECT_NE(dst, src);
  EXPECT_NE(dst->block_kind, src->block_kind);
  EXPECT_NE(dst->block_value, src->block_value);
  EXPECT_NE(dst->block_offset, src->block_offset);
  EXPECT_EQ(dst->totelem, src->totelem);
  EXPECT_EQ(mask_value_at(*dst, 5000), 128);
  mask_free(src);
  EXPECT_EQ(mask_value_at(*dst, 5000), 128);
  mask_free(dst);
}

/**
 * The three block tables #mask_copy allocates, per block. Spelled from the element types rather than
 * as `6`, since that is what the accounting has to track if a table's type ever changes.
 */
static constexpr int64_t mask_per_block_bytes = int64_t(sizeof(int8_t) + sizeof(uint8_t) +
                                                       sizeof(int));

TEST(sculpt_layers_mask, size_in_bytes_counts_what_a_copy_allocates)
{
  /* The undo system charges a captured mask against its memory budget with this (sculpt_undo.cc:1730,
   * 1905, 1914), and what an undo step actually holds is a #mask_copy. So this has to count the copy's
   * allocations — the struct, the three per-block tables, and the dense payload — and not, say,
   * #totelem, which for a uniform mask is arbitrarily large while the copy costs nothing per element.
   * Undercounting here does not misreport a number in a panel: it lets the undo stack grow past the
   * user's memory limit before the eviction pass notices.
   *
   * A uniform mask is the case where the two spellings diverge most: 10000 elements, zero bytes of
   * payload. */
  SculptLayerMask *uniform = mask_new(10000, SCULPT_LAYER_MASK_VERT_BLOCK, 255);
  ASSERT_EQ(uniform->blocks_num, 3);
  ASSERT_EQ(uniform->data_num, 0);
  EXPECT_EQ(mask_size_in_bytes(*uniform),
            int64_t(sizeof(SculptLayerMask)) + 3 * mask_per_block_bytes);
  mask_free(uniform);

  /* One dense block and one uniform: the payload term appears, and it is sized by #data_num rather
   * than by #totelem — the uniform block stores nothing. */
  Array<float> dense(8192, 1.0f);
  dense[10] = 0.0f;
  SculptLayerMask *mixed = mask_compress(dense, SCULPT_LAYER_MASK_VERT_BLOCK);
  ASSERT_NE(mixed, nullptr);
  ASSERT_EQ(mixed->blocks_num, 2);
  ASSERT_EQ(mixed->data_num, SCULPT_LAYER_MASK_VERT_BLOCK);
  EXPECT_EQ(mask_size_in_bytes(*mixed),
            int64_t(sizeof(SculptLayerMask)) + 2 * mask_per_block_bytes +
                int64_t(SCULPT_LAYER_MASK_VERT_BLOCK));
  mask_free(mixed);
}

TEST(sculpt_layers_mask, size_in_bytes_tracks_a_block_going_dense)
{
  /* The accounting has to move when the storage does. Two masks over the same domain, differing only
   * in that one block is dense in the second: the difference must be exactly that block's payload,
   * which is the property an implementation that read #totelem (identical in both) would lose. */
  Array<float> all_uniform(8192, 1.0f);
  SculptLayerMask *before = mask_compress(all_uniform, SCULPT_LAYER_MASK_VERT_BLOCK);
  ASSERT_NE(before, nullptr);
  ASSERT_EQ(before->data_num, 0) << "a fully uniform mask is what this compares against";

  Array<float> one_dense(8192, 1.0f);
  one_dense[10] = 0.0f;
  SculptLayerMask *after = mask_compress(one_dense, SCULPT_LAYER_MASK_VERT_BLOCK);
  ASSERT_NE(after, nullptr);
  ASSERT_EQ(after->blocks_num, before->blocks_num) << "the domain must be the only thing unchanged";
  ASSERT_EQ(after->totelem, before->totelem);

  EXPECT_EQ(mask_size_in_bytes(*after) - mask_size_in_bytes(*before),
            int64_t(SCULPT_LAYER_MASK_VERT_BLOCK));

  mask_free(before);
  mask_free(after);
}

TEST(sculpt_layers_mask, size_in_bytes_of_a_neutralized_mask_is_the_struct_alone)
{
  /* A mask neutralized by #mask_blend_read describes no blocks and leaves its three tables null, and
   * #mask_copy carries those nulls over verbatim rather than allocating zero-length arrays. The
   * per-block and payload terms therefore both vanish, and the answer is the bare struct — no
   * multiplication by a null table's length, which is the shape that would otherwise have needed its
   * own branch. */
  SculptLayerMask *mask = mask_new(10000, SCULPT_LAYER_MASK_VERT_BLOCK, 255);
  ASSERT_NE(mask, nullptr);
  /* Only the counts are neutralized here; the arrays stay allocated so #mask_free still owns them,
   * which is the same treatment `sculpt_layers_tree.composite_ignores_a_mask_describing_no_blocks`
   * gives it. The accounting must follow the counts, since those are what #mask_copy reads. */
  const int blocks_num = mask->blocks_num;
  mask->blocks_num = 0;
  mask->data_num = 0;

  EXPECT_EQ(mask_size_in_bytes(*mask), int64_t(sizeof(SculptLayerMask)));

  mask->blocks_num = blocks_num;
  mask_free(mask);
}

TEST(sculpt_layers_mask, expand_compress_round_trip)
{
  /* The round trip is what the edit session does on enter and exit, so drift here would corrupt a
   * user's mask a little on every session. */
  Array<float> dense(8192, 1.0f);
  for (const int i : IndexRange(100, 50)) {
    dense[i] = 0.0f;
  }
  SculptLayerMask *mask = mask_compress(dense, SCULPT_LAYER_MASK_VERT_BLOCK);
  Array<float> back(8192);
  mask_expand(*mask, back);
  for (const int i : IndexRange(8192)) {
    EXPECT_NEAR(back[i], dense[i], 1.0f / 255.0f) << "element " << i;
  }
  mask_free(mask);
}

TEST(sculpt_layers_mask, repeated_session_round_trips_are_idempotent)
{
  /* Opening and closing an edit session without painting is expand followed by compress, and a user
   * may do that any number of times. Every trip must reproduce the previous mask *exactly* — not
   * merely within a quantization step — or the mask would fade and its storage would creep towards
   * dense as uniform blocks stopped agreeing with themselves. #mask_expand emits `value / 255` and
   * #mask_compress rounds `value * 255 + 0.5` back, so the pair is exact for anything that already
   * came out of a compress; this pins that.
   *
   * The buffer deliberately mixes a dense block (partially painted) with uniform ones, since it is
   * the dense block's storage that would grow. */
  Array<float> painted(12288, 1.0f);
  for (const int i : IndexRange(100, 50)) {
    painted[i] = 0.0f;
  }
  painted[200] = 0.5f;
  painted[201] = 1.0f / 3.0f;
  for (const int i : IndexRange(4096, 4096)) {
    painted[i] = 0.0f;
  }

  SculptLayerMask *mask = mask_compress(painted, SCULPT_LAYER_MASK_VERT_BLOCK);
  ASSERT_NE(mask, nullptr);
  ASSERT_EQ(mask->block_kind[0], SCULPT_LAYER_MASK_BLOCK_DENSE)
      << "a dense block is what this test is about";
  const int data_num_after_first = mask->data_num;

  Array<uint8_t> reference(mask->totelem);
  for (const int i : IndexRange(mask->totelem)) {
    reference[i] = mask_value_at(*mask, i);
  }

  for (const int session : IndexRange(5)) {
    Array<float> dense(mask->totelem);
    mask_expand(*mask, dense);
    SculptLayerMask *next = mask_compress(dense, SCULPT_LAYER_MASK_VERT_BLOCK);
    mask_free(mask);
    mask = next;
    ASSERT_NE(mask, nullptr);
    EXPECT_EQ(mask->data_num, data_num_after_first) << "storage grew on session " << session;
    for (const int i : IndexRange(mask->totelem)) {
      ASSERT_EQ(mask_value_at(*mask, i), reference[i])
          << "session " << session << ", element " << i;
    }
  }
  mask_free(mask);
}

TEST(sculpt_layers_mask, compress_collapses_uniform_blocks)
{
  /* Block 0 is mixed, block 1 is all ones. Only block 0 may take space — without this collapse the
   * sparse layout degrades to dense after a few edit sessions. */
  Array<float> dense(8192, 1.0f);
  dense[10] = 0.0f;
  SculptLayerMask *mask = mask_compress(dense, SCULPT_LAYER_MASK_VERT_BLOCK);
  EXPECT_EQ(mask->blocks_num, 2);
  EXPECT_EQ(mask->block_kind[0], SCULPT_LAYER_MASK_BLOCK_DENSE);
  EXPECT_EQ(mask->block_kind[1], SCULPT_LAYER_MASK_BLOCK_UNIFORM);
  EXPECT_EQ(mask->block_value[1], 255);
  EXPECT_EQ(mask->data_num, SCULPT_LAYER_MASK_VERT_BLOCK);
  mask_free(mask);
}

TEST(sculpt_layers_mask, compress_all_uniform_stores_nothing)
{
  Array<float> dense(8192, 0.0f);
  SculptLayerMask *mask = mask_compress(dense, SCULPT_LAYER_MASK_VERT_BLOCK);
  EXPECT_EQ(mask->data_num, 0);
  EXPECT_EQ(mask->block_value[0], 0);
  mask_free(mask);
}

TEST(sculpt_layers_mask, tail_block_is_partial)
{
  /* The last block covers fewer than `block_size` elements. Reading or writing past `totelem`
   * inside it is the easiest way to corrupt the heap here. */
  Array<float> dense(5000, 1.0f);
  dense[4999] = 0.0f;
  SculptLayerMask *mask = mask_compress(dense, SCULPT_LAYER_MASK_VERT_BLOCK);
  EXPECT_EQ(mask->blocks_num, 2);
  EXPECT_EQ(mask->block_kind[1], SCULPT_LAYER_MASK_BLOCK_DENSE);
  EXPECT_EQ(mask->data_num, 5000 - SCULPT_LAYER_MASK_VERT_BLOCK);
  Array<float> back(5000);
  mask_expand(*mask, back);
  EXPECT_NEAR(back[4999], 0.0f, 1.0f / 255.0f);
  EXPECT_NEAR(back[4998], 1.0f, 1.0f / 255.0f);
  mask_free(mask);
}

TEST(sculpt_layers_mask, multiply_of_uniform_blocks_stays_uniform)
{
  /* The composite's fast path depends on this: two uniform blocks must fold to a scalar, never to a
   * dense block, or a folder mask would push every layer under it onto the slow path. */
  SculptLayerMask *a = mask_new(8192, SCULPT_LAYER_MASK_VERT_BLOCK, 255);
  SculptLayerMask *b = mask_new(8192, SCULPT_LAYER_MASK_VERT_BLOCK, 128);
  SculptLayerMask *product = mask_multiply(*a, *b);
  EXPECT_EQ(product->data_num, 0);
  EXPECT_EQ(product->block_kind[0], SCULPT_LAYER_MASK_BLOCK_UNIFORM);
  EXPECT_EQ(product->block_value[0], 128);
  mask_free(a);
  mask_free(b);
  mask_free(product);
}

TEST(sculpt_layers_mask, multiply_by_uniform_255_is_identity)
{
  /* A fully opaque folder must leave its subtree's mask untouched, element for element. Truncating
   * instead of rounding in #mask_multiply loses a step off every value here, so a folder nobody
   * masked would quietly darken everything below it. The dense side is what pins the per-element
   * path: the uniform-by-uniform shortcut never reaches it. */
  SculptLayerMask *a = mask_new(8192, SCULPT_LAYER_MASK_VERT_BLOCK, 255);
  Array<float> dense(8192, 1.0f);
  dense[5] = 0.5f;
  SculptLayerMask *b = mask_compress(dense, SCULPT_LAYER_MASK_VERT_BLOCK);
  SculptLayerMask *product = mask_multiply(*a, *b);
  EXPECT_EQ(mask_value_at(*product, 5), mask_value_at(*b, 5));
  EXPECT_EQ(mask_value_at(*product, 6), 255);
  mask_free(a);
  mask_free(b);
  mask_free(product);
}

TEST(sculpt_layers_mask, multiply_by_uniform_zero_annihilates_dense_block)
{
  /* Masking a folder to zero is how a whole subtree gets parked, so the annihilation must survive
   * meeting a *dense* block: without the zero shortcut the product would be stored element by
   * element, and the composite would take its slow path over a region that cannot contribute
   * anything at all. */
  SculptLayerMask *a = mask_new(8192, SCULPT_LAYER_MASK_VERT_BLOCK, 0);
  Array<float> dense(8192, 1.0f);
  dense[5] = 0.5f;
  SculptLayerMask *b = mask_compress(dense, SCULPT_LAYER_MASK_VERT_BLOCK);
  ASSERT_EQ(b->block_kind[0], SCULPT_LAYER_MASK_BLOCK_DENSE)
      << "the dense operand is the whole point of this case";

  SculptLayerMask *product = mask_multiply(*a, *b);
  ASSERT_NE(product, nullptr);
  EXPECT_EQ(product->block_kind[0], SCULPT_LAYER_MASK_BLOCK_UNIFORM);
  EXPECT_EQ(product->block_value[0], 0);
  EXPECT_EQ(product->data_num, 0) << "an annihilated block must cost no dense storage";
  EXPECT_EQ(mask_value_at(*product, 5), 0);

  mask_free(a);
  mask_free(b);
  mask_free(product);
}

TEST(sculpt_layers_mask, multiply_of_mismatched_domains_is_null)
{
  /* Nothing in the tree guarantees that a folder's mask and its parent's were allocated for the
   * same element count, and a product built from one side's block table would index past the end of
   * the other. Null is the module's existing "no mask" state, which every consumer already
   * handles. */
  SculptLayerMask *a = mask_new(8192, SCULPT_LAYER_MASK_VERT_BLOCK, 255);
  SculptLayerMask *b = mask_new(4096, SCULPT_LAYER_MASK_VERT_BLOCK, 128);
  EXPECT_EQ(mask_multiply(*a, *b), nullptr);
  mask_free(a);
  mask_free(b);
}

/* -------------------------------------------------------------------- */
/** \name Weight fold (#mask_block_weight / #mask_elem_weight)
 *
 * The fold is the single spelling of a masked layer's weight, shared by the vertex composite and by
 * every direction of the multires grid composite. These tests pin the fold's *arithmetic* against
 * literals, which no consumer can be refactored out from under. That a given composite actually
 * routes through it is a separate question, answered for the vertex domain by
 * `sculpt_layers_tree.composite_weights_match_the_shared_mask_authority`, which drives the real
 * public composite; the grid domain cannot be checked without a live #SubdivCCG.
 * \{ */

/** Build a mask over one block whose bytes are `values`. */
static SculptLayerMask *mask_from_bytes(const Span<uint8_t> values)
{
  Array<float> dense(values.size());
  for (const int64_t i : values.index_range()) {
    dense[i] = float(values[i]) * (1.0f / 255.0f);
  }
  return mask_compress(dense, int(values.size()));
}

TEST(sculpt_layers_mask, weight_unmasked_is_the_bare_scalar)
{
  const CompositeMask masks;
  const MaskBlockWeight w = mask_block_weight(masks, 0, 0.75f);
  EXPECT_FALSE(w.skip);
  EXPECT_EQ(w.fold, MaskFold::Uniform);
  EXPECT_FLOAT_EQ(mask_elem_weight(w, 0), 0.75f);
  EXPECT_FLOAT_EQ(mask_elem_weight(w, 123), 0.75f);
}

TEST(sculpt_layers_mask, weight_uniform_block_folds_into_the_scalar)
{
  SculptLayerMask *mask = mask_new(4096, SCULPT_LAYER_MASK_VERT_BLOCK, 128);
  const CompositeMask masks{mask, nullptr};
  const MaskBlockWeight w = mask_block_weight(masks, 0, 2.0f);
  EXPECT_FALSE(w.skip);
  EXPECT_EQ(w.fold, MaskFold::Uniform);
  /* A uniform block must cost exactly what an unmasked one does in the element loop. */
  EXPECT_FLOAT_EQ(mask_elem_weight(w, 0), 2.0f * 128.0f * (1.0f / 255.0f));
  mask_free(mask);
}

TEST(sculpt_layers_mask, weight_uniform_zero_block_is_skipped)
{
  /* Skipped rather than multiplied by zero for every element: masking a folder to zero is a common
   * way to park a whole subtree. */
  SculptLayerMask *mask = mask_new(4096, SCULPT_LAYER_MASK_VERT_BLOCK, 0);
  const CompositeMask masks{mask, nullptr};
  EXPECT_TRUE(mask_block_weight(masks, 0, 1.0f).skip);
  mask_free(mask);
}

TEST(sculpt_layers_mask, weight_dense_block_reads_per_element)
{
  const Array<uint8_t> bytes = {0, 64, 128, 255};
  SculptLayerMask *mask = mask_from_bytes(bytes);
  ASSERT_NE(mask, nullptr);
  ASSERT_EQ(mask->block_kind[0], SCULPT_LAYER_MASK_BLOCK_DENSE);
  const CompositeMask masks{mask, nullptr};
  const MaskBlockWeight w = mask_block_weight(masks, 0, 0.5f);
  EXPECT_FALSE(w.skip);
  EXPECT_EQ(w.fold, MaskFold::Single);
  for (const int i : bytes.index_range()) {
    EXPECT_FLOAT_EQ(mask_elem_weight(w, i), 0.5f * float(bytes[i]) * (1.0f / 255.0f))
        << "element " << i;
  }
  mask_free(mask);
}

TEST(sculpt_layers_mask, weight_pair_normalizes_in_one_step)
{
  /* `a * b / 255^2` in one step, not `(a/255) * (b/255)`: 1/255 is not exact in binary floating
   * point, so folding a fully opaque (255) folder in as a separate factor would round twice and
   * land further from the true value than folding it in once. The one-step spelling is pinned here
   * against its own literal, so reintroducing the two-step scaling fails this test.
   *
   * The two forms are deliberately *not* asserted bit for bit equal: `1/65025` and `(1/255)^2` are
   * a single ulp apart as floats, so an opaque secondary moves the primary's weight by up to an ulp
   * (it does here, on elements 1 and 2). That is harmless, because the defence against a drifting
   * base is that forward and inverse are the *same* expression with a negated weight — see
   * #weight_negation_is_exact — and never that two different expressions agree. What does matter is
   * that the pair form is the more accurate of the two, which the last assertion holds down. */
  const Array<uint8_t> bytes = {17, 96, 200, 255};
  SculptLayerMask *primary = mask_from_bytes(bytes);
  SculptLayerMask *opaque = mask_new(int(bytes.size()), int(bytes.size()), 255);
  ASSERT_NE(primary, nullptr);
  ASSERT_NE(opaque, nullptr);

  const MaskBlockWeight alone = mask_block_weight(CompositeMask{primary, nullptr}, 0, 0.25f);
  const MaskBlockWeight paired = mask_block_weight(CompositeMask{primary, opaque}, 0, 0.25f);
  ASSERT_EQ(paired.fold, MaskFold::Pair);
  for (const int i : bytes.index_range()) {
    const float one_step = 0.25f * float(int(bytes[i]) * 255) * (1.0f / (255.0f * 255.0f));
    EXPECT_EQ(mask_elem_weight(paired, i), one_step) << "element " << i;
    EXPECT_FLOAT_EQ(mask_elem_weight(paired, i), mask_elem_weight(alone, i)) << "element " << i;
    /* Against a reference computed in double: normalizing once is never worse than the single
     * mask's own rounding, whereas scaling each side to 0..1 in turn is strictly worse here. */
    const double exact = 0.25 * double(bytes[i]) / 255.0;
    EXPECT_LE(std::abs(double(mask_elem_weight(paired, i)) - exact),
              std::abs(double(mask_elem_weight(alone, i)) - exact))
        << "element " << i;
  }
  mask_free(primary);
  mask_free(opaque);
}

TEST(sculpt_layers_mask, weight_pair_with_uniform_zero_side_is_skipped)
{
  /* A uniform zero on either side annihilates the block even when the other side is dense. */
  const Array<uint8_t> bytes = {10, 20, 30, 40};
  SculptLayerMask *dense = mask_from_bytes(bytes);
  SculptLayerMask *zero = mask_new(int(bytes.size()), int(bytes.size()), 0);
  EXPECT_TRUE(mask_block_weight(CompositeMask{dense, zero}, 0, 1.0f).skip);
  EXPECT_TRUE(mask_block_weight(CompositeMask{zero, dense}, 0, 1.0f).skip);
  mask_free(dense);
  mask_free(zero);
}

TEST(sculpt_layers_mask, weight_negation_is_exact)
{
  /* The inverse direction is the forward fold with a negated weight, and every multires direction
   * relies on the two cancelling bit for bit — that is the whole defence against a base that drifts
   * a little on each flush. Negation is exact in binary floating point; this pins that the fold
   * does not launder it through anything that is not. */
  const Array<uint8_t> bytes = {3, 77, 191, 255};
  SculptLayerMask *primary = mask_from_bytes(bytes);
  SculptLayerMask *secondary = mask_new(int(bytes.size()), int(bytes.size()), 137);
  const CompositeMask masks{primary, secondary};
  const MaskBlockWeight fwd = mask_block_weight(masks, 0, 0.8f);
  const MaskBlockWeight inv = mask_block_weight(masks, 0, -0.8f);
  for (const int i : bytes.index_range()) {
    EXPECT_EQ(mask_elem_weight(fwd, i), -mask_elem_weight(inv, i)) << "element " << i;
  }
  mask_free(primary);
  mask_free(secondary);
}

TEST(sculpt_layers_mask, weight_expressions_are_pinned)
{
  /* The fold's arithmetic, written out against literals rather than against another expression of
   * itself. Both composites now call #mask_block_weight, so a reworded fold would move every caller
   * together and stay invisible to any test that only compares one caller to another; this is where
   * the numbers themselves are held down.
   *
   * Every branch is covered: uniform single, dense single, uniform pair, the mixed pair where one
   * side is uniform and the other dense, and the dense pair. */
  const Array<uint8_t> dense_bytes = {0, 5, 64, 128, 200, 254, 255, 41};
  const int block_size = int(dense_bytes.size());
  const float weight = 0.6f;

  SculptLayerMask *dense = mask_from_bytes(dense_bytes);
  SculptLayerMask *uniform = mask_new(block_size, block_size, 173);
  const Array<uint8_t> other_bytes = {255, 200, 128, 64, 5, 0, 90, 33};
  SculptLayerMask *dense_b = mask_from_bytes(other_bytes);
  ASSERT_NE(dense, nullptr);
  ASSERT_NE(uniform, nullptr);
  ASSERT_NE(dense_b, nullptr);

  /* Single mask, uniform block. */
  {
    const MaskBlockWeight w = mask_block_weight(CompositeMask{uniform, nullptr}, 0, weight);
    const float expected = weight * float(173) * (1.0f / 255.0f);
    for (const int i : IndexRange(block_size)) {
      EXPECT_EQ(mask_elem_weight(w, i), expected) << "uniform single, element " << i;
    }
  }
  /* Single mask, dense block. */
  {
    const MaskBlockWeight w = mask_block_weight(CompositeMask{dense, nullptr}, 0, weight);
    for (const int i : IndexRange(block_size)) {
      const float expected = weight * float(dense_bytes[i]) * (1.0f / 255.0f);
      EXPECT_EQ(mask_elem_weight(w, i), expected) << "dense single, element " << i;
    }
  }
  /* Two masks, both uniform. */
  {
    SculptLayerMask *uniform_b = mask_new(block_size, block_size, 91);
    const MaskBlockWeight w = mask_block_weight(CompositeMask{uniform, uniform_b}, 0, weight);
    const float expected = weight * float(int(173) * int(91)) * (1.0f / (255.0f * 255.0f));
    for (const int i : IndexRange(block_size)) {
      EXPECT_EQ(mask_elem_weight(w, i), expected) << "uniform pair, element " << i;
    }
    mask_free(uniform_b);
  }
  /* Two masks, one uniform and one dense — the mixed branch. */
  {
    const MaskBlockWeight w = mask_block_weight(CompositeMask{dense, uniform}, 0, weight);
    for (const int i : IndexRange(block_size)) {
      const float expected = weight * float(int(dense_bytes[i]) * int(173)) *
                             (1.0f / (255.0f * 255.0f));
      EXPECT_EQ(mask_elem_weight(w, i), expected) << "mixed pair, element " << i;
    }
  }
  /* Two masks, both dense. */
  {
    const MaskBlockWeight w = mask_block_weight(CompositeMask{dense, dense_b}, 0, weight);
    for (const int i : IndexRange(block_size)) {
      const float expected = weight * float(int(dense_bytes[i]) * int(other_bytes[i])) *
                             (1.0f / (255.0f * 255.0f));
      EXPECT_EQ(mask_elem_weight(w, i), expected) << "dense pair, element " << i;
    }
  }

  mask_free(dense);
  mask_free(uniform);
  mask_free(dense_b);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Grid domain block layout
 * \{ */

TEST(sculpt_layers_mask, grid_mask_cuts_one_block_per_grid)
{
  /* The multires paths use a *grid* index as a block index, which is only correct when a grid mask
   * is cut one block per grid. That correspondence is what the edit session establishes by passing
   * `grid_area` as the block size, and it is derived from the CCG rather than from a constant:
   * `grid_area == CCG_grid_size(level)^2`, and #SubdivCCG stores grids back to back in chunks of
   * that size. */
  const int grid_size = CCG_grid_size(3);
  const int grid_area = grid_size * grid_size;
  const int grids_num = 7;
  const int totelem = grids_num * grid_area;

  /* A distinct value per grid: a block that straddled two grids could not compress to a uniform
   * block at all, so the uniformity assertions below are what pin the alignment. */
  Array<float> dense(totelem);
  for (const int grid : IndexRange(grids_num)) {
    for (const int i : IndexRange(grid_area)) {
      dense[grid * grid_area + i] = float(grid) / float(grids_num);
    }
  }

  SculptLayerMask *mask = mask_compress(dense, grid_area);
  EXPECT_EQ(mask->block_size, grid_area);
  EXPECT_EQ(mask->blocks_num, grids_num);
  for (const int grid : IndexRange(grids_num)) {
    const MaskBlock block = mask_block(*mask, grid);
    EXPECT_TRUE(block.uniform) << "grid " << grid;
    EXPECT_EQ(block.value, mask_value_at(*mask, grid * grid_area)) << "grid " << grid;
    /* The last element of the grid must belong to the same block as the first. An off-by-one in the
     * block size shows up here first, and on a real mesh it shows up as a seam between grids. */
    EXPECT_EQ(mask_value_at(*mask, grid * grid_area + grid_area - 1), block.value)
        << "grid " << grid;
  }
  mask_free(mask);
}

TEST(sculpt_layers_mask, grid_composite_requires_the_grid_area_block_size)
{
  /* #grid_masks_for_composite fails *open*: a grid mask cut at the wrong block size is not rejected
   * loudly, the layer simply contributes fully. A producer that reached for
   * #SCULPT_LAYER_MASK_VERT_BLOCK would therefore author weights that are silently ignored, which
   * is why the edit session's grid path must never mention that constant. */
  const int grid_size = CCG_grid_size(3);
  const int grid_area = grid_size * grid_size;
  const int grids_num = 400;
  const int totelem = grids_num * grid_area;

  SculptLayer layer;
  layer.domain = SCULPT_LAYER_DOMAIN_GRID;
  layer.totelem = totelem;

  layer.base.mask = mask_new(totelem, grid_area, 128);
  EXPECT_EQ(grid_masks_for_composite(layer, totelem, grid_area).primary, layer.base.mask);
  mask_free(layer.base.mask);

  /* The same domain, cut the mesh way. It is not stale, so #node_mask_for_composite keeps it and
   * only the grid-aware entry point can tell the difference. */
  layer.base.mask = mask_new(totelem, SCULPT_LAYER_MASK_VERT_BLOCK, 128);
  EXPECT_EQ(node_mask_for_composite(layer, totelem).primary, layer.base.mask);
  EXPECT_EQ(grid_masks_for_composite(layer, totelem, grid_area).primary, nullptr);
  mask_free(layer.base.mask);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name REC exemption (#SCULPT_LAYER_REC_EXEMPT)
 *
 * The decision alone, without a stroke, a session or a #SculptSession: the exemption is a pure
 * function of one bit on the node, which is exactly what makes it testable here. No teardown is
 * needed and no guard is used — the bit lives on each case's own local #SculptLayer and dies with
 * it, which is the whole point of moving it off a process-wide slot.
 *
 * #rec_exempt_set, which decides *which* node carries the bit, needs a #Mesh with a layer tree, so
 * it is pinned in `sculpt_layers_tree_test.cc` by
 * `rec_exempt_set_moves_the_single_bit_and_reports_the_change` and
 * `rec_exempt_set_repairs_a_tree_with_several_exempt_layers`, with
 * `composite_ignores_every_mask_of_an_armed_rec_layer` driving the real composite over an armed
 * layer. What is pinned here is what the mask resolution does once the bit is set.
 * \{ */

TEST(sculpt_layers_mask, rec_exemption_drops_the_layers_own_mask)
{
  const int totelem = 10000;
  SculptLayer layer;
  layer.base.mask = mask_new(totelem, SCULPT_LAYER_MASK_VERT_BLOCK, 128);

  EXPECT_EQ(node_mask_for_composite(layer, totelem).primary, layer.base.mask);

  layer.base.flag |= SCULPT_LAYER_REC_EXEMPT;
  EXPECT_EQ(node_mask_for_composite(layer, totelem).primary, nullptr);

  /* Clearing the bit puts the mask back: the exemption hides a mask, it never discards one. */
  layer.base.flag &= ~SCULPT_LAYER_REC_EXEMPT;
  EXPECT_EQ(node_mask_for_composite(layer, totelem).primary, layer.base.mask);

  mask_free(layer.base.mask);
}

TEST(sculpt_layers_mask, rec_exemption_is_per_node)
{
  /* The bit is what scopes the exemption, and it is carried by exactly the node it is set on.
   * A second layer — the same mesh or another one entirely — is untouched, which is the property
   * a process-wide slot keyed on #SculptLayerTreeNode::uid could not provide: uids restart at 1
   * per mesh, so an armed uid matched a layer of every other mesh in the scene. */
  const int totelem = 10000;
  SculptLayer armed;
  SculptLayer other;
  armed.base.mask = mask_new(totelem, SCULPT_LAYER_MASK_VERT_BLOCK, 128);
  other.base.mask = mask_new(totelem, SCULPT_LAYER_MASK_VERT_BLOCK, 128);
  /* Deliberately the same uid, which is exactly the collision the old mechanism could not tell
   * apart. */
  armed.base.uid = 1;
  other.base.uid = 1;

  armed.base.flag |= SCULPT_LAYER_REC_EXEMPT;
  EXPECT_EQ(node_mask_for_composite(armed, totelem).primary, nullptr);
  EXPECT_EQ(node_mask_for_composite(other, totelem).primary, other.base.mask);

  mask_free(armed.base.mask);
  mask_free(other.base.mask);
}

TEST(sculpt_layers_mask, rec_exemption_drops_the_folder_chain_too)
{
  /* The reason the exemption returns before the chain is read rather than clearing #primary
   * afterwards: a folder mask left in force scales the recorded delta exactly as the layer's own
   * would, and the delta is stored raw, so it would reintroduce the very division REC exists to
   * avoid. */
  const int totelem = 10000;
  SculptLayerGroup folder;
  group_runtime_ensure(folder);
  folder.base.uid = 3;
  folder.base.mask = mask_new(totelem, SCULPT_LAYER_MASK_VERT_BLOCK, 64);

  SculptLayer layer;
  layer.base.uid = 7;
  layer.base.parent = &folder;

  /* The layer carries no mask of its own, so the chain is the only one there is. */
  EXPECT_NE(node_mask_for_composite(layer, totelem).primary, nullptr);

  layer.base.flag |= SCULPT_LAYER_REC_EXEMPT;
  const CompositeMask exempt = node_mask_for_composite(layer, totelem);
  EXPECT_EQ(exempt.primary, nullptr);
  EXPECT_EQ(exempt.secondary, nullptr);

  mask_free(folder.base.mask);
  group_runtime_free(folder);
}

TEST(sculpt_layers_mask, rec_exemption_reaches_the_grid_entry_point)
{
  /* The grid composite resolves its masks through #grid_masks_for_composite, which delegates to
   * #node_mask_for_composite. An exemption honored on one domain and not the other would let the
   * multires REC path reconstruct a `T_old` the displacement evaluator never composed, and the
   * difference would settle into the base on the next flush. */
  const int grid_size = CCG_grid_size(3);
  const int grid_area = grid_size * grid_size;
  const int grids_num = 7;
  const int totelem = grids_num * grid_area;

  SculptLayer layer;
  layer.domain = SCULPT_LAYER_DOMAIN_GRID;
  layer.base.uid = 11;
  layer.totelem = totelem;
  layer.base.mask = mask_new(totelem, grid_area, 128);

  EXPECT_EQ(grid_masks_for_composite(layer, totelem, grid_area).primary, layer.base.mask);

  layer.base.flag |= SCULPT_LAYER_REC_EXEMPT;
  EXPECT_EQ(grid_masks_for_composite(layer, totelem, grid_area).primary, nullptr);

  mask_free(layer.base.mask);
}

TEST(sculpt_layers_mask, rec_exemption_is_unset_by_default)
{
  /* The bit has to default to clear, because "not exempt" is the state every layer spends nearly
   * all of its life in and nothing initializes the flag beyond the DNA default. A bit that read as
   * set on a fresh node would invert the exemption: every layer would lose its mask until something
   * happened to arm and disarm REC on it. */
  const int totelem = 10000;
  SculptLayer layer;
  EXPECT_EQ(layer.base.flag & SCULPT_LAYER_REC_EXEMPT, 0);
  layer.base.mask = mask_new(totelem, SCULPT_LAYER_MASK_VERT_BLOCK, 128);

  EXPECT_EQ(node_mask_for_composite(layer, totelem).primary, layer.base.mask);

  mask_free(layer.base.mask);
}

TEST(sculpt_layers_mask, rec_exemption_is_a_no_op_on_a_stale_mask)
{
  /* The two ways a layer can reach the unmasked path — the mask is stale, or REC is armed on it —
   * have to agree, because the recorded delta is stored raw either way. A staleness rejection that
   * somehow depended on the exemption (or an exemption that resurrected a rejected mask) would let
   * the composite weight a stroke the recorder measured unweighted, and the difference would settle
   * into the base on the next flush. */
  const int totelem = 10000;
  SculptLayer layer;
  /* Sized for a domain this composite does not have, which is what #is_stale_mask rejects. */
  layer.base.mask = mask_new(totelem / 2, SCULPT_LAYER_MASK_VERT_BLOCK, 128);

  /* Rejected on its own merits, with nothing exempt. */
  EXPECT_EQ(node_mask_for_composite(layer, totelem).primary, nullptr);

  /* And the exemption changes nothing: already unmasked, still unmasked. */
  layer.base.flag |= SCULPT_LAYER_REC_EXEMPT;
  const CompositeMask exempt = node_mask_for_composite(layer, totelem);
  EXPECT_EQ(exempt.primary, nullptr);
  EXPECT_EQ(exempt.secondary, nullptr);

  /* Disarming is what makes the mask usable again at the count it was cut for, so the two
   * rejections have to compose rather than one masking the other. */
  layer.base.flag &= ~SCULPT_LAYER_REC_EXEMPT;

  /* The mask itself survives the exemption, so disarming REC puts the layer back exactly where it
   * was rather than leaving it permanently unmasked. */
  EXPECT_EQ(node_mask_for_composite(layer, totelem / 2).primary, layer.base.mask);

  mask_free(layer.base.mask);
}

/** \} */

}  // namespace blender::bke::sculpt_layers::tests
