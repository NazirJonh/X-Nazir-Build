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

#include <climits>

#include "DNA_ID.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"

#include "BLI_string.h"

#include "ED_screen.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../outliner_intern.hh"
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

void OUTLINER_OT_stack_layer_focus(wmOperatorType *ot)
{
  ot->name = "Focus Stack";
  ot->idname = "OUTLINER_OT_stack_layer_focus";
  ot->description = "Show the layer stack of an object in the Outliner";
  ot->exec = stack_focus_exec;
  ot->poll = ED_operator_outliner_active;
  ot->flag = OPTYPE_REGISTER;

  RNA_def_string(ot->srna, "object", nullptr, MAX_ID_NAME - 2, "Object", "Object to focus");
  RNA_def_string(ot->srna,
                 "library",
                 nullptr,
                 MAX_ID_NAME - 2,
                 "Library",
                 "Name of the library the object is linked from; empty for a local object");
  RNA_def_int(ot->srna,
              "sub_index",
              -1,
              -1,
              SHRT_MAX,
              "Index",
              "Stack index within the object, such as a material slot; -1 uses the active one",
              -1,
              SHRT_MAX);
  RNA_def_boolean(ot->srna,
                  "enter_edit_mode",
                  true,
                  "Enter Edit Mode",
                  "Enter the edit mode the source works in");
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
