/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The view pass a layered material previews in the 3D Viewport: choosing it (the header menu, the
 * cycle operator), keeping it equal with the Image Editors, and returning it to Combined when
 * Sculpt Mode is left (the Restore Combined on Exit option) and back when the object re-enters.
 * Not an operator: the menu goes through RNA setters, and the mode transitions call
 * `object_sculpt_mode_enter/exit` directly.
 */

#include "BLI_function_ref.hh"
#include "BLI_index_range.hh"
/* The ListBase loops below need the iterator's definition: the DNA header only forward-declares
 * it (a DNA header cannot include a BLI one). */
#include "BLI_listbase_iterator.hh"
#include "BLI_set.hh"
#include "BLI_span.hh"

#include "DNA_layer_types.h"
#include "DNA_material_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_layer.hh"
/* The ID_IS_EDITABLE check expands to the library runtime's tag: the struct and its enum live
 * here, the DNA header of the library only forward-declares the struct. */
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_generate.hh"
#include "BKE_wm_runtime.hh"

#include "ED_image.hh"
#include "ED_paint.hh"

#include "RNA_define.hh"

#include "UI_resources.hh"

#include "WM_api.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Scene options (the active window's scene owns both)
 * \{ */

/** The scene of the active window: its tool settings own the options to sync the view pass and to
 * restore Combined on mode exit. */
static Scene *view_pass_sync_scene(Main &bmain)
{
  for (wmWindowManager &wm : bmain.wm) {
    const wmWindow *win = wm.runtime->winactive;
    if (win == nullptr) {
      win = static_cast<const wmWindow *>(wm.windows.first);
    }
    if (win != nullptr && win->scene != nullptr) {
      return win->scene;
    }
  }
  return nullptr;
}

bool ED_paint_view_pass_sync_get(Main &bmain)
{
  const Scene *scene = view_pass_sync_scene(bmain);
  return scene == nullptr || scene->toolsettings == nullptr ||
         scene->toolsettings->paint_mode.view_pass_sync_off == 0;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The menu's action items (the checkboxes)
 * \{ */

void ED_paint_view_pass_action_items_add(EnumPropertyItem **r_item,
                                         int *r_totitem,
                                         Main *bmain,
                                         const bool for_image_editor)
{
  EnumPropertyItem separator{};
  separator.identifier = "";
  RNA_enum_item_add(r_item, r_totitem, &separator);

  EnumPropertyItem sync{};
  sync.value = RNA_ENUM_VALUE_ACTION;
  sync.identifier = "SYNC_VIEW_PASS";
  sync.name = "Sync Views";
  /* The canvas menu speaks of "this editor"; the material menu has no editor of its own. */
  if (for_image_editor) {
    sync.description = "Show the same paint pass in the 3D Viewport and in this editor";
  }
  else {
    sync.description = "Show the same paint pass in the 3D Viewport and in the Image Editor";
  }
  sync.icon = (bmain != nullptr && ED_paint_view_pass_sync_get(*bmain)) ? ICON_CHECKBOX_HLT :
                                                                          ICON_CHECKBOX_DEHLT;
  RNA_enum_item_add(r_item, r_totitem, &sync);

  EnumPropertyItem restore{};
  restore.value = RNA_enum_value_action(1);
  restore.identifier = "RESTORE_COMBINED_ON_EXIT";
  restore.name = "Restore Combined on Exit";
  if (for_image_editor) {
    restore.description =
        "Return the 3D Viewport's view pass to Combined when the object leaves Sculpt Mode, so it "
        "is shaded as the material again while outside the mode; the pass comes back when the "
        "object re-enters Sculpt Mode";
  }
  else {
    restore.description =
        "Return the view pass to Combined when the object leaves Sculpt Mode, so it is shaded as "
        "the material again while outside the mode; the pass comes back when the object re-enters "
        "Sculpt Mode";
  }
  restore.icon = (bmain != nullptr && ED_paint_view_pass_restore_get(*bmain)) ? ICON_CHECKBOX_HLT :
                                                                                ICON_CHECKBOX_DEHLT;
  RNA_enum_item_add(r_item, r_totitem, &restore);
}

bool ED_paint_view_pass_action_apply(Main &bmain, const int value)
{
  if (!RNA_enum_value_is_action(value)) {
    return false;
  }
  switch (RNA_enum_value_action_index(value)) {
    case 0:
      ED_paint_view_pass_sync_toggle(bmain);
      return true;
    case 1:
      ED_paint_view_pass_restore_toggle(bmain);
      return true;
    default:
      /* An action no checkbox above corresponds to: consumed all the same, so the setters never
       * store it as a pass. */
      return true;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Image Editors that follow the 3D Viewport's pass
 * \{ */

/**
 * Put every Image Editor that shows a composite of \a ma on \a pass. An editor that shows a layer
 * map is left alone: it is the canvas a stroke writes into, and a pass chosen elsewhere must not
 * take it away.
 */
static void view_pass_images_set(Main &bmain, Material &ma, const int pass)
{
  /* The Image Editor's list has no Height; keep what it shows rather than guessing. */
  if (pass == PAINT_MATERIAL_CHANNEL_HEIGHT) {
    return;
  }
  for (bScreen &screen : bmain.screens) {
    for (ScrArea &area : screen.areabase) {
      for (SpaceLink &space : area.spacedata) {
        if (space.spacetype != SPACE_IMAGE) {
          continue;
        }
        SpaceImage &sima = reinterpret_cast<SpaceImage &>(space);
        if (sima.mode != SI_MODE_PAINT || (sima.flag & SI_PAINT_COMPOSITE_MODE) == 0 ||
            sima.image == nullptr || ED_space_image_composite_material_get(&bmain, sima.image) != &ma)
        {
          continue;
        }
        sima.material_paint_pass = pass;
      }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Choosing a pass (menu and cycle operator)
 * \{ */

void ED_paint_material_view_pass_apply(Main &bmain, Material &ma)
{
  /* A pass chosen by hand is the view the material should resume: a pass parked by an earlier
   * mode exit would otherwise come back over it at the next entry. */
  ma.paint_layers_view_pass_prev = 0;
  BKE_paint_layers_view_pass_sync(&bmain, ma);
  const int pass = BKE_paint_layers_view_pass_get(ma);
  if (ED_paint_view_pass_sync_get(bmain)) {
    view_pass_images_set(bmain, ma, pass);
  }

  /* A Sculpt Mode stroke can span several selected objects: the pass is the view of all their
   * active materials, not only the active object's. */
  Set<Material *> materials;
  for (wmWindowManager &wm : bmain.wm) {
    for (wmWindow &window : wm.windows) {
      if (window.scene == nullptr) {
        continue;
      }
      const ViewLayer *view_layer = BKE_view_layer_find(window.scene, window.view_layer_name);
      if (view_layer == nullptr) {
        continue;
      }
      for (const Base &base : view_layer->object_bases) {
        const Object *ob = base.object;
        if ((base.flag & BASE_SELECTED) == 0 || ob == nullptr || ob->type != OB_MESH ||
            ob->mode != OB_MODE_SCULPT)
        {
          continue;
        }
        Material *other = BKE_object_material_get(const_cast<Object *>(ob), ob->actcol);
        if (other != nullptr && other != &ma && BKE_paint_layers_is_layered(*other) &&
            ID_IS_EDITABLE(&other->id))
        {
          materials.add(other);
        }
      }
    }
  }

  for (Material *other : materials) {
    other->paint_layers_view_pass_prev = 0;
    BKE_paint_layers_view_pass_set(*other,
                                   BKE_paint_layers_view_pass_available(*other, pass) ?
                                       pass :
                                       PAINT_LAYER_PASS_COMBINED);
    /* Synced even when the pass did not change: the viewer nodes are derived data and may be
     * missing after a copy or an undo. */
    BKE_paint_layers_view_pass_sync(&bmain, *other);
  }
  WM_main_add_notifier(NC_MATERIAL | ND_SHADING, nullptr);
  WM_main_add_notifier(NC_SPACE | ND_SPACE_IMAGE, nullptr);
}

void ED_paint_view_pass_sync_toggle(Main &bmain)
{
  Scene *scene = view_pass_sync_scene(bmain);
  if (scene == nullptr || scene->toolsettings == nullptr) {
    return;
  }
  char &sync_off = scene->toolsettings->paint_mode.view_pass_sync_off;
  sync_off = (sync_off == 0) ? 1 : 0;
  /* The menu's checkbox lives in the item callbacks, not in an RNA button state, so no property
   * update runs for it: this notifier is what redraws the header the checkbox is shown in. */
  WM_main_add_notifier(NC_SPACE | ND_SPACE_VIEW3D, nullptr);
  if (sync_off != 0) {
    return;
  }
  /* Turned on: the 3D Viewport's pass is the reference the Image Editors catch up with. */
  ViewLayer *view_layer = BKE_view_layer_default_view(scene);
  Object *ob = (view_layer != nullptr) ? BKE_view_layer_active_object_get(view_layer) : nullptr;
  Material *ma = (ob != nullptr) ? BKE_object_material_get(ob, ob->actcol) : nullptr;
  if (ma != nullptr && BKE_paint_layers_is_layered(*ma)) {
    ED_paint_material_view_pass_apply(bmain, *ma);
  }
}

void ED_paint_material_view_pass_from_image(Main &bmain, SpaceImage &sima)
{
  if (sima.image == nullptr || (sima.flag & SI_PAINT_COMPOSITE_MODE) == 0 ||
      !ED_paint_view_pass_sync_get(bmain))
  {
    return;
  }
  Material *ma = ED_space_image_composite_material_get(&bmain, sima.image);
  if (ma == nullptr || !BKE_paint_layers_is_layered(*ma) || !ID_IS_EDITABLE(&ma->id)) {
    return;
  }
  const int pass = sima.material_paint_pass;
  BKE_paint_layers_view_pass_set(*ma,
                                 BKE_paint_layers_view_pass_available(*ma, pass) ?
                                     pass :
                                     PAINT_LAYER_PASS_COMBINED);
  ED_paint_material_view_pass_apply(bmain, *ma);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Restore Combined on exit, recall on re-entry
 * \{ */

/**
 * The scene the mode transition reads its options from: \a scene when the caller has one -- the
 * switch may run for a background window or another file, whose scene is not the active window's
 * -- else the active window's, the one a menu click names. Both options live in the same tool
 * settings, so this one scene answers for the pair.
 */
static const Scene *view_pass_option_scene(Main &bmain, const Scene *scene)
{
  return (scene != nullptr) ? scene : view_pass_sync_scene(bmain);
}

/** The Restore Combined on Exit option as \a scene (possibly null, meaning no scene at all) has
 * it. */
static bool view_pass_restore_scene_get(const Scene *scene)
{
  return scene == nullptr || scene->toolsettings == nullptr ||
         scene->toolsettings->paint_mode.view_pass_restore_off == 0;
}

/** The Sync Views option as \a scene (possibly null, meaning no scene at all) has it. */
static bool view_pass_sync_scene_get(const Scene *scene)
{
  return scene == nullptr || scene->toolsettings == nullptr ||
         scene->toolsettings->paint_mode.view_pass_sync_off == 0;
}

bool ED_paint_view_pass_restore_get(Main &bmain)
{
  return view_pass_restore_scene_get(view_pass_sync_scene(bmain));
}

void ED_paint_view_pass_restore_toggle(Main &bmain)
{
  Scene *scene = view_pass_sync_scene(bmain);
  if (scene == nullptr || scene->toolsettings == nullptr) {
    return;
  }
  char &restore_off = scene->toolsettings->paint_mode.view_pass_restore_off;
  restore_off = (restore_off == 0) ? 1 : 0;
  /* The menu's checkbox lives in the item callbacks, not in an RNA button state, so no property
   * update runs for it: this notifier is what redraws the header the checkbox is shown in. */
  WM_main_add_notifier(NC_SPACE | ND_SPACE_VIEW3D, nullptr);
}

/**
 * The layered, editable materials of \a ob's slots, each once however many slots share it: the
 * view pass is the material's, so one visit per material is all the work there is, and a slot
 * order can never make the outcome depend on which slot came first.
 */
static void view_pass_slot_materials_foreach(Object &ob, FunctionRef<void(Material &)> fn)
{
  Set<Material *> visited;
  for (int slot = 1; slot <= ob.totcol; slot++) {
    Material *ma = BKE_object_material_get(&ob, slot);
    if (ma == nullptr || !BKE_paint_layers_is_layered(*ma) || !ID_IS_EDITABLE(&ma->id)) {
      continue;
    }
    if (visited.add(ma)) {
      fn(*ma);
    }
  }
}

/**
 * The shared tail of parking and recalling: store \a pass on the material, rebuild its preview
 * nodes, and move the Image Editors that follow the 3D Viewport's pass with it. Returns what
 * #BKE_paint_layers_view_pass_set answered, so a pass the material can no longer show is left
 * unstored. \a sync_images is the option as the scene the transition runs in has it, not as the
 * active window has it.
 */
static bool view_pass_apply_and_sync(Main &bmain, Material &ma, const int pass, const bool sync_images)
{
  if (!BKE_paint_layers_view_pass_set(ma, pass)) {
    return false;
  }
  /* The sync, not a store alone: the pass's viewer nodes are derived data, and a switch shows
   * through the mix factor moving rather than through nodes being removed. */
  BKE_paint_layers_view_pass_sync(&bmain, ma);
  if (sync_images) {
    view_pass_images_set(bmain, ma, pass);
  }
  return true;
}

/** The two notifications parking and recalling end with: the material and the Image Editors. */
static void view_pass_notify()
{
  WM_main_add_notifier(NC_MATERIAL | ND_SHADING, nullptr);
  WM_main_add_notifier(NC_SPACE | ND_SPACE_IMAGE, nullptr);
}

void ED_paint_object_view_pass_restore(Main &bmain, const Scene *scene, Object &ob)
{
  const Scene *option_scene = view_pass_option_scene(bmain, scene);
  const bool restore = view_pass_restore_scene_get(option_scene);
  const bool sync_images = view_pass_sync_scene_get(option_scene);
  /* The materials some other Sculpt Mode object still previews through. Global Sculpt Mode keeps
   * many objects in the mode at once, and they share materials: parking one of these would flip
   * the shading of an object that stays in the mode, so the pass only parks when the leaving
   * object is the last user in the mode. */
  Set<Material *> sculpt_users;
  for (Object &other : bmain.objects) {
    if (&other == &ob || (other.mode & OB_MODE_SCULPT) == 0) {
      continue;
    }
    for (const int slot : IndexRange(1, other.totcol)) {
      if (Material *ma = BKE_object_material_get(&other, slot)) {
        sculpt_users.add(ma);
      }
    }
  }
  bool changed = false;
  view_pass_slot_materials_foreach(ob, [&](Material &ma) {
    if (!restore) {
      /* The pass stays what it is, so a pass remembered by an earlier exit would only come back
       * stale: forget it. */
      ma.paint_layers_view_pass_prev = 0;
      return;
    }
    if (sculpt_users.contains(&ma)) {
      /* Still previewed by an object that stays in the mode: leave the pass as it is, so nothing
       * about the shared material moves while painting goes on. */
      return;
    }
    if (BKE_paint_layers_view_pass_get(ma) == PAINT_LAYER_PASS_COMBINED) {
      /* Already on Combined -- by the user's own choice, or parked moments ago by another object
       * leaving the mode with this material shared. The remembered pass stays for the re-entry. */
      return;
    }
    ma.paint_layers_view_pass_prev = ma.paint_layers_view_pass;
    /* The sync, not a store alone: the pass's viewer nodes are derived data, and Combined is
     * shown through the mix factor dropping to zero rather than through nodes being removed. */
    if (view_pass_apply_and_sync(bmain, ma, PAINT_LAYER_PASS_COMBINED, sync_images)) {
      changed = true;
    }
  });
  if (changed) {
    view_pass_notify();
  }
}

void ED_paint_object_view_pass_recall(Main &bmain, const Scene *scene, Object &ob)
{
  const Scene *option_scene = view_pass_option_scene(bmain, scene);
  const bool restore = view_pass_restore_scene_get(option_scene);
  const bool sync_images = view_pass_sync_scene_get(option_scene);
  const Span<int> passes = BKE_paint_layers_view_passes();
  bool changed = false;
  view_pass_slot_materials_foreach(ob, [&](Material &ma) {
    const int prev = ma.paint_layers_view_pass_prev;
    /* Consumed whatever the restore manages: a memory that failed once must not linger. */
    ma.paint_layers_view_pass_prev = 0;
    if (!restore) {
      /* The option was switched off while out of the mode, so this memory would only come back
       * stale: forget it, the same as an exit under the option off forgets its own. */
      return;
    }
    if (prev <= 0 || prev >= int(passes.size())) {
      return;
    }
    if (!view_pass_apply_and_sync(bmain, ma, passes[prev], sync_images)) {
      /* The remembered channel left the material's set while out of the mode: stay on Combined. */
      return;
    }
    changed = true;
  });
  if (changed) {
    view_pass_notify();
  }
}

/**
 * A multi-object entry is done: submit the active (#Object::actcol) material of every object in
 * \a others -- the ones that entered the mode beside \a active -- to the pass \a active's own
 * active material shows, its recall having just run. In the mode the pass is the view of all the
 * entered objects, the way a pass chosen in the menu is: without this, secondaries with other
 * materials would sit on the passes their own recalls brought back until the user happened to
 * pick one. A material the common pass cannot show takes Combined, and only the actcol material
 * of each object moves -- the one the brush and the preview see.
 */
void ED_paint_object_view_pass_align(Main &bmain, const Scene *scene, Object &active, Span<Object *> others)
{
  if (others.is_empty()) {
    /* A single-object entry has nothing to align with. */
    return;
  }
  const Scene *option_scene = view_pass_option_scene(bmain, scene);
  if (!view_pass_restore_scene_get(option_scene)) {
    /* The option off leaves every material on the pass its own recall brought back. */
    return;
  }
  Material *ref_ma = BKE_object_material_get(&active, active.actcol);
  if (ref_ma == nullptr || !BKE_paint_layers_is_layered(*ref_ma)) {
    /* No layered reference: there is no common view to submit the secondaries to. */
    return;
  }
  const bool sync_images = view_pass_sync_scene_get(option_scene);
  const int pass = BKE_paint_layers_view_pass_get(*ref_ma);
  /* The materials the active previews through, its actcol one included: a shared material is one
   * storage for every object using it, so moving one would turn the active itself away from the
   * view its recall just restored. Seeding the dedup with them also collapses an object list
   * naming one material twice. */
  Set<Material *> aligned;
  view_pass_slot_materials_foreach(active, [&](Material &ma) { aligned.add(&ma); });
  bool changed = false;
  for (Object *ob : others) {
    Material *ma = BKE_object_material_get(ob, ob->actcol);
    if (ma == nullptr || !aligned.add(ma) || !BKE_paint_layers_is_layered(*ma) ||
        !ID_IS_EDITABLE(&ma->id))
    {
      continue;
    }
    /* The memory is spent, the same as a recall's: the object entered the mode and took the
     * common view, so a pass parked by its own earlier exit must not resurrect over that. */
    ma->paint_layers_view_pass_prev = 0;
    if (view_pass_apply_and_sync(bmain,
                                 *ma,
                                 BKE_paint_layers_view_pass_available(*ma, pass) ?
                                     pass :
                                     PAINT_LAYER_PASS_COMBINED,
                                 sync_images))
    {
      changed = true;
    }
  }
  if (changed) {
    view_pass_notify();
  }
}

/** \} */

}  // namespace blender
