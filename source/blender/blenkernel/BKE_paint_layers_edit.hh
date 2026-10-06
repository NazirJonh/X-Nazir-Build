/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * The high-level Paint Layers authoring verbs: one Add policy and the marker-addressed edits
 * whose rules are more than a direct `BKE_paint_layers_*` call. Every authoring path -- the
 * Outliner's stack source, `Material.paint_layers.new` and friends, the Layer Material tab --
 * goes through these, so the defaults (names, channels, the base mask, the source wiring) are
 * decided in exactly one place.
 *
 * The verbs only touch `Material::paint_layers` through the `BKE_paint_layers_*` API and never
 * need a #bContext, so a unit test can drive them without an editor. Reports, notifiers, the
 * eager bake of a new Material row and the active-row cursor stay the caller's business.
 */

#include <string>

#include "BLI_span.hh"
#include "BLI_string_ref.hh"

#include "BKE_paint_layers.hh"

namespace blender {

/**
 * What kind of row an Add creates. The layer kinds (Paint, Fill, Folder, Material) add a stack
 * row; the effect and mask kinds add a correction under the anchor row, the mask ones under its
 * mask section.
 */
enum class PaintLayerAddKind : int8_t {
  Paint,
  Fill,
  Folder,
  Material,
  EffectPaint,
  EffectFill,
  EffectMeshMap,
  EffectMaterial,
  EffectNodeGroup,
  EffectStack,
  MaskPaint,
  MaskFill,
  MaskMeshMap,
  MaskMaterial,
  MaskNodeGroup,
  MaskStack,
};

struct PaintLayerAddParams {
  PaintLayerAddKind kind = PaintLayerAddKind::Paint;
  /** The row the Add was aimed at, or null for the top of the stack. */
  MaterialPaintLayer *anchor = nullptr;
  /**
   * Where to put a layer kind relative to \a anchor. An \a anchor that is a folder takes the row
   * `Into` regardless: an Add onto a folder means filling it.
   */
  PaintLayerPlace place = PaintLayerPlace::Above;
  /** The data-block for Material / Node Group kinds; ignored by the others. */
  ID *source = nullptr;
  /** The initial color for a Fill layer; null keeps the default. */
  const float *fill_color = nullptr;
  /**
   * The name for the new row, or null to derive the unique default (a `"<base> N"` name one past
   * the highest N in use; a Material row names itself after its source). A correction's base name
   * is fixed by its kind; \a name overrides it.
   */
  const char *name = nullptr;
};

/**
 * Add a row with the shared authoring policy: the default channels a fresh Paint or Fill takes
 * part in, the base mask a first mask correction layers over, the source wiring for a Material or
 * Node Group correction, and the unique default names. Returns the new row, or null when the Add
 * is refused (a correction with no anchor, a Material kind with no material source, a source that
 * cannot be set).
 */
MaterialPaintLayer *BKE_paint_layers_add_with_policy(Material &ma,
                                                     const PaintLayerAddParams &params);

/**
 * The next default name for a new row: `"<base> N"`, one past the highest N in use anywhere in
 * \a ma's description. Rows are identified by their marker, never by name, so this only keeps the
 * list readable and two rows sharing a name (after a rename or a paste) is fine.
 */
std::string BKE_paint_layers_unique_default_name(const Material &ma, StringRef base);

/**
 * Move \a from relative to \a anchor, applying the row-kind rules: a correction only ever moves
 * within its owner's corrections (and never into a folder), and a move `Into` a folder groups the
 * two rows instead of relocating. \a anchor may be null to move to the top of the stack.
 */
bool BKE_paint_layers_edit_move(Material &ma,
                                MaterialPaintLayer &from,
                                MaterialPaintLayer *anchor,
                                PaintLayerPlace place);

/**
 * Add or remove \a layer's mask. One base mask per layer: an Add when one exists is refused, and
 * a Remove takes the base together with every correction over it.
 */
bool BKE_paint_layers_edit_mask_set(Material &ma, MaterialPaintLayer &layer, bool add);

/**
 * Reorder \a from to where \a to sits, within their shared owner list: both rows must be Layer
 * rows hanging on the same list.
 */
bool BKE_paint_layers_edit_reorder(Material &ma, MaterialPaintLayer &from, MaterialPaintLayer &to);

/**
 * Group the two Layer rows into a new folder, which takes their place. They must hang on the same
 * list. Returns the folder, or null on refusal.
 */
MaterialPaintLayer *BKE_paint_layers_edit_merge_down(Material &ma,
                                                     MaterialPaintLayer &upper,
                                                     MaterialPaintLayer &lower);

/**
 * Group \a rows -- every one a Layer row -- into a new folder. Returns the folder, or null on
 * refusal.
 */
MaterialPaintLayer *BKE_paint_layers_edit_group_range(Material &ma,
                                                      const Span<MaterialPaintLayer *> rows);

/**
 * Add an empty folder above \a anchor (or at the top when null), with its unique default name.
 */
MaterialPaintLayer *BKE_paint_layers_edit_group_add(Material &ma, MaterialPaintLayer *anchor);

}  // namespace blender
