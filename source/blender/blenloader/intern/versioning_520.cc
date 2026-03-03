/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup blenloader
 */

#define DNA_DEPRECATED_ALLOW

#include "DNA_ID.h"

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

/* Ensure View3D spaces have the TAG_BAR region for category filtering. */
static void do_versions_ensure_view3d_has_tag_bar_region(Main *bmain)
{
  int region_count = 0;
  for (bScreen &screen : bmain->screens) {
    for (ScrArea &area : screen.areabase) {
      if (area.spacetype == SPACE_VIEW3D) {
        /* Check if TAG_BAR region already exists */
        ARegion *region = do_versions_ensure_region(&area.regionbase, RGN_TYPE_TAG_BAR, __func__, RGN_TYPE_TOOLS);
        /* Set up the region */
        region->regiontype = RGN_TYPE_TAG_BAR;
        region->alignment = RGN_ALIGN_TOP;  /* Top region, full width */
        region->flag = 0; /* Visible by default */
        region_count++;
      }
    }
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

void blo_do_versions_520(FileData * /*fd*/, Library * /*lib*/, Main *bmain)
{
  /* Add TAG_BAR region to existing View3D spaces */
  do_versions_ensure_view3d_has_tag_bar_region(bmain);

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
