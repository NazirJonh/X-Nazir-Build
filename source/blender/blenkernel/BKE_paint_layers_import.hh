/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * Importing rows from another material. This is the general copy mechanism Smart assets use, not
 * part of the asset format itself.
 */

#include <cstdint>

#include "BLI_span.hh"

#include "BKE_paint_layers.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Importing rows from another material
 *
 * A smart asset is a Material whose stack holds the rows the user saved. Bringing it into another
 * material copies those rows (and, for a Smart Mask, a row's mask stack) across the material
 * boundary: new markers, remapped anchor points, copies of the maps, and channels filtered against
 * the target's set unless the import is allowed to widen it.
 * \{ */

/** What an import copies: rows of the stack, or one row's mask stack. */
enum class PaintLayerImportMode : int8_t {
  /** The stack rows named by the caller. */
  Rows = 0,
  /** The mask items of the single row named by the caller. */
  Mask,
};

enum PaintLayerImportFlag : uint16_t {
  /** Lay the imported mask over the target's own instead of replacing it. */
  PAINT_LAYER_IMPORT_APPEND_MASK = 1 << 0,
  /**
   * Widen the target's channel set with the channels the imported rows take part in. A smart asset
   * sets it: the asset's channels have to survive an import into a material that never enabled
   * them, the way a smart material brings its own channels in Substance Painter.
   */
  PAINT_LAYER_IMPORT_ENABLE_SOURCE_CHANNELS = 1 << 1,
  /**
   * Drop Curve Patch effects instead of copying them. A Smart asset sets it: the payload's curves
   * are in the source geometry's space, so an imported copy would show nothing meaningful.
   */
  PAINT_LAYER_IMPORT_SKIP_CURVE_PATCH = 1 << 2,
};

/**
 * What an import does with the maps a copied row carries.
 *
 * A map is either painted or read from somewhere else. Painted maps are the ones whose pixels
 * exist only in the source's memory: a generated image, or one the user painted on since it was
 * loaded. Any other map is a texture the row reads, whose pixels live in a file.
 */
enum class PaintLayerImageImportPolicy : int8_t {
  /** Legacy: copy every map with its pixels, whether it is painted or reads a file. */
  Copy = 0,
  /**
   * Read external maps by reference (the same Image, with one more user) and copy painted maps
   * with their pixels. A stroke on a shared external map copies it first (copy-on-paint), so the
   * imported row cannot paint into the source's texture.
   */
  Share = 1,
  /**
   * Read external maps by reference, as #PaintLayerImageImportPolicy::Share, and leave painted maps
   * behind: the record keeps its flat value and no map. What a Smart asset saved from painted maps
   * without their pixels wants, so it is small and depends on no generated buffer.
   */
  Strip = 2,
};

struct PaintLayerImportParams {
  PaintLayerImportMode mode = PaintLayerImportMode::Rows;
  /** The row the import is aimed at, or null to append rows at the top of the stack. */
  MaterialPaintLayer *anchor = nullptr;
  PaintLayerPlace place = PaintLayerPlace::Above;
  /** Treat Above/Below on a folder as Into, the way the Add buttons do. */
  bool fill_folder = false;
  uint16_t flags = 0;
  /**
   * How the maps of the copied rows travel; #PaintLayerImageImportPolicy::Copy keeps the legacy
   * behavior of copying every map with its pixels.
   */
  PaintLayerImageImportPolicy image_policy = PaintLayerImageImportPolicy::Copy;
};

struct PaintLayerImportResult {
  /** The first inserted row, or the mask's owner in #PaintLayerImportMode::Mask. */
  MaterialPaintLayer *first = nullptr;
  /** Number of imported rows, or mask items in mask mode. */
  int rows = 0;
  /**
   * Channel records dropped because the target does not take part in them. Rows mode only: mask
   * items keep their Base Color record as the mask's own grey, whatever the content set says.
   */
  int skipped_channels = 0;
  /**
   * A source row carried an active object mask; its per-object choices stay with the objects.
   * Rows mode only; a mask import does not read the source row's content.
   */
  bool dropped_object_masks = false;
  /** Curve Patch effects left out because #PAINT_LAYER_IMPORT_SKIP_CURVE_PATCH is set. */
  int dropped_curve_patches = 0;
  /**
   * Number of channel records whose map was replaced by its flat value because
   * #PaintLayerImportParams::image_policy was #PaintLayerImageImportPolicy::Strip: painted maps
   * only, mask items' Base Color records, corrections and children included.
   */
  int stripped_maps = 0;
};

/**
 * Copy \a rows of \a src into \a dst at \a params' place.
 *
 * The rows have to be rows of \a src's stack (see #BKE_paint_layers_owner_list); a correction or a
 * mask item is refused. In #PaintLayerImportMode::Mask exactly one row is expected, and its
 * #MaterialPaintLayer::mask_stack is copied onto \a params.anchor, replacing the anchor's mask
 * unless #PAINT_LAYER_IMPORT_APPEND_MASK is set.
 *
 * Each copied row gets a marker of its own, its maps travel by \a params' image policy (a copy with
 * its pixels by default; an external map may be shared by reference, and a painted map may be left
 * behind as the record's flat value), its anchor point readers are remapped to the copied points,
 * and a reader that named a point outside the imported set loses the reference. A Material row brings a local copy of the material
 * it reads, the same way an ordinary duplicate does, so the asset does not depend on the source
 * file's material staying loaded. Object masks are per object and keyed by the old marker, so they
 * are not imported; the copied row still keeps the mesh/UV-tiles mode but carries no exclusions.
 *
 * \return false when the import is refused (no rows, a foreign row, a missing mask, an invalid
 * anchor); \a dst is left untouched in that case.
 */
bool BKE_paint_layers_import_rows(Main &bmain,
                                  Material &dst,
                                  Material &src,
                                  Span<const MaterialPaintLayer *> rows,
                                  const PaintLayerImportParams &params,
                                  PaintLayerImportResult *r_result = nullptr);

/** \} */

}  // namespace blender
