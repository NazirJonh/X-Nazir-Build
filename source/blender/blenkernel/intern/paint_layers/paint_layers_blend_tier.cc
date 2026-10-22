/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The runtime blend tier of the Paint Layers generator.
 *
 * While a blend mode is being scrubbed its channel is built with the shared dynamic group instead
 * of a static Mix node, so changing the mode recompiles no shader. That is only worth its warm-frame
 * cost for a moment: a channel stays dynamic for a short hold after the last edit, then a timer
 * demotes it back to a static Mix node. The state is deliberately not in DNA -- the only persisted
 * trace is the root tree's `pbr_paint_blend_dynamic` dirty bit, which tells a loader that the saved
 * tree was built in the dynamic tier and has to be rebuilt statically (#BKE_paint_layers_blend_load_post).
 */

#include "BKE_paint_layers.hh"

#include "BLI_listbase_iterator.hh"
#include "BLI_map.hh"
#include "BLI_time.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BKE_idprop.hh"
#include "BKE_main.hh"

#include "DNA_ID.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_uuid_types.h"

#include "paint_layers_generate_intern.hh"
#include "paint_layers_intern.hh"

namespace blender {

namespace {

/* TODO: The tier lifecycle below was only checked by logs for a single row, a correction and a
 * non-Pass-Through folder while scrubbing. Not yet verified by hand; if one of these misbehaves,
 * start here:
 * - Undo/redo in the middle of a scrub: #blend_tier_reconcile drops every record and relies on the
 *   persisted #TREE_ROOT_BLEND_DYNAMIC_PROP bit to rebuild a restored dynamic tree statically. A
 *   step restored with the bit stale (regeneration had not run yet at the undo push) would keep
 *   the dynamic group until the next edit.
 * - Saving inside the 2.5 s hold window: the file holds dynamic group instances plus the bit, and
 *   #BKE_paint_layers_blend_load_post must demote them; a loader that skips the callback
 *   (linked or appended material, background run without the editor init) keeps them.
 * - A folder demoted by the timer: seen in logs only up to the last scrub edit, the demotion
 *   itself was never reached (Blender was closed first). */
/** How long a channel stays in the dynamic tier after its last blend edit, in seconds. */
constexpr double kBlendTierHoldSeconds = 2.5;

struct BlendTierEntry {
  bUUID marker;
  int channel;
  /** #BLI_time_now_seconds() value after which the tier's timer demotes the channel. */
  double deadline;
  /**
   * False for the first edit of a burst: the channel keeps its static Mix, so a single blend change
   * costs what it always did. A second edit inside the hold window promotes it to the dynamic group.
   */
  bool dynamic;
};

/**
 * The runtime-only state: per material (by `ID::session_uid`, which survives an undo step unlike a
 * pointer) the channels currently built with the dynamic group.
 *
 * `session_uid` is the key rather than the `Material` pointer because a record may outlive the
 * material: undo and file loads replace the DNA, and a stale pointer would tag freed memory. The
 * demotion resolves the uid back to a material in `Main` and drops records whose material is gone.
 */
Map<uint32_t, Vector<BlendTierEntry>> &blend_tiers()
{
  static Map<uint32_t, Vector<BlendTierEntry>> tiers;
  return tiers;
}

BlendTierEntry *entry_find(Vector<BlendTierEntry> &entries,
                           const bUUID marker,
                           const int channel)
{
  for (BlendTierEntry &entry : entries) {
    if (entry.channel == channel && BLI_uuid_equal(entry.marker, marker)) {
      return &entry;
    }
  }
  return nullptr;
}

/** Whether \a tree carries the persisted "saved in the dynamic tier" dirty bit. */
bool tree_blend_dynamic_get(bNodeTree &tree)
{
  const IDProperty *props = IDP_GetProperties(&tree.id);
  if (props == nullptr) {
    return false;
  }
  const IDProperty *prop = IDP_GetPropertyTypeFromGroup(
      props, bke::paint_layers::TREE_ROOT_BLEND_DYNAMIC_PROP, IDP_INT);
  return prop != nullptr && IDP_int_get(prop) != 0;
}

/**
 * Forget every record and rebuild the materials whose saved tree carries the dirty bit. A tree
 * written inside the hold window may contain the dynamic group; the tier it came from is gone with
 * the file's runtime state, so the only consistent recovery is a static rebuild.
 */
static void blend_tier_reconcile(Main &bmain)
{
  blend_tiers().clear();
  for (Material &ma : bmain.materials) {
    if (!BKE_paint_layers_is_layered(ma) || ma.paint_layers_tree == nullptr) {
      continue;
    }
    if (tree_blend_dynamic_get(*ma.paint_layers_tree)) {
      BKE_paint_layers_tag_edited(ma);
    }
  }
}

}  // namespace

bool bke::paint_layers::paint_layers_blend_tier_is_dynamic(const Material &ma,
                                                          const bUUID marker,
                                                          const int channel)
{
  /* The deadline is not checked here on purpose: only the timer removes expired records, and in
   * background mode there is no timer, so a channel simply stays dynamic until the next edit. */
  const Vector<BlendTierEntry> *entries = blend_tiers().lookup_ptr(ma.id.session_uid);
  if (entries == nullptr) {
    return false;
  }
  for (const BlendTierEntry &entry : *entries) {
    if (entry.channel == channel && BLI_uuid_equal(entry.marker, marker)) {
      return entry.dynamic;
    }
  }
  return false;
}

void bke::paint_layers::paint_layers_blend_tier_note(const Material &ma,
                                                     const bUUID marker,
                                                     const int channel)
{
  Vector<BlendTierEntry> &entries = blend_tiers().lookup_or_add_default(ma.id.session_uid);
  const double deadline = BLI_time_now_seconds() + kBlendTierHoldSeconds;
  if (BlendTierEntry *entry = entry_find(entries, marker, channel)) {
    /* A repeated edit inside the window is a scrub: promote the channel and push the deadline. */
    entry->deadline = deadline;
    entry->dynamic = true;
    return;
  }
  entries.append(BlendTierEntry{marker, channel, deadline, false});
}

void bke::paint_layers::paint_layers_blend_tier_forget(const Material &ma,
                                                       const bUUID marker,
                                                       const int channel)
{
  Map<uint32_t, Vector<BlendTierEntry>> &tiers = blend_tiers();
  Vector<BlendTierEntry> *entries = tiers.lookup_ptr(ma.id.session_uid);
  if (entries == nullptr) {
    return;
  }
  for (int64_t i = 0; i < entries->size(); i++) {
    const BlendTierEntry &entry = (*entries)[i];
    if (entry.channel == channel && BLI_uuid_equal(entry.marker, marker)) {
      entries->remove(i);
      break;
    }
  }
  if (entries->is_empty()) {
    tiers.remove(ma.id.session_uid);
  }
}

void bke::paint_layers::paint_layers_blend_tier_forget_row(const Material &ma, const bUUID marker)
{
  Map<uint32_t, Vector<BlendTierEntry>> &tiers = blend_tiers();
  Vector<BlendTierEntry> *entries = tiers.lookup_ptr(ma.id.session_uid);
  if (entries == nullptr) {
    return;
  }
  for (int64_t i = entries->size() - 1; i >= 0; i--) {
    if (BLI_uuid_equal((*entries)[i].marker, marker)) {
      entries->remove(i);
    }
  }
  if (entries->is_empty()) {
    tiers.remove(ma.id.session_uid);
  }
}

void bke::paint_layers::paint_layers_blend_tier_prune(Material &ma)
{
  Map<uint32_t, Vector<BlendTierEntry>> &tiers = blend_tiers();
  Vector<BlendTierEntry> *entries = tiers.lookup_ptr(ma.id.session_uid);
  if (entries == nullptr) {
    return;
  }
  for (int64_t i = entries->size() - 1; i >= 0; i--) {
    const BlendTierEntry &entry = (*entries)[i];
    const MaterialPaintLayer *layer = BKE_paint_layers_find(ma, entry.marker);
    if (layer == nullptr || !paint_layer_blend_dynamic_eligible(ma, *layer, entry.channel)) {
      entries->remove(i);
    }
  }
  if (entries->is_empty()) {
    tiers.remove(ma.id.session_uid);
  }
}

double BKE_paint_layers_blend_tier_next_deadline()
{
  double next = -1.0;
  for (const auto item : blend_tiers().items()) {
    for (const BlendTierEntry &entry : item.value) {
      if (next < 0.0 || entry.deadline < next) {
        next = entry.deadline;
      }
    }
  }
  return next;
}

bool BKE_paint_layers_blend_tier_demote_due(Main &bmain, const double now)
{
  Map<uint32_t, Vector<BlendTierEntry>> &tiers = blend_tiers();
  Vector<uint32_t> affected;
  Vector<uint32_t> rebuild;
  for (auto item : tiers.items()) {
    bool removed = false;
    bool removed_dynamic = false;
    Vector<BlendTierEntry> &entries = item.value;
    for (int64_t i = entries.size() - 1; i >= 0; i--) {
      if (entries[i].deadline <= now) {
        removed_dynamic = removed_dynamic || entries[i].dynamic;
        entries.remove(i);
        removed = true;
      }
    }
    if (removed) {
      affected.append(item.key);
    }
    if (removed_dynamic) {
      rebuild.append(item.key);
    }
  }
  for (const uint32_t uid : affected) {
    Vector<BlendTierEntry> *entries = tiers.lookup_ptr(uid);
    if (entries != nullptr && entries->is_empty()) {
      tiers.remove(uid);
    }
    if (!rebuild.contains(uid)) {
      /* Only a pending first edit expired: the graph never left the static Mix. */
      continue;
    }
    /* The material may have been freed since the record was made; a dangling record is simply
     * forgotten, there is no tree left to rebuild. */
    for (Material &ma : bmain.materials) {
      if (ma.id.session_uid != uid || !BKE_paint_layers_is_layered(ma)) {
        continue;
      }
      /* A full regeneration is what rebuilds the channel as a static Mix node; the tier mask is in
       * the layer's topology hash, so the group no longer matches and is rebuilt. */
      BKE_paint_layers_tag_edited(ma);
      break;
    }
  }
  return !affected.is_empty();
}

void BKE_paint_layers_blend_tier_clear()
{
  blend_tiers().clear();
}

void BKE_paint_layers_blend_load_post(Main &bmain)
{
  blend_tier_reconcile(bmain);
}

void BKE_paint_layers_blend_undo_post(Main &bmain)
{
  blend_tier_reconcile(bmain);
}

}  // namespace blender
