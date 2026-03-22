/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Template for building the panel layout for the active object's modifiers.
 */

#include "BKE_context.hh"
#include "BKE_modifier.hh"
#include "BKE_screen.hh"

#include "BLI_listbase.h"

#include "ED_buttons.hh"
#include "ED_object.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"

namespace blender::ui {

static void modifier_panel_id(void *md_link, char *r_name)
{
  ModifierData *md = static_cast<ModifierData *>(md_link);
  BKE_modifier_type_panel_id(ModifierType(md->type), r_name);
}

void template_modifiers(Layout * /*layout*/, bContext *C)
{
  ARegion *region = CTX_wm_region(C);

  Object *ob = ed::object::context_active_object(C);
  ListBaseT<ModifierData> *modifiers = &ob->modifiers;

  int insert_index = -1; /* -1 means append at end (default behavior) */
  const bool show_ghost = ED_buttons_drop_active("GEOMETRY_NODES_MODIFIER", &insert_index);

  /* Force rebuild if we need to show the ghost panel, or if the panels don't match the data. */
  const bool panels_match = !show_ghost &&
                            panel_list_matches_data(region, modifiers, modifier_panel_id);

  if (!panels_match) {
    panels_free_instanced(C, region);

    int i = 0;
    for (ModifierData &md : *modifiers) {
      /* Insert ghost panel before the current modifier if this is the insertion point. */
      if (show_ghost && i == insert_index) {
        panel_add_instanced(C, region, &region->panels, "MOD_PT_Ghost", nullptr);
      }

      const ModifierTypeInfo *mti = BKE_modifier_get_info(ModifierType(md.type));
      if (mti->panel_register == nullptr) {
        /* Increment index even if we skip this modifier, to match the list position. */
        i++;
        continue;
      }

      char panel_idname[MAX_NAME];
      modifier_panel_id(&md, panel_idname);

      /* Create custom data RNA pointer. */
      PointerRNA *md_ptr = MEM_new<PointerRNA>(__func__);
      *md_ptr = RNA_pointer_create_id_subdata(ob->id, RNA_Modifier, &md);

      panel_add_instanced(C, region, &region->panels, panel_idname, md_ptr);
      i++;
    }

    /* Insert ghost panel at the end if needed. */
    if (show_ghost && i == insert_index) {
      panel_add_instanced(C, region, &region->panels, "MOD_PT_Ghost", nullptr);
    }
  }
  else {
    /* Assuming there's only one group of instanced panels, update the custom data pointers. */
    Panel *panel = static_cast<Panel *>(region->panels.first);
    for (ModifierData &md : *modifiers) {
      const ModifierTypeInfo *mti = BKE_modifier_get_info(ModifierType(md.type));
      if (mti->panel_register == nullptr) {
        continue;
      }

      /* Move to the next instanced panel corresponding to the next modifier. */
      while ((panel->type == nullptr) || !(panel->type->flag & PANEL_TYPE_INSTANCED)) {
        panel = panel->next;
        BLI_assert(panel !=
                   nullptr); /* There shouldn't be fewer panels than modifiers with UIs. */
      }

      PointerRNA *md_ptr = MEM_new<PointerRNA>(__func__);
      *md_ptr = RNA_pointer_create_id_subdata(ob->id, RNA_Modifier, &md);
      panel_custom_data_set(panel, md_ptr);

      panel = panel->next;
    }
  }
}

}  // namespace blender::ui
