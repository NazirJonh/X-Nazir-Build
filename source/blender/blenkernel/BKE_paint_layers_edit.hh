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

struct Brush;

/** What kind of row the Add creates. */
enum class PaintLayerAddKind : int8_t {
  /** A painted layer. */
  Paint = 0,
  /** A flat-colour Fill layer. */
  Fill,
  /**
   * A procedural Gradient layer: a Shader Editor node chain reading the UV, with no maps. The
   * payload defaults to a linear black-to-white ramp across the unit square.
   */
  Gradient,
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
  /**
   * A content correction of the anchor row (or of the row an anchor correction hangs on) that lays
   * what another material shows, #PaintLayerAddParams::source, over the row's colour. Refused for
   * no anchor row, no source, the owner itself and a source that reads the owner.
   */
  CorrectionMaterial,
  /** A mask correction reading #PaintLayerAddParams::source: the grey of its Base Color. */
  MaskCorrectionMaterial,
  /** The inert Anchor Point marker inside the anchor row's effects or mask. */
  AnchorPoint,
  /** An Anchor Point Reader as a row of the stack, reading #PaintLayerAddParams::anchor_point_source. */
  AnchorPointLayer,
  /**
   * An Anchor Point Reader as a content correction of the anchor row, reading
   * #PaintLayerAddParams::anchor_point_source.
   */
  AnchorPointCorrection,
  /**
   * An Anchor Point Reader as a mask item of the anchor row, reading
   * #PaintLayerAddParams::anchor_point_source.
   */
  AnchorPointMaskCorrection,
  /** A content correction of the anchor row whose source is a procedural Gradient. */
  GradientCorrection,
  /** A mask correction of the anchor row whose coverage is a procedural Gradient's grey. */
  MaskGradient,
  /**
   * A Curve Patch as a content effect of a row (or of the row an effect hangs on): its stamped
   * maps are laid over the row's colour, below the row's own blend. The payload (a fresh curve
   * container and a duplicate of the active brush) is created with the effect; the seeded control
   * curve is the caller's business, through the editors wrapper. Refused without
   * #PaintLayerAddParams::bmain: the payload IDs need a database. There is deliberately no
   * standalone Curve Patch row.
   */
  CurvePatchEffect,
};

struct PaintLayerAddParams {
  /** What the new row is. */
  PaintLayerAddKind kind = PaintLayerAddKind::Paint;
  /** The row the Add was aimed at, or null for the top of the stack. */
  MaterialPaintLayer *anchor = nullptr;
  /**
   * Where to put the new row relative to \a anchor. An \a anchor that is a folder takes the row
   * `Into` regardless of an `Above` when #fill_folder is set: an Add onto a folder means filling
   * it.
   */
  PaintLayerPlace place = PaintLayerPlace::Above;
  /**
   * Whether an \a anchor folder turns `Above` into `Into`. The Add buttons leave it set; a drop
   * clears it, because its line is drawn beside the folder and the row lands where the line was,
   * not inside the folder.
   */
  bool fill_folder = true;
  /**
   * The name for the new row, or null to derive the unique default (a `"Layer N"` name one past
   * the highest N in use).
   */
  const char *name = nullptr;
  /** The initial colour for a Fill, a Fill correction or a Fill mask correction; null keeps the default. */
  const float *fill_color = nullptr;
  /** The filter of a #PaintLayerAddKind::Filter Add, an #eMaterialPaintLayerFilter. */
  int16_t filter = 0;
  /** The material a #PaintLayerAddKind::Material, ::CorrectionMaterial or ::MaskCorrectionMaterial
   * Add reads. */
  Material *source = nullptr;
  /**
   * When set, a #PaintLayerAddKind::Material row reads a local copy of \a source made in this
   * database rather than \a source itself: each row then has settings of its own, and a linked
   * source becomes editable. Null makes the row share \a source.
   */
  Main *bmain = nullptr;
  /**
   * The brush a #PaintLayerAddKind::CurvePatch row snapshots (a full duplicate is made). Null
   * falls back to a fresh default brush, so the row always carries a tool state of its own.
   */
  Brush *curve_patch_brush = nullptr;
  /**
   * Where an #PaintLayerAddKind::AnchorPoint marker lands: the anchor row's effects or its mask.
   * The three reader kinds take their list from the kind itself. \a insert_after names the item
   * the new one follows, or null for the end; it is ignored when it belongs to another list.
   */
  PaintLayerAnchorPointList anchor_point_list = PaintLayerAnchorPointList::Effects;
  MaterialPaintLayer *insert_after = nullptr;
  /** The point an #PaintLayerAddKind::AnchorPointLayer/Correction/MaskCorrection reads. */
  bUUID anchor_point_source = {};
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
