/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 *
 * Navigation and activation for the Stack Layers display mode.
 *
 * Everything here is phrased in rows and ordinals. What a row *is* -- a paint layer, a shape key,
 * whatever comes next -- is the business of the #StackSource the space is set to, so the operators
 * below never mention a material, an image or a node.
 */

#include <algorithm>
#include <climits>
#include <cstdio>
#include <functional>

#include "DNA_defs.h"
#include "DNA_ID.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_report.hh"

#include "BLI_listbase_wrapper.hh"
#include "BLI_map.hh"
#include "BLI_math_color.h"
#include "BLI_mempool.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_uuid.h"

#include "BLT_translation.hh"

#include "ED_object.hh"
#include "ED_outliner_stack_automation.hh"
#include "ED_screen.hh"
#include "ED_undo.hh"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"
#include "RNA_enum_types.hh"
#include "RNA_define.hh"

#include "UI_interface_c.hh"
#include "UI_interface_icons.hh"
#include "UI_interface_layout.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../outliner_intern.hh"
#include "../tree/tree_iterator.hh"
#include "outliner_stack_source.hh"

namespace blender::ed::outliner {

/**
 * The identity property every operator that addresses an existing row shares: the row's marker,
 * which survives the renumbering an edit puts the position through. Read through
 * #stack_operator_ordinal_get, so a caller names a row either way.
 */
static void rna_def_stack_row_marker(wmOperatorType *ot)
{
  RNA_def_string(ot->srna,
                 "marker",
                 nullptr,
                 0,
                 "Marker",
                 "The row's identity marker, as this mode issued it; takes precedence over the "
                 "position, which is only the row's at the time of the call");
}

void OUTLINER_OT_stack_layers_back(wmOperatorType *ot)
{
  ot->name = "Back to Stack Objects";
  ot->idname = "OUTLINER_OT_stack_layers_back";
  ot->description =
      "Return to the View Layer; hold Alt to stay and show the objects that have a layer stack";
  ot->invoke = stack_back_invoke;
  ot->exec = stack_back_exec;
  ot->poll = ED_operator_outliner_active;
  ot->flag = OPTYPE_REGISTER;
  RNA_def_boolean(ot->srna,
                  "keep_view",
                  false,
                  "Keep Mode",
                  "Stay in Stack Layers and show the objects that have a layer stack, rather "
                  "than leaving the mode");
}

void OUTLINER_OT_stack_layer_activate(wmOperatorType *ot)
{
  ot->name = "Activate Stack Layer";
  ot->idname = "OUTLINER_OT_stack_layer_activate";
  ot->description = "Make a stack layer the one the rest of Blender acts on";
  ot->exec = stack_row_activate_exec;
  ot->poll = stack_row_activate_poll;
  ot->flag = OPTYPE_UNDO | OPTYPE_INTERNAL;

  RNA_def_int(ot->srna,
              "ordinal",
              0,
              0,
              SHRT_MAX,
              "Ordinal",
              "Position of the layer in the stack",
              0,
              SHRT_MAX);
  rna_def_stack_row_marker(ot);
}

}  // namespace blender::ed::outliner
