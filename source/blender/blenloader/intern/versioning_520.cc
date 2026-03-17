/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup blenloader
 */

#define DNA_DEPRECATED_ALLOW

#include "DNA_ID.h"
#include "DNA_space_types.h"
#include "DNA_userdef_types.h"
#include "DNA_view3d_types.h"
#include "DNA_windowmanager_types.h"

#include "BLI_listbase.h"
#include "BLI_listbase_iterator.hh"
#include "BLI_sys_types.h"

#include "BKE_main.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_screen.hh"

#include "ED_screen.hh"

#include "readfile.hh"

#include "versioning_common.hh"

// #include "CLG_log.h"

namespace blender {

// static CLG_LogRef LOG = {"blend.doversion"};

/* Ensure editors that support category filtering have the TAG_BAR region. */
static void do_versions_ensure_spaces_have_tag_bar_region(Main *bmain)
{
  for (bScreen &screen : bmain->screens) {
    for (ScrArea &area : screen.areabase) {
      if (ELEM(area.spacetype, SPACE_VIEW3D, SPACE_PROPERTIES, SPACE_NODE, SPACE_IMAGE)) {
        ARegion *region = do_versions_ensure_region(
            &area.regionbase, RGN_TYPE_TAG_BAR, __func__, RGN_TYPE_TOOLS);

        ARegion *insert_after = nullptr;
        ARegion *alignment_source = nullptr;
        if (ELEM(area.spacetype, SPACE_NODE, SPACE_IMAGE)) {
          ARegion *tools_region = nullptr;
          ARegion *ui_region = nullptr;
          ARegion *header_region = nullptr;
          ARegion *tool_header_region = nullptr;
          for (ARegion &iter_region : area.regionbase) {
            if (iter_region.regiontype == RGN_TYPE_TOOLS && tools_region == nullptr) {
              tools_region = &iter_region;
            }
            else if (iter_region.regiontype == RGN_TYPE_UI && ui_region == nullptr) {
              ui_region = &iter_region;
            }
            else if (iter_region.regiontype == RGN_TYPE_HEADER && header_region == nullptr) {
              header_region = &iter_region;
            }
            else if (iter_region.regiontype == RGN_TYPE_TOOL_HEADER && tool_header_region == nullptr) {
              tool_header_region = &iter_region;
            }
          }

          /* Match VIEW_3D behavior: left toolbar is processed before top TAG_BAR so TAG_BAR does not
           * push down the toolbar. */
          if (tools_region && ui_region &&
              BLI_findindex(&area.regionbase, tools_region) > BLI_findindex(&area.regionbase, ui_region))
          {
            BLI_remlink(&area.regionbase, tools_region);
            BLI_insertlinkbefore(&area.regionbase, ui_region, tools_region);
          }

          /* Keep TAG_BAR stacked like in VIEW_3D:
           * below tool/header rows, not affecting left toolbar, and before side UI region
           * in region traversal order. */
          insert_after = tools_region ? tools_region : (tool_header_region ? tool_header_region : header_region);
          alignment_source = tool_header_region ? tool_header_region : header_region;
          if (insert_after != nullptr && insert_after != region) {
            BLI_remlink(&area.regionbase, region);
            BLI_insertlinkafter(&area.regionbase, insert_after, region);
          }
          if (ui_region && BLI_findindex(&area.regionbase, region) > BLI_findindex(&area.regionbase, ui_region)) {
            BLI_remlink(&area.regionbase, region);
            BLI_insertlinkbefore(&area.regionbase, ui_region, region);
          }
        }

        region->regiontype = RGN_TYPE_TAG_BAR;
        region->alignment = alignment_source ? alignment_source->alignment : RGN_ALIGN_TOP;
        if (ELEM(area.spacetype, SPACE_NODE, SPACE_IMAGE)) {
          region->flag |= RGN_FLAG_HIDDEN;
        }
        else {
          region->flag &= ~RGN_FLAG_HIDDEN;
        }
        region->overlap = true;
      }
    }
  }
}

static void do_versions_init_tag_filter_state_in_spaces(Main *bmain)
{
  for (bScreen &screen : bmain->screens) {
    for (ScrArea &area : screen.areabase) {
      for (SpaceLink &sl : area.spacedata) {
        if (sl.spacetype == SPACE_NODE) {
          SpaceNode *snode = reinterpret_cast<SpaceNode *>(&sl);
          snode->active_tag_filter_tags[0] = '\0';
          snode->tag_filter_enabled = 0;
          snode->tag_bar_scroll_offset = 0;
        }
        else if (sl.spacetype == SPACE_IMAGE) {
          SpaceImage *sima = reinterpret_cast<SpaceImage *>(&sl);
          sima->active_tag_filter_tags[0] = '\0';
          sima->tag_filter_enabled = 0;
          sima->tag_bar_scroll_offset = 0;
        }
      }
    }
  }
}

static void do_versions_init_category_tabs_display_and_zoom_in_spaces(Main *bmain)
{
  for (bScreen &screen : bmain->screens) {
    for (ScrArea &area : screen.areabase) {
      for (SpaceLink &sl : area.spacedata) {
        switch (sl.spacetype) {
          case SPACE_VIEW3D: {
            View3D *v3d = reinterpret_cast<View3D *>(&sl);
            v3d->category_tabs_display_mode = U.category_tabs_display_mode;
            v3d->category_tabs_zoom_icon = U.category_tabs_zoom_icon;
            v3d->category_tabs_zoom_mixed = U.category_tabs_zoom_mixed;
            v3d->category_tabs_zoom_text = U.category_tabs_zoom_text;
            break;
          }
          case SPACE_PROPERTIES: {
            SpaceProperties *sbuts = reinterpret_cast<SpaceProperties *>(&sl);
            sbuts->category_tabs_display_mode = U.category_tabs_display_mode;
            sbuts->category_tabs_zoom_icon = U.category_tabs_zoom_icon;
            sbuts->category_tabs_zoom_mixed = U.category_tabs_zoom_mixed;
            sbuts->category_tabs_zoom_text = U.category_tabs_zoom_text;
            break;
          }
          case SPACE_NODE: {
            SpaceNode *snode = reinterpret_cast<SpaceNode *>(&sl);
            snode->category_tabs_display_mode = U.category_tabs_display_mode;
            snode->category_tabs_zoom_icon = U.category_tabs_zoom_icon;
            snode->category_tabs_zoom_mixed = U.category_tabs_zoom_mixed;
            snode->category_tabs_zoom_text = U.category_tabs_zoom_text;
            break;
          }
          case SPACE_IMAGE: {
            SpaceImage *sima = reinterpret_cast<SpaceImage *>(&sl);
            sima->category_tabs_display_mode = U.category_tabs_display_mode;
            sima->category_tabs_zoom_icon = U.category_tabs_zoom_icon;
            sima->category_tabs_zoom_mixed = U.category_tabs_zoom_mixed;
            sima->category_tabs_zoom_text = U.category_tabs_zoom_text;
            break;
          }
          default:
            break;
        }
      }
    }
  }
}

static void do_versions_clear_category_runtime_lists_in_wm(Main *bmain)
{
  for (wmWindowManager &wm : bmain->wm) {
    BLI_listbase_clear(&wm.category_glyph_mappings);
    BLI_listbase_clear(&wm.category_glyph_overrides);
    BLI_listbase_clear(&wm.category_tags);
    wm.category_tags_active_index = 0;
  }
}

/* Saving file extension is now a property of the the File Output node. So inherit this
 * setting from the active scene to restore the old behavior.
 * Note: One limitation is that node groups containing file outputs that are not part of any
 * scene are not affected by versioning. */
static void do_version_file_output_use_file_extension_recursive(bNodeTree &node_tree,
                                                                const Scene &scene)
{
  for (bNode &node : node_tree.nodes) {
    if (node.type_legacy == CMP_NODE_OUTPUT_FILE) {
      NodeCompositorFileOutput *data = static_cast<NodeCompositorFileOutput *>(node.storage);
      data->use_file_extension = (scene.r.scemode & R_EXTENSION) != 0;
    }
    else if (node.type_legacy == NODE_GROUP) {
      bNodeTree *ngroup = id_cast<bNodeTree *>(node.id);
      if (ngroup) {
        do_version_file_output_use_file_extension_recursive(*ngroup, scene);
      }
    }
  }
}

void do_versions_after_linking_520(FileData * /*fd*/, Main *bmain)
{
  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 2)) {
    for (Scene &scene : bmain->scenes) {
      bNodeTree *node_tree = version_get_scene_compositor_node_tree(bmain, &scene);
      if (node_tree == nullptr) {
        continue;
      }
      do_version_file_output_use_file_extension_recursive(*node_tree, scene);
    }
  }
  /**
   * Always bump subversion in BKE_blender_version.h when adding versioning
   * code here, and wrap it inside a MAIN_VERSION_FILE_ATLEAST check.
   *
   * \note Keep this message at the bottom of the function.
   */
}

static void do_versions_init_tag_category_memory(Main *bmain)
{
  for (bScreen &screen : bmain->screens) {
    for (ScrArea &area : screen.areabase) {
      for (SpaceLink &sl : area.spacedata) {
        switch (sl.spacetype) {
          case SPACE_VIEW3D: {
            View3D *v3d = reinterpret_cast<View3D *>(&sl);
            v3d->tag_last_active_categories[0] = '\0';
            break;
          }
          case SPACE_PROPERTIES: {
            SpaceProperties *sbuts = reinterpret_cast<SpaceProperties *>(&sl);
            sbuts->tag_last_active_categories[0] = '\0';
            break;
          }
          case SPACE_NODE: {
            SpaceNode *snode = reinterpret_cast<SpaceNode *>(&sl);
            snode->tag_last_active_categories[0] = '\0';
            break;
          }
          case SPACE_IMAGE: {
            SpaceImage *sima = reinterpret_cast<SpaceImage *>(&sl);
            sima->tag_last_active_categories[0] = '\0';
            break;
          }
          default:
            break;
        }
      }
    }
  }
}

void blo_do_versions_520(FileData * /*fd*/, Library * /*lib*/, Main *bmain)
{
  /* Category runtime lists in WM are rebuilt by Python on startup and must never be trusted from
   * blend-file contents (older experimental files may contain stale raw pointers here).
   * Clear unconditionally for all 5.2 loads before any Python-side sync touches them. */
  do_versions_clear_category_runtime_lists_in_wm(bmain);

  /* Add TAG_BAR region to editors that support category filtering. */
  do_versions_ensure_spaces_have_tag_bar_region(bmain);

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 8)) {
    do_versions_init_tag_category_memory(bmain);
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 5)) {
    do_versions_init_tag_filter_state_in_spaces(bmain);
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 6)) {
    do_versions_init_category_tabs_display_and_zoom_in_spaces(bmain);
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 4)) {
    /* Initialize new category_tabs_inactive_behavior field to DEFAULT */
    U.category_tabs_inactive_behavior = USER_CATEGORY_TABS_INACTIVE_DEFAULT;
    /* Initialize new category_tabs_shape field to CAPSULE */
    U.category_tabs_shape = USER_CATEGORY_TABS_SHAPE_CAPSULE;
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 1)) {
    for (Scene &scene : bmain->scenes) {
      scene.r.mode |= R_SAVE_OUTPUT;
    }
  }
  /**
   * Always bump subversion in BKE_blender_version.h when adding versioning
   * code here, and wrap it inside a MAIN_VERSION_FILE_ATLEAST check.
   *
   * \note Keep this message at the bottom of the function.
   */
}

}  // namespace blender
