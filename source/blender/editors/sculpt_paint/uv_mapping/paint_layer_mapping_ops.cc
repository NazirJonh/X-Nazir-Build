/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "paint_layer_mapping_ops.hh"

#include <optional>

#include "DNA_material_types.h"
#include "DNA_windowmanager_types.h"

#include "BLI_assert.h"

#include "BKE_context.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_target.hh"
#include "BKE_report.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"

#include "uv_mapping_target.hh"

namespace blender::ed::sculpt_paint::uv_mapping {

/* -------------------------------------------------------------------- */
/** \name Scale Lock Toggle
 * \{ */

/** Toggle the scale lock of the row the Mapping panel and the gizmo edit. */
static bool layer_mapping_scale_lock_toggle_poll(bContext *C)
{
  PaintLayerRowMappingTarget target;
  return target.resolve(*C);
}

static wmOperatorStatus layer_mapping_scale_lock_toggle_exec(bContext *C, wmOperator * /*op*/)
{
  PaintLayerRowMappingTarget target;
  if (!target.resolve(*C)) {
    return OPERATOR_CANCELLED;
  }
  if (!target.set_scale_locked(!target.scale_locked())) {
    return OPERATOR_CANCELLED;
  }
  WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, target.material());
  return OPERATOR_FINISHED;
}

void PAINT_OT_layer_mapping_scale_lock_toggle(wmOperatorType *ot)
{
  ot->name = "Toggle Layer Mapping Scale Lock";
  ot->idname = "PAINT_OT_layer_mapping_scale_lock_toggle";
  ot->description = "Lock or unlock the scale axes of the shown layer row's UV mapping";
  ot->exec = layer_mapping_scale_lock_toggle_exec;
  ot->poll = layer_mapping_scale_lock_toggle_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Reset Layer Mapping
 *
 * The Mapping panel's reset: the mapping values of the shown row go back to their defaults, all
 * at once or one part at a time. Scale and Rotation hold the tile's centre on the canvas while
 * they put the size and the angle back, the way the panel's fields and the gizmo do. The offset
 * is the tile's position itself, so resetting it -- or everything at once -- moves the tile back
 * to the UV origin. The enabled flag and the UV wrap are modes, not values, so no reset touches
 * them.
 * \{ */

/** Which parts of the shown row's mapping the reset puts back. */
enum LayerMappingResetMode {
  LAYER_MAPPING_RESET_ALL = 0,
  LAYER_MAPPING_RESET_OFFSET = 1,
  LAYER_MAPPING_RESET_SCALE = 2,
  LAYER_MAPPING_RESET_ROTATION = 3,
};

static const EnumPropertyItem layer_mapping_reset_items[] = {
    {LAYER_MAPPING_RESET_ALL,
     "ALL",
     0,
     "Reset All",
     "Put the offset, the scale and the rotation back to their defaults; the tile returns to the "
     "UV origin"},
    {LAYER_MAPPING_RESET_OFFSET,
     "OFFSET",
     0,
     "Reset Offset",
     "Move the tile back to the UV origin"},
    {LAYER_MAPPING_RESET_SCALE,
     "SCALE",
     0,
     "Reset Scale",
     "Size the tile back to one, keeping its centre on the canvas"},
    {LAYER_MAPPING_RESET_ROTATION,
     "ROTATION",
     0,
     "Reset Rotation",
     "Turn the tile back upright around its centre"},
    {0, nullptr, 0, nullptr, nullptr},
};

struct LayerMappingResetTarget {
  Material *ma = nullptr;
  MaterialPaintLayer *row = nullptr;
};

/** The row the Mapping panel edits: the reset answers with it, so it is enabled with the panel. */
static std::optional<LayerMappingResetTarget> layer_mapping_reset_target(bContext *C)
{
  Material *ma = BKE_paint_layers_active_material_get(CTX_data_active_object(C));
  MaterialPaintLayer *row = (ma != nullptr) ? mapping_shown_row_get(*C, *ma) : nullptr;
  if (row == nullptr || !BKE_paint_layers_mapping_available(*ma, *row)) {
    return std::nullopt;
  }
  return LayerMappingResetTarget{ma, row};
}

static bool layer_mapping_reset_poll(bContext *C)
{
  return layer_mapping_reset_target(C).has_value();
}

static wmOperatorStatus layer_mapping_reset_exec(bContext *C, wmOperator *op)
{
  const std::optional<LayerMappingResetTarget> target = layer_mapping_reset_target(C);
  if (!target.has_value()) {
    BKE_report(op->reports, RPT_ERROR, "The shown layer row takes no UV mapping");
    return OPERATOR_CANCELLED;
  }
  Material &ma = *target->ma;
  MaterialPaintLayer &row = *target->row;

  const float zero_offset[2] = {0.0f, 0.0f};
  const float unit_scale[2] = {1.0f, 1.0f};
  switch (RNA_enum_get(op->ptr, "mode")) {
    case LAYER_MAPPING_RESET_OFFSET:
      /* The offset is the tile's position itself: putting it back is what moves the tile home. */
      BKE_paint_layers_mapping_set_offset(ma, &row, zero_offset);
      break;
    case LAYER_MAPPING_RESET_SCALE:
      BKE_paint_layers_mapping_set_scale_holding_tile(ma, &row, unit_scale);
      break;
    case LAYER_MAPPING_RESET_ROTATION:
      BKE_paint_layers_mapping_set_rotation_holding_tile(ma, &row, 0.0f);
      break;
    case LAYER_MAPPING_RESET_ALL:
      /* One raw call for all: the defaults are the UV origin, so the tile returns there, and the
       * setters normalize and follow the scale lock the same way the panel's fields do. */
      BKE_paint_layers_mapping_set_values(ma, &row, zero_offset, unit_scale, 0.0f);
      break;
    default:
      BLI_assert_unreachable();
      return OPERATOR_CANCELLED;
  }

  WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, &ma);
  return OPERATOR_FINISHED;
}

void PAINT_OT_layer_mapping_reset(wmOperatorType *ot)
{
  ot->name = "Reset Layer Mapping";
  ot->idname = "PAINT_OT_layer_mapping_reset";
  ot->description =
      "Put the shown layer row's UV mapping values back to their defaults, all at once or one "
      "part at a time";
  ot->exec = layer_mapping_reset_exec;
  ot->poll = layer_mapping_reset_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  ot->prop = RNA_def_enum(ot->srna,
                          "mode",
                          layer_mapping_reset_items,
                          LAYER_MAPPING_RESET_ALL,
                          "Mode",
                          "Which part of the mapping to reset");
}

/** \} */

}  // namespace blender::ed::sculpt_paint::uv_mapping
