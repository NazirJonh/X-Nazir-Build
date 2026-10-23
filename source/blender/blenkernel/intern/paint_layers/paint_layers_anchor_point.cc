/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Anchor Point: the rules of rank, resolution and participation. One module so the generator, the
 * CPU composite, the bake and the GPU ask the same questions and cannot disagree about whether a
 * reader takes part.
 */

#include <algorithm>

#include "BLI_hash.hh"
#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"

#include "BKE_paint_layers.hh"

#include "paint_layers_intern.hh"
#include "paint_layers_kind.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Build order
 * \{ */

/**
 * Walk \a list in the order the generator builds it: children of a folder before the folder, a row
 * before its effects, its effects before its mask stack. \a next is the next free position;
 * \a result receives the position of \a marker.
 */
static void anchor_point_rank_walk(const ListBaseT<MaterialPaintLayer> &list,
                                   const bUUID &marker,
                                   int &next,
                                   int &result)
{
  for (const MaterialPaintLayer &row : list) {
    if (BKE_paint_layers_is_folder(row)) {
      anchor_point_rank_walk(row.children, marker, next, result);
    }
    const int row_rank = next++;
    if (!BLI_uuid_is_nil(marker) && BLI_uuid_equal(row.marker, marker)) {
      result = row_rank;
    }
    for (const MaterialPaintLayer &effect : row.effects) {
      const int rank = next++;
      if (!BLI_uuid_is_nil(marker) && BLI_uuid_equal(effect.marker, marker)) {
        result = rank;
      }
    }
    for (const MaterialPaintLayer &item : row.mask_stack) {
      const int rank = next++;
      if (!BLI_uuid_is_nil(marker) && BLI_uuid_equal(item.marker, marker)) {
        result = rank;
      }
    }
  }
}

int paint_layer_anchor_point_rank(const Material &ma, const bUUID &marker)
{
  int next = 0;
  int result = -1;
  anchor_point_rank_walk(ma.paint_layers, marker, next, result);
  return result;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Resolution
 * \{ */

/** Which list of the material holds \a point; the list decides what the point publishes. */
static bool anchor_point_list_find(const Material &ma,
                                   const MaterialPaintLayer &point,
                                   const MaterialPaintLayer **r_owner,
                                   PaintLayerAnchorPointTarget &r_target)
{
  bool found = false;
  BKE_paint_layers_foreach(
      ma, [&](const MaterialPaintLayer &row, const MaterialPaintLayer * /*parent*/) -> bool {
        for (const MaterialPaintLayer &effect : row.effects) {
          if (&effect == &point) {
            *r_owner = &row;
            r_target = PaintLayerAnchorPointTarget::Content;
            found = true;
            return false;
          }
        }
        for (const MaterialPaintLayer &item : row.mask_stack) {
          if (&item == &point) {
            *r_owner = &row;
            r_target = PaintLayerAnchorPointTarget::Mask;
            found = true;
            return false;
          }
        }
        return true;
      });
  return found;
}

/** The row that owns \a reader when it is a correction or mask item, otherwise null. */
static const MaterialPaintLayer *anchor_point_reader_owner(const Material &ma,
                                                           const MaterialPaintLayer &reader)
{
  if (reader.role == MA_PAINT_LAYER_ROLE_LAYER) {
    return nullptr;
  }
  const MaterialPaintLayer *owner = nullptr;
  BKE_paint_layers_foreach(
      ma, [&](const MaterialPaintLayer &row, const MaterialPaintLayer * /*parent*/) -> bool {
        for (const MaterialPaintLayer &effect : row.effects) {
          if (&effect == &reader) {
            owner = &row;
            return false;
          }
        }
        for (const MaterialPaintLayer &item : row.mask_stack) {
          if (&item == &reader) {
            owner = &row;
            return false;
          }
        }
        return true;
      });
  return owner;
}

static bool anchor_point_lookup(const Material &ma,
                                const MaterialPaintLayer &reader,
                                const bUUID &marker,
                                PaintLayerAnchorPointRef &r_ref);

/** Whether \a reader names a marker that resolves at all; an invalid edge is not part of the
 * graph the depth and cycle walks follow. */
static bool anchor_point_reader_has_slice(const Material &ma, const MaterialPaintLayer &reader)
{
  if (!paint_layer_kind_reads_anchor_point(reader)) {
    return false;
  }
  PaintLayerAnchorPointRef ref;
  return anchor_point_lookup(ma, reader, reader.anchor_point_source, ref) &&
         ref.source_row != nullptr;
}

/** The readers of the slice of \a reader, without rank checks: the graph the depth walk follows. */
static void anchor_point_slice_readers(const Material &ma,
                                       const MaterialPaintLayer &source_row,
                                       const PaintLayerAnchorPointTarget target,
                                       const MaterialPaintLayer *stop_before,
                                       Vector<const MaterialPaintLayer *> &r_readers);

static void anchor_point_row_readers(const Material &ma,
                                     const MaterialPaintLayer &row,
                                     Vector<const MaterialPaintLayer *> &r_readers)
{
  if (anchor_point_reader_has_slice(ma, row)) {
    r_readers.append(&row);
  }
  for (const MaterialPaintLayer &effect : row.effects) {
    if (anchor_point_reader_has_slice(ma, effect)) {
      r_readers.append(&effect);
    }
  }
  for (const MaterialPaintLayer &item : row.mask_stack) {
    if (anchor_point_reader_has_slice(ma, item)) {
      r_readers.append(&item);
    }
  }
  for (const MaterialPaintLayer &child : row.children) {
    anchor_point_row_readers(ma, child, r_readers);
  }
}

static void anchor_point_slice_readers(const Material &ma,
                                       const MaterialPaintLayer &source_row,
                                       const PaintLayerAnchorPointTarget target,
                                       const MaterialPaintLayer *stop_before,
                                       Vector<const MaterialPaintLayer *> &r_readers)
{
  if (target == PaintLayerAnchorPointTarget::Mask) {
    /* A mask slice is the mask stack alone: the row's content, corrections and children do not
     * feed it, and counting them would invent cycles and depth the mask does not have. */
    for (const MaterialPaintLayer &item : source_row.mask_stack) {
      if (stop_before != nullptr && &item == stop_before) {
        break;
      }
      if (anchor_point_reader_has_slice(ma, item)) {
        r_readers.append(&item);
      }
    }
    return;
  }
  /* A content slice contains the source row's own source, its corrections above the marker and,
   * for a folder, every child: a reader anywhere in there is part of the chain. */
  if (anchor_point_reader_has_slice(ma, source_row)) {
    r_readers.append(&source_row);
  }
  for (const MaterialPaintLayer &effect : source_row.effects) {
    if (stop_before != nullptr && &effect == stop_before) {
      break;
    }
    if (anchor_point_reader_has_slice(ma, effect)) {
      r_readers.append(&effect);
    }
  }
  for (const MaterialPaintLayer &child : source_row.children) {
    anchor_point_row_readers(ma, child, r_readers);
  }
}

/**
 * Follow the reader chain, looking for a loop and measuring its depth. The rank rules must not be
 * used here: a loop and a forward reference look alike from one edge, and a loop has to be
 * reported as such.
 */
static bool anchor_point_lookup(const Material &ma,
                                const MaterialPaintLayer &reader,
                                const bUUID &marker,
                                PaintLayerAnchorPointRef &r_ref);

static bool anchor_point_chain_walk(const Material &ma,
                                    const MaterialPaintLayer &reader,
                                    Vector<bUUID> &path,
                                    int depth,
                                    int &r_max_depth,
                                    bool &r_cycle)
{
  for (const bUUID &visited : path) {
    if (BLI_uuid_equal(visited, reader.marker)) {
      r_cycle = true;
      return false;
    }
  }
  path.append(reader.marker);
  r_max_depth = std::max(r_max_depth, depth);
  PaintLayerAnchorPointRef ref;
  if (!anchor_point_lookup(ma, reader, reader.anchor_point_source, ref) ||
      ref.source_row == nullptr)
  {
    path.pop_last();
    return true;
  }
  Vector<const MaterialPaintLayer *> readers;
  anchor_point_slice_readers(ma, *ref.source_row, ref.target, ref.point, readers);
  for (const MaterialPaintLayer *nested : readers) {
    if (!anchor_point_chain_walk(ma, *nested, path, depth + 1, r_max_depth, r_cycle)) {
      return false;
    }
  }
  path.pop_last();
  return true;
}

/** The marker lookup and the list it sits in, without the order rules of #paint_layer_anchor_point_resolve. */
static bool anchor_point_lookup(const Material &ma,
                                const MaterialPaintLayer &reader,
                                const bUUID &marker,
                                PaintLayerAnchorPointRef &r_ref)
{
  if (!paint_layer_kind_reads_anchor_point(reader) || BLI_uuid_is_nil(marker)) {
    return false;
  }
  const MaterialPaintLayer *point = const_cast<MaterialPaintLayer *>(
      BKE_paint_layers_find(const_cast<Material &>(ma), marker));
  /* Only a marker publishes a slice: any other element would be sliced "before itself" and would
   * not block its row from being substituted by a cache. */
  if (point == nullptr || !paint_layer_kind_is_anchor_point_marker(*point)) {
    return false;
  }
  const MaterialPaintLayer *source_row = nullptr;
  PaintLayerAnchorPointTarget target = PaintLayerAnchorPointTarget::None;
  if (!anchor_point_list_find(ma, *point, &source_row, target) || source_row == nullptr) {
    return false;
  }
  r_ref.point = point;
  r_ref.source_row = source_row;
  r_ref.target = target;
  r_ref.issue = PaintLayerAnchorPointIssue::None;
  r_ref.rank = paint_layer_anchor_point_rank(ma, marker);
  return true;
}

PaintLayerAnchorPointRef paint_layer_anchor_point_resolve(const Material &ma,
                                                          const MaterialPaintLayer &reader)
{
  PaintLayerAnchorPointRef ref;
  if (!anchor_point_lookup(ma, reader, reader.anchor_point_source, ref)) {
    return ref;
  }

  /* Within the reader's own row the generator builds every effect before the mask stack, so a
   * correction cannot read a mask point of that same row even when the point sits above it in the
   * list. Checked before the rank, which would call it a forward reference. */
  if (reader.role == MA_PAINT_LAYER_ROLE_EFFECT &&
      ref.target == PaintLayerAnchorPointTarget::Mask &&
      anchor_point_reader_owner(ma, reader) == ref.source_row)
  {
    ref.issue = PaintLayerAnchorPointIssue::OrderInRow;
    return ref;
  }

  /* A forward reference is the common mistake and must read as one: check the order before walking
   * the chain, since a point above the reader also sits in the range the chain walk would follow. */
  const int reader_rank = paint_layer_anchor_point_rank(ma, reader.marker);
  if (reader_rank < 0) {
    ref.issue = PaintLayerAnchorPointIssue::NotFound;
    return ref;
  }
  if (ref.rank >= reader_rank) {
    ref.issue = PaintLayerAnchorPointIssue::Forward;
    return ref;
  }

  /* A loop is a property of the graph, not of one edge: check it before the remaining rules. */
  {
    Vector<bUUID> path;
    int max_depth = 0;
    bool cycle = false;
    anchor_point_chain_walk(ma, reader, path, 1, max_depth, cycle);
    if (cycle) {
      ref.issue = PaintLayerAnchorPointIssue::Cycle;
      return ref;
    }
    /* Depths up to the hard limit are valid references; their evaluation mode decides whether
     * they stay live (Never), move to the baked slice (Auto/Always) or warn. Only a chain that
     * would explode the graph is an error. */
    if (max_depth > PAINT_LAYER_ANCHOR_POINT_DEPTH_HARD) {
      ref.issue = PaintLayerAnchorPointIssue::ChainTooDeep;
      return ref;
    }
  }

  return ref;
}

bool paint_layer_anchor_point_source_valid(const Material &ma,
                                           const MaterialPaintLayer &reader,
                                           const bUUID &marker)
{
  PaintLayerAnchorPointRef ref;
  if (!anchor_point_lookup(ma, reader, marker, ref) || ref.source_row == nullptr) {
    return false;
  }
  if (reader.role == MA_PAINT_LAYER_ROLE_EFFECT &&
      ref.target == PaintLayerAnchorPointTarget::Mask &&
      anchor_point_reader_owner(ma, reader) == ref.source_row)
  {
    return false;
  }
  const int reader_rank = paint_layer_anchor_point_rank(ma, reader.marker);
  if (reader_rank < 0 || ref.rank >= reader_rank) {
    return false;
  }
  /* The readers inside the candidate slice start the walk with this reader already on the path,
   * so a slice that reads the reader back is a loop. */
  Vector<const MaterialPaintLayer *> nested_readers;
  anchor_point_slice_readers(ma, *ref.source_row, ref.target, ref.point, nested_readers);
  Vector<bUUID> path;
  path.append(reader.marker);
  int max_depth = 1;
  bool cycle = false;
  for (const MaterialPaintLayer *nested : nested_readers) {
    if (!anchor_point_chain_walk(ma, *nested, path, 2, max_depth, cycle)) {
      break;
    }
  }
  return !cycle && max_depth <= PAINT_LAYER_ANCHOR_POINT_DEPTH_MAX;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Slice description hash
 * \{ */

/**
 * Fold \a value into \a h, order sensitive. Not #get_default_hash(h, value): that XORs its
 * arguments, so reordering two corrections above a marker, or two equal ones, would leave the hash
 * of the slice unchanged and a cache of it would survive an edit that changes its pixels.
 */
static uint64_t anchor_point_hash_mix(const uint64_t h, const uint64_t value)
{
  return h ^ (value + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2));
}

static uint64_t anchor_point_hash_uuid(const uint64_t h, const bUUID &uuid)
{
  return anchor_point_hash_mix(h, UUID(uuid).hash());
}

/**
 * Hash \a row's description. \a top is the slice's source row: its own visibility, opacity, blend
 * and object mask are not part of the slice, so scrubbing them must not invalidate a cache. A nested
 * reader folds in the hash of the slice it reads, so a change deeper in the chain reaches this one.
 */
static uint64_t anchor_point_hash_row(uint64_t h,
                                      const Material &ma,
                                      const MaterialPaintLayer &row,
                                      const MaterialPaintLayer *stop_content,
                                      const MaterialPaintLayer *stop_mask,
                                      const bool top,
                                      const int depth)
{
  h = anchor_point_hash_mix(h, get_default_hash(int(row.source), int(row.kind_subtype), int(row.role)));
  if (!top) {
    h = anchor_point_hash_mix(
        h, get_default_hash(int(row.blend), int(row.flag), row.opacity, int(row.object_mask_flag)));
  }
  if (paint_layer_kind_reads_anchor_point(row)) {
    h = anchor_point_hash_uuid(h, row.anchor_point_source);
    if (depth < PAINT_LAYER_ANCHOR_POINT_DEPTH_MAX + 4) {
      /* The slice this reader reads is part of what it lays. */
      const PaintLayerAnchorPointRef nested = paint_layer_anchor_point_resolve(ma, row);
      if (nested.issue == PaintLayerAnchorPointIssue::None && nested.source_row != nullptr &&
          nested.source_row != &row)
      {
        const bool nested_content = nested.target == PaintLayerAnchorPointTarget::Content;
        h = anchor_point_hash_row(h,
                                  ma,
                                  *nested.source_row,
                                  nested_content ? nested.point : nullptr,
                                  nested_content ? nullptr : nested.point,
                                  true,
                                  depth + 1);
      }
    }
  }
  for (int i = 0; i < row.channels_num; i++) {
    const MaterialPaintLayerChannel &record = row.channels[i];
    h = anchor_point_hash_mix(h,
                              get_default_hash(int(record.channel),
                                               int(record.state),
                                               int(record.flag),
                                               record.image != nullptr ?
                                                   record.image->id.session_uid :
                                                   0));
    h = anchor_point_hash_mix(
        h, get_default_hash(record.value[0], record.value[1], record.value[2], record.value[3]));
  }
  h = anchor_point_hash_mix(h,
                            get_default_hash(row.mapping.offset[0],
                                             row.mapping.offset[1],
                                             row.mapping.scale[0],
                                             row.mapping.scale[1],
                                             row.mapping.rotation));
  h = anchor_point_hash_mix(h, get_default_hash(int(row.mapping.flag), int(row.mapping.wrap)));
  /* What a filter or a map-coverage operator is set to, and what a Material row reads: values the
   * description of the slice carries, though they hold no pixels. */
  if (const PaintLayerKindInfo *kind = paint_layer_kind_of(row)) {
    for (const int i : IndexRange(kind->params_num)) {
      h = anchor_point_hash_mix(h, get_default_hash(paint_layer_kind_param_get(row, i)));
    }
  }
  for (const MaterialPaintLayerIdColor &color : row.id_colors) {
    h = anchor_point_hash_mix(
        h,
        get_default_hash(color.color[0], color.color[1], color.color[2], color.tolerance, color.flag));
  }
  h = anchor_point_hash_mix(h, get_default_hash(int(row.mask_source_channels)));
  if (paint_layer_kind_has_flag(row, PAINT_LAYER_KIND_READS_SOURCE)) {
    h = anchor_point_hash_mix(h, row.material != nullptr ? row.material->id.session_uid : 0);
  }
  /* A content slice never reads the mask, a mask slice never reads the corrections or children.
   * At the top the point that cuts the slice names which half is read; a nested row is fully
   * inside the slice, so both are. */
  const bool reads_content = !top || stop_mask == nullptr;
  const bool reads_mask = !top || stop_content == nullptr;
  if (reads_content) {
    for (const MaterialPaintLayer &effect : row.effects) {
      if (stop_content != nullptr && &effect == stop_content) {
        break;
      }
      h = anchor_point_hash_row(h, ma, effect, nullptr, nullptr, false, depth);
    }
    for (const MaterialPaintLayer &child : row.children) {
      h = anchor_point_hash_row(h, ma, child, nullptr, nullptr, false, depth);
    }
  }
  if (reads_mask) {
    for (const MaterialPaintLayer &item : row.mask_stack) {
      if (stop_mask != nullptr && &item == stop_mask) {
        break;
      }
      h = anchor_point_hash_row(h, ma, item, nullptr, nullptr, false, depth);
    }
  }
  return h;
}

uint64_t paint_layer_anchor_point_slice_hash(const Material &ma,
                                             const MaterialPaintLayer &reader,
                                             const int channel)
{
  const PaintLayerAnchorPointRef ref = paint_layer_anchor_point_resolve(ma, reader);
  if (ref.issue != PaintLayerAnchorPointIssue::None || ref.source_row == nullptr) {
    return 0;
  }
  const bool content = ref.target == PaintLayerAnchorPointTarget::Content;
  /* A mask slice is the same grey whichever channel a reader lays it in: its hash must not depend
   * on that channel, or the one cache entry would be rendered once per channel. */
  const int hash_channel = content ? channel : -1;
  const uint64_t h = anchor_point_hash_uuid(
      anchor_point_hash_mix(0, get_default_hash(int(ref.target), hash_channel, int(reader.role))),
      ref.source_row->marker);
  return anchor_point_hash_row(h,
                               ma,
                               *ref.source_row,
                               content ? ref.point : nullptr,
                               content ? nullptr : ref.point,
                               true,
                               0);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Participation and counts
 * \{ */

/**
 * Whether the row \a source takes part in \a channel, as the readers of its own points see it.
 * A correction of the row that reads one of those points is left out: its slice is what this
 * answers for, so counting it would ask itself (and never end). Readers of other rows' points
 * still count, as the order rules keep those acyclic.
 */
static bool anchor_point_source_participates(const Material &ma,
                                             const MaterialPaintLayer &source,
                                             const int channel)
{
  if (paint_layer_channel_contributes(ma, source, paint_layer_channel_find(source, channel)) ||
      paint_layer_adjustment_acts(ma, source, channel) ||
      paint_layer_row_reads_source(ma, source, channel))
  {
    return true;
  }
  for (const MaterialPaintLayer &effect : source.effects) {
    if (paint_layer_kind_reads_anchor_point(effect) &&
        paint_layer_anchor_point_resolve(ma, effect).source_row == &source)
    {
      continue;
    }
    if (paint_layer_effect_lays(ma, effect, channel)) {
      return true;
    }
  }
  return paint_layer_row_reads_anchor_point(ma, source, channel);
}

bool paint_layer_row_reads_anchor_point(const Material &ma,
                                        const MaterialPaintLayer &reader,
                                        const int channel)
{
  if (!paint_layer_kind_reads_anchor_point(reader)) {
    return false;
  }
  const PaintLayerAnchorPointRef ref = paint_layer_anchor_point_resolve(ma, reader);
  if (ref.issue != PaintLayerAnchorPointIssue::None) {
    return false;
  }
  if (paint_layer_channel_filtered(ma, channel)) {
    return false;
  }
  /* The reader's records are its channel switches: a channel it has no live record for is not laid,
   * and so is neither built nor rendered. Everything that asks whether a reader takes part goes
   * through here. */
  if (!paint_layer_channel_live(paint_layer_channel_find(reader, channel))) {
    return false;
  }
  /* A tiled mask slice is not supported: the sampler and the bake are single-tile. A content
   * slice is refused only when a nested mask reader in it has that problem. */
  if (paint_layer_anchor_point_slice_is_udim(ma, reader)) {
    return false;
  }
  /* Always cannot bake a slice that contains an object mask, and it has no live path to fall back
   * to: the reader is told to stop taking part (the issue says why). Auto stays live instead. */
  if (ref.target != PaintLayerAnchorPointTarget::None &&
      BKE_paint_layers_bake_mode_get(reader) == MA_PAINT_LAYER_BAKE_ALWAYS &&
      paint_layer_anchor_point_slice_has_object_mask(ma, reader))
  {
    return false;
  }
  switch (ref.target) {
    case PaintLayerAnchorPointTarget::None:
      return false;
    case PaintLayerAnchorPointTarget::Mask:
      /* A mask slice read as content is a grey, and a tangent normal has no grey. */
      return channel != PAINT_MATERIAL_CHANNEL_NORMAL;
    case PaintLayerAnchorPointTarget::Content:
      /* The slice exists in every channel its row takes part in. */
      return anchor_point_source_participates(ma, *ref.source_row, channel);
  }
  return false;
}

int paint_layer_anchor_point_mask_read_channel(const Material &ma,
                                               const MaterialPaintLayer &reader)
{
  const PaintLayerAnchorPointRef ref = paint_layer_anchor_point_resolve(ma, reader);
  if (ref.issue != PaintLayerAnchorPointIssue::None || ref.source_row == nullptr ||
      ref.target == PaintLayerAnchorPointTarget::Mask)
  {
    return PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  }
  /* A mask reader reading a content slice takes its coverage, which exists in every channel the
   * source row takes part in. Base Color is only the fallback when none does. */
  for (int channel = 0; channel < PAINT_MATERIAL_CHANNEL_NUM; channel++) {
    /* A filtered channel is refused by the reader's own participation, so it cannot be the one. */
    if (!paint_layer_channel_filtered(ma, channel) &&
        paint_layer_row_participates(ma, *ref.source_row, channel))
    {
      return channel;
    }
  }
  return PAINT_MATERIAL_CHANNEL_BASE_COLOR;
}

const char *paint_layer_anchor_point_issue_text(const PaintLayerAnchorPointIssue issue)
{
  switch (issue) {
    case PaintLayerAnchorPointIssue::None:
      return nullptr;
    case PaintLayerAnchorPointIssue::NotFound:
      return "The Anchor Point does not exist; this reader lays nothing";
    case PaintLayerAnchorPointIssue::Forward:
      return "The Anchor Point is built after this reader; move the point lower in the stack";
    case PaintLayerAnchorPointIssue::OrderInRow:
      return "A correction cannot read the mask of its own row's Anchor Point";
    case PaintLayerAnchorPointIssue::Cycle:
      return "The Anchor Points form a loop";
    case PaintLayerAnchorPointIssue::ObjectMaskInSlice:
      return "The slice contains a row with an object mask and cannot be baked";
    case PaintLayerAnchorPointIssue::UdimUnsupported:
      return "A mask Anchor Point is not supported in a UDIM material";
    case PaintLayerAnchorPointIssue::ChainTooDeep:
      return "The chain of Anchor Points is too deep";
    case PaintLayerAnchorPointIssue::LimitExceeded:
      return "The material has more Anchor Points or readers than the live path allows";
  }
  return nullptr;
}

Vector<const MaterialPaintLayer *> paint_layer_anchor_point_readers(const Material &ma,
                                                                   const bUUID &point)
{
  Vector<const MaterialPaintLayer *> readers;
  if (BLI_uuid_is_nil(point)) {
    return readers;
  }
  BKE_paint_layers_foreach(
      ma, [&](const MaterialPaintLayer &row, const MaterialPaintLayer * /*parent*/) -> bool {
        const auto consider = [&](const MaterialPaintLayer &candidate) {
          if (paint_layer_kind_reads_anchor_point(candidate) &&
              BLI_uuid_equal(candidate.anchor_point_source, point))
          {
            readers.append(&candidate);
          }
        };
        consider(row);
        for (const MaterialPaintLayer &effect : row.effects) {
          consider(effect);
        }
        for (const MaterialPaintLayer &item : row.mask_stack) {
          consider(item);
        }
        return true;
      });
  return readers;
}

bool paint_layer_anchor_point_is_read(const Material &ma, const bUUID &marker)
{
  return !paint_layer_anchor_point_readers(ma, marker).is_empty();
}

void paint_layer_anchor_point_counts(const Material &ma, int &r_points, int &r_readers)
{
  r_points = 0;
  r_readers = 0;
  const auto marker_is_used = [&](const MaterialPaintLayer &marker) {
    for (const MaterialPaintLayer *reader : paint_layer_anchor_point_readers(ma, marker.marker)) {
      if ((reader->flag & MA_PAINT_LAYER_ENABLED) == 0) {
        continue;
      }
      if (paint_layer_anchor_point_resolve(ma, *reader).issue ==
              PaintLayerAnchorPointIssue::None &&
          !paint_layer_anchor_point_slice_is_udim(ma, *reader))
      {
        return true;
      }
    }
    return false;
  };
  BKE_paint_layers_foreach(
      ma, [&](const MaterialPaintLayer &row, const MaterialPaintLayer * /*parent*/) -> bool {
        const auto consider = [&](const MaterialPaintLayer &candidate) {
          if ((candidate.flag & MA_PAINT_LAYER_ENABLED) == 0) {
            return;
          }
          if (paint_layer_kind_is_anchor_point_marker(candidate)) {
            if (marker_is_used(candidate)) {
              r_points++;
            }
          }
          else if (paint_layer_kind_reads_anchor_point(candidate)) {
            const PaintLayerAnchorPointRef ref = paint_layer_anchor_point_resolve(ma, candidate);
            if (ref.issue == PaintLayerAnchorPointIssue::None &&
                !paint_layer_anchor_point_slice_is_udim(ma, candidate))
            {
              r_readers++;
            }
          }
        };
        consider(row);
        for (const MaterialPaintLayer &effect : row.effects) {
          consider(effect);
        }
        for (const MaterialPaintLayer &item : row.mask_stack) {
          consider(item);
        }
        return true;
      });
}

/** Whether \a row or anything below it holds a point that at least one reader names. */
static bool anchor_point_row_blocks(const Material &ma, const MaterialPaintLayer &row)
{
  const auto read = [&](const MaterialPaintLayer &candidate) {
    return paint_layer_kind_is_anchor_point_marker(candidate) &&
           paint_layer_anchor_point_is_read(ma, candidate.marker);
  };
  if (read(row)) {
    return true;
  }
  for (const MaterialPaintLayer &effect : row.effects) {
    if (read(effect)) {
      return true;
    }
  }
  for (const MaterialPaintLayer &item : row.mask_stack) {
    if (read(item)) {
      return true;
    }
  }
  for (const MaterialPaintLayer &child : row.children) {
    if (anchor_point_row_blocks(ma, child)) {
      return true;
    }
  }
  return false;
}

bool paint_layer_anchor_point_blocks_bake(const Material &ma, const MaterialPaintLayer &layer)
{
  return anchor_point_row_blocks(ma, layer);
}

static bool anchor_point_slice_has_object_mask_walk(const Material &ma,
                                                    const MaterialPaintLayer &reader,
                                                    Vector<bUUID> &visited);

/** Whether a nested reader's own slice brings an object mask into the current slice. */
static bool anchor_point_nested_slice_has_object_mask(const Material &ma,
                                                      const MaterialPaintLayer &row,
                                                      Vector<bUUID> &visited)
{
  return paint_layer_kind_reads_anchor_point(row) &&
         anchor_point_slice_has_object_mask_walk(ma, row, visited);
}

/** Scan a row that is fully inside the slice: itself, its corrections, mask and children. */
static bool anchor_point_row_tree_has_object_mask(const Material &ma,
                                                  const MaterialPaintLayer &row,
                                                  Vector<bUUID> &visited)
{
  if ((row.object_mask_flag & MA_PAINT_LAYER_OBJECT_MASK_ACTIVE) != 0) {
    return true;
  }
  if (anchor_point_nested_slice_has_object_mask(ma, row, visited)) {
    return true;
  }
  for (const MaterialPaintLayer &effect : row.effects) {
    if ((effect.object_mask_flag & MA_PAINT_LAYER_OBJECT_MASK_ACTIVE) != 0 ||
        anchor_point_nested_slice_has_object_mask(ma, effect, visited))
    {
      return true;
    }
  }
  for (const MaterialPaintLayer &item : row.mask_stack) {
    if ((item.object_mask_flag & MA_PAINT_LAYER_OBJECT_MASK_ACTIVE) != 0 ||
        anchor_point_nested_slice_has_object_mask(ma, item, visited))
    {
      return true;
    }
  }
  for (const MaterialPaintLayer &child : row.children) {
    if (anchor_point_row_tree_has_object_mask(ma, child, visited)) {
      return true;
    }
  }
  return false;
}

static bool anchor_point_slice_has_object_mask_walk(const Material &ma,
                                                    const MaterialPaintLayer &reader,
                                                    Vector<bUUID> &visited)
{
  for (const bUUID &seen : visited) {
    if (BLI_uuid_equal(seen, reader.marker)) {
      return false;
    }
  }
  visited.append(reader.marker);
  const PaintLayerAnchorPointRef ref = paint_layer_anchor_point_resolve(ma, reader);
  bool found = false;
  if (ref.issue == PaintLayerAnchorPointIssue::None && ref.source_row != nullptr) {
    if (ref.target == PaintLayerAnchorPointTarget::Mask) {
      for (const MaterialPaintLayer &item : ref.source_row->mask_stack) {
        if (&item == ref.point) {
          break;
        }
        if ((item.object_mask_flag & MA_PAINT_LAYER_OBJECT_MASK_ACTIVE) != 0 ||
            anchor_point_nested_slice_has_object_mask(ma, item, visited))
        {
          found = true;
          break;
        }
      }
    }
    else {
      /* The source row's own object mask is applied after the slice. Its content (a reader source,
       * the corrections above the marker, and the children) is inside it. */
      if (anchor_point_nested_slice_has_object_mask(ma, *ref.source_row, visited)) {
        found = true;
      }
      if (!found) {
        for (const MaterialPaintLayer &effect : ref.source_row->effects) {
          if (&effect == ref.point) {
            break;
          }
          if ((effect.object_mask_flag & MA_PAINT_LAYER_OBJECT_MASK_ACTIVE) != 0 ||
              anchor_point_nested_slice_has_object_mask(ma, effect, visited))
          {
            found = true;
            break;
          }
        }
      }
      if (!found) {
        for (const MaterialPaintLayer &child : ref.source_row->children) {
          if (anchor_point_row_tree_has_object_mask(ma, child, visited)) {
            found = true;
            break;
          }
        }
      }
    }
  }
  visited.pop_last();
  return found;
}

bool paint_layer_anchor_point_slice_has_object_mask(const Material &ma,
                                                    const MaterialPaintLayer &reader)
{
  Vector<bUUID> visited;
  return anchor_point_slice_has_object_mask_walk(ma, reader, visited);
}

/**
 * Whether \a row's mask map is a tiled image. A mask item reads one channel: Base Color for a
 * painted/fill map, or #paint_layer_mask_source_channel for a Material item. Other channel records
 * on the item must not trip UDIM.
 */
static bool anchor_point_row_has_tiled_image(const MaterialPaintLayer &row)
{
  const int channel = (row.source == MA_PAINT_LAYER_SOURCE_MATERIAL) ?
                          paint_layer_mask_source_channel(row) :
                          PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  for (int i = 0; i < row.channels_num; i++) {
    if (row.channels[i].channel != channel) {
      continue;
    }
    const Image *image = row.channels[i].image;
    if (image != nullptr && image->source == IMA_SRC_TILED) {
      return true;
    }
  }
  return false;
}

static bool anchor_point_slice_is_udim_walk(const Material &ma,
                                            const MaterialPaintLayer &reader,
                                            Vector<bUUID> &visited);

/** Whether \a row or a nested reader in its content slice contributes a mask slice with a tiled
 * map. A content slice's own tiled maps are not an issue: only a mask slice is single-tile. */
static bool anchor_point_content_slice_has_nested_mask_udim(const Material &ma,
                                                             const MaterialPaintLayer &source_row,
                                                             const MaterialPaintLayer *stop_before,
                                                             Vector<bUUID> &visited)
{
  if (paint_layer_kind_reads_anchor_point(source_row) &&
      anchor_point_slice_is_udim_walk(ma, source_row, visited))
  {
    return true;
  }
  for (const MaterialPaintLayer &effect : source_row.effects) {
    if (stop_before != nullptr && &effect == stop_before) {
      break;
    }
    if (paint_layer_kind_reads_anchor_point(effect) &&
        anchor_point_slice_is_udim_walk(ma, effect, visited))
    {
      return true;
    }
  }
  for (const MaterialPaintLayer &child : source_row.children) {
    /* A child is fully inside the content slice, including its mask stack (as object-mask walks
     * do): a mask reader with a tiled map there must trip UDIM. */
    if (anchor_point_content_slice_has_nested_mask_udim(ma, child, nullptr, visited)) {
      return true;
    }
    for (const MaterialPaintLayer &item : child.mask_stack) {
      if (paint_layer_kind_reads_anchor_point(item) &&
          anchor_point_slice_is_udim_walk(ma, item, visited))
      {
        return true;
      }
    }
  }
  return false;
}

/**
 * Whether a slice depends on a tiled map. A mask slice reads only the elements of the source
 * row's `mask_stack` up to the point -- the base included -- and the slices nested readers among
 * them read; the row's content maps never reach it. A content slice read by a nested reader does
 * read its own source's content, so that is followed too.
 */
static bool anchor_point_slice_is_udim_walk(const Material &ma,
                                            const MaterialPaintLayer &reader,
                                            Vector<bUUID> &visited)
{
  for (const bUUID &seen : visited) {
    if (BLI_uuid_equal(seen, reader.marker)) {
      return false;
    }
  }
  visited.append(reader.marker);
  const PaintLayerAnchorPointRef ref = paint_layer_anchor_point_resolve(ma, reader);
  bool found = false;
  if (ref.issue == PaintLayerAnchorPointIssue::None && ref.source_row != nullptr) {
    if (ref.target == PaintLayerAnchorPointTarget::Mask) {
      for (const MaterialPaintLayer &item : ref.source_row->mask_stack) {
        if (&item == ref.point) {
          break;
        }
        if (anchor_point_row_has_tiled_image(item)) {
          found = true;
          break;
        }
        if (paint_layer_kind_reads_anchor_point(item) &&
            anchor_point_slice_is_udim_walk(ma, item, visited))
        {
          found = true;
          break;
        }
      }
    }
    else if (ref.target == PaintLayerAnchorPointTarget::Content) {
      found = anchor_point_content_slice_has_nested_mask_udim(
          ma, *ref.source_row, ref.point, visited);
    }
  }
  visited.pop_last();
  return found;
}

bool paint_layer_anchor_point_slice_is_udim(const Material &ma,
                                            const MaterialPaintLayer &reader)
{
  const PaintLayerAnchorPointRef ref = paint_layer_anchor_point_resolve(ma, reader);
  if (ref.issue != PaintLayerAnchorPointIssue::None || ref.source_row == nullptr) {
    return false;
  }
  Vector<bUUID> visited;
  return anchor_point_slice_is_udim_walk(ma, reader, visited);
}

int paint_layer_anchor_point_chain_depth(const Material &ma, const MaterialPaintLayer &reader)
{
  Vector<bUUID> path;
  int max_depth = 0;
  bool cycle = false;
  anchor_point_chain_walk(ma, reader, path, 1, max_depth, cycle);
  return max_depth;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Reference edits
 * \{ */

bUUID BKE_paint_layers_anchor_point_source_get(const MaterialPaintLayer &reader)
{
  return paint_layer_kind_reads_anchor_point(reader) ? reader.anchor_point_source : bUUID{};
}

bool BKE_paint_layers_is_anchor_point(const MaterialPaintLayer &row)
{
  return paint_layer_kind_is_anchor_point_marker(row);
}

bool BKE_paint_layers_reads_anchor_point(const MaterialPaintLayer &row)
{
  return paint_layer_kind_reads_anchor_point(row);
}

bool BKE_paint_layers_anchor_point_source_set(Material &ma,
                                              MaterialPaintLayer *reader,
                                              const bUUID &marker)
{
  if (reader == nullptr || !paint_layer_kind_reads_anchor_point(*reader)) {
    return false;
  }
  /* Owned by the material: a row, a content correction or a mask item of it. */
  if (BKE_paint_layers_find(ma, reader->marker) != reader) {
    return false;
  }
  /* Writing the value it already has is not an edit, even for a reference that no longer
   * resolves. */
  if (BLI_uuid_equal(reader->anchor_point_source, marker)) {
    return true;
  }
  if (!BLI_uuid_is_nil(marker)) {
    /* Only a marker publishes a slice: a correction or a mask item has a marker of its own, but it
     * names an element to edit, not a slice to read. Refuse it before it becomes a permanent
     * NotFound issue. */
    const MaterialPaintLayer *point = BKE_paint_layers_find(ma, marker);
    if (point == nullptr || !paint_layer_kind_is_anchor_point_marker(*point)) {
      return false;
    }
  }
  reader->anchor_point_source = marker;
  /* A new target may make an old bake describe a different slice. Release the ones that no longer
   * resolve; a resolver-valid but changed target is discarded by the planner's hash. */
  paint_layer_anchor_point_release_broken_bakes(ma);
  BKE_paint_layers_tag_edited(ma);
  return true;
}

bool BKE_paint_layers_object_mask_role_allowed(const MaterialPaintLayer &layer)
{
  return layer.role == MA_PAINT_LAYER_ROLE_LAYER;
}

/** \} */

}  // namespace blender
