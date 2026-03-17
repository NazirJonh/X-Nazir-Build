/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup blenloader
 */

#define DNA_DEPRECATED_ALLOW

#include "NOD_geometry_nodes_srna.hh"

#include "DNA_ID.h"
#include "DNA_brush_types.h"
#include "DNA_space_types.h"
#include "DNA_curve_types.h"
#include "DNA_modifier_types.h"
#include "DNA_node_tree_interface_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_userdef_types.h"
#include "DNA_view3d_types.h"
#include "DNA_windowmanager_types.h"
#include "BLI_listbase.h"
#include "BLI_listbase_iterator.hh"
#include "BLI_string.h"
#include "BLI_sys_types.h"

#include "BKE_animsys.h"
#include "BKE_curves.hh"
#include "BKE_idprop.hh"
#include "BKE_main.hh"
#include "BKE_mesh_legacy_convert.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_screen.hh"

#include "ED_screen.hh"

#include "SEQ_iterator.hh"
#include "SEQ_sequencer.hh"

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

      ARegion *region = do_versions_ensure_region(
          &area.regionbase, RGN_TYPE_TAG_BAR, __func__, RGN_TYPE_TOOLS);
      region->regiontype = RGN_TYPE_TAG_BAR;
      region->alignment = RGN_ALIGN_TOP;
      region->flag = 0;
    }
  }
}

static void version_geometry_nodes_properties(Main &bmain, Object &object, NodesModifierData &nmd)
{
  const IDProperty *old_props = nmd.settings_legacy.properties;
  if (!old_props) {
    /* Versioning has already been done, this check makes the function idempotent. */
    return;
  }
  if (!nmd.node_group) {
    IDP_FreeProperty(nmd.settings_legacy.properties);
    nmd.settings_legacy.properties = nullptr;
    return;
  }
  if (ID_MISSING(&nmd.node_group->id)) {
    return;
  }
  const bNodeTree &ntree = *nmd.node_group;
  ntree.ensure_interface_cache();

  IDProperty *system_props = bke::idprop::create_group("NodesModifierProperties").release();

  IDProperty *inputs = bke::idprop::create_group("inputs").release();
  IDP_AddToGroup(system_props, inputs);

  const std::string inputs_path_prefix = fmt::format("modifiers[\"{}\"]", nmd.modifier.name);
  for (const bNodeTreeInterfaceSocket *input : ntree.interface_inputs()) {
    const StringRefNull identifier = input->identifier;
    IDProperty *old_value_prop = IDP_GetPropertyFromGroup(old_props, identifier);
    if (!old_value_prop) {
      continue;
    }

    IDProperty *group = bke::idprop::create_group(identifier).release();
    IDP_AddToGroup(inputs, group);

    if (input->flag & NODE_INTERFACE_SOCKET_LAYER_SELECTION) {
      IDP_AddToGroup(
          group, bke::idprop::create("type", int(nodes::GeometryNodesInputType::Layer)).release());
      const StringRefNull layer_name = [&]() {
        const IDProperty *layer_name = IDP_GetPropertyFromGroup(old_props, identifier);
        if (layer_name) {
          return StringRefNull(IDP_string_get(layer_name));
        }
        return StringRefNull();
      }();
      IDP_AddToGroup(group, bke::idprop::create("layer_name", layer_name).release());
      continue;
    }

    IDProperty *new_value_prop = IDP_CopyProperty(old_value_prop);
    STRNCPY(new_value_prop->name, "value");
    IDP_AddToGroup(group, new_value_prop);

    const std::string old_value_path = fmt::format("[\"{}\"]", identifier);
    const std::string new_value_path = fmt::format(".properties.inputs.{}.value", identifier);
    BKE_animdata_fix_paths_rename_all_ex(&bmain,
                                         &object.id,
                                         inputs_path_prefix.c_str(),
                                         old_value_path.c_str(),
                                         new_value_path.c_str(),
                                         0,
                                         0,
                                         false,
                                         false);

    if (IDOverrideLibrary *override_library = object.id.override_library) {
      for (IDOverrideLibraryProperty &prop : override_library->properties) {
        const StringRef path = prop.rna_path;
        const int64_t i = path.find(inputs_path_prefix);
        if (i == StringRef::not_found) {
          continue;
        }
        if (path.drop_known_prefix(inputs_path_prefix) != old_value_path) {
          continue;
        }
        MEM_delete(prop.rna_path);
        prop.rna_path = BLI_sprintfN("%s%s", inputs_path_prefix.c_str(), new_value_path.c_str());
      }
    }

    bool use_attribute = false;
    if (const IDProperty *use_attribute_prop = IDP_GetPropertyFromGroup(
            old_props, identifier + "_use_attribute"))
    {
      /* This property changed to an enum property and animation is not versioned. */
      if (use_attribute_prop->type == IDP_INT) {
        use_attribute = bool(IDP_int_get(use_attribute_prop));
      }
      else {
        use_attribute = bool(IDP_bool_get(use_attribute_prop));
      }
    }

    const auto input_type = use_attribute ? nodes::GeometryNodesInputType::Attribute :
                                            nodes::GeometryNodesInputType::Value;
    IDP_AddToGroup(group, bke::idprop::create("type", int(input_type)).release());
    const StringRefNull attribute_name = [&]() {
      const IDProperty *attribute_name = IDP_GetPropertyFromGroup(old_props,
                                                                  identifier + "_attribute_name");
      if (attribute_name) {
        return StringRefNull(IDP_string_get(attribute_name));
      }
      return StringRefNull();
    }();
    IDP_AddToGroup(group, bke::idprop::create("attribute_name", attribute_name).release());
  }

  IDProperty *outputs = bke::idprop::create_group("outputs").release();
  IDP_AddToGroup(system_props, outputs);
  for (const bNodeTreeInterfaceSocket *output : ntree.interface_outputs()) {
    const StringRef identifier = output->identifier;
    IDProperty *old_name_prop = IDP_GetPropertyFromGroup(old_props,
                                                         identifier + "_attribute_name");
    if (!old_name_prop) {
      continue;
    }
    IDProperty *group = bke::idprop::create_group(identifier).release();
    IDP_AddToGroup(outputs, group);

    IDProperty *new_value_prop = IDP_CopyProperty(old_name_prop);
    STRNCPY(new_value_prop->name, "attribute_name");
    IDP_AddToGroup(group, new_value_prop);
  }

  if (nmd.modifier.system_properties) {
    IDP_FreeProperty(nmd.modifier.system_properties);
  }
  nmd.modifier.system_properties = system_props;
  IDP_FreeProperty(nmd.settings_legacy.properties);
  nmd.settings_legacy.properties = nullptr;
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

/* Saving file extension is now a property of the File Output node. So inherit this
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

static void version_clear_strip_linear_modifier_flag(Main &bmain)
{
  for (Scene &scene : bmain.scenes) {
    Editing *ed = seq::editing_get(&scene);
    if (ed != nullptr) {
      seq::foreach_strip(&ed->seqbase, [&](Strip *strip) {
        constexpr int flag_linear_modifiers = 1 << 23;
        strip->flag &= ~flag_linear_modifiers;
        return true;
      });
    }
  }
}

static void fix_single_point_curves_custom_knots(Main *bmain)
{
  /* Fix corrupted flagu/flagv values created by older versions of the Curve Pen tool.
   * The tool could create loose vertices with invalid flag values (e.g. -2), where
   * CU_NURB_CUSTOM was set alongside other flags and knotsu/knotsv was left null,
   * causing a crash when opening these files in newer versions. */
  for (Curve &cu : bmain->curves) {
    for (Nurb *nu = static_cast<Nurb *>(cu.nurb.first); nu != nullptr; nu = nu->next) {
      if (nu->knotsu == nullptr && (nu->flagu & CU_NURB_CUSTOM)) {
        nu->flagu &= (CU_NURB_CYCLIC | CU_NURB_BEZIER | CU_NURB_ENDPOINT);
      }
      if (nu->knotsv == nullptr && (nu->flagv & CU_NURB_CUSTOM)) {
        nu->flagv &= (CU_NURB_CYCLIC | CU_NURB_BEZIER | CU_NURB_ENDPOINT);
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

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 16)) {
    for (Object &object : bmain->objects) {
      for (ModifierData &md : object.modifiers) {
        if (md.type == eModifierType_Nodes) {
          version_geometry_nodes_properties(
              *bmain, object, reinterpret_cast<NodesModifierData &>(md));
        }
      }
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

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 4)) {
    for (Brush &brush : bmain->brushes) {
      if (brush.gpencil_settings != nullptr) {
        brush.blend = 0;
      }
    }
  }
  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 5)) {
    FOREACH_NODETREE_BEGIN (bmain, node_tree, id_owner) {
      for (bNode &node : node_tree->nodes) {
        if (node.type_legacy == FN_NODE_INPUT_VECTOR) {
          auto &data = *static_cast<NodeInputVector *>(node.storage);
          data.vector[3] = 0.0f;
          data.dimensions = 3;
        }
      }
    }
    FOREACH_NODETREE_END;
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 6)) {
    for (Scene &scene : bmain->scenes) {
      SequencerToolSettings *sequencer_tool_settings = seq::tool_settings_ensure(&scene);
      sequencer_tool_settings->snap_flag |= SEQ_SNAP_TO_ALL_CHANNEL_STRIPS;
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 7)) {
    for (Scene &scene : bmain->scenes) {
      scene.r.anisotropic_filter = 2;
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 9)) {
    for (Mesh &mesh : bmain->meshes) {
      bke::mesh_freestyle_marks_to_generic(mesh);
    }
  }

  /* Convert H.264 codec value for older files (2.79), see #155775. */
  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 10)) {
    for (Scene &scene : bmain->scenes) {
      if (scene.r.ffcodecdata.codec == 28) {
        scene.r.ffcodecdata.codec = 27;
      }
    }
  }

  /* Disable "unified" flags for Grease Pencil Draw mode. */
  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 11)) {
    for (Scene &scene : bmain->scenes) {
      if (scene.toolsettings->gp_paint) {
        UnifiedPaintSettings &settings =
            scene.toolsettings->gp_paint->paint.unified_paint_settings;
        settings.flag &= ~(UNIFIED_PAINT_SIZE | UNIFIED_PAINT_ALPHA | UNIFIED_PAINT_COLOR);
      }
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 12)) {
    for (bScreen &screen : bmain->screens) {
      for (ScrArea &area : screen.areabase) {
        for (SpaceLink &space : area.spacedata) {
          if (space.spacetype == SPACE_NODE) {
            SpaceNode *space_node = reinterpret_cast<SpaceNode *>(&space);
            space_node->overlay.flag |= SN_OVERLAY_SHOW_RENDER_REGION;
            space_node->overlay.passepartout_alpha = 0.5f;
          }
        }
      }
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 13)) {
    version_clear_strip_linear_modifier_flag(*bmain);
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 14)) {
    fix_single_point_curves_custom_knots(bmain);
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 15)) {
    for (Scene &scene : bmain->scenes) {
      scene.r.scemode |= R_USE_TEXTURE_CACHE;
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 16)) {
    for (Brush &brush : bmain->brushes) {
      if (brush.gpencil_settings != nullptr) {
        brush.gpencil_settings->curve_type = CURVE_TYPE_POLY;
        brush.gpencil_settings->conversion_threshold = 0.001f;
      }
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
