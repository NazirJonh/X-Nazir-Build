/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup spoutliner
 *
 * Internal seams of the paint-layers stack source, shared between its implementation and its unit
 * test. The paint source reads and writes `Material::paint_layers` directly through the
 * `BKE_paint_layers_*` API by marker, so these helpers name the concrete material type the source
 * is built on -- exactly what the generic Outliner code must not see. Declaring them once here lets
 * the test drive them without an Outliner (or a #bContext at all).
 */

#include <cstdint>
#include <memory>

#include "BLI_vector.hh"

#include "outliner_stack_source.hh"

namespace blender {

struct bContext;
struct EnumPropertyItem;
struct ID;
struct Main;
struct Material;
struct MaterialPaintLayer;
struct PointerRNA;
struct PropertyRNA;
struct ReportList;

/** The channel a paint record belongs to; defined in `DNA_scene_types.h`. */
enum eMaterialPaintChannel : int8_t;

namespace ed::outliner {

/**
 * Rows for a layered material, built from its DNA description rather than a graph: one flat Paint
 * row per entry of `Material::paint_layers`, value and mode columns addressed through the row.
 *
 * \param channel: the #eMaterialPaintChannel the value columns address. Until the per (row,
 *                 channel) settings exist it only affects which edits rebuild the rows.
 */
void paint_stack_rows_from_description(const Material &material,
                                       int channel,
                                       Vector<StackRow> &r_rows);

/** The description row a flat-walk ordinal names, or null. */
MaterialPaintLayer *paint_description_row_for_ordinal(Material &material, int ordinal);

/** The ordinal #paint_stack_rows_from_description gives \a target, or -1. */
int paint_layers_ordinal_of(const Material &material, const MaterialPaintLayer *target);

/** The description edits behind the source's rename and duplicate verbs, free of a #bContext. */
bool paint_layers_edit_rename(Material &material, int ordinal, const char *name);
int paint_layers_edit_duplicate(Main &bmain, Material &material, int ordinal);
/** Set the row's display color tag; zero clears it. Only a folder row accepts one. */
bool paint_layers_edit_color_tag(Material &material, int ordinal, int color_tag);
/** Drop the paint target (the active row); false when there was none. */
bool paint_layers_edit_target_clear(Material &material);

/**
 * Create the drop handler of the paint-layers source: the built-in image and material actions,
 * plus the FileHandler fallback for files no built-in action takes.
 */
std::unique_ptr<StackDropHandler> paint_layers_drop_handler_create();

/** The icon the channel menu shows for \a channel, the one its enum item uses. */
int paint_channel_icon(int channel);

/**
 * The channel enum items the image-assign popup offers: the material's channel set, minus the
 * custom slot, each with #paint_channel_icon.
 */
const EnumPropertyItem *stack_channel_image_assign_channel_itemf(bContext *C,
                                                                 PointerRNA *ptr,
                                                                 PropertyRNA *prop,
                                                                 bool *r_free);

/**
 * Why an image cannot land on \a layer's \a channel, reported to \a reports, or true when it can:
 * the one answer the drop popup and a clipboard paste give. \a layer may be null when the image
 * would create a row of its own, in which case only the material's channel set is checked.
 */
bool stack_image_assign_check(const Material &material,
                              const MaterialPaintLayer *layer,
                              eMaterialPaintChannel channel,
                              ReportList *reports);

/** True when \a material takes a paste; reports why it does not otherwise. */
bool stack_clipboard_material_editable_check(const Material &material, ReportList *reports);

/**
 * The Material Symbols codepoints `space_outliner.py` repeats by hand for its header buttons.
 * The test pins these values so the Python constants cannot drift silently from the C++ ones.
 *
 * These three have external linkage -- `extern` here, defined in the .cc -- because the test needs
 * to read them; the rest of `paint_glyph` stays `constexpr` in the .cc, since only the code there
 * draws those glyphs and nothing outside has to name them.
 */
namespace paint_glyph {
extern const uint32_t BRUSH;
extern const uint32_t FILL;
extern const uint32_t GRADIENT;
}  // namespace paint_glyph

}  // namespace ed::outliner
}  // namespace blender
