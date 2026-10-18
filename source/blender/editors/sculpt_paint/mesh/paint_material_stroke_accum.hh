/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Per-stroke material accumulator shared by Sculpt and Image Editor material paint.
 */

#pragma once

#include <array>
#include <memory>
#include <mutex>

#include "BLI_map.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_utildefines.h"

struct ImBuf;

namespace blender::ed::sculpt_paint::paint::image {

/**
 * Per-stroke pre-stroke color and coverage accumulator for material channel maps.
 *
 * The raster engine normally blends each dab straight into the live image. That converges for Mix
 * - re-mixing toward the same target changes nothing - but every other mode compounds: a slow drag
 * is dozens of dabs, so Add saturates to white and Multiply crushes to black, and wherever the
 * stroke overlaps itself the application count jumps, leaving hard edges along the overlap.
 *
 * So for the non-Mix modes the engine switches to the model the vertex engine already uses (see
 * #StrokeCache::material_mix_base_color): accumulate the stroke's coverage separately, and blend
 * that once against a pre-stroke value frozen at the texel's first touch. This is what lets
 * paint_material_blend.hh's promise - that a channel value means the same thing in both engines -
 * hold for modes other than Mix.
 *
 * Storage is lazy per #block_size square of texels, so it costs memory only where the stroke
 * actually lands, and only for a stroke that needs it.
 */
class MaterialStrokeAccum {
 public:
  /** Side of one lazily allocated texel block, matching the image undo tile grid. */
  static constexpr int block_size = 64;
  static constexpr int block_texel_num = block_size * block_size;

  struct Block {
    explicit Block(const bool scalar_mix)
        /* Left uninitialized on purpose: every read of an entry is guarded by #orig_valid. */
        : orig(new float4[block_texel_num]),
          mix(scalar_mix ? nullptr : std::make_unique<float4[]>(block_texel_num)),
          mix_alpha(scalar_mix ? std::make_unique<float[]>(block_texel_num) : nullptr),
          orig_valid(std::make_unique<bool[]>(block_texel_num))
    {
    }

    /** Scene-linear color from before the stroke, captured per texel by #orig_valid. */
    std::unique_ptr<float4[]> orig;
    /** Stroke coverage so far, pre-multiplied exactly like #StrokeCache::material_mix_base_color.
     * Null in scalar mode. */
    std::unique_ptr<float4[]> mix;
    /**
     * Scalar mode only (erase strokes, whose paint color is constant per target): the alpha
     * component of the coverage, which evolves independently of the color components.
     */
    std::unique_ptr<float[]> mix_alpha;
    /**
     * Whether the matching #orig entry has been captured yet. A texel is always read before it is
     * written, so the image still holds its pre-stroke value at that first touch.
     *
     * Deliberately a byte per texel rather than a bit set: neighboring texels of one block are
     * filled by different threads, and distinct bytes can be written concurrently where bits
     * sharing a word cannot.
     */
    std::unique_ptr<bool[]> orig_valid;
  };

  /**
   * \param scalar_mix: Accumulate only the coverage alpha per texel. Valid when every dab of the
   * stroke paints one constant color per target (erase), so the color part of the coverage is
   * that color times the alpha and can be rebuilt on demand.
   */
  explicit MaterialStrokeAccum(const bool scalar_mix = false) : scalar_mix_(scalar_mix) {}

  bool scalar_mix() const
  {
    return scalar_mix_;
  }

  /**
   * The block covering (\a block_x, \a block_y) of \a buffer, allocating it on first use.
   *
   * \note Only the allocation is serialized. Callers then write disjoint texels of the returned
   * block in parallel, which is safe for all three arrays above.
   */
  Block &block_ensure(const ImBuf *buffer, const int block_x, const int block_y)
  {
    const Key key{buffer, block_x, block_y};
    /* High bits of the multiplicative hash, so neighboring blocks of one image land in different
     * shards. */
    Shard &shard = shards_[(key.hash() >> 40) & (shard_num - 1)];
    {
      std::lock_guard lock(shard.mutex);
      if (const std::unique_ptr<Block> *found = shard.blocks.lookup_ptr(key)) {
        return **found;
      }
    }
    /* Allocate outside the lock. If another thread inserted the same key meanwhile, that block
     * wins while ours is dropped. */
    std::unique_ptr<Block> fresh = std::make_unique<Block>(scalar_mix_);
    std::lock_guard lock(shard.mutex);
    if (const std::unique_ptr<Block> *found = shard.blocks.lookup_ptr(key)) {
      return **found;
    }
    return *shard.blocks.lookup_or_add(key, std::move(fresh));
  }

 private:
  struct Key {
    const ImBuf *buffer;
    int block_x;
    int block_y;

    bool operator==(const Key &other) const
    {
      return buffer == other.buffer && block_x == other.block_x && block_y == other.block_y;
    }

    uint64_t hash() const
    {
      /* Knuth's multiplicative constant, folded in one field at a time. */
      constexpr uint64_t mul = 0x9E3779B97F4A7C15ull;
      uint64_t value = uint64_t(uintptr_t(buffer));
      value = value * mul + uint64_t(uint32_t(block_x));
      value = value * mul + uint64_t(uint32_t(block_y));
      return value;
    }
  };

  static constexpr int shard_num = 64;
  struct Shard {
    std::mutex mutex;
    Map<Key, std::unique_ptr<Block>> blocks;
  };
  std::array<Shard, shard_num> shards_;
  bool scalar_mix_ = false;
};

}  // namespace blender::ed::sculpt_paint::paint::image
