/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Weight mask editing sessions for sculpt layer tree nodes (see #SculptLayerMask).
 *
 * A node's mask is stored sparsely and is not something a brush can write to directly. Rather than
 * grow a second mask toolset, a session expands the node's mask into the mesh's standard
 * `.sculpt_mask` attribute and parks the user's own sculpt mask for the duration: the Mask brush,
 * the gesture operators, flood fill, the mask filters, the viewport overlay and its undo steps then
 * all edit the layer's mask with no changes of their own. Closing the session compresses the
 * attribute back onto the node and puts the user's mask back exactly as it was.
 *
 * Both domains are handled here. The mesh (vertex) domain parks the `.sculpt_mask` attribute; the
 * multires grid domain parks #SubdivCCG::masks. The two differ only in where the dense buffer lives
 * and in how a mask is cut into blocks — everything between opening and closing a session is the
 * existing mask toolset either way.
 */

#include <climits>
#include <limits>
#include <string>
#include <utility>

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_workspace_types.h"

#include "BLI_array.hh"
#include "BLI_index_mask.hh"
#include "BLI_listbase_iterator.hh"
#include "BLI_span.hh"

#include "BKE_attribute.hh"
#include "BKE_context.hh"
#include "BKE_main.hh"
#include "BKE_mesh.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_report.hh"
#include "BKE_sculpt_layers.hh"
#include "BKE_subdiv_ccg.hh"

#include "ED_sculpt.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "sculpt_intern.hh"
#include "sculpt_undo.hh"

namespace blender::ed::sculpt_paint::layers {

/** Block size for a mask on the mesh (vertex) domain.
 *
 * File-local and named for its domain on purpose. The grid domain must cut its masks one block per
 * grid (`block_size == grid_area`), and a grid mask cut any other way is *silently ignored* by
 * #bke::sculpt_layers::grid_masks_for_composite rather than rejected — correct-looking code with no
 * crash and no warning. The grid path in this file therefore never mentions this constant: it takes
 * its block size from the live #SubdivCCG::grid_area, and anything later factored out to serve both
 * domains must take the block size as a parameter rather than reach for this one. */
static constexpr int vert_block_size = SCULPT_LAYER_MASK_VERT_BLOCK;

static SculptSession *session_of(Object &object)
{
  return object.runtime->sculpt_session;
}

static Mesh &mesh_of(Object &object)
{
  return *id_cast<Mesh *>(object.data);
}

/**
 * Refresh what the PBVH caches about the mask after the whole buffer was replaced.
 *
 * A session swaps the entire mask buffer in one go, so unlike a brush stroke there is no set of
 * touched nodes to narrow this to: every leaf's fully-masked / fully-unmasked summary can have
 * changed, and those summaries are what the draw code and the automasking read. Without this the
 * viewport keeps drawing the mask the session replaced.
 *
 * #bke::pbvh::update_mask_mesh and #bke::pbvh::update_mask_grids are used rather than their per-node
 * counterparts because both already answer the case this needs and a hand-rolled loop does not: an
 * absent buffer means a mask of zero everywhere, which the per-node functions cannot be handed (they
 * index the span), and both exit paths can leave the buffer absent.
 */
static void mask_buffer_refresh_pbvh(Object &object)
{
  bke::pbvh::Tree *pbvh = bke::object::pbvh_get(object);
  if (pbvh == nullptr) {
    return;
  }
  IndexMaskMemory memory;
  const IndexMask node_mask = bke::pbvh::all_leaf_nodes(*pbvh, memory);
  switch (pbvh->type()) {
    case bke::pbvh::Type::Mesh:
      bke::pbvh::update_mask_mesh(mesh_of(object), node_mask, *pbvh);
      break;
    case bke::pbvh::Type::Grids: {
      const SculptSession *ss = session_of(object);
      if (ss == nullptr || ss->subdiv_ccg == nullptr) {
        return;
      }
      bke::pbvh::update_mask_grids(*ss->subdiv_ccg, node_mask, *pbvh);
      break;
    }
    case bke::pbvh::Type::BMesh:
      /* Dynamic topology carries no sculpt layers, so no session can be open on it. */
      return;
  }
  pbvh->tag_masks_changed(node_mask);
  /* The session boundary swaps which buffer feeds each overlay attribute (the live mask buffer
   * switches between the user's mask and the layer's weights), so both attributes go stale on every
   * leaf even though only one was painted. */
  pbvh->tag_layer_masks_changed(node_mask);
}

/**
 * Invalidate the cached folder chain products that \a node's mask takes part in.
 *
 * Only a *folder* mask is folded into a chain: #bke::sculpt_layers::chain_mask is the product of the
 * masks of every node from the root down to a group inclusive, so it contains group masks alone. A
 * layer's own mask is read straight off the node by #node_mask_for_composite and is never cached
 * anywhere, so editing it invalidates nothing. Editing a folder's mask invalidates that folder's own
 * chain and every chain below it, which is exactly the downward walk #tag_chain_mask_dirty performs
 * — note that the argument is the edited folder itself, not its parent: the parent's chain does not
 * contain this folder's mask and must not be thrown away.
 */
static void tag_masked_chains_dirty(SculptLayerTreeNode &node)
{
  if (SculptLayerGroup *group = bke::sculpt_layers::node_as_group(&node)) {
    bke::sculpt_layers::tag_chain_mask_dirty(*group);
  }
}

int mask_edit_active_uid(const SculptSession &ss)
{
  return ss.layers.mask_edit.node_uid;
}

int mask_edit_active_uid(const Object &object)
{
  const SculptSession *ss = object.runtime->sculpt_session;
  return ss == nullptr ? 0 : mask_edit_active_uid(*ss);
}

MaskLayout mask_layout_for(const bool on_grids,
                           const int verts_num,
                           const int grids_num,
                           const int grid_area)
{
  if (!on_grids) {
    if (verts_num <= 0) {
      return {};
    }
    return {verts_num, SCULPT_LAYER_MASK_VERT_BLOCK};
  }
  if (grids_num <= 0 || grid_area <= 0) {
    return {};
  }
  /* One block per grid. This is the whole contract #grid_masks_for_composite enforces, and it
   * enforces it by *ignoring* a mask cut any other way, so getting it wrong here would produce a
   * mask the user paints and the composite never reads. See #mask_edit_begin_grids, which derives
   * the same layout from the same two numbers when it opens a session. */
  const int64_t totelem = int64_t(grids_num) * grid_area;
  if (totelem > int64_t(std::numeric_limits<int>::max())) {
    /* #SculptLayerMask counts elements in an `int`; a truncated count would silently describe a
     * smaller domain than the one being painted. Unreachable in practice. */
    return {};
  }
  return {int(totelem), grid_area};
}

/**
 * Open a session on the mesh (vertex) domain. The caller has already established that the PBVH is
 * of #bke::pbvh::Type::Mesh and that no session is open.
 *
 * NOTE: `.sculpt_mask` is the persistent store on this domain, not a cache of it, so there is no
 * flush to trace through as there is on the grid domain — the attribute already *is* what gets
 * written to disk. The single writer to protect is therefore #BLO_write_file, which the save,
 * auto-save and startup-file paths bracket with #bke::sculpt_layers::MaskEditSuspendGuard.
 */
static bool mask_edit_begin_mesh(Object &object, SculptSession &ss, SculptLayerTreeNode &node)
{
  Mesh &mesh = mesh_of(object);
  const int totelem = mesh.verts_num;
  if (totelem == 0) {
    return false;
  }

  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  /* Sampled *before* the attribute is looked up or added: #lookup_or_add_for_write_span creates the
   * attribute when it is missing, so a #contains test after it always answers true and the exit path
   * would leave a mask behind on a mesh that never had one — silently changing how every brush
   * behaves from then on. */
  const bool had_vert_mask = attributes.contains(".sculpt_mask");

  bke::SpanAttributeWriter<float> mask = attributes.lookup_or_add_for_write_span<float>(
      ".sculpt_mask", bke::AttrDomain::Point);
  if (!mask) {
    return false;
  }

  Array<float> saved_vert_mask;
  if (had_vert_mask) {
    saved_vert_mask = Array<float>(mask.span.as_span());
  }

  /* A missing mask, and a stale one alike, start the session fully opaque. A stale mask describes a
   * different topology, so there is no meaningful way to expand it over this one; every consumer
   * already ignores it (#is_stale_mask fails open), which means an opaque mask reproduces exactly
   * the surface the user is looking at right now. */
  if (node.mask == nullptr || bke::sculpt_layers::is_stale_mask(*node.mask, totelem)) {
    bke::sculpt_layers::mask_free(node.mask);
    node.mask = bke::sculpt_layers::mask_new(totelem, vert_block_size, 255);
  }
  /* #mask_new only returns null for an empty domain, which the vertex count check above ruled out. */
  BLI_assert(node.mask != nullptr);
  bke::sculpt_layers::mask_expand(*node.mask, mask.span);
  mask.finish();

  ss.layers.mask_edit.node_uid = node.uid;
  ss.layers.mask_edit.on_grids = false;
  ss.layers.mask_edit.had_vert_mask = had_vert_mask;
  ss.layers.mask_edit.saved_vert_mask = std::move(saved_vert_mask);
  /* Set explicitly rather than relied on as the leftover of a previous close: all three fields are
   * only meaningful for a grid-domain session, and the doc comments on
   * #SculptLayerMaskEdit::grid_area, #SculptLayerMaskEdit::grids_num and
   * #SculptLayerMaskEdit::ccg_id all promise zero here. That
   * currently holds only because the whole struct is reset on close; stated explicitly so it
   * survives a future partial reset. */
  ss.layers.mask_edit.grid_area = 0;
  ss.layers.mask_edit.grids_num = 0;
  ss.layers.mask_edit.ccg_id = 0;
  return true;
}

/**
 * Open a session on the multires grid domain. The caller has already established that the PBVH is
 * of #bke::pbvh::Type::Grids and that no session is open.
 *
 * The grid counterpart of the mesh path's `.sculpt_mask` attribute is #SubdivCCG::masks, which is
 * allocated only when the mesh actually carries a mask (see the `need_mask` branch in
 * `subdiv_ccg.cc`), so an absent mask is an empty array rather than a missing attribute.
 *
 * NOTE: the leak this domain has to be protected from is continuous, not occasional. A base flush
 * calls `multiresModifier_reshapeFromCCG(..., MultiresReshapeFromCCGMode::Base)`, which reaches
 * #multires_reshape_assign_final_coords_from_ccg (`multires_reshape_ccg.cc`); that function copies
 * `subdiv_ccg->masks` straight into `grid_element.mask`, and `grid_element.mask` points into the
 * persistent `CD_GRID_PAINT_MASK` layer on the base mesh (`multires_reshape_util.cc`), not into a
 * copy. The session's own restore on exit cannot undo that write, and the next CCG rebuild
 * re-derives `masks` from the now-corrupted base layer.
 *
 * Painting the session's mask does *not* leave the CCG clean: #flush_update_step (`sculpt.cc`)
 * tags multires with `MULTIRES_COORDS_MODIFIED` for every dab on a multires object, before it
 * branches on the update type, so a Mask-brush dab sets `subdiv_ccg->dirty.coords` exactly as a
 * geometry dab does — and so do the mask gestures and flood fill, which call
 * #multires_mark_as_modified themselves (`paint_mask.cc`). The dirty flags are therefore set
 * throughout a grid session by the very brush the session exists to enable, and the flush that
 * consumes them is not a rare event to be caught at a handful of call sites: it also runs from
 * #object_update_from_subsurf_ccg on every depsgraph re-evaluation. Both flush primitives park the
 * session themselves for that reason (see #bke::sculpt_layers::MaskEditSuspendGuard); this session
 * only refuses the two cases it can detect at open and at close (level change, domain mismatch).
 */
static bool mask_edit_begin_grids(SculptSession &ss, SculptLayerTreeNode &node)
{
  SubdivCCG *subdiv_ccg = ss.subdiv_ccg;
  if (subdiv_ccg == nullptr) {
    return false;
  }
  /* A grid-cut mask is only meaningful on a #SCULPT_LAYER_DOMAIN_GRID layer: on a vertex-domain
   * layer #node_mask_for_composite sees `elem_num == verts_num`, calls the mask stale, and ignores
   * it, so a session opened here would let the user paint a mask that silently does nothing. A
   * folder has no domain of its own and must not be refused here; #node_as_layer already returns
   * null for one, which this check relies on to leave folders alone. */
  if (const SculptLayer *layer = bke::sculpt_layers::node_as_layer(&node);
      layer != nullptr && layer->domain != SCULPT_LAYER_DOMAIN_GRID)
  {
    return false;
  }
  /* Read off the live CCG rather than assumed from the modifier's level: these are what
   * #SubdivCCG::masks was actually sized by, and they are the only numbers the block layout below
   * may be derived from. */
  const int grid_area = subdiv_ccg->grid_area;
  const int grids_num = subdiv_ccg->grids_num;
  if (grid_area <= 0 || grids_num <= 0) {
    return false;
  }
  /* #SubdivCCG stores one element per grid point, grids back to back in #grid_area sized chunks,
   * with #masks indexed exactly as #positions is (see #SubdivCCG::positions). That is the whole
   * basis for cutting the node's mask one block per grid below: block `g` then covers the same
   * elements as grid `g`, which is the contract #grid_masks_for_composite enforces. */
  const int64_t totelem = int64_t(grids_num) * grid_area;
  if (totelem > int64_t(std::numeric_limits<int>::max())) {
    /* #SculptLayerMask counts its elements in an `int`. Unreachable in practice — the CCG would be
     * tens of gigabytes first — but the mask would silently describe a truncated domain. */
    return false;
  }

  /* Sampled before the array is materialized below, for the same reason the mesh path samples the
   * attribute before adding it: afterwards the answer is always "yes" and the exit path would leave
   * a mask on a mesh that never had one. */
  const bool had_grid_mask = !subdiv_ccg->masks.is_empty();
  Array<float> saved_grid_mask;
  if (had_grid_mask) {
    if (subdiv_ccg->masks.size() != totelem) {
      /* The CCG disagrees with itself. Refused rather than repaired: whatever produced it is not
       * something this session can reason about, and expanding over it would write out of range. */
      return false;
    }
    saved_grid_mask = Array<float>(subdiv_ccg->masks.as_span());
  }
  else {
    /* Left uninitialized on purpose: #mask_expand writes every one of the `totelem` elements it was
     * just sized to, so a fill would be overwritten in its entirety. */
    subdiv_ccg->masks.reinitialize(totelem);
  }

  /* Three states start the session fully opaque, and the third is the one specific to this domain.
   * A missing mask and a stale one behave as on the mesh path. A mask cut at some other block size
   * is *also* discarded: #grid_masks_for_composite drops a grid mask whose `block_size` is not the
   * grid area, so keeping it would author weights that are silently ignored — no crash, no warning,
   * the layer simply contributing fully. Note that this test cannot stand in for #is_stale_mask and
   * must follow it: #mask_blend_read neutralizes an unusable mask by resetting its block size to
   * #SCULPT_LAYER_MASK_VERT_BLOCK, so `block_size` alone never identifies the domain. */
  if (node.mask == nullptr || bke::sculpt_layers::is_stale_mask(*node.mask, totelem) ||
      node.mask->block_size != grid_area)
  {
    bke::sculpt_layers::mask_free(node.mask);
    node.mask = bke::sculpt_layers::mask_new(int(totelem), grid_area, 255);
  }
  /* #mask_new only returns null for an empty domain, which the grid count check above ruled out. */
  BLI_assert(node.mask != nullptr);
  bke::sculpt_layers::mask_expand(*node.mask, subdiv_ccg->masks.as_mutable_span());

  ss.layers.mask_edit.node_uid = node.uid;
  ss.layers.mask_edit.on_grids = true;
  ss.layers.mask_edit.had_grid_mask = had_grid_mask;
  ss.layers.mask_edit.saved_grid_mask = std::move(saved_grid_mask);
  ss.layers.mask_edit.grid_area = grid_area;
  ss.layers.mask_edit.grids_num = grids_num;
  /* The only token that survives a rebuild at the same level over the same base topology, which
   * leaves every geometric number above unchanged. See #SculptLayerMaskEdit::ccg_id. */
  ss.layers.mask_edit.ccg_id = subdiv_ccg->id;
  return true;
}

bool mask_edit_begin(Depsgraph &depsgraph,
                     Main &bmain,
                     Object &object,
                     SculptLayerTreeNode &node)
{
  SculptSession *ss = session_of(object);
  if (ss == nullptr) {
    return false;
  }
  /* Refused rather than asserted. An operator is expected to poll for this and report it, and a
   * debug build that aborted here would turn a message the user can act on into a crash on any
   * path whose poll and execution are not perfectly in step. */
  if (ss->layers.mask_edit.node_uid != 0) {
    return false;
  }
  /* Uid 0 is the root group, which is never drawn as a row, and is also this session's "not open"
   * sentinel — a session on it could not be represented, let alone closed. */
  if (node.uid == 0) {
    return false;
  }

  /* Refused while REC is armed, not merely while a stroke is in flight: recording writes the full
   * stroke delta into the layer regardless of its mask (see #node_mask_for_composite), so letting
   * the two run together would show the user a surface that does not match what is being stored.
   * #SculptSession::layers::rec_active is the armed state; #recording is only true for the duration
   * of one stroke, and testing that alone would let a session open between two dabs. */
  if (ss->layers.rec_active || ss->layers.recording) {
    return false;
  }

  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(object);
  if (pbvh == nullptr) {
    return false;
  }

  /* Multires only, and *before* the node's weights replace #SubdivCCG::masks: drain any base sculpt
   * edits the CCG is still holding lazily. #multires_flush_sculpt_updates copies `masks` straight
   * into the base mesh's persistent `CD_GRID_PAINT_MASK` layer whenever `dirty.coords` or
   * `dirty.hidden` is set, so a stroke made *before* the session opened would otherwise have its
   * deferred flush land in the middle of the session and write the layer's weights there.
   *
   * Draining here is a cleanliness measure, not a guarantee: the CCG does not stay clean during the
   * session, because mask painting itself sets `dirty.coords` (see the NOTE on
   * #mask_edit_begin_grids). What actually keeps the layer's weights out of the base mesh is that
   * both flush primitives park the session themselves. This call only ensures that the base edits
   * made *before* the session are flushed against the layer set they were made with, rather than
   * against whatever the session leaves behind. */
  if (pbvh->type() == bke::pbvh::Type::Grids) {
    flush_pending_multires_base(object);

    /* Materialized *before* the session exists, which is what makes the destructive path
     * unreachable rather than something to recover from. #BKE_sculpt_mask_layers_ensure is called
     * by every mask tool that can run during a session — the Mask brush above all, which is
     * exempted from the brush block precisely so it can author the layer's mask — and on a multires
     * object with no `CD_GRID_PAINT_MASK` it creates the layer, tags #ID_RECALC_GEOMETRY and
     * re-evaluates the depsgraph synchronously. The rebuilt CCG re-derives #SubdivCCG::masks from
     * the freshly zeroed layer, so the session's in-flight weights would be gone and the close path
     * would compress an all-transparent mask onto the node. Doing it here means every later call
     * finds the layer present and takes its harmless branch.
     *
     * The same re-evaluation invalidates #SculptSession::subdiv_ccg and the PBVH, so the session
     * state both are read from is refreshed before anything below touches them. */
    if (MultiresModifierData *mmd = ss->multires_modifier) {
      BKE_sculpt_mask_layers_ensure(&depsgraph, &bmain, &object, mmd);
      BKE_sculpt_update_object_for_edit(&depsgraph, &object, false);
      pbvh = bke::object::pbvh_get(object);
      if (pbvh == nullptr || pbvh->type() != bke::pbvh::Type::Grids) {
        return false;
      }
    }
  }

  bool opened = false;
  switch (pbvh->type()) {
    case bke::pbvh::Type::Mesh:
      opened = mask_edit_begin_mesh(object, *ss, node);
      break;
    case bke::pbvh::Type::Grids:
      opened = mask_edit_begin_grids(*ss, node);
      break;
    case bke::pbvh::Type::BMesh:
      /* Dynamic topology carries no sculpt layers, so there is nothing to mask. */
      break;
  }
  if (!opened) {
    /* Every failure above is refused before any state is stored, so there is nothing to undo. */
    return false;
  }

  /* Not because anything dangles: a cached chain never aliases this pointer, since #chain_mask
   * stores a #mask_copy or a #mask_multiply and both allocate. It is staleness. The cached
   * product was folded from a mask that is gone, and the domain of the one replaced here is what
   * decided how it folded (see the mismatch fallback in #chain_mask), so the product has to be
   * rebuilt from the mask this session installs rather than assumed equivalent to it. */
  tag_masked_chains_dirty(node);
  mask_buffer_refresh_pbvh(object);
  return true;
}

bool mask_edit_enter(Depsgraph &depsgraph, Main &bmain, Object &object, SculptLayerTreeNode &node)
{
  SculptSession *ss = session_of(object);
  if (ss == nullptr) {
    return false;
  }
  /* Disarmed through #rec_active_set rather than by writing the flag, because the flag has a DNA
   * mirror (#SCULPT_LAYER_REC_EXEMPT) that decides whether the composite honors this very mask. The
   * worst case that contract exists for is exactly this one: a node entering a mask edit carries a
   * weight map by definition, so the protective multires flush is not a theoretical branch here. */
  rec_active_set(object, false);
  if (!mask_edit_begin(depsgraph, bmain, object, node)) {
    /* Left disarmed, even though this is the rollback of a *failed* entry, because the two halves of
     * #rec_active_set are not inverses of each other: disarming only clears the flag, while arming
     * pins the active layer to #SCULPT_LAYER_ENABLED with `influence = 1.0f` (see
     * #layer_toggle_rec_exec). Re-arming here would therefore overwrite an influence the user set
     * while REC was armed, with no undo record to get it back. Disarming is one-way on every path
     * for that reason; see the note in #mask_edit_end. */
    return false;
  }
  return true;
}

/**
 * Close a session that was opened on the mesh (vertex) domain. \a node is null when it was deleted
 * while its mask was being edited.
 */
static void mask_edit_end_mesh(Object &object, SculptSession &ss, SculptLayerTreeNode *node)
{
  Mesh &mesh = mesh_of(object);
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  /* Scoped so the writer is finished and destroyed before the attribute can be removed below. */
  {
    bke::SpanAttributeWriter<float> mask = attributes.lookup_or_add_for_write_span<float>(
        ".sculpt_mask", bke::AttrDomain::Point);
    if (mask) {
      /* The node can be gone: nothing stops the user from deleting the layer or folder while its
       * mask is being edited. The parked mask is still restored below — the session's job of putting
       * the user's own mask back does not depend on the node surviving. */
      if (node != nullptr) {
        bke::sculpt_layers::mask_free(node->mask);
        node->mask = bke::sculpt_layers::mask_compress(mask.span, vert_block_size);
      }
      /* The vertex count can have changed under the session (an Edit Mode round trip, a script). The
       * parked buffer then describes a topology this mesh no longer has, so it is dropped rather
       * than copied into a buffer of a different length. */
      if (ss.layers.mask_edit.had_vert_mask &&
          ss.layers.mask_edit.saved_vert_mask.size() == mask.span.size())
      {
        mask.span.copy_from(ss.layers.mask_edit.saved_vert_mask);
      }
      else {
        /* Destructive on purpose: a user who did have a sculpt mask loses it here. A buffer of a
         * different length cannot be put back, so the trade is between removing the attribute and
         * leaving the layer's weights in it. Those weights would pass for the user's own mask and
         * quietly steer every brush, which is the worse of the two — an absent mask is at least a
         * state the user can see and repaint. */
        ss.layers.mask_edit.had_vert_mask = false;
      }
      mask.finish();
    }
  }
  /* Removed rather than left holding the layer's weights: a mesh that had no `.sculpt_mask` before
   * the session must have none after it, or every brush would silently start honoring a mask the
   * user never painted. */
  if (!ss.layers.mask_edit.had_vert_mask) {
    attributes.remove(".sculpt_mask");
  }
}

/**
 * Close a session that was opened on the multires grid domain. \a node is null when it was deleted
 * while its mask was being edited.
 *
 * The mirror image of #mask_edit_begin_grids: compress #SubdivCCG::masks back onto the node at the
 * same block size it was expanded at, then either put the user's own grid mask back byte for byte or
 * return the array to the empty state it was found in.
 */
static void mask_edit_end_grids(SculptSession &ss, SculptLayerTreeNode *node)
{
  SubdivCCG *subdiv_ccg = ss.subdiv_ccg;
  if (subdiv_ccg == nullptr) {
    /* The CCG is gone, and with it both the authored weights and the buffer the parked mask would be
     * restored into. Nothing can be salvaged; the caller still clears the session state. */
    return;
  }

  MutableSpan<float> masks = subdiv_ccg->masks.as_mutable_span();
  /* Only compressed while the CCG still describes the domain the session opened on. A subdivision
   * level change rebuilds the CCG and re-derives #SubdivCCG::masks from the base mesh, so the
   * expanded weights are already gone and compressing what replaced them would store the user's own
   * sculpt mask onto the node — at a block size that no longer matches the grids, at that. Task 9
   * owns closing a session before such a change; this is the backstop for when it does not.
   *
   * `grid_area` alone cannot catch a base-topology change that rebuilds the CCG at the same
   * subdivision level: `grids_num` changes while `grid_area` does not, and `masks.size() ==
   * expected` is computed from the *live* `grids_num` and `grid_area`, so it is self-consistent by
   * construction and cannot see the rebuild either. `grids_num` is therefore checked against the
   * value recorded at open, the same way `grid_area` is.
   *
   * Neither of those, nor `masks.size()`, moves at all when the rebuild happens at the *same* level
   * over the *same* base topology — and that rebuild still replaces the painted weights with the
   * user's own sculpt mask, re-derived from `CD_GRID_PAINT_MASK`. Only the CCG's identity separates
   * the two cases, so it is checked alongside the geometry (see #SculptLayerMaskEdit::ccg_id). */
  const int64_t expected = int64_t(subdiv_ccg->grids_num) * subdiv_ccg->grid_area;
  const bool domain_intact = !masks.is_empty() &&
                             subdiv_ccg->id == ss.layers.mask_edit.ccg_id &&
                             subdiv_ccg->grid_area == ss.layers.mask_edit.grid_area &&
                             subdiv_ccg->grids_num == ss.layers.mask_edit.grids_num &&
                             masks.size() == expected;

  /* The node can be gone: nothing stops the user from deleting the layer or folder while its mask is
   * being edited. The parked mask is still restored below — the session's job of putting the user's
   * own mask back does not depend on the node surviving. */
  if (domain_intact && node != nullptr) {
    bke::sculpt_layers::mask_free(node->mask);
    /* One block per grid, which is what makes a grid index a block index for every multires path
     * that reads this mask (#grid_masks_for_composite rejects any other cut, silently). */
    node->mask = bke::sculpt_layers::mask_compress(masks, subdiv_ccg->grid_area);
  }

  /* The grid count can have changed under the session, exactly as the vertex count can on the mesh
   * path. The parked buffer then describes a domain this CCG no longer has and is dropped rather
   * than copied into a buffer of a different length. */
  if (ss.layers.mask_edit.had_grid_mask &&
      ss.layers.mask_edit.saved_grid_mask.size() == masks.size())
  {
    masks.copy_from(ss.layers.mask_edit.saved_grid_mask);
  }
  else {
    /* Emptied rather than left holding the layer's weights, for the reason the mesh path removes the
     * attribute: those weights would pass for the user's own mask and quietly steer every brush. An
     * empty array is the state every reader already treats as "no mask" (see #gather_mask_grids). */
    subdiv_ccg->masks.reinitialize(0);
  }
}

void mask_edit_end(Object &object)
{
  SculptSession *ss = session_of(object);
  if (ss == nullptr || ss->layers.mask_edit.node_uid == 0) {
    return;
  }
  /* A suspended session has the *user's* mask in the standard storage and its own weights parked, so
   * compressing from here would store the user's sculpt mask onto the node — the precise corruption
   * the suspend exists to prevent, with the two buffers swapped. Resumed first rather than asserted
   * against: the brackets are RAII and cannot strand a suspend, but an exit path that fires from
   * inside one (a script saving and then leaving sculpt mode from a handler) must still close
   * correctly rather than abort a debug build. The forcing form is required because the brackets
   * nest — one ordinary resume would only drop the innermost of several outstanding holds. */
  bke::sculpt_layers::mask_edit_force_resume(object);
  Mesh &mesh = mesh_of(object);
  SculptLayerTreeNode *node = bke::sculpt_layers::node_find_by_uid(mesh,
                                                                  ss->layers.mask_edit.node_uid);

  /* Dispatched on what the session was *opened* on, not on the live PBVH type. Removing the multires
   * modifier mid-session would otherwise send a grid session down the mesh path, which ends by
   * removing a `.sculpt_mask` attribute that session never created. */
  if (ss->layers.mask_edit.on_grids) {
    mask_edit_end_grids(*ss, node);
  }
  else {
    mask_edit_end_mesh(object, *ss, node);
  }

  /* REC is deliberately *not* re-armed here, even though entering the session disarmed it. Arming is
   * not a flag assignment: #layer_toggle_rec_exec pins the active layer to enabled with
   * `influence = 1.0`, brackets that in an undo push, and refuses outright when the active layer
   * sits in a disabled folder — none of which this path can reproduce. The tree can have moved under
   * the session (a layer or its folder switched off), so replaying the arming from here would pin
   * state the user just changed, with no undo record and past the refusal that protects
   * `positions == base + sum(data * effective)`. Leaving REC disarmed costs the user one click
   * through the operator that owns those invariants. */
  ss->layers.mask_edit = SculptLayerMaskEdit{};

  if (node != nullptr) {
    tag_masked_chains_dirty(*node);
  }
  mask_buffer_refresh_pbvh(object);

  /* The node's weights are only in force once the session has stored them, so the composed surface
   * changes here and nowhere else. Recomputed canonically from `base + layers` rather than nudged,
   * which is what keeps the base from drifting across repeated sessions. */
  commit_layers_change(object);
}

/* -------------------------------------------------------------------- */
/** \name Weight mask operators
 *
 * One set of operators serves both node kinds, as the container does: a folder's mask and a layer's
 * mask differ only in how the composite folds them (#chain_mask against #node_mask_for_composite),
 * never in how they are authored.
 *
 * Each operator names its target by `node_uid`, with 0 meaning "the active layer". The tree view's
 * row buttons and the folder context menu pass an explicit uid, because a folder has no "active"
 * state to fall back on (nothing in the data names one); the Python layer menu passes nothing and
 * gets the active layer.
 *
 * \{ */

static bool mask_ops_poll(bContext *C)
{
  const Object *ob = CTX_data_active_object(C);
  return ob && (ob->mode & OB_MODE_SCULPT);
}

/* UI refresh after a mask change. The redraw belongs to the operator here for the same reason it
 * does everywhere else in this module — #mask_edit_begin and #mask_edit_end deliberately send no
 * notifier of their own. */
static void mask_ui_notify(bContext *C, Object &object)
{
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, &object);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, &mesh_of(object).id);
}

/** The layout every mask on \a object must be cut at, or a zeroed one when it carries no elements. */
static MaskLayout mask_layout_for_object(Object &object)
{
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(object);
  if (pbvh == nullptr) {
    return {};
  }
  switch (pbvh->type()) {
    case bke::pbvh::Type::Mesh:
      return mask_layout_for(false, mesh_of(object).verts_num, 0, 0);
    case bke::pbvh::Type::Grids: {
      const SculptSession *ss = session_of(object);
      if (ss == nullptr || ss->subdiv_ccg == nullptr) {
        return {};
      }
      /* Read off the live CCG rather than derived from the modifier's level: these are the numbers
       * #SubdivCCG::masks is actually sized by, and so the only ones a block layout may come from. */
      return mask_layout_for(true, 0, ss->subdiv_ccg->grids_num, ss->subdiv_ccg->grid_area);
    }
    case bke::pbvh::Type::BMesh:
      /* Dynamic topology carries no sculpt layers, so there is nothing to mask. */
      return {};
  }
  return {};
}

struct MaskOpContext {
  Object *object = nullptr;
  Depsgraph *depsgraph = nullptr;
  Mesh *mesh = nullptr;
  SculptLayerTreeNode *node = nullptr;
  MaskLayout layout;
  bool grids = false;
};

/**
 * Resolve the operator's target and the layout its mask must take, reporting every refusal.
 *
 * The domain check is the load-bearing part. A vertex-domain layer on a multires object (a layer
 * left behind by a modifier change) would otherwise be given a grid-cut mask, which
 * #node_mask_for_composite calls stale and ignores — the user would paint weights that do nothing.
 * A folder is exempt because it has no domain of its own; #node_as_layer returning null for one is
 * what that relies on.
 */
static bool mask_op_context_get(bContext *C, wmOperator *op, MaskOpContext &r_ctx)
{
  Object *object = CTX_data_active_object(C);
  if (object == nullptr) {
    return false;
  }
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  BKE_sculpt_update_object_for_edit(depsgraph, object, false);
  if (!is_supported(*object)) {
    BKE_report(op->reports, RPT_ERROR, "Sculpt layers are not available for this object");
    return false;
  }
  Mesh &mesh = mesh_of(*object);

  const int node_uid = RNA_int_get(op->ptr, "node_uid");
  SculptLayerTreeNode *node = nullptr;
  if (node_uid != 0) {
    node = bke::sculpt_layers::node_find_by_uid(mesh, node_uid);
  }
  else if (SculptLayer *layer = bke::sculpt_layers::active_get(mesh)) {
    /* Uid 0 names the root group, which is never drawn as a row, so it is free to mean "unset". */
    node = &layer->base;
  }
  if (node == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "Select a sculpt layer or folder first");
    return false;
  }

  if (const SculptLayer *layer = bke::sculpt_layers::node_as_layer(node);
      layer != nullptr && layer->domain != domain_for(*object))
  {
    BKE_report(op->reports,
               RPT_ERROR,
               "This layer was stored for a different element domain than the object uses now; "
               "repair or remove the layer before masking it");
    return false;
  }

  const MaskLayout layout = mask_layout_for_object(*object);
  if (layout.totelem == 0) {
    BKE_report(op->reports, RPT_ERROR, "This object has no elements to mask");
    return false;
  }

  r_ctx.object = object;
  r_ctx.depsgraph = depsgraph;
  r_ctx.mesh = &mesh;
  r_ctx.node = node;
  r_ctx.layout = layout;
  r_ctx.grids = bke::object::pbvh_get(*object)->type() == bke::pbvh::Type::Grids;
  if (r_ctx.grids) {
    /* A mask change moves the composed surface, so any base sculpt edits the CCG is still holding
     * lazily must be consumed while the live CCG and the stored weights still agree — exactly as
     * every other layer operator does on entry. */
    flush_pending_multires_base(*object);
  }
  return true;
}

/** True when a mask editing session is open on \a node specifically. */
static bool mask_session_open_on(const Object &object, const SculptLayerTreeNode &node)
{
  const int uid = mask_edit_active_uid(object);
  return uid != 0 && uid == node.uid;
}

/**
 * Refuse an operator that cannot act while a mask editing session is open, naming the node the
 * session is on.
 *
 * A session is per-*object* state (#SculptSession::layers::mask_edit is one field, not one per
 * node), so acting on a node other than the one the session is open on is just as unsafe as acting
 * on that node itself: during a session the standard mask storage holds the open node's weights,
 * and on the multires grid domain #commit_layers_change reaches `DEG_id_tag_update(...,
 * ID_RECALC_GEOMETRY)`, which *rebuilds* the CCG rather than flushing it — discarding the open
 * session's expanded weights outright rather than merely leaving them stale (see the NOTE on
 * #mask_edit_begin_grids). Acting on the open node itself is refused for the reason
 * #undo::push_sculpt_layer_mask does: during a session #SculptLayerTreeNode::mask holds a value the
 * next close overwrites, so capturing or replacing it here would be a silent no-op.
 *
 * The two cases are told apart only for the report text — both are refused — matching the message
 * #layer_mask_edit_toggle_exec already gives when refusing to open a second session.
 */
static bool mask_op_refuse_during_session(wmOperator *op,
                                          const Object &object,
                                          const SculptLayerTreeNode &node)
{
  const int open_uid = mask_edit_active_uid(object);
  if (open_uid == 0) {
    return false;
  }
  if (open_uid == node.uid) {
    BKE_reportf(op->reports,
                RPT_ERROR,
                "The weight mask of '%s' is being edited; finish the mask edit first",
                node.name);
    return true;
  }
  const Mesh &mesh = *id_cast<const Mesh *>(object.data);
  const SculptLayerTreeNode *open_node = bke::sculpt_layers::node_find_by_uid(mesh, open_uid);
  BKE_reportf(op->reports,
              RPT_ERROR,
              "A weight mask is already being edited on '%s'; finish that edit first",
              open_node ? open_node->name : "");
  return true;
}

bool mask_edit_refuse_deform(const Object &object, ReportList *reports)
{
  if (mask_edit_active_uid(object) == 0) {
    return false;
  }
  if (reports != nullptr) {
    BKE_report(reports, RPT_ERROR, "Close the sculpt layer mask session to sculpt geometry");
  }
  return true;
}

bool mask_edit_refuse_ccg_rebuild(wmOperator *op, const Object &object)
{
  const SculptSession *ss = object.runtime->sculpt_session;
  if (ss == nullptr || ss->layers.mask_edit.node_uid == 0 || !ss->layers.mask_edit.on_grids) {
    return false;
  }
  const Mesh &mesh = *id_cast<const Mesh *>(object.data);
  const SculptLayerTreeNode *open_node = bke::sculpt_layers::node_find_by_uid(
      mesh, ss->layers.mask_edit.node_uid);
  BKE_reportf(op->reports,
              RPT_ERROR,
              "A weight mask is being edited on '%s'; finish that edit first",
              open_node ? open_node->name : "");
  return true;
}

/** True when \a node's mask can be read as it stands over \a layout. */
static bool mask_is_usable(const SculptLayerTreeNode &node, const MaskLayout &layout)
{
  return node.mask != nullptr && !bke::sculpt_layers::is_stale_mask(*node.mask, layout.totelem) &&
         node.mask->block_size == layout.block_size;
}

/**
 * Run the whole-mask edit through the *session's* dense buffer instead of the node's sparse mask.
 *
 * This is the second of the two options Task 10 left open for the mask operators, and the one the
 * value edits take: while a session is open the standard mask storage holds the node's weights, so
 * the existing flood fill already edits exactly the right buffer, with the per-node #Type::Mask undo
 * steps, the grid dirty flags and the PBVH mask tagging that go with it. Re-deriving any of that
 * here would be a second spelling of code that already exists (see #layer_mask_isolate_exec, which
 * delegates to the same operator for the same reason).
 *
 * The inner operator's status is returned rather than assumed: a failed poll on
 * `PAINT_OT_mask_flood_fill` must not be reported back to the user as success.
 */
static wmOperatorStatus mask_flood_fill_delegate(bContext *C, const char *mode, const float value)
{
  PointerRNA props = WM_operator_properties_create("PAINT_OT_mask_flood_fill");
  RNA_enum_set_identifier(C, &props, "mode", mode);
  RNA_float_set(&props, "value", value);
  const wmOperatorStatus status = WM_operator_name_call(
      C, "PAINT_OT_mask_flood_fill", wm::OpCallContext::ExecDefault, &props, nullptr);
  WM_operator_properties_free(&props);
  return status;
}

/** Register the `node_uid` property every mask operator is addressed by. */
static void mask_op_properties(wmOperatorType *ot)
{
  PropertyRNA *prop = RNA_def_int(
      ot->srna,
      "node_uid",
      0,
      0,
      INT_MAX,
      "Item ID",
      "Unique id of the sculpt layer or folder to act on; zero means the active layer",
      0,
      INT_MAX);
  /* This addresses the operator at one tree row; it is not a setting to carry into the next run.
   * These operators are #OPTYPE_REGISTER, so without #PROP_SKIP_SAVE the window manager restores
   * the previous value whenever a caller leaves the property unset — and the panel's own "Add Mask"
   * button deliberately leaves it unset to mean "the active layer". One invocation from a folder's
   * context menu, which does set it, would then pin every later invocation to that folder. */
  RNA_def_property_flag(prop, PropertyFlag(PROP_HIDDEN | PROP_SKIP_SAVE));
}

/* -------------------------------------------------------------------- */
/** \name Session UI state
 * \{ */

/**
 * Fill `r_tkey` and `r_workspace` for the 3D viewport's tool slot, false when there is no viewport
 * on screen.
 *
 * The key is built by #WM_toolsystem_key_from_context rather than by hand: `bToolKey::mode` for
 * #SPACE_VIEW3D is an #eContextObjectMode, not an #eObjectMode, and a constant from the wrong
 * family would silently name a tool slot that does not exist.
 *
 * Which viewport is found does not matter. A #bToolRef is stored on the #WorkSpace keyed by
 * (space type, mode), not per area, so every 3D viewport of a workspace shares one active tool and
 * yields the same key.
 */
static bool view3d_tool_key_get(bContext *C, bToolKey *r_tkey, WorkSpace **r_workspace)
{
  bScreen *screen = CTX_wm_screen(C);
  WorkSpace *workspace = CTX_wm_workspace(C);
  if (screen == nullptr || workspace == nullptr) {
    return false;
  }
  Main *bmain = CTX_data_main(C);
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  if (bmain == nullptr || view_layer == nullptr) {
    return false;
  }
  for (ScrArea &area : screen->areabase) {
    if (area.spacetype != SPACE_VIEW3D) {
      continue;
    }
    if (!WM_toolsystem_key_from_context(*bmain, scene, view_layer, &area, r_tkey)) {
      continue;
    }
    *r_workspace = workspace;
    return true;
  }
  return false;
}

/** Idname of the viewport's active tool, empty when it cannot be read. */
static std::string view3d_tool_id_get(bContext *C)
{
  bToolKey tkey;
  WorkSpace *workspace = nullptr;
  if (!view3d_tool_key_get(C, &tkey, &workspace)) {
    return "";
  }
  const bToolRef *tref = WM_toolsystem_ref_find(workspace, &tkey);
  return (tref != nullptr) ? std::string(tref->idname) : std::string("");
}

/**
 * Make `tool_id` the viewport's active tool. Failure is ignored by every caller: a mask session is
 * an edit of data, and neither a missing viewport nor a tool that will not switch is a reason to
 * refuse it.
 *
 * Callable from the Properties editor, which is where the sculpt layer panel lives:
 * #WM_toolsystem_ref_set_by_id_ex sets the `space_type` operator property explicitly, and
 * #WM_OT_tool_set_by_id does not consult `context.space_data` once that property is set.
 */
static void view3d_tool_id_set(bContext *C, const char *tool_id)
{
  bToolKey tkey;
  WorkSpace *workspace = nullptr;
  if (!view3d_tool_key_get(C, &tkey, &workspace)) {
    return;
  }
  WM_toolsystem_ref_set_by_id_ex(C, workspace, &tkey, tool_id, false);
}

/** Idname of the tool a mask edit session activates, and the value the restore keys off. */
static constexpr const char *mask_tool_id = "builtin_brush.mask";

/**
 * What the user is told when opening a session disarmed REC.
 *
 * Shared by both entry points so the two read alike, and worth saying at all because the disarm is
 * one-way: arming pins the active layer's influence and carries its own undo record, so
 * #mask_edit_end cannot replay it and the user has to press REC again themselves.
 */
static constexpr const char *rec_disarmed_note =
    "Layer recording (REC) was turned off and is not turned back on when the mask edit finishes";

/**
 * Open a mask edit session and put the UI into the state that edit needs: REC disarmed and the Mask
 * brush active. #mask_edit_enter owns the REC half, one-way on both outcomes; only the tool is this
 * function's own, because only a #bContext can reach the tool system.
 *
 * The tool is switched *after* the session opens, so a refused open leaves the user holding the tool
 * they had rather than the mask brush of a session that does not exist.
 */
static bool mask_edit_enter_ui(bContext *C,
                               Depsgraph &depsgraph,
                               Main &bmain,
                               Object &object,
                               SculptLayerTreeNode &node)
{
  std::string prev_tool_id = view3d_tool_id_get(C);
  if (!mask_edit_enter(depsgraph, bmain, object, node)) {
    return false;
  }
  /* Non-null: #mask_edit_enter refuses without a session. */
  SculptSession &ss = *session_of(object);

  /* Never parked when it already *is* the mask tool, which is the self-poisoning case. A session
   * that closed without passing through #mask_edit_exit_ui — a layer selection, leaving Sculpt
   * Mode — leaves the mask tool active, so the next entry would park that idname and the restore
   * below, which fires only while the mask tool is still active, would match itself and put the
   * user's real tool out of reach for good. An empty parked value means "nothing to restore", which
   * is the honest answer once the original tool is no longer known. */
  if (prev_tool_id != mask_tool_id) {
    ss.layers.mask_edit.saved_tool_id = std::move(prev_tool_id);
  }

  view3d_tool_id_set(C, mask_tool_id);
  return true;
}

void mask_edit_exit_ui(bContext *C, Object &object)
{
  SculptSession *ss = session_of(object);
  if (ss == nullptr || ss->layers.mask_edit.node_uid == 0) {
    return;
  }
  const std::string saved_tool_id = ss->layers.mask_edit.saved_tool_id;

  /* Conditional, unlike REC: a user who picked a different tool during the session made a choice,
   * and closing the session is not a reason to overrule it. */
  if (!saved_tool_id.empty() && view3d_tool_id_get(C) == mask_tool_id) {
    view3d_tool_id_set(C, saved_tool_id.c_str());
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add / remove
 * \{ */

static wmOperatorStatus layer_mask_add_exec(bContext *C, wmOperator *op)
{
  MaskOpContext ctx;
  if (!mask_op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  /* A session already gave the node a mask when it opened, so there is nothing to add. */
  if (mask_op_refuse_during_session(op, *ctx.object, *ctx.node)) {
    return OPERATOR_CANCELLED;
  }
  if (mask_is_usable(*ctx.node, ctx.layout)) {
    BKE_reportf(op->reports, RPT_ERROR, "'%s' already carries a weight mask", ctx.node->name);
    return OPERATOR_CANCELLED;
  }

  undo::push_begin(*CTX_data_scene(C), *ctx.object, op);
  undo::push_sculpt_layer_mask(*ctx.object, *ctx.node);
  bke::sculpt_layers::mask_free(ctx.node->mask);
  /* Fully opaque, which is what makes adding a mask a no-op on the surface: an all-255 mask weights
   * every element at 1, exactly the contribution the node had with no mask at all. The user then
   * carves weight *away* with the Mask brush, and #mask_edit_begin starts from the same value for
   * the same reason. Replacing an unusable mask (stale, or cut for the other domain) is likewise
   * invisible — every consumer was already ignoring it. Hence no #commit_layers_change here. */
  ctx.node->mask = bke::sculpt_layers::mask_new(ctx.layout.totelem, ctx.layout.block_size, 255);
  tag_masked_chains_dirty(*ctx.node);
  tag_layer_mask_overlay_dirty(*ctx.object);

  /* Adding a mask and then editing it is one intention, so the session opens here rather than
   * making the user find "Edit Mask" as a second step. The session marker rides the same undo step
   * so that undoing the add also closes the session it opened. */
  /* Read before the entry below, which disarms it. */
  const SculptSession *ss = session_of(*ctx.object);
  const bool rec_was_armed = ss != nullptr && ss->layers.rec_active;
  const bool entered = mask_edit_enter_ui(
      C, *ctx.depsgraph, *CTX_data_main(C), *ctx.object, *ctx.node);
  if (entered) {
    /* Pushed only on success, and therefore after the open rather than before it. A marker left
     * behind by a refused open is harmless to undo — nothing to close — but a redo would replay it
     * and open a session this execution never created. The late push is admissible because
     * #push_sculpt_layer_mask_session writes two scalars onto the step and reads no layer state, so
     * unlike #push_sculpt_layer_mask above it has nothing to capture before the mutation. */
    undo::push_sculpt_layer_mask_session(*ctx.object, ctx.node->uid, true);
  }
  undo::push_end(*ctx.object);

  if (!entered) {
    /* The mask itself was added, which is what the operator is named for, so this is a warning
     * rather than a cancel — cancelling would discard a mask that is on the node. */
    BKE_report(op->reports, RPT_WARNING, "Added the weight mask, but could not start editing it");
  }
  /* Reported on both outcomes, unlike the message above: #mask_edit_enter disarms REC before it can
   * refuse, so a failed entry costs the user their armed recording just as a successful one does.
   * Its own report rather than folded into either message, because only this one is conditional. */
  if (rec_was_armed) {
    BKE_report(op->reports, RPT_INFO, rec_disarmed_note);
  }
  mask_ui_notify(C, *ctx.object);
  return OPERATOR_FINISHED;
}

void SCULPT_OT_layer_mask_add(wmOperatorType *ot)
{
  ot->name = "Add Sculpt Layer Mask";
  ot->idname = "SCULPT_OT_layer_mask_add";
  ot->description =
      "Give this sculpt layer or folder a per-element weight mask, fully opaque, and where "
      "possible open it for painting with the mask tools";
  ot->exec = layer_mask_add_exec;
  ot->poll = mask_ops_poll;
  /* No #OPTYPE_UNDO: the operator pushes its own sculpt undo step; a global push would insert a
   * memfile step that does not compose with the stroke SCULPT steps (see #SCULPT_OT_layer_solo_base). */
  ot->flag = OPTYPE_REGISTER;
  mask_op_properties(ot);
}

static wmOperatorStatus layer_mask_remove_exec(bContext *C, wmOperator *op)
{
  MaskOpContext ctx;
  if (!mask_op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  /* Refused rather than routed, unlike the value edits below: removing the mask a session is
   * authoring would be undone by that session's own close, which writes the dense buffer back onto
   * the node. Closing the session on the user's behalf instead would silently apply an edit they
   * have not finished. */
  if (mask_op_refuse_during_session(op, *ctx.object, *ctx.node)) {
    return OPERATOR_CANCELLED;
  }
  if (ctx.node->mask == nullptr) {
    BKE_reportf(op->reports, RPT_ERROR, "'%s' carries no weight mask", ctx.node->name);
    return OPERATOR_CANCELLED;
  }

  session_state_ensure(*ctx.object);
  undo::push_begin(*CTX_data_scene(C), *ctx.object, op);
  undo::push_sculpt_layer_mask(*ctx.object, *ctx.node);
  bke::sculpt_layers::mask_free(ctx.node->mask);
  ctx.node->mask = nullptr;
  tag_masked_chains_dirty(*ctx.node);
  tag_layer_mask_overlay_dirty(*ctx.object);
  /* The node's full contribution comes back wherever the mask was attenuating it, so the composed
   * surface moves. Recomputed canonically rather than nudged, as everywhere in this module. */
  commit_layers_change(*ctx.depsgraph, *ctx.object);
  undo::push_end(*ctx.object);
  mask_ui_notify(C, *ctx.object);
  return OPERATOR_FINISHED;
}

void SCULPT_OT_layer_mask_remove(wmOperatorType *ot)
{
  ot->name = "Remove Sculpt Layer Mask";
  ot->idname = "SCULPT_OT_layer_mask_remove";
  ot->description =
      "Remove this sculpt layer or folder's weight mask, restoring its full contribution";
  ot->exec = layer_mask_remove_exec;
  ot->poll = mask_ops_poll;
  ot->flag = OPTYPE_REGISTER;
  mask_op_properties(ot);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Value edits
 *
 * Invert, Clear and Fill all rewrite every weight, and all three are natural things to reach for in
 * the middle of a mask edit. They therefore route through the session's dense buffer when one is
 * open on the target node (see #mask_flood_fill_delegate) and edit the node's sparse mask directly
 * otherwise. The two paths agree on what each operation means, so the user sees one behavior.
 * \{ */

/** Replace \a node's mask with a uniform one. Shared by Clear and Fill. */
static wmOperatorStatus mask_uniform_exec(bContext *C,
                                          wmOperator *op,
                                          const uint8_t fill,
                                          const char *flood_mode,
                                          const float flood_value)
{
  MaskOpContext ctx;
  if (!mask_op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  if (mask_session_open_on(*ctx.object, *ctx.node)) {
    return mask_flood_fill_delegate(C, flood_mode, flood_value);
  }
  /* A session open on some *other* node parks that node's weights in the very storage this
   * function is about to overwrite outright; see #mask_op_refuse_during_session. */
  if (mask_op_refuse_during_session(op, *ctx.object, *ctx.node)) {
    return OPERATOR_CANCELLED;
  }
  if (ctx.node->mask == nullptr) {
    BKE_reportf(
        op->reports, RPT_INFO, "'%s' carries no weight mask; add one first", ctx.node->name);
    return OPERATOR_CANCELLED;
  }

  session_state_ensure(*ctx.object);
  undo::push_begin(*CTX_data_scene(C), *ctx.object, op);
  undo::push_sculpt_layer_mask(*ctx.object, *ctx.node);
  bke::sculpt_layers::mask_free(ctx.node->mask);
  /* Built fresh at the object's layout rather than refilled in place, so a mask that was stale or
   * cut for the other domain is repaired by the same call that fills it. */
  ctx.node->mask = bke::sculpt_layers::mask_new(ctx.layout.totelem, ctx.layout.block_size, fill);
  tag_masked_chains_dirty(*ctx.node);
  tag_layer_mask_overlay_dirty(*ctx.object);
  commit_layers_change(*ctx.depsgraph, *ctx.object);
  undo::push_end(*ctx.object);
  mask_ui_notify(C, *ctx.object);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus layer_mask_clear_exec(bContext *C, wmOperator *op)
{
  return mask_uniform_exec(C, op, 0, "VALUE", 0.0f);
}

void SCULPT_OT_layer_mask_clear(wmOperatorType *ot)
{
  ot->name = "Clear Sculpt Layer Mask";
  ot->idname = "SCULPT_OT_layer_mask_clear";
  /* "Contributes nothing" and not "has no mask": Remove Mask is the way back to a full
   * contribution, and the description says so because the two read alike in a menu. */
  ot->description =
      "Set every weight of this sculpt layer or folder's mask to zero, so it contributes nothing";
  ot->exec = layer_mask_clear_exec;
  ot->poll = mask_ops_poll;
  ot->flag = OPTYPE_REGISTER;
  mask_op_properties(ot);
}

static wmOperatorStatus layer_mask_fill_exec(bContext *C, wmOperator *op)
{
  return mask_uniform_exec(C, op, 255, "VALUE", 1.0f);
}

void SCULPT_OT_layer_mask_fill(wmOperatorType *ot)
{
  ot->name = "Fill Sculpt Layer Mask";
  ot->idname = "SCULPT_OT_layer_mask_fill";
  ot->description =
      "Set every weight of this sculpt layer or folder's mask to one, so it contributes fully";
  ot->exec = layer_mask_fill_exec;
  ot->poll = mask_ops_poll;
  ot->flag = OPTYPE_REGISTER;
  mask_op_properties(ot);
}

static wmOperatorStatus layer_mask_invert_exec(bContext *C, wmOperator *op)
{
  MaskOpContext ctx;
  if (!mask_op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  if (mask_session_open_on(*ctx.object, *ctx.node)) {
    return mask_flood_fill_delegate(C, "INVERT", 0.0f);
  }
  /* A session open on some *other* node parks that node's weights in the very storage this
   * function is about to overwrite outright; see #mask_op_refuse_during_session. */
  if (mask_op_refuse_during_session(op, *ctx.object, *ctx.node)) {
    return OPERATOR_CANCELLED;
  }
  /* Unlike Clear and Fill, this one reads the stored weights, so a mask that cannot be indexed over
   * the live domain has to be refused rather than repaired — there is no meaningful way to invert a
   * buffer that describes a different topology. */
  if (!mask_is_usable(*ctx.node, ctx.layout)) {
    BKE_reportf(op->reports,
                RPT_ERROR,
                "'%s' carries no usable weight mask; add one, or remove the stale one first",
                ctx.node->name);
    return OPERATOR_CANCELLED;
  }

  /* Expanded, inverted and compressed rather than flipped byte by byte through #mask_block: the
   * round trip goes through the one authority on the block layout, and #mask_compress collapses
   * uniform blocks again, so an inverted mask does not degrade to dense storage. */
  Array<float> dense(ctx.layout.totelem);
  bke::sculpt_layers::mask_expand(*ctx.node->mask, dense.as_mutable_span());
  for (float &value : dense) {
    value = 1.0f - value;
  }

  session_state_ensure(*ctx.object);
  undo::push_begin(*CTX_data_scene(C), *ctx.object, op);
  undo::push_sculpt_layer_mask(*ctx.object, *ctx.node);
  bke::sculpt_layers::mask_free(ctx.node->mask);
  ctx.node->mask = bke::sculpt_layers::mask_compress(dense.as_span(), ctx.layout.block_size);
  tag_masked_chains_dirty(*ctx.node);
  tag_layer_mask_overlay_dirty(*ctx.object);
  commit_layers_change(*ctx.depsgraph, *ctx.object);
  undo::push_end(*ctx.object);
  mask_ui_notify(C, *ctx.object);
  return OPERATOR_FINISHED;
}

void SCULPT_OT_layer_mask_invert(wmOperatorType *ot)
{
  ot->name = "Invert Sculpt Layer Mask";
  ot->idname = "SCULPT_OT_layer_mask_invert";
  ot->description = "Invert every weight of this sculpt layer or folder's mask";
  ot->exec = layer_mask_invert_exec;
  ot->poll = mask_ops_poll;
  ot->flag = OPTYPE_REGISTER;
  mask_op_properties(ot);
}

static wmOperatorStatus layer_mask_toggle_exec(bContext *C, wmOperator *op)
{
  MaskOpContext ctx;
  if (!mask_op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  /* The row button is only drawn for a node that carries a mask, so this is reachable from a script
   * or a stale menu alone — reported rather than silently ignored, since "nothing happened" would
   * otherwise be indistinguishable from a switch that does not work. Add Mask is the way in. */
  if (ctx.node->mask == nullptr) {
    BKE_reportf(op->reports, RPT_ERROR, "'%s' carries no weight mask to switch", ctx.node->name);
    return OPERATOR_CANCELLED;
  }
  /* Uid 0 is the root folder, which is never drawn as a row and whose metadata
   * #undo::push_sculpt_layer_metadata refuses to capture. Guarded here exactly as
   * #layer_mask_edit_toggle_exec guards it. */
  if (ctx.node->uid == 0) {
    BKE_report(op->reports, RPT_ERROR, "Select a sculpt layer or folder first");
    return OPERATOR_CANCELLED;
  }
  /* A session anywhere is refused, not just one on this node: the session is per-object state, and
   * #commit_layers_change below reaches ID_RECALC_GEOMETRY on the grid domain, which rebuilds the
   * CCG and discards the session's expanded weights outright. See #mask_op_refuse_during_session. */
  if (mask_op_refuse_during_session(op, *ctx.object, *ctx.node)) {
    return OPERATOR_CANCELLED;
  }

  /* Derive the runtime base from the still-consistent pre-change state, before the bit moves: the
   * effective mask of a node scales its contribution, so flipping it moves the composed surface.
   * #mask_op_context_get has already consumed any pending multires base edits for the same reason —
   * skipping that flush dents the base irreversibly rather than merely leaving it stale. */
  session_state_ensure(*ctx.object);

  const bool enable = !bke::sculpt_layers::mask_enabled(*ctx.node);
  undo::push_begin(*CTX_data_scene(C), *ctx.object, op);
  /* Captures #SculptLayerTreeNode::flag whole, which is where the bit lives; no undo field of its
   * own is needed. The matching restore additionally invalidates a folder's chain cache. */
  undo::push_sculpt_layer_metadata(*ctx.object, *ctx.node);
  bke::sculpt_layers::mask_enabled_set(*ctx.node, enable);
  /* A no-op for a layer, whose mask caches nowhere; for a folder this is what makes the change
   * visible, since #chain_mask would otherwise keep serving the product built a moment ago. */
  tag_masked_chains_dirty(*ctx.node);
  /* The overlay gates on the same bit (a disabled mask hides nothing), so the toggle changes what
   * it has to draw even though no weight moved. */
  tag_layer_mask_overlay_dirty(*ctx.object);
  commit_layers_change(*ctx.depsgraph, *ctx.object);
  undo::push_end(*ctx.object);
  /* Two spellings rather than one format string chosen by a ternary, so both remain extractable for
   * translation. */
  if (enable) {
    BKE_reportf(op->reports, RPT_INFO, "Enabled the weight mask of '%s'", ctx.node->name);
  }
  else {
    BKE_reportf(op->reports, RPT_INFO, "Disabled the weight mask of '%s'", ctx.node->name);
  }
  mask_ui_notify(C, *ctx.object);
  return OPERATOR_FINISHED;
}

void SCULPT_OT_layer_mask_toggle(wmOperatorType *ot)
{
  ot->name = "Toggle Sculpt Layer Mask";
  ot->idname = "SCULPT_OT_layer_mask_toggle";
  /* "Keeping the weights" is the whole difference from Remove Mask, which reads alike in a menu. */
  ot->description =
      "Switch this sculpt layer or folder's weight mask on or off, keeping the painted weights";
  ot->exec = layer_mask_toggle_exec;
  ot->poll = mask_ops_poll;
  /* No #OPTYPE_UNDO: the operator pushes its own sculpt undo step; a global push would insert a
   * memfile step that does not compose with the stroke SCULPT steps. */
  ot->flag = OPTYPE_REGISTER;
  mask_op_properties(ot);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Edit session toggle
 * \{ */

static wmOperatorStatus layer_mask_edit_toggle_exec(bContext *C, wmOperator *op)
{
  MaskOpContext ctx;
  if (!mask_op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  SculptSession *ss = session_of(*ctx.object);
  if (ss == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "Weight masks can only be edited in Sculpt Mode");
    return OPERATOR_CANCELLED;
  }

  if (mask_session_open_on(*ctx.object, *ctx.node)) {
    undo::push_begin(*CTX_data_scene(C), *ctx.object, op);
    /* No #push_sculpt_layer_mask on this half: the capture is refused while the session is open,
     * and it is not wanted either. Undoing a close reopens the session (see #mask_session_boundary)
     * and expands the very mask this close just stored, which is the state the user left. */
    undo::push_sculpt_layer_mask_session(*ctx.object, ctx.node->uid, false);
    /* Before #mask_edit_end, which clears the session struct the parked UI state lives on. */
    mask_edit_exit_ui(C, *ctx.object);
    /* Compresses the painted weights onto the node, puts the user's own sculpt mask back and
     * recomposes the surface itself, so nothing more is needed here. */
    mask_edit_end(*ctx.object);
    undo::push_end(*ctx.object);
    BKE_reportf(op->reports, RPT_INFO, "Applied the weight mask of '%s'", ctx.node->name);
    mask_ui_notify(C, *ctx.object);
    return OPERATOR_FINISHED;
  }

  /* Every refusal #mask_edit_begin can make is checked here first, because that function
   * deliberately reports nothing: it is called from paths that have no #ReportList, so the message
   * the user acts on is the operator's to write. */
  if (const int open_uid = mask_edit_active_uid(*ss); open_uid != 0) {
    const SculptLayerTreeNode *open_node = bke::sculpt_layers::node_find_by_uid(*ctx.mesh,
                                                                               open_uid);
    BKE_reportf(op->reports,
                RPT_ERROR,
                "A weight mask is already being edited on '%s'; finish that edit first",
                open_node ? open_node->name : "");
    return OPERATOR_CANCELLED;
  }
  if (ctx.node->uid == 0) {
    BKE_report(op->reports, RPT_ERROR, "Select a sculpt layer or folder first");
    return OPERATOR_CANCELLED;
  }

  undo::push_begin(*CTX_data_scene(C), *ctx.object, op);
  /* Captured before the session opens, and while capturing is still allowed: #mask_edit_begin
   * replaces a missing or unusable mask with an opaque one, and undoing the open has to be able to
   * put back whatever was there. */
  undo::push_sculpt_layer_mask(*ctx.object, *ctx.node);
  /* Read before the entry below, which disarms it. */
  const bool rec_was_armed = ss->layers.rec_active;
  if (!mask_edit_enter_ui(C, *ctx.depsgraph, *CTX_data_main(C), *ctx.object, *ctx.node)) {
    /* The refusals reported above are the ones worth a message of their own; what is left here is a
     * stroke still recording into a layer (#mask_edit_begin refuses on
     * #SculptSession::layers::recording, which no poll can rule out because a modal stroke is in
     * flight) and the object having changed under the operator, its session or PBVH gone. The step
     * is closed rather than abandoned: its payloads only swap the node's mask for an identical
     * value, so restoring it is a no-op the user cannot see. */
    undo::push_end(*ctx.object);
    BKE_report(op->reports, RPT_ERROR, "Could not start editing the weight mask");
    /* Said on the refusal path too: #mask_edit_enter disarms REC before it can refuse, so a failed
     * open costs the user their armed recording just as a successful one does. */
    if (rec_was_armed) {
      BKE_report(op->reports, RPT_INFO, rec_disarmed_note);
    }
    return OPERATOR_CANCELLED;
  }
  /* Pushed only on success, and therefore after the open rather than before it, for the reason
   * #layer_mask_add_exec spells out: a marker left behind by a refused open is harmless to undo, but
   * a redo would replay it and open a session this execution never created. */
  undo::push_sculpt_layer_mask_session(*ctx.object, ctx.node->uid, true);
  undo::push_end(*ctx.object);

  /* The one place the user is told what a session is, because nothing on screen says it: the mask
   * they paint does not change the surface until this operator is run again. Live preview is a
   * separate piece of work and this message must not imply it exists.
   *
   * The REC half is named only when it actually happened, because the disarm is silent and one-way:
   * without it a user who had recording armed would find their next stroke no longer going into the
   * layer, with nothing having said so. */
  BKE_reportf(op->reports,
              RPT_INFO,
              "Editing the weight mask of '%s'. Paint it with the mask tools; the layer's shape "
              "updates when you finish the mask edit%s%s",
              ctx.node->name,
              rec_was_armed ? ". " : "",
              rec_was_armed ? rec_disarmed_note : "");
  mask_ui_notify(C, *ctx.object);
  return OPERATOR_FINISHED;
}

void SCULPT_OT_layer_mask_edit_toggle(wmOperatorType *ot)
{
  ot->name = "Edit Sculpt Layer Mask";
  ot->idname = "SCULPT_OT_layer_mask_edit_toggle";
  ot->description =
      "Start or finish painting this sculpt layer or folder's weight mask with the regular mask "
      "tools. The layer's shape updates when the edit is finished";
  ot->exec = layer_mask_edit_toggle_exec;
  ot->poll = mask_ops_poll;
  ot->flag = OPTYPE_REGISTER;
  mask_op_properties(ot);
}

/** \} */

/** \} */

}  // namespace blender::ed::sculpt_paint::layers
