/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * The high-level Paint Layers authoring verbs: one Add policy and the marker-addressed edits whose
 * rules are more than a direct `BKE_paint_layers_*` call. Every authoring path -- the Outliner's
 * stack source, `Material.paint_layers.new` and friends -- goes through these, so the defaults
 * (names, channels) are decided in exactly one place.
 *
 * The verbs only touch `Material::paint_layers` through the `BKE_paint_layers_*` API and never
 * need a #bContext, so a unit test can drive them without an editor. Reports, notifiers and the
 * active-row cursor stay the caller's business.
 */

#include <string>

#include "BLI_span.hh"
#include "BLI_string_ref.hh"

#include "BKE_paint_layers.hh"

namespace blender {

/** What kind of row the Add creates. */
enum class PaintLayerAddKind : int8_t {
  /** A painted layer. */
  Paint = 0,
  /** A flat-colour Fill layer. */
  Fill,
  /** An empty folder, made to be filled. */
  Folder,
  /**
   * A content correction of the anchor row (or of the row an anchor correction hangs on): a map
   * laid over its colour, painted into like a layer. It never becomes a row of the stack.
   */
  Correction,
  /**
   * A flat-colour content correction of the same row: one colour laid over the row's colour at the
   * correction's own opacity, taking #PaintLayerAddParams::fill_color.
   */
  CorrectionFill,
  /**
   * A mask correction of the anchor row (or of the row an anchor correction or mask correction hangs
   * on): a grey map painted over its mask's coverage in Mask mode. The base mask is made first, white,
   * when the row has none.
   */
  MaskCorrection,
  /** A flat-grey mask correction of the same row, taking #PaintLayerAddParams::fill_color. */
  MaskCorrectionFill,
  /**
   * A filter correction of the anchor row (or of the row an anchor correction hangs on): it changes
   * the colour the row has so far and owns no map. Which one is #PaintLayerAddParams::filter.
   */
  Filter,
  /**
   * A filter as a row of the stack: it changes the stack below it, laid by the row's own opacity,
   * blend and mask. Which one is #PaintLayerAddParams::filter. Refused inside an isolating folder.
   */
  Adjustment,
  /**
   * A filter in the mask of the anchor row (or of the row an anchor correction or mask correction
   * hangs on): it changes the mask's coverage, in no channel. Which one is
   * #PaintLayerAddParams::filter. The base mask is made first, white, when the row has none.
   */
  MaskFilter,
  /**
   * A row that lays what another material shows, #PaintLayerAddParams::source. Refused for no
   * source, for the owner itself and for a source that reads the owner.
   */
  Material,
};

struct PaintLayerAddParams {
  /** What the new row is. */
  PaintLayerAddKind kind = PaintLayerAddKind::Paint;
  /** The row the Add was aimed at, or null for the top of the stack. */
  MaterialPaintLayer *anchor = nullptr;
  /**
   * Where to put the new row relative to \a anchor. An \a anchor that is a folder takes the row
   * `Into` regardless of an `Above`: an Add onto a folder means filling it.
   */
  PaintLayerPlace place = PaintLayerPlace::Above;
  /**
   * The name for the new row, or null to derive the unique default (a `"Layer N"` name one past
   * the highest N in use).
   */
  const char *name = nullptr;
  /** The initial colour for a Fill, a Fill correction or a Fill mask correction; null keeps the default. */
  const float *fill_color = nullptr;
  /** The filter of a #PaintLayerAddKind::Filter Add, an #eMaterialPaintLayerFilter. */
  int16_t filter = 0;
  /** The material a #PaintLayerAddKind::Material Add reads. */
  Material *source = nullptr;
};

/**
 * Add a Paint row with the shared authoring policy: the unique default name and the default
 * channels a fresh Paint takes part in. Returns the new row, or null when the Add is refused (an
 * anchor that is not part of \a ma's stack).
 */
MaterialPaintLayer *BKE_paint_layers_add_with_policy(Material &ma,
                                                     const PaintLayerAddParams &params);

/**
 * The next default name for a new row: `"<base> N"`, one past the highest N in use anywhere in
 * \a ma's description. Rows are identified by their marker, never by name, so this only keeps the
 * list readable and two rows sharing a name (after a rename) is fine.
 */
std::string BKE_paint_layers_unique_default_name(const Material &ma, StringRef base);

/**
 * Reorder \a from to where \a to sits, within their shared list: \a from takes the position \a to
 * had. Returns false when \a from and \a to are the same row, hang on different lists (a folder's
 * children and the top level, say) or either is not part of \a ma.
 */
bool BKE_paint_layers_edit_reorder(Material &ma, MaterialPaintLayer &from, MaterialPaintLayer &to);

/**
 * Move \a from relative to \a anchor with the Outliner's drop rules: a move `Into` a plain row
 * groups the two rows into a new folder instead of relocating. \a anchor may be null to move to the
 * top of the stack. A row never moves into its own subtree.
 */
bool BKE_paint_layers_edit_move(Material &ma,
                                MaterialPaintLayer &from,
                                MaterialPaintLayer *anchor,
                                PaintLayerPlace place);

/**
 * Group \a upper and the row \a lower below it into a new folder, which takes their place. They
 * must hang on the same list. Returns the folder, or null on refusal.
 */
MaterialPaintLayer *BKE_paint_layers_edit_merge_down(Material &ma,
                                                     MaterialPaintLayer &upper,
                                                     MaterialPaintLayer &lower);

/** Group \a rows into a new folder. Returns the folder, or null on refusal. */
MaterialPaintLayer *BKE_paint_layers_edit_group_range(Material &ma,
                                                      Span<MaterialPaintLayer *> rows);

/**
 * Add an empty folder above \a anchor (or at the top when null), with its unique default name.
 */
MaterialPaintLayer *BKE_paint_layers_edit_group_add(Material &ma, MaterialPaintLayer *anchor);

}  // namespace blender
