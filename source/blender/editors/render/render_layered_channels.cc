/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edrend
 *
 * The channel checkboxes shared by the New Layered Material and Convert Material to Layered
 * dialogs, so both offer the same channels in the same order and read them back the same way.
 */

#include <iterator>
#include <optional>

#include "BLI_utildefines.h"

#include "DNA_scene_types.h"

#include "BLT_translation.hh"

#include "BKE_paint.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_types.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"

#include "render_intern.hh" /* own include */

namespace blender {


/* -------------------------------------------------------------------- */
/** \name Shared channel dialog
 * \{ */

/* The names, the order and the channels mirror #BKE_paint_material_channels; the table is spelled
 * out because the RNA property names below are a stored interface (keymaps and scripts read them):
 * deriving them from a UI string would silently rename one if a label ever changed. */
static const LayeredChannelOption layered_channels[] = {
    {PAINT_MATERIAL_CHANNEL_BASE_COLOR, "channel_base_color", N_("Base Color")},
    {PAINT_MATERIAL_CHANNEL_METALLIC, "channel_metallic", N_("Metallic")},
    {PAINT_MATERIAL_CHANNEL_ROUGHNESS, "channel_roughness", N_("Roughness")},
    {PAINT_MATERIAL_CHANNEL_SPECULAR, "channel_specular", N_("Specular")},
    {PAINT_MATERIAL_CHANNEL_NORMAL, "channel_normal", N_("Normal")},
    {PAINT_MATERIAL_CHANNEL_HEIGHT, "channel_height", N_("Height")},
    {PAINT_MATERIAL_CHANNEL_ALPHA, "channel_alpha", N_("Alpha")},
    {PAINT_MATERIAL_CHANNEL_AO, "channel_ao", N_("AO")},
    {PAINT_MATERIAL_CHANNEL_EMISSION, "channel_emission", N_("Emission")},
    /* The channels past Emission are map-only Principled inputs most materials never paint, so
     * they sit in the Advanced sub-panel and start unticked. */
    {PAINT_MATERIAL_CHANNEL_EMISSION_STRENGTH,
     "channel_emission_strength",
     N_("Emission Strength"),
     true},
    {PAINT_MATERIAL_CHANNEL_TRANSMISSION, "channel_transmission", N_("Transmission"), true},
    {PAINT_MATERIAL_CHANNEL_SUBSURFACE, "channel_subsurface", N_("Subsurface"), true},
    {PAINT_MATERIAL_CHANNEL_COAT, "channel_coat", N_("Coat"), true},
    {PAINT_MATERIAL_CHANNEL_COAT_ROUGHNESS, "channel_coat_roughness", N_("Coat Roughness"), true},
    {PAINT_MATERIAL_CHANNEL_SHEEN, "channel_sheen", N_("Sheen"), true},
    {PAINT_MATERIAL_CHANNEL_SPECULAR_TINT, "channel_specular_tint", N_("Specular Tint"), true},
    {PAINT_MATERIAL_CHANNEL_ANISOTROPIC, "channel_anisotropic", N_("Anisotropic"), true},
};

const LayeredChannelOption *layered_channel_options(int &r_count)
{
  r_count = int(std::size(layered_channels));
  return layered_channels;
}

PaintChannelMask layered_channel_mask_from_op(wmOperator *op)
{
  PaintChannelMask mask = 0;
  for (const LayeredChannelOption &entry : layered_channels) {
    if (RNA_boolean_get(op->ptr, entry.prop_name)) {
      mask |= uint32_t(1) << int(entry.channel);
    }
  }
  return mask;
}

void layered_channels_ui(bContext *C, wmOperator *op)
{
  ui::Layout &col = op->layout->column(false);
  for (const LayeredChannelOption &entry : layered_channels) {
    if (entry.advanced) {
      continue;
    }
    ui::Layout &row = col.row(true);
    row.prop(op->ptr, entry.prop_name, UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  if (ui::Layout *advanced = op->layout->panel(
          C, "layered_channels_advanced", true, IFACE_("Advanced")))
  {
    ui::Layout &advanced_col = advanced->column(false);
    for (const LayeredChannelOption &entry : layered_channels) {
      if (entry.advanced) {
        advanced_col.prop(op->ptr, entry.prop_name, UI_ITEM_NONE, std::nullopt, ICON_NONE);
      }
    }
  }
}

void layered_channels_rna_define(wmOperatorType *ot,
                                 const PaintChannelMask default_mask,
                                 const char *description)
{
#ifndef NDEBUG
  /* The table is a hand-kept copy of #BKE_paint_material_channels (minus Custom); a channel added
   * there but not here would silently vanish from the dialog. */
  {
    int table_index = 0;
    for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
      if (info.channel == PAINT_MATERIAL_CHANNEL_CUSTOM) {
        continue;
      }
      BLI_assert(table_index < int(std::size(layered_channels)));
      BLI_assert(layered_channels[table_index].channel == info.channel);
      table_index++;
    }
    BLI_assert(table_index == int(std::size(layered_channels)));
  }
#endif
  for (const LayeredChannelOption &entry : layered_channels) {
    RNA_def_boolean(ot->srna,
                    entry.prop_name,
                    (default_mask & (uint32_t(1) << int(entry.channel))) != 0,
                    entry.ui_name,
                    description);
  }
}

/** \} */

}  // namespace blender
