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
#include "BLI_listbase.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "DNA_material_types.h"

#include "BKE_paint_layers.hh"

#include "paint_layers_intern.hh"

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
  if (paint_layer_kind_reads_anchor_point(row)) {
    r_readers.append(&row);
  }
  for (const MaterialPaintLayer &effect : row.effects) {
    if (paint_layer_kind_reads_anchor_point(effect)) {
      r_readers.append(&effect);
    }
  }
  for (const MaterialPaintLayer &item : row.mask_stack) {
    if (paint_layer_kind_reads_anchor_point(item)) {
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
  /* A content slice contains the source row's own source, its corrections above the marker and,
   * for a folder, every child: a reader anywhere in there is part of the chain. */
  if (paint_layer_kind_reads_anchor_point(source_row)) {
    r_readers.append(&source_row);
  }
  for (const MaterialPaintLayer &effect : source_row.effects) {
    if (stop_before != nullptr && &effect == stop_before) {
      break;
    }
    if (paint_layer_kind_reads_anchor_point(effect)) {
      r_readers.append(&effect);
    }
  }
  for (const MaterialPaintLayer &child : source_row.children) {
    anchor_point_row_readers(ma, child, r_readers);
  }
  if (target == PaintLayerAnchorPointTarget::Mask) {
    for (const MaterialPaintLayer &item : source_row.mask_stack) {
      if (stop_before != nullptr && &item == stop_before) {
        break;
      }
      if (paint_layer_kind_reads_anchor_point(item)) {
        r_readers.append(&item);
      }
    }
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
    if (max_depth > PAINT_LAYER_ANCHOR_POINT_DEPTH_MAX) {
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
  /* A content slice never reads the mask, a mask slice never reads the corrections or children. */
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
  const uint64_t h = anchor_point_hash_uuid(
      anchor_point_hash_mix(0, get_default_hash(int(ref.target), channel, int(reader.role))),
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
  switch (ref.target) {
    case PaintLayerAnchorPointTarget::None:
      return false;
    case PaintLayerAnchorPointTarget::Mask:
      /* A mask slice read as content is a grey, and a tangent normal has no grey. */
      return channel != PAINT_MATERIAL_CHANNEL_NORMAL;
    case PaintLayerAnchorPointTarget::Content:
      /* The slice exists in every channel its row takes part in. */
      return paint_layer_row_participates(ma, *ref.source_row, channel);
  }
  return false;
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
  BKE_paint_layers_foreach(
      ma, [&](const MaterialPaintLayer &row, const MaterialPaintLayer * /*parent*/) -> bool {
        const auto consider = [&](const MaterialPaintLayer &candidate) {
          if (paint_layer_kind_is_anchor_point_marker(candidate)) {
            r_points++;
          }
          else if (paint_layer_kind_reads_anchor_point(candidate)) {
            r_readers++;
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

/** Whether \a row or any row below it has an active object mask. */
static bool anchor_point_row_has_object_mask(const MaterialPaintLayer &row)
{
  if ((row.object_mask_flag & MA_PAINT_LAYER_OBJECT_MASK_ACTIVE) != 0) {
    return true;
  }
  for (const MaterialPaintLayer &child : row.children) {
    if (anchor_point_row_has_object_mask(child)) {
      return true;
    }
  }
  return false;
}

bool paint_layer_anchor_point_slice_has_object_mask(const Material &ma,
                                                    const MaterialPaintLayer &reader)
{
  const PaintLayerAnchorPointRef ref = paint_layer_anchor_point_resolve(ma, reader);
  if (ref.issue != PaintLayerAnchorPointIssue::None || ref.source_row == nullptr) {
    return false;
  }
  /* The source row's own object mask is applied after the slice, so it is not part of it. What a
   * folder holds is: the children are composed into the folder's content. */
  for (const MaterialPaintLayer &child : ref.source_row->children) {
    if (anchor_point_row_has_object_mask(child)) {
      return true;
    }
  }
  return false;
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
  if (BLI_uuid_equal(reader->anchor_point_source, marker)) {
    return true;
  }
  reader->anchor_point_source = marker;
  BKE_paint_layers_tag_edited(ma);
  return true;
}

bool BKE_paint_layers_object_mask_role_allowed(const MaterialPaintLayer &layer)
{
  return layer.role == MA_PAINT_LAYER_ROLE_LAYER;
}

/** \} */

}  // namespace blender
