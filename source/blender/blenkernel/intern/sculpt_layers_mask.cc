/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Sparse `uint8` weight maps attached to sculpt layer tree nodes. See #SculptLayerMask.
 */

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

#include "BKE_sculpt_layers.hh"

#include "BLI_array.hh"
#include "BLI_index_range.hh"
#include "BLI_listbase_iterator.hh"
#include "BLI_math_base.h"
#include "BLI_span.hh"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"

#include "BKE_attribute.hh"
#include "BKE_main.hh"
#include "BKE_mesh.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_subdiv_ccg.hh"

#include "BLO_read_write.hh"

#include "CLG_log.h"

#include "MEM_guardedalloc.h"

static CLG_LogRef LOG = {"bke.sculpt_layers"};

/* [DEBUG-stats] Block-sparsity statistics for #mask_compress, used to check the spec's assumption
 * that VERT-block sparsity holds because vertex indices are locally coherent (a typical mask
 * leaves most blocks uniform). Tied to the module-wide #SCULPT_LAYERS_DEBUG_LOG switch (see
 * `BKE_sculpt_layers.hh`), so flipping that flag reaches this probe too instead of needing its own
 * switch hunted down separately. */
#define SCULPT_LAYERS_DEBUG_MASK_STATS SCULPT_LAYERS_DEBUG_LOG

namespace blender::bke::sculpt_layers {

int mask_blocks_num(const int64_t totelem, const int block_size)
{
  BLI_assert(block_size > 0);
  BLI_assert(totelem >= 0);
  const int64_t blocks = int64_t(
      divide_ceil_ul(uint64_t(std::max<int64_t>(totelem, 0)), uint64_t(block_size)));
  /* The count itself stays 32-bit: a grid mask is cut one block per grid, and a vertex mask one per
   * 4096 vertices, so this is bounded by the mesh's grid or vertex count either way — both of which
   * are 32-bit themselves. Only the *element* count needed widening. */
  BLI_assert(blocks <= std::numeric_limits<int>::max());
  return int(blocks);
}

SculptLayerMask *mask_new(const int64_t totelem, const int block_size, const uint8_t fill)
{
  if (totelem <= 0) {
    return nullptr;
  }
  SculptLayerMask *mask = MEM_new_zeroed<SculptLayerMask>(__func__);
  mask->totelem = totelem;
  mask->block_size = block_size;
  mask->blocks_num = mask_blocks_num(totelem, block_size);
  mask->data_num = 0;
  mask->data = nullptr;
  mask->block_kind = MEM_new_array_zeroed<int8_t>(size_t(mask->blocks_num), __func__);
  mask->block_value = MEM_new_array_zeroed<uint8_t>(size_t(mask->blocks_num), __func__);
  mask->block_offset = MEM_new_array_zeroed<int>(size_t(mask->blocks_num), __func__);
  for (const int block : IndexRange(mask->blocks_num)) {
    mask->block_kind[block] = SCULPT_LAYER_MASK_BLOCK_UNIFORM;
    mask->block_value[block] = fill;
    mask->block_offset[block] = -1;
  }
  return mask;
}

void mask_free(SculptLayerMask *mask)
{
  if (mask == nullptr) {
    return;
  }
  MEM_SAFE_DELETE(mask->block_kind);
  MEM_SAFE_DELETE(mask->block_value);
  MEM_SAFE_DELETE(mask->block_offset);
  MEM_SAFE_DELETE(mask->data);
  /* Allocated by #MEM_new_zeroed above, so it is freed the same way, unlike the tree nodes this
   * mask hangs off of (those also originate from the C-style blend reader and use
   * #MEM_delete_void instead; see the ID lifetime notes in BKE_sculpt_layers.hh). */
  MEM_SAFE_DELETE(mask);
}

SculptLayerMask *mask_copy(const SculptLayerMask &src)
{
  SculptLayerMask *dst = MEM_new_zeroed<SculptLayerMask>(__func__);
  *dst = src;
  /* A neutralized mask (see #mask_blend_read) describes no blocks and leaves its three block
   * tables null. Copied verbatim, tables and all: allocating zero-length arrays instead would give
   * the copy non-null tables under `blocks_num == 0`, a shape nothing else in the module produces
   * and that no consumer is written against. The shallow struct copy above already carried the
   * null pointers over, so there is nothing left to do. */
  if (src.blocks_num == 0) {
    dst->data = nullptr;
    return dst;
  }
  dst->block_kind = MEM_new_array_zeroed<int8_t>(size_t(src.blocks_num), __func__);
  dst->block_value = MEM_new_array_zeroed<uint8_t>(size_t(src.blocks_num), __func__);
  dst->block_offset = MEM_new_array_zeroed<int>(size_t(src.blocks_num), __func__);
  memcpy(dst->block_kind, src.block_kind, sizeof(int8_t) * size_t(src.blocks_num));
  memcpy(dst->block_value, src.block_value, sizeof(uint8_t) * size_t(src.blocks_num));
  memcpy(dst->block_offset, src.block_offset, sizeof(int) * size_t(src.blocks_num));
  if (src.data_num > 0) {
    dst->data = MEM_new_array_zeroed<uint8_t>(size_t(src.data_num), __func__);
    memcpy(dst->data, src.data, sizeof(uint8_t) * size_t(src.data_num));
  }
  else {
    dst->data = nullptr;
  }
  return dst;
}

int64_t mask_size_in_bytes(const SculptLayerMask &mask)
{
  /* Mirrors what #mask_copy allocates, which is what an undo capture actually costs. A neutralized
   * mask has null tables under `blocks_num == 0`, so the per-block term vanishes on its own. */
  return int64_t(sizeof(SculptLayerMask)) +
         int64_t(mask.blocks_num) *
             int64_t(sizeof(int8_t) + sizeof(uint8_t) + sizeof(int)) +
         int64_t(mask.data_num) * int64_t(sizeof(uint8_t));
}

uint8_t mask_value_at(const SculptLayerMask &mask, const int64_t elem)
{
  BLI_assert(elem >= 0 && elem < mask.totelem);
  /* The element index is 64-bit (a grid domain counts `grids_num * grid_area`), but the block index
   * it divides down to is bounded by #blocks_num, which is 32-bit — see #mask_blocks_num. */
  const int64_t block = elem / mask.block_size;
  if (mask.block_kind[block] == SCULPT_LAYER_MASK_BLOCK_UNIFORM) {
    return mask.block_value[block];
  }
  return mask.data[mask.block_offset[block] + (elem % mask.block_size)];
}

/* Elements this block actually covers; the tail block is short. The result always fits in an `int`,
 * being bounded by #block_size, even though the element count it subtracts from does not. */
static int mask_block_extent(const SculptLayerMask &mask, const int block)
{
  const int64_t start = int64_t(block) * mask.block_size;
  return int(std::min(int64_t(mask.block_size), mask.totelem - start));
}

MaskBlock mask_block(const SculptLayerMask &mask, const int block)
{
  if (mask.block_kind[block] == SCULPT_LAYER_MASK_BLOCK_UNIFORM) {
    return {true, mask.block_value[block], nullptr};
  }
  return {false, 0, mask.data + mask.block_offset[block]};
}

MaskBlockWeight mask_block_weight(const CompositeMask &masks, const int block, const float weight)
{
  MaskBlockWeight result;
  result.weight = weight;
  if (masks.primary == nullptr) {
    /* Unmasked: one pointer test, which is the property that makes this feature affordable to
     * ship on meshes that carry no masks at all. */
    return result;
  }

  const MaskBlock block_a = mask_block(*masks.primary, block);
  if (masks.secondary == nullptr) {
    if (block_a.uniform) {
      result.weight = weight * float(block_a.value) * (1.0f / 255.0f);
      result.skip = (result.weight == 0.0f);
      return result;
    }
    result.fold = MaskFold::Single;
    result.data_a = block_a.data;
    return result;
  }

  const MaskBlock block_b = mask_block(*masks.secondary, block);
  if (block_a.uniform && block_b.uniform) {
    result.weight = weight * float(int(block_a.value) * int(block_b.value)) *
                    (1.0f / (255.0f * 255.0f));
    result.skip = (result.weight == 0.0f);
    return result;
  }
  /* A uniform zero on either side annihilates the block even when the other side is dense, so the
   * skip above is not the only place it can be caught. Worth the check: masking a folder to zero is
   * a common way to park a whole subtree, and without this the composite would walk it element by
   * element to add nothing. */
  if ((block_a.uniform && block_a.value == 0) || (block_b.uniform && block_b.value == 0)) {
    result.skip = true;
    return result;
  }
  result.fold = MaskFold::Pair;
  result.data_a = block_a.uniform ? nullptr : block_a.data;
  result.data_b = block_b.uniform ? nullptr : block_b.data;
  result.value_a = block_a.value;
  result.value_b = block_b.value;
  return result;
}

void mask_expand(const SculptLayerMask &mask, MutableSpan<float> r_dense)
{
  BLI_assert(r_dense.size() == mask.totelem);
  threading::parallel_for(IndexRange(mask.blocks_num), 32, [&](const IndexRange range) {
    for (const int64_t block : range) {
      const int64_t start = block * mask.block_size;
      const int extent = mask_block_extent(mask, int(block));
      const MaskBlock src = mask_block(mask, int(block));
      if (src.uniform) {
        const float value = float(src.value) * (1.0f / 255.0f);
        r_dense.slice(start, extent).fill(value);
      }
      else {
        for (const int i : IndexRange(extent)) {
          r_dense[start + i] = float(src.data[i]) * (1.0f / 255.0f);
        }
      }
    }
  });
}

SculptLayerMask *mask_compress(const Span<float> dense, const int block_size)
{
  const int64_t totelem = dense.size();
  SculptLayerMask *mask = mask_new(totelem, block_size, 0);
  if (mask == nullptr) {
    return nullptr;
  }

  /* Quantize once into a scratch buffer, then decide per block. Quantizing inside the block loop
   * would round the same value twice — once for the uniformity test, once for storage — and the
   * two could disagree on a boundary. */
  Vector<uint8_t> quantized(totelem);
  threading::parallel_for(IndexRange(totelem), 8192, [&](const IndexRange range) {
    for (const int64_t i : range) {
      /* The lower bound is an ordered test rather than #std::clamp so that NaN maps to 0. Every
       * comparison against NaN is false, so #std::clamp returns it unchanged (neither bound
       * compares true) and the conversion to `uint8_t` would then be undefined. NaN reaches here
       * from the standard mask storage, which a script can write freely. */
      const float value = dense[i];
      quantized[i] = value > 0.0f ? uint8_t(std::min(value, 1.0f) * 255.0f + 0.5f) : 0;
    }
  });

  /* First pass decides each block's kind and how much room the dense ones need; the second pass
   * fills a single allocation. Growing one buffer per dense block would fragment the heap on a
   * mask that covers a large region. */
  /* Accumulated in 64-bit because the element count feeding it is: #data_num and #block_offset are
   * both 32-bit in DNA, so the total is *refused* below rather than silently wrapped into an offset
   * that would index #data out of bounds. */
  int64_t data_num = 0;
  for (const int block : IndexRange(mask->blocks_num)) {
    const int64_t start = int64_t(block) * block_size;
    const int extent = mask_block_extent(*mask, block);
    const uint8_t first = quantized[start];
    bool uniform = true;
    for (const int i : IndexRange(extent)) {
      if (quantized[start + i] != first) {
        uniform = false;
        break;
      }
    }
    if (uniform) {
      mask->block_kind[block] = SCULPT_LAYER_MASK_BLOCK_UNIFORM;
      mask->block_value[block] = first;
      mask->block_offset[block] = -1;
    }
    else {
      mask->block_kind[block] = SCULPT_LAYER_MASK_BLOCK_DENSE;
      mask->block_offset[block] = int(data_num);
      data_num += extent;
    }
  }

  if (data_num > std::numeric_limits<int>::max()) {
    /* Fails open, as every other unusable-mask path does (see #is_stale_mask): no mask means the
     * node composites at full strength, which is the safe reading of a weight map too large to
     * address. The partly-filled block table is discarded with it, so the truncated offsets written
     * above are never read. */
    mask_free(mask);
    return nullptr;
  }
  mask->data_num = int(data_num);
  if (data_num > 0) {
    mask->data = MEM_new_array_zeroed<uint8_t>(size_t(data_num), __func__);
    for (const int block : IndexRange(mask->blocks_num)) {
      if (mask->block_kind[block] != SCULPT_LAYER_MASK_BLOCK_DENSE) {
        continue;
      }
      const int64_t start = int64_t(block) * block_size;
      const int extent = mask_block_extent(*mask, block);
      memcpy(mask->data + mask->block_offset[block],
             quantized.data() + start,
             sizeof(uint8_t) * size_t(extent));
    }
  }

#if SCULPT_LAYERS_DEBUG_MASK_STATS
  /* Counted here rather than folded into the block loop above: the loop above must stay branch-free
   * on the hot path, and this count only exists to be printed. */
  int dense_blocks_num = 0;
  for (const int block : IndexRange(mask->blocks_num)) {
    if (mask->block_kind[block] == SCULPT_LAYER_MASK_BLOCK_DENSE) {
      dense_blocks_num++;
    }
  }
  const float dense_blocks_fraction = mask->blocks_num > 0 ?
                                           float(dense_blocks_num) / float(mask->blocks_num) :
                                           0.0f;
  const float data_fraction = totelem > 0 ? float(mask->data_num) / float(totelem) : 0.0f;
  CLOG_INFO(&LOG,
            "mask_compress: blocks_num=%d, dense_blocks=%d (%.1f%% of blocks), data_num=%d, "
            "data_num/totelem=%.1f%%",
            mask->blocks_num,
            dense_blocks_num,
            double(dense_blocks_fraction * 100.0f),
            mask->data_num,
            double(data_fraction * 100.0f));
#endif

  return mask;
}

SculptLayerMask *mask_multiply(const SculptLayerMask &a, const SculptLayerMask &b)
{
  /* Checked rather than asserted: masks go stale whenever the mesh's element count changes, so two
   * nodes of one tree disagreeing about the domain is a data state, not a caller bug. See the
   * rationale on the declaration. */
  if (a.totelem != b.totelem || a.block_size != b.block_size) {
    return nullptr;
  }
  SculptLayerMask *result = mask_new(a.totelem, a.block_size, 0);
  if (result == nullptr) {
    /* An empty domain, or a mask neutralized by #mask_blend_read: there is nothing to attenuate. */
    return nullptr;
  }

  /* Sized for the worst case, then trimmed: a two-pass count would have to run the same block
   * comparison twice, and the transient over-allocation is bounded by the dense inputs. */
  Vector<uint8_t> scratch;
  int data_num = 0;
  for (const int block : IndexRange(result->blocks_num)) {
    const MaskBlock block_a = mask_block(a, block);
    const MaskBlock block_b = mask_block(b, block);
    const int extent = mask_block_extent(*result, block);

    if (block_a.uniform && block_b.uniform) {
      /* The case the composite fast path depends on: stays a scalar. */
      result->block_kind[block] = SCULPT_LAYER_MASK_BLOCK_UNIFORM;
      result->block_value[block] = uint8_t((int(block_a.value) * int(block_b.value) + 127) / 255);
      result->block_offset[block] = -1;
      continue;
    }
    /* A uniform zero on either side annihilates the block, keeping it uniform. Worth the check:
     * masking a folder to zero is a common way to park a whole subtree. */
    if ((block_a.uniform && block_a.value == 0) || (block_b.uniform && block_b.value == 0)) {
      result->block_kind[block] = SCULPT_LAYER_MASK_BLOCK_UNIFORM;
      result->block_value[block] = 0;
      result->block_offset[block] = -1;
      continue;
    }

    result->block_kind[block] = SCULPT_LAYER_MASK_BLOCK_DENSE;
    result->block_offset[block] = data_num;
    /* #Vector::resize keeps what is already there, so the blocks written on earlier iterations
     * survive into the single allocation below. */
    scratch.resize(data_num + extent);
    bool uniform = true;
    for (const int i : IndexRange(extent)) {
      const int va = block_a.uniform ? int(block_a.value) : int(block_a.data[i]);
      const int vb = block_b.uniform ? int(block_b.value) : int(block_b.data[i]);
      /* Rounded rather than truncated so the identities the composite leans on hold exactly: a
       * factor of 255 is neutral, since `255 * x + 127` divided by 255 leaves a remainder of 127
       * — below the divisor, so the quotient is exactly `x` and nothing is lost. That also makes
       * 255 by 255 land on 255 without overflowing the byte, and a factor of 0 stays 0. Plain
       * `va * vb / 255` would lose a step off every value it touches and would make a fully opaque
       * folder darken its subtree. */
      const uint8_t value = uint8_t((va * vb + 127) / 255);
      scratch[data_num + i] = value;
      uniform &= (value == scratch[data_num]);
    }
    /* A product of two dense blocks is often constant even though neither input was — a smooth edge
     * against a zeroed region is the everyday case. Without this the block would stay dense, and
     * since #chain_mask folds one product per folder level, a subtree under nested folders would
     * degrade monotonically towards dense storage and never recover: exactly the growth
     * #mask_compress exists to avoid. The scratch is rewound, so the collapsed block costs nothing
     * in #data either. */
    if (uniform) {
      result->block_kind[block] = SCULPT_LAYER_MASK_BLOCK_UNIFORM;
      result->block_value[block] = scratch[data_num];
      result->block_offset[block] = -1;
      scratch.resize(data_num);
      continue;
    }
    data_num += extent;
  }

  result->data_num = data_num;
  if (data_num > 0) {
    result->data = MEM_new_array_zeroed<uint8_t>(size_t(data_num), __func__);
    memcpy(result->data, scratch.data(), sizeof(uint8_t) * size_t(data_num));
  }
  return result;
}

void mask_blend_write(BlendWriter *writer, const SculptLayerMask &mask)
{
  /* Written with its pointer members intact, unlike #SculptLayerGroup::runtime in
   * #group_blend_write_recursive. They are not this session's incidental addresses but the keys the
   * reader resolves each array from below, so nulling them would make every saved mask unreadable.
   * They are memfile-undo safe for the same reason the neighboring #SculptLayer::data pointer is:
   * an unchanged mask keeps its allocations, so the bytes written are identical between steps. */
  writer->write_struct(&mask);

  writer->write_int8_array(mask.blocks_num, mask.block_kind);
  writer->write_uint8_array(mask.blocks_num, mask.block_value);
  writer->write_int32_array(mask.blocks_num, mask.block_offset);
  if (mask.data_num > 0) {
    writer->write_uint8_array(mask.data_num, mask.data);
  }
}

void mask_blend_read(BlendDataReader *reader, SculptLayerMask *mask)
{
  /* #blocks_num sizes three separate arrays, so #BLO_read_array_and_validate_size is the wrong
   * tool here: the first failing read would zero the count that the other two still need, leaving
   * them sized by a number that no longer describes them. Each read reports on its own — with
   * `&=` rather than `&&`, since all of them must run to relink their pointers — and the mask as a
   * whole is dropped at the end if any failed. */
  bool valid = BLO_read_array(reader, &mask->block_kind, mask->blocks_num);
  valid &= BLO_read_array(reader, &mask->block_value, mask->blocks_num);
  valid &= BLO_read_array(reader, &mask->block_offset, mask->blocks_num);
  if (mask->data_num > 0) {
    /* #data_num sizes only this array, but it is cross-checked against #block_offset below, so it
     * is read the same way rather than zeroed independently. */
    valid &= BLO_read_array(reader, &mask->data, mask->data_num);
  }
  else {
    /* Nothing was written for an all-uniform mask, so the slot holds a stale file address. */
    mask->data = nullptr;
  }

  /* A truncated or hand-edited file must not send the composite loops past the end of a block
   * table. Dropping the mask is recoverable; reading out of bounds is not. The block count and
   * element count must agree, or #mask_value_at would index a block that was never allocated. */
  if (valid) {
    valid = mask->block_size > 0 && mask->totelem >= 0 && mask->data_num >= 0 &&
            mask->blocks_num == mask_blocks_num(mask->totelem, mask->block_size);
  }
  /* Every dense block must name a range that lies inside #data. A uniform block is self-contained,
   * so its offset is not consulted here.
   *
   * Tested as "not uniform" rather than "is dense", which is the predicate #mask_block and
   * #mask_value_at read the table with. Spelled the other way round, a kind that is neither value —
   * anything a truncated or hand-edited file can put in an `int8_t` — would skip validation here and
   * still be dereferenced as dense there, indexing #data by an offset nobody checked (and #data is
   * null outright when `data_num == 0`). The two spellings must not drift apart. */
  if (valid) {
    for (const int block : IndexRange(mask->blocks_num)) {
      if (mask->block_kind[block] == SCULPT_LAYER_MASK_BLOCK_UNIFORM) {
        continue;
      }
      /* Normalized so an out-of-enum value read from the file does not survive into the tree, where
       * every later reader would have to keep making the same allowance. */
      mask->block_kind[block] = SCULPT_LAYER_MASK_BLOCK_DENSE;
      const int offset = mask->block_offset[block];
      const int extent = mask_block_extent(*mask, block);
      if (offset < 0 || extent <= 0 || int64_t(offset) + int64_t(extent) > int64_t(mask->data_num))
      {
        valid = false;
        break;
      }
    }
  }

  if (!valid) {
    /* Neutralized rather than freed: whatever the reader did allocate is released here instead of
     * being left half-populated under a zeroed #blocks_num, which would otherwise leave the block
     * table pointers dangling. A zero #totelem is the same "stale mask" state the tree already
     * uses for a mismatched element count, and a zero #blocks_num makes every block loop a no-op.
     * #block_size is reset too: nothing downstream re-checks a neutralized mask's invariants, and
     * #mask_value_at divides by it while #mask_blocks_num asserts it is positive, so it must hold
     * even though this mask no longer describes any blocks. */
    mask->totelem = 0;
    mask->block_size = SCULPT_LAYER_MASK_VERT_BLOCK;
    mask->blocks_num = 0;
    mask->data_num = 0;
    MEM_SAFE_DELETE(mask->block_kind);
    MEM_SAFE_DELETE(mask->block_value);
    MEM_SAFE_DELETE(mask->block_offset);
    MEM_SAFE_DELETE(mask->data);
  }
}

/* -------------------------------------------------------------------- */
/** \name Weight mask editing session: suspend and resume
 *
 * See the overview on #MaskEditSuspendGuard in `BKE_sculpt_layers.hh`. The pair deliberately does
 * *not* compress onto the node, refresh the PBVH or re-compose the surface: nothing observes the
 * mask between the two calls (every bracketed region is synchronous), and doing any of it would
 * turn a save into a visible geometry change.
 * \{ */

static SculptSession *session_of(Object &object)
{
  return object.runtime->sculpt_session;
}

/**
 * Park the session's weights and put the user's own `.sculpt_mask` back. Returns false, changing
 * nothing, when the swap cannot be made.
 */
static bool mask_edit_suspend_mesh(Object &object, SculptSession &ss)
{
  Mesh &mesh = *id_cast<Mesh *>(object.data);
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();

  /* Everything that can refuse is decided before a single buffer is touched, so a refusal leaves
   * the session exactly as it was. A suspend fires on operations the user never asked for — an
   * auto-save on a timer, a depsgraph re-evaluation — and must therefore never be the thing that
   * destroys their mask. The close path may make that trade because it is terminating; a
   * suspend cannot.
   *
   * The vertex count can have changed under the session (an Edit Mode round trip, a script), which
   * leaves the parked buffer describing a topology this mesh no longer has. */
  const bool have_user_mask = ss.layers.mask_edit.had_vert_mask;
  const int64_t verts_num = mesh.verts_num;
  if (have_user_mask && ss.layers.mask_edit.saved_vert_mask.size() != verts_num) {
    return false;
  }

  Array<float> parked;
  /* Scoped so the writer is finished and destroyed before the attribute can be removed below. */
  {
    bke::SpanAttributeWriter<float> mask = attributes.lookup_or_add_for_write_span<float>(
        ".sculpt_mask", bke::AttrDomain::Point);
    if (!mask) {
      return false;
    }
    if (mask.span.size() != verts_num) {
      /* The attribute disagrees with the mesh it belongs to. Refused for the same reason as above:
       * neither buffer can be trusted to describe the other. */
      mask.finish();
      return false;
    }
    parked = Array<float>(mask.span.as_span());
    if (have_user_mask) {
      mask.span.copy_from(ss.layers.mask_edit.saved_vert_mask);
    }
    mask.finish();
  }
  /* A mesh that had no `.sculpt_mask` before the session must appear to have none while suspended,
   * or the bracketed write would record an all-zero mask the user never painted. */
  if (!have_user_mask) {
    attributes.remove(".sculpt_mask");
  }
  ss.layers.mask_edit.suspended_dense = std::move(parked);
  return true;
}

static void mask_edit_resume_mesh(Object &object, SculptSession &ss)
{
  Mesh &mesh = *id_cast<Mesh *>(object.data);
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  /* Re-added when the suspend removed it, which is the case for a mesh that carried no mask of its
   * own. */
  bke::SpanAttributeWriter<float> mask = attributes.lookup_or_add_for_write_span<float>(
      ".sculpt_mask", bke::AttrDomain::Point);
  if (!mask) {
    return;
  }
  /* A length mismatch means the topology changed while suspended. The session's weights cannot be
   * put back over a domain they do not describe; the close path will find the mask stale and start
   * the node from scratch, which is the same answer it gives for every other topology change. */
  if (ss.layers.mask_edit.suspended_dense.size() == mask.span.size()) {
    mask.span.copy_from(ss.layers.mask_edit.suspended_dense);
  }
  mask.finish();
}

static bool mask_edit_suspend_grids(SculptSession &ss)
{
  SubdivCCG *subdiv_ccg = ss.subdiv_ccg;
  if (subdiv_ccg == nullptr) {
    /* Nothing to park, and nothing the bracketed write could pick up from it either. Reported as
     * refused so a caller that can defer its write does so rather than assume it is safe. */
    return false;
  }
  MutableSpan<float> masks = subdiv_ccg->masks.as_mutable_span();
  if (masks.is_empty()) {
    return false;
  }
  /* Decided before anything is touched, as on the mesh path: a parked buffer of a different length
   * belongs to a CCG that has since been rebuilt, and putting it back is not something a
   * suspend is allowed to give up on the user's behalf. */
  if (ss.layers.mask_edit.had_grid_mask &&
      ss.layers.mask_edit.saved_grid_mask.size() != masks.size())
  {
    return false;
  }

  ss.layers.mask_edit.suspended_dense = Array<float>(masks.as_span());
  if (ss.layers.mask_edit.had_grid_mask) {
    masks.copy_from(ss.layers.mask_edit.saved_grid_mask);
  }
  else {
    /* Emptied rather than left holding the layer's weights: an empty array is the state every
     * reader already treats as "no mask" (see #gather_mask_grids), and it is therefore what the
     * bracketed flush will write to `CD_GRID_PAINT_MASK`. */
    subdiv_ccg->masks.reinitialize(0);
  }
  return true;
}

static void mask_edit_resume_grids(SculptSession &ss)
{
  SubdivCCG *subdiv_ccg = ss.subdiv_ccg;
  if (subdiv_ccg == nullptr) {
    return;
  }
  const int64_t parked = ss.layers.mask_edit.suspended_dense.size();
  /* Checked against the geometry recorded at open, not merely against the live array's length: a
   * CCG rebuilt while suspended re-derives #SubdivCCG::masks from the base mesh, and a rebuild at
   * the same subdivision level keeps `grid_area` while changing `grids_num`. Putting the parked
   * weights back over that would author a mask for a domain that no longer exists.
   *
   * The identity check is what catches the rebuild that moves none of those numbers — same level,
   * same base topology — after which the live array holds the user's own sculpt mask rather than
   * the buffer this resume was parked from. See #SculptLayerMaskEdit::ccg_id. */
  if (subdiv_ccg->id != ss.layers.mask_edit.ccg_id ||
      subdiv_ccg->grid_area != ss.layers.mask_edit.grid_area ||
      subdiv_ccg->grids_num != ss.layers.mask_edit.grids_num ||
      parked != int64_t(ss.layers.mask_edit.grids_num) * ss.layers.mask_edit.grid_area)
  {
    return;
  }
  if (subdiv_ccg->masks.size() != parked) {
    subdiv_ccg->masks.reinitialize(parked);
  }
  subdiv_ccg->masks.as_mutable_span().copy_from(ss.layers.mask_edit.suspended_dense);
}

/**
 * Take one reference on the open session on \a object being parked. Returns false only when a
 * session is open and could not be parked; "no session at all" is a success, because the guarded
 * region then has nothing to protect against.
 */
static bool mask_edit_suspend(Object &object, const MaskEditDomains domains)
{
  SculptSession *ss = session_of(object);
  if (ss == nullptr || ss->layers.mask_edit.node_uid == 0) {
    return true;
  }
  if (domains == MaskEditDomains::GridsOnly && !ss->layers.mask_edit.on_grids) {
    /* Reported as success, not as a refusal: the guarded region cannot see this session's weights
     * at all, so there is nothing for the caller to warn about. See #MaskEditDomains::GridsOnly. */
    return true;
  }
  if (ss->layers.mask_edit.suspend_depth > 0) {
    /* Already parked by an enclosing guard. Only the reference is taken; the swap happened once,
     * at the transition from zero. */
    ss->layers.mask_edit.suspend_depth++;
    return true;
  }
  const bool suspended = ss->layers.mask_edit.on_grids ? mask_edit_suspend_grids(*ss) :
                                                         mask_edit_suspend_mesh(object, *ss);
  if (!suspended) {
    /* Latched: the condition behind a refusal — a parked buffer describing a domain the object no
     * longer has — does not clear itself, and the flush primitives are re-entered on every
     * depsgraph re-evaluation, so an unlatched report would fill the log with one line per redraw.
     * Logged here rather than at each call site so that no bracket, present or future, can refuse
     * silently. */
    if (!ss->layers.mask_edit.suspend_refusal_reported) {
      ss->layers.mask_edit.suspend_refusal_reported = true;
      CLOG_WARN(&LOG,
                "Sculpt layer mask session could not be suspended: its parked mask no longer "
                "describes this object. The user's sculpt mask for this session is lost; the "
                "session will be closed as stale.");
    }
    /* The depth stays at zero, so the matching resume is a no-op and cannot overwrite the live
     * buffer with a parked one that was never filled. */
    return false;
  }
  ss->layers.mask_edit.suspend_depth = 1;
  return true;
}

/** Release one reference taken by #mask_edit_suspend; the swap back happens at zero. */
static void mask_edit_resume(Object &object, const MaskEditDomains domains)
{
  SculptSession *ss = session_of(object);
  if (ss == nullptr || ss->layers.mask_edit.suspend_depth == 0) {
    return;
  }
  /* Filtered exactly as the suspend was. A guard that declined to park a mesh-domain session took
   * no reference on it, so releasing one here would un-park a session an enclosing guard still
   * needs parked. */
  if (domains == MaskEditDomains::GridsOnly && !ss->layers.mask_edit.on_grids) {
    return;
  }
  ss->layers.mask_edit.suspend_depth--;
  if (ss->layers.mask_edit.suspend_depth > 0) {
    /* An enclosing guard still needs the session parked. */
    return;
  }
  if (ss->layers.mask_edit.on_grids) {
    mask_edit_resume_grids(*ss);
  }
  else {
    mask_edit_resume_mesh(object, *ss);
  }
  ss->layers.mask_edit.suspended_dense = Array<float>();
}

void mask_edit_force_resume(Object &object)
{
  SculptSession *ss = session_of(object);
  if (ss == nullptr || ss->layers.mask_edit.suspend_depth == 0) {
    return;
  }
  /* Collapsed to one outstanding reference so the release below performs the swap. Any guard still
   * alive resumes into a depth of zero afterwards, which is a no-op — correct, because the session
   * it was holding parked is about to stop existing. */
  ss->layers.mask_edit.suspend_depth = 1;
  mask_edit_resume(object, MaskEditDomains::All);
}

/** Give up the session open on \a object, if any. See #mask_edit_abandon_all. */
static void mask_edit_abandon(Object &object)
{
  SculptSession *ss = session_of(object);
  if (ss == nullptr || ss->layers.mask_edit.node_uid == 0) {
    return;
  }
  /* The suspend *is* the "put the user's mask back" half; abandoning is that half without the
   * matching resume. A session an enclosing guard already parked needs nothing done to the storage,
   * only the state cleared below — dropping the depth is what keeps that guard's destructor from
   * re-installing the layer's weights over the user's mask on its way out. */
  bool restored = true;
  if (ss->layers.mask_edit.suspend_depth == 0) {
    restored = ss->layers.mask_edit.on_grids ? mask_edit_suspend_grids(*ss) :
                                               mask_edit_suspend_mesh(object, *ss);
  }
  if (!restored) {
    /* The parked buffer no longer describes this object, so the layer's weights stay in the standard
     * mask storage. Not latched the way #mask_edit_suspend's refusal is: this runs once per global
     * undo rather than once per re-evaluation, and the session is gone afterwards. */
    CLOG_WARN(&LOG,
              "Sculpt layer mask session on object '%s' was abandoned without restoring the user's "
              "sculpt mask: the parked mask no longer describes this object",
              object.id.name + 2);
  }
  else {
    CLOG_INFO(&LOG,
              "Sculpt layer mask session on object '%s' was abandoned across a global undo step; "
              "the in-progress mask edit is lost",
              object.id.name + 2);
  }
  ss->layers.mask_edit = SculptLayerMaskEdit{};
}

void mask_edit_abandon_all(Main &bmain)
{
  for (Object &object : bmain.objects) {
    mask_edit_abandon(object);
  }
}

MaskEditSuspendGuard::MaskEditSuspendGuard(Main &bmain) : bmain_(&bmain)
{
  for (Object &object : bmain.objects) {
    if (!mask_edit_suspend(object, domains_)) {
      refused_ = true;
    }
  }
}

MaskEditSuspendGuard::MaskEditSuspendGuard(Object &object, const MaskEditDomains domains)
    : object_(&object), domains_(domains)
{
  refused_ = !mask_edit_suspend(object, domains_);
}

MaskEditSuspendGuard::~MaskEditSuspendGuard()
{
  /* Re-scanned rather than replayed from a recorded list: #mask_edit_resume is a no-op on any
   * object that was not suspended, so the scan needs no state and cannot go stale if an object is
   * added between the two points. */
  if (bmain_ != nullptr) {
    for (Object &object : bmain_->objects) {
      mask_edit_resume(object, domains_);
    }
  }
  if (object_ != nullptr) {
    mask_edit_resume(*object_, domains_);
  }
}

/** \} */

}  // namespace blender::bke::sculpt_layers
