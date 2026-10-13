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

#include "BLI_string_ref.hh"

#include "BKE_paint_layers.hh"

namespace blender {

/** What kind of row the Add creates. */
enum class PaintLayerAddKind : int8_t {
  /** A painted layer. */
  Paint = 0,
  /** A flat-colour Fill layer. */
  Fill,
};

struct PaintLayerAddParams {
  /** What the new row is. */
  PaintLayerAddKind kind = PaintLayerAddKind::Paint;
  /** The row the Add was aimed at, or null for the top of the stack. */
  MaterialPaintLayer *anchor = nullptr;
  /** Where to put the new row relative to \a anchor. */
  PaintLayerPlace place = PaintLayerPlace::Above;
  /**
   * The name for the new row, or null to derive the unique default (a `"Layer N"` name one past
   * the highest N in use).
   */
  const char *name = nullptr;
  /** The initial colour for a Fill; null keeps the transparent default. */
  const float *fill_color = nullptr;
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
 * Reorder \a from to where \a to sits in the stack: \a from takes the position \a to had.
 * Returns false when \a from and \a to are the same row or either is not part of \a ma.
 */
bool BKE_paint_layers_edit_reorder(Material &ma, MaterialPaintLayer &from, MaterialPaintLayer &to);

}  // namespace blender
