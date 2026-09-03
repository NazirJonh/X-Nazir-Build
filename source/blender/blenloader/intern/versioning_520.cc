/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup blenloader
 */

#define DNA_DEPRECATED_ALLOW

#include <array>

#include "NOD_geometry_nodes_srna.hh"

#include "DNA_ID.h"
#include "DNA_brush_types.h"
#include "DNA_camera_types.h"
#include "DNA_curve_types.h"
#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_node_tree_interface_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_texture_types.h"
#include "DNA_userdef_types.h"
#include "DNA_view3d_types.h"
#include "DNA_windowmanager_types.h"
#include "DNA_xr_types.h"

#include "BKE_asset.hh"

#include "BLI_listbase.h"
#include "BLI_listbase_iterator.hh"
#include "BLI_math_vector.h"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_string_utils.hh"
#include "BLI_sys_types.h"

#include "MEM_guardedalloc.h"

#include "BKE_anim_visualization.h"
#include "BKE_animsys.h"
#include "BKE_attribute.hh"
#include "BKE_brush.hh"
#include "BKE_colorband.hh"
#include "BKE_colortools.hh"
#include "BKE_curves.hh"
#include "BKE_idprop.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_lib_override.hh"
#include "BKE_main.hh"
#include "BKE_mesh_legacy_convert.hh"
#include "BKE_multires.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_paint.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"
#include "BKE_sculpt_layers.hh"

#include "ED_screen.hh"

#include "SEQ_effects.hh"
#include "SEQ_iterator.hh"
#include "SEQ_sequencer.hh"

#include "BLO_read_write.hh"
#include "readfile.hh"

#include "versioning_common.hh"

// #include "CLG_log.h"

namespace blender {

// static CLG_LogRef LOG = {"blend.doversion"};

/**
 * Drop the TAG_BAR region from Properties editors.
 *
 * Earlier versions of this file gave the editor one. It has nothing to filter -- it shows the
 * active object's data rather than browsing taggable content -- and `space_buttons.cc` no longer
 * registers a region type for it, so a leftover region would sit in the file forever with nothing
 * to initialize or draw it.
 */
static void do_versions_remove_properties_tag_bar_region(Main *bmain)
{
  for (bScreen &screen : bmain->screens) {
    for (ScrArea &area : screen.areabase) {
      /* Also the inactive space data: an area the user has switched away from keeps its regions,
       * and they come back with it. */
      for (SpaceLink &sl : area.spacedata) {
        if (sl.spacetype != SPACE_PROPERTIES) {
          continue;
        }
        ListBaseT<ARegion> *regionbase = (&sl == area.spacedata.first) ? &area.regionbase :
                                                                        &sl.regionbase;
        ARegion *region_next = nullptr;
        for (ARegion *region = static_cast<ARegion *>(regionbase->first); region;
             region = region_next)
        {
          region_next = static_cast<ARegion *>(region->next);
          if (region->regiontype == RGN_TYPE_TAG_BAR) {
            MEM_delete(region->runtime);
            BLI_freelinkN(regionbase, region);
          }
        }
      }
    }
  }
}

/* Ensure editors that support category filtering have the TAG_BAR region. */
static void do_versions_ensure_spaces_have_tag_bar_region(Main *bmain)
{
  for (bScreen &screen : bmain->screens) {
    for (ScrArea &area : screen.areabase) {
      if (ELEM(area.spacetype, SPACE_VIEW3D, SPACE_NODE, SPACE_IMAGE)) {
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
          snode->tabs_state.active_tag_filter_tags[0] = '\0';
          snode->tabs_state.tag_filter_enabled = 0;
          snode->tabs_state.tag_bar_scroll_offset = 0;
        }
        else if (sl.spacetype == SPACE_IMAGE) {
          SpaceImage *sima = reinterpret_cast<SpaceImage *>(&sl);
          sima->tabs_state.active_tag_filter_tags[0] = '\0';
          sima->tabs_state.tag_filter_enabled = 0;
          sima->tabs_state.tag_bar_scroll_offset = 0;
        }
      }

      if (ELEM(area.spacetype, SPACE_VIEW3D, SPACE_NODE, SPACE_IMAGE)) {
        ARegion *region = do_versions_ensure_region(
            &area.regionbase, RGN_TYPE_TAG_BAR, __func__, RGN_TYPE_TOOLS);
        region->regiontype = RGN_TYPE_TAG_BAR;
        region->alignment = RGN_ALIGN_TOP;
        /* Keep the tag bar hidden by default for Node/Image editors, visible elsewhere. */
        if (ELEM(area.spacetype, SPACE_NODE, SPACE_IMAGE)) {
          region->flag |= RGN_FLAG_HIDDEN;
        }
        else {
          region->flag &= ~RGN_FLAG_HIDDEN;
        }
      }
    }
  }
}

static void version_geometry_nodes_properties(FileData &fd,
                                              Main &bmain,
                                              Object &object,
                                              NodesModifierData &nmd)
{
  const IDProperty *old_props = nmd.settings_legacy.properties;
  if (!old_props) {
    /* Versioning has already been done, this check makes the function idempotent. */
    return;
  }
  if (!nmd.node_group) {
    IDP_FreeProperty(nmd.settings_legacy.properties);
    nmd.settings_legacy.properties = nullptr;
    BLO_reportf_wrap(fd.reports,
                     RPT_WARNING,
                     "Modifier '%s' from Object '%s' is missing its Geometry Node Group, its "
                     "settings will be lost (reset to default).",
                     nmd.modifier.name,
                     BKE_id_name(object.id));
    return;
  }
  if (ID_MISSING(&nmd.node_group->id)) {
    /* Keeping the old idproperties is not an option, and not really useful, since if the
     * blend-file is saved in this current state, it won't be re-versioned here later anyway.
     *
     * Furthermore, the whole remaining part of the code expects this to be nullptr, and keeping it
     * at runtime actually causes weird issues in depsgraph nodes building phase.
     *
     * So all in all, it's simpler and safer to also just lose these values here - if file is not
     * saved in this state, next loading will do the versioning if the node-group is available
     * again, otherwise that data is lost.
     */
    IDP_FreeProperty(nmd.settings_legacy.properties);
    nmd.settings_legacy.properties = nullptr;
    BLO_reportf_wrap(
        fd.reports,
        RPT_WARNING,
        "Modifier '%s' from Object '%s' is using a missing linked Geometry Node Group, its "
        "settings will be lost (reset to default) if the file is saved in this state.",
        nmd.modifier.name,
        BKE_id_name(object.id));
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
        const IDProperty *layer_name = IDP_GetPropertyTypeFromGroup(
            old_props, identifier, IDP_STRING);
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
      else if (use_attribute_prop->type == IDP_BOOLEAN) {
        use_attribute = bool(IDP_bool_get(use_attribute_prop));
      }
    }

    const auto input_type = use_attribute ? nodes::GeometryNodesInputType::Attribute :
                                            nodes::GeometryNodesInputType::Value;
    IDP_AddToGroup(group, bke::idprop::create("type", int(input_type)).release());
    const StringRefNull attribute_name = [&]() {
      const IDProperty *attribute_name = IDP_GetPropertyTypeFromGroup(
          old_props, identifier + "_attribute_name", IDP_STRING);
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
    IDProperty *old_name_prop = IDP_GetPropertyTypeFromGroup(
        old_props, identifier + "_attribute_name", IDP_STRING);
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

static void sanitize_node_tree_interface_socket_identifiers(bNodeTree &node_tree)
{
  node_tree.ensure_interface_cache();
  Set<StringRef> all_identifiers;
  Map<std::string, StringRefNull> identifier_map;
  for (bNodeTreeInterfaceItem *item : node_tree.interface_items()) {
    if (item->item_type == NodeTreeInterfaceItemType::Panel) {
      continue;
    }
    auto &socket = *bke::node_interface::get_item_as<bNodeTreeInterfaceSocket>(item);
    /* Socket identifiers are required to be valid RNA identifiers and unique. */
    if (!RNA_validate_identifier(socket.identifier, true)) {
      std::string prev_identifier(socket.identifier);
      RNA_identifier_sanitize(socket.identifier, true);
      if (all_identifiers.contains(socket.identifier)) {
        std::string new_identifier = BLI_uniquename_cb(
            [&](StringRef name) { return all_identifiers.contains(name); },
            '_',
            socket.identifier);
        MEM_SAFE_DELETE(socket.identifier);
        socket.identifier = BLI_strdup(new_identifier.c_str());
      }
      identifier_map.add(std::move(prev_identifier), socket.identifier);
    }
    all_identifiers.add(socket.identifier);
  }

  /* Rename all the node socket identifiers that got changed in the interface. */
  if (!identifier_map.is_empty()) {
    for (bNode &node : node_tree.nodes) {
      if (!(node.is_group_input() || node.is_group_output())) {
        continue;
      }
      ListBaseT<bNodeSocket> sockets = node.is_group_output() ? node.inputs : node.outputs;
      for (bNodeSocket &socket : sockets) {
        if (identifier_map.contains(socket.identifier)) {
          version_node_socket_identifier_set(socket, identifier_map.lookup(socket.identifier));
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
            v3d->tabs_state.category_tabs_display_mode = U.category_tabs_display_mode;
            v3d->tabs_state.category_tabs_zoom_icon = U.category_tabs_zoom_icon;
            v3d->tabs_state.category_tabs_zoom_mixed = U.category_tabs_zoom_mixed;
            v3d->tabs_state.category_tabs_zoom_text = U.category_tabs_zoom_text;
            break;
          }
          case SPACE_PROPERTIES: {
            SpaceProperties *sbuts = reinterpret_cast<SpaceProperties *>(&sl);
            sbuts->tabs_state.category_tabs_display_mode = U.category_tabs_display_mode;
            sbuts->tabs_state.category_tabs_zoom_icon = U.category_tabs_zoom_icon;
            sbuts->tabs_state.category_tabs_zoom_mixed = U.category_tabs_zoom_mixed;
            sbuts->tabs_state.category_tabs_zoom_text = U.category_tabs_zoom_text;
            break;
          }
          case SPACE_NODE: {
            SpaceNode *snode = reinterpret_cast<SpaceNode *>(&sl);
            snode->tabs_state.category_tabs_display_mode = U.category_tabs_display_mode;
            snode->tabs_state.category_tabs_zoom_icon = U.category_tabs_zoom_icon;
            snode->tabs_state.category_tabs_zoom_mixed = U.category_tabs_zoom_mixed;
            snode->tabs_state.category_tabs_zoom_text = U.category_tabs_zoom_text;
            break;
          }
          case SPACE_IMAGE: {
            SpaceImage *sima = reinterpret_cast<SpaceImage *>(&sl);
            sima->tabs_state.category_tabs_display_mode = U.category_tabs_display_mode;
            sima->tabs_state.category_tabs_zoom_icon = U.category_tabs_zoom_icon;
            sima->tabs_state.category_tabs_zoom_mixed = U.category_tabs_zoom_mixed;
            sima->tabs_state.category_tabs_zoom_text = U.category_tabs_zoom_text;
            break;
          }
          default:
            break;
        }
      }
    }
  }
}

/* Heal corrupt per-space category-tab zoom factors. The valid range is [0.5, 2.5]; a stored 0.0
 * (from spaces written before the zoom feature, or copied from a 0.0 preference) collapses every
 * category tab to zero width and makes the tab bar disappear. Only non-positive values are reset,
 * so user-customized zooms are preserved. */
static void do_versions_fix_category_tabs_zoom_in_spaces(Main *bmain)
{
  auto heal = [](float &zoom) {
    if (zoom <= 0.0f) {
      zoom = 1.0f;
    }
  };
  for (bScreen &screen : bmain->screens) {
    for (ScrArea &area : screen.areabase) {
      for (SpaceLink &sl : area.spacedata) {
        switch (sl.spacetype) {
          case SPACE_VIEW3D: {
            View3D *v3d = reinterpret_cast<View3D *>(&sl);
            heal(v3d->tabs_state.category_tabs_zoom_icon);
            heal(v3d->tabs_state.category_tabs_zoom_mixed);
            heal(v3d->tabs_state.category_tabs_zoom_text);
            break;
          }
          case SPACE_PROPERTIES: {
            SpaceProperties *sbuts = reinterpret_cast<SpaceProperties *>(&sl);
            heal(sbuts->tabs_state.category_tabs_zoom_icon);
            heal(sbuts->tabs_state.category_tabs_zoom_mixed);
            heal(sbuts->tabs_state.category_tabs_zoom_text);
            break;
          }
          case SPACE_NODE: {
            SpaceNode *snode = reinterpret_cast<SpaceNode *>(&sl);
            heal(snode->tabs_state.category_tabs_zoom_icon);
            heal(snode->tabs_state.category_tabs_zoom_mixed);
            heal(snode->tabs_state.category_tabs_zoom_text);
            break;
          }
          case SPACE_IMAGE: {
            SpaceImage *sima = reinterpret_cast<SpaceImage *>(&sl);
            heal(sima->tabs_state.category_tabs_zoom_icon);
            heal(sima->tabs_state.category_tabs_zoom_mixed);
            heal(sima->tabs_state.category_tabs_zoom_text);
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
        constexpr eStripFlag flag_linear_modifiers = eStripFlag(1 << 23);
        strip->flag &= ~flag_linear_modifiers;
        return true;
      });
    }
  }
}

static void version_text_strip_space_line(Main &bmain)
{
  for (Scene &scene : bmain.scenes) {
    Editing *ed = seq::editing_get(&scene);
    if (ed == nullptr) {
      continue;
    }

    seq::foreach_strip(&ed->seqbase, [&](Strip *strip) {
      if (strip->type == STRIP_TYPE_TEXT && strip->effectdata != nullptr) {
        TextVars *data = static_cast<TextVars *>(strip->effectdata);
        data->space_line = 1.0f;
      }
      return true;
    });
  }
}

static void version_compositor_effect_initialized(Main &bmain)
{
  /* A file with compositor effects that was saved, opened in
   * previous version and saved there, would have lost the
   * compositor effect data since earlier versions would not
   * write it. Ensure the effect data is not null. */
  for (Scene &scene : bmain.scenes) {
    if (scene.ed) {
      seq::foreach_strip(&scene.ed->seqbase, [&](Strip *strip) {
        if (strip->type == STRIP_TYPE_COMPOSITOR) {
          seq::effect_ensure_initialized(strip);
        }
        return true;
      });
    }
  }
}

static void version_text_strip_abs_space_line(Main &bmain)
{
  for (Scene &scene : bmain.scenes) {
    Editing *ed = seq::editing_get(&scene);
    if (ed == nullptr) {
      continue;
    }

    seq::foreach_strip(&ed->seqbase, [&](Strip *strip) {
      if (strip->type == STRIP_TYPE_TEXT && strip->effectdata != nullptr) {
        TextVars *data = static_cast<TextVars *>(strip->effectdata);
        data->abs_space_line = 60.0f;
        data->flag &= ~SEQ_TEXT_USE_ABSOLUTE_LINE_SPACING;
      }
      return true;
    });
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

static void version_strip_modifier_show_preview_flag(Main &bmain)
{
  for (Scene &scene : bmain.scenes) {
    Editing *ed = seq::editing_get(&scene);
    if (ed == nullptr) {
      continue;
    }
    seq::foreach_strip(&ed->seqbase, [&](Strip *strip) {
      for (StripModifierData &smd : strip->modifiers) {
        if ((smd.flag & STRIP_MODIFIER_FLAG_MUTE) == 0) {
          smd.flag |= STRIP_MODIFIER_FLAG_SHOW_PREVIEW;
        }
      }
      return true;
    });
  }
}

static void version_scene_strip_view_layer_name(Main &bmain)
{
  for (const Scene &scene : bmain.scenes) {
    Editing *ed = seq::editing_get(&scene);
    if (ed == nullptr) {
      continue;
    }

    seq::foreach_strip(&ed->seqbase, [&](Strip *strip) {
      if (strip->type != STRIP_TYPE_SCENE || strip->scene == nullptr) {
        return true;
      }
      strip->scene_view_layer_name = BLI_strdup(BKE_view_layer_default_render(strip->scene)->name);
      return true;
    });
  }
}

/* Compositor node trees with an image input and an image output can likely be used as strip
 * modifiers. */
static void enable_compositor_nodes_is_strip_modifier(Main &bmain)
{
  for (bNodeTree &group : bmain.nodetrees) {
    if (group.type != NTREE_COMPOSIT) {
      continue;
    }
    bool has_image_input = false;
    bool has_image_output = false;
    group.tree_interface.foreach_item([&](const bNodeTreeInterfaceItem &item) {
      if (item.item_type != NodeTreeInterfaceItemType::Socket) {
        /* Continue. */
        return true;
      }
      const auto &socket = reinterpret_cast<const bNodeTreeInterfaceSocket &>(item);
      if (socket.flag & NODE_INTERFACE_SOCKET_INPUT) {
        has_image_input = has_image_input || STREQ(socket.socket_type, "NodeSocketColor");
        /* Continue. */
        return true;
      }
      if (socket.flag & NODE_INTERFACE_SOCKET_OUTPUT) {
        has_image_output = has_image_output || STREQ(socket.socket_type, "NodeSocketColor");
        /* Continue. */
        return true;
      }
      /* Break. */
      return false;
    });

    if (has_image_input && has_image_output) {
      if (!group.compositor_node_asset_traits) {
        group.compositor_node_asset_traits = MEM_new<CompositorNodeAssetTraits>(__func__);
      }
      group.compositor_node_asset_traits->flag |= COMPOSIT_NODE_ASSET_STRIP_MODIFIER;
      bke::node_update_asset_metadata(group);
    }
  }
}

static void versioning_replace_legacy_compositor_switch_node(bNodeTree *node_tree)
{
  version_node_input_socket_name(node_tree, CMP_NODE_SWITCH, "On", "True");
  version_node_input_socket_name(node_tree, CMP_NODE_SWITCH, "Off", "False");
  version_node_output_socket_name(node_tree, CMP_NODE_SWITCH, "Image", "Output");

  for (bNode &node : node_tree->nodes) {
    if (node.type_legacy == CMP_NODE_SWITCH) {
      node.type_legacy = GEO_NODE_SWITCH;
      NodeSwitch *storage = MEM_new<NodeSwitch>(__func__);
      storage->input_type = SOCK_RGBA;
      STRNCPY_UTF8(node.idname, "GeometryNodeSwitch");
      node.storage = storage;
    }
  }
}

void do_versions_after_linking_520(FileData *fd, Main *bmain)
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
              *fd, *bmain, object, reinterpret_cast<NodesModifierData &>(md));
        }
      }
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 27)) {
    version_scene_strip_view_layer_name(*bmain);
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 36)) {
    /* Shift animation data to accommodate the new thin wall input. */
    version_node_socket_index_animdata(bmain, NTREE_SHADER, SH_NODE_BSDF_PRINCIPLED, 5, 1, 31);
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 44)) {
    /* We have to remove the invalid motion paths. Re-baking into clip space on file load would be
     * very expensive. */
    for (Object &object : bmain->objects) {
      if (object.mpath && (object.avs.path_bakeflag & MOTIONPATH_BAKE_CAMERA_SPACE)) {
        animviz_free_motionpath(object.mpath);
        object.mpath = nullptr;
        object.avs.path_bakeflag &= ~MOTIONPATH_BAKE_HAS_PATHS;
      }
      if (object.pose && (object.pose->avs.path_bakeflag & MOTIONPATH_BAKE_CAMERA_SPACE)) {
        for (bPoseChannel &pose_bone : object.pose->chanbase) {
          if (pose_bone.mpath) {
            animviz_free_motionpath(pose_bone.mpath);
            pose_bone.mpath = nullptr;
          }
        }
        object.pose->avs.path_bakeflag &= ~MOTIONPATH_BAKE_HAS_PATHS;
      }
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 47)) {
    /* Re-encode multires displacement grids into the well-conditioned tangent space (see
     * #BKE_multires_construct_tangent_matrix). The conversion rewrites the mesh's shared
     * #CD_MDISPS in place, so each mesh must be converted exactly once: a mesh used by several
     * objects (linked duplicates) or an object carrying two Multires modifiers would otherwise be
     * decoded a second time with the legacy frames and permanently corrupt the displacement. */
    Set<const void *> converted_meshes;
    for (Object &ob : bmain->objects) {
      for (ModifierData &md : ob.modifiers) {
        if (md.type != eModifierType_Multires) {
          continue;
        }
        if (ob.data == nullptr || !converted_meshes.add(ob.data)) {
          continue;
        }
        MultiresModifierData *mmd = reinterpret_cast<MultiresModifierData *>(&md);
        multires_do_versions_tangent_space_conversion(&ob, mmd);
      }
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 49)) {
    bke::sculpt_layers::sculpt_layers_after_lib_link_fixups(*bmain);
  }

  /**
   * Always bump subversion in BKE_blender_version.h when adding versioning
   * code here, and wrap it inside a MAIN_VERSION_FILE_ATLEAST check.
   *
   * \note Keep this message at the bottom of the function.
   */
}

static bool face_set_brush_color_is_uninitialized(const float color[3])
{
  static const float legacy_primary[3] = {1.0f, 0.5f, 0.2f};
  static const float legacy_secondary[3] = {0.2f, 0.5f, 1.0f};
  static const float previous_default[3] = {71.0f / 255.0f, 114.0f / 255.0f, 179.0f / 255.0f};

  if (is_zero_v3(color)) {
    return true;
  }
  if (compare_v3v3(color, legacy_primary, FLT_EPSILON)) {
    return true;
  }
  if (compare_v3v3(color, legacy_secondary, FLT_EPSILON)) {
    return true;
  }
  if (compare_v3v3(color, previous_default, FLT_EPSILON)) {
    return true;
  }
  return false;
}

/**
 * Initializes the Custom-channel range for files that predate material paint channels.
 * Per-channel enable/value/blend live on #Brush.material_paint and need no versioning: the brush
 * settings are allocated on demand by #BKE_brush_material_paint_ensure, already at their defaults.
 */
static void version_material_paint_channel_defaults(Main &bmain)
{
  for (Scene &scene : bmain.scenes) {
    ToolSettings *ts = scene.toolsettings;
    if (ts == nullptr) {
      continue;
    }
    PaintModeSettings &paint_mode = ts->paint_mode;
    /* A zero-length range is the zero-initialized state of an older file, never a valid setting.
     * Only the maximum needs restoring; the minimum default is already 0. */
    if (paint_mode.channel_custom_range[0] == 0.0f && paint_mode.channel_custom_range[1] == 0.0f) {
      paint_mode.channel_custom_range[1] = 1.0f;
    }
  }
}

/**
 * Material paint used to blend every channel with the single brush-wide #Brush.blend, while
 * #BrushMaterialPaintChannel.blend was stored but never read. Base Color now blends with its own
 * stored mode, so carry the brush's mode over to it; otherwise such brushes would silently fall
 * back to Mix, which is the zero value the field was left at.
 *
 * The scalar channels are deliberately left alone: they no longer blend with anything but Mix (see
 * #BKE_paint_material_channel_blend_mode), so there is nothing to preserve for them.
 */
static void version_material_paint_base_color_blend_from_brush(Main &bmain)
{
  for (Brush &brush : bmain.brushes) {
    if (brush.material_paint == nullptr) {
      continue;
    }
    brush.material_paint->channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].blend = brush.blend;
  }
}

static void version_material_paint_channel_height_defaults(Main &bmain)
{
  for (Brush &brush : bmain.brushes) {
    if (brush.material_paint == nullptr) {
      continue;
    }
    BrushMaterialPaintChannel &height =
        brush.material_paint->channels[PAINT_MATERIAL_CHANNEL_HEIGHT];
    /* Trailing DNA growth should zero-fill; set explicit defaults for clarity. */
    height.use = 0;
    height.value[0] = 0.0f;
    height.value[1] = 0.0f;
    height.value[2] = 0.0f;
    height.blend = 0;
  }
}

static void version_material_paint_channel_alpha_ao_emission_defaults(Main &bmain)
{
  for (Brush &brush : bmain.brushes) {
    if (brush.material_paint == nullptr) {
      continue;
    }
    BrushMaterialPaintChannel &alpha =
        brush.material_paint->channels[PAINT_MATERIAL_CHANNEL_ALPHA];
    /* Trailing DNA growth should zero-fill; set explicit defaults for clarity. */
    alpha.use = 0;
    alpha.value[0] = 1.0f;
    alpha.value[1] = 0.0f;
    alpha.value[2] = 0.0f;
    alpha.blend = 0;
    brush.material_paint->use_alpha_map = 1;
    brush.material_paint->use_alpha_stroke_mask = 1;

    BrushMaterialPaintChannel &ao = brush.material_paint->channels[PAINT_MATERIAL_CHANNEL_AO];
    ao.use = 0;
    ao.value[0] = 1.0f;
    ao.value[1] = 0.0f;
    ao.value[2] = 0.0f;
    ao.blend = 0;

    BrushMaterialPaintChannel &emission =
        brush.material_paint->channels[PAINT_MATERIAL_CHANNEL_EMISSION];
    emission.use = 0;
    emission.value[0] = 0.0f;
    emission.value[1] = 0.0f;
    emission.value[2] = 0.0f;
    emission.blend = 0;
  }
}

/**
 * The legacy #PaintModeSettings::visible_material_channels_deprecated was added with a zero
 * in-class default, which
 * is indistinguishable from "user hid every channel" at read time. Gated purely on file version
 * (never on the field's value) so a user's deliberate all-hidden choice made after upgrading is
 * never clobbered by a later load of the same file.
 */
static void version_material_paint_channel_visibility_defaults(Main &bmain)
{
  for (Scene &scene : bmain.scenes) {
    ToolSettings *ts = scene.toolsettings;
    if (ts == nullptr) {
      continue;
    }
    ts->paint_mode.visible_material_channels_deprecated = PAINT_MATERIAL_CHANNELS_VISIBLE_DEFAULT;
  }
}

static void version_material_paint_channel_shader_visibility_defaults(Main &bmain)
{
  /* The default matches the shading behavior before this bitmask existed, where display was
   * driven purely by whether the channel's attribute was present. */
  for (Scene &scene : bmain.scenes) {
    ToolSettings *ts = scene.toolsettings;
    if (ts == nullptr) {
      continue;
    }
    ts->paint_mode.material_shader_visible_channels =
        PAINT_MATERIAL_CHANNELS_SHADER_VISIBLE_DEFAULT;
  }
}

/**
 * `PaintModeSettings::material_paint_flag` reuses bytes that were padding in older files, so it
 * reads back as zero - indistinguishable from "the user turned brush sync off". Gated purely on
 * file version (never on the field's value) so a deliberate opt-out made after upgrading survives
 * later loads of the same file.
 */
static void version_material_paint_brush_sync_defaults(Main &bmain)
{
  for (Scene &scene : bmain.scenes) {
    ToolSettings *ts = scene.toolsettings;
    if (ts == nullptr) {
      continue;
    }
    ts->paint_mode.material_paint_flag |= PAINT_MATERIAL_BRUSH_SYNC;
  }
}

/**
 * Both visibility bitmasks only gained a non-zero DNA default partway through this feature's
 * development; scenes created by an in-between dev build were written with zero, which the UI
 * reads as "every channel hidden". Unlike the two functions above this one cannot overwrite
 * unconditionally, because #version_material_paint_channel_visibility_defaults /
 * #version_material_paint_channel_shader_visibility_defaults (called just before this, in the
 * same merged versioning gate) may since have turned a zero into a deliberate all-hidden choice
 * for this same file - so only an all-zero mask is restored to the default, any other value is
 * left alone.
 */
static void version_material_paint_channel_visibility_fix_zeros(Main &bmain)
{
  for (Scene &scene : bmain.scenes) {
    ToolSettings *ts = scene.toolsettings;
    if (ts == nullptr) {
      continue;
    }
    if (ts->paint_mode.visible_material_channels_deprecated == 0) {
      ts->paint_mode.visible_material_channels_deprecated = PAINT_MATERIAL_CHANNELS_VISIBLE_DEFAULT;
    }
    if (ts->paint_mode.material_shader_visible_channels == 0) {
      ts->paint_mode.material_shader_visible_channels =
          PAINT_MATERIAL_CHANNELS_SHADER_VISIBLE_DEFAULT;
    }
  }
}

/**
 * Move the scene-wide channel visibility mask onto the two #Paint modes that paint PBR channels,
 * which own it independently from now on. Both start from the old shared value, so an upgraded
 * file paints exactly what it painted before; they only drift apart once the user edits one.
 *
 * The value is copied as-is, including an all-hidden one: that is a deliberate user choice, and
 * #version_material_paint_channel_visibility_fix_zeros already repaired the files where a zero
 * meant "never initialized". The other #Paint types in #ToolSettings are intentionally left at
 * their read-time value: they have no PBR channels and no RNA for this field.
 */
static void version_material_paint_channel_visibility_per_paint(Main &bmain)
{
  for (Scene &scene : bmain.scenes) {
    ToolSettings *ts = scene.toolsettings;
    if (ts == nullptr) {
      continue;
    }
    const int visible_channels = ts->paint_mode.visible_material_channels_deprecated;
    if (ts->sculpt != nullptr) {
      ts->sculpt->paint.visible_material_channels = visible_channels;
    }
    ts->imapaint.paint.visible_material_channels = visible_channels;
  }
}

/**
 * `PaintModeSettings::new_channel_image_size` was added with a C++ default member initializer
 * (4096), but that initializer only applies to freshly constructed structs; files saved before
 * this field existed have it zero-filled on read, which is not one of the enum's valid sizes and
 * shows as a blank dropdown.
 */
static void version_material_paint_channel_image_size_defaults(Main &bmain)
{
  for (Scene &scene : bmain.scenes) {
    ToolSettings *ts = scene.toolsettings;
    if (ts == nullptr) {
      continue;
    }
    if (ts->paint_mode.new_channel_image_size == 0) {
      ts->paint_mode.new_channel_image_size = PAINT_NEW_CHANNEL_IMAGE_SIZE_4K;
    }
  }
}

static void version_material_paint_channel_source_mtex_defaults(Main &bmain)
{
  /* Files saved by earlier revisions of this branch have a zeroed `source_mtex`: size 0 and an
   * invalid brush map mode, which would make sampling silently return nothing. Preserve any
   * already-linked Tex; only mapping defaults need repair. */
  for (Brush &brush : bmain.brushes) {
    if (brush.material_paint == nullptr) {
      continue;
    }
    for (int i = 0; i < PAINT_MATERIAL_CHANNEL_NUM; i++) {
      MTex &mtex = brush.material_paint->channels[i].source_mtex;
      if (!is_zero_v3(mtex.size) && mtex.brush_map_mode <= MTEX_MAP_MODE_STENCIL) {
        continue;
      }
      Tex *tex = mtex.tex;
      mtex = blender::dna::shallow_copy(MTex());
      mtex.tex = tex;
    }
  }
}

/* Trailing DNA growth zero-fills, which would leave a zero-sized bake buffer. Maps mode and the
 * brush layout are correct at zero, so only the size needs restoring. */
static void version_material_paint_source_defaults_one(BrushMaterialPaint *material_paint)
{
  if (material_paint != nullptr && material_paint->source_bake_size == 0) {
    material_paint->source_bake_size = 1024;
  }
}

static void version_material_paint_source_defaults(Main &bmain)
{
  for (Brush &brush : bmain.brushes) {
    version_material_paint_source_defaults_one(brush.material_paint);
  }
  /* The per-brush presets in the tool settings hold their own #BrushMaterialPaint, which grows
   * with the same DNA and therefore needs the same defaults. */
  for (Scene &scene : bmain.scenes) {
    if (scene.toolsettings == nullptr) {
      continue;
    }
    for (PaintMaterialBrushPreset &preset :
         scene.toolsettings->paint_mode.material_paint_brush_presets)
    {
      version_material_paint_source_defaults_one(preset.material_paint);
    }
  }
}

static void version_solid_color_width_height_defaults(Main &bmain)
{
  for (Scene &scene : bmain.scenes) {
    Editing *ed = seq::editing_get(&scene);
    if (ed == nullptr) {
      continue;
    }
    seq::foreach_strip(&ed->seqbase, [&](Strip *strip) {
      if (strip->type == STRIP_TYPE_COLOR && strip->effectdata != nullptr) {
        SolidColorVars *data = static_cast<SolidColorVars *>(strip->effectdata);
        data->width = scene.r.xsch;
        data->height = scene.r.ysch;
      }
      return true;
    });
  }
}

static void do_versions_init_tag_category_memory(Main *bmain)
{
  for (bScreen &screen : bmain->screens) {
    for (ScrArea &area : screen.areabase) {
      for (SpaceLink &sl : area.spacedata) {
        switch (sl.spacetype) {
          case SPACE_VIEW3D: {
            View3D *v3d = reinterpret_cast<View3D *>(&sl);
            v3d->tabs_state.tag_last_active_categories[0] = '\0';
            break;
          }
          case SPACE_PROPERTIES: {
            SpaceProperties *sbuts = reinterpret_cast<SpaceProperties *>(&sl);
            sbuts->tabs_state.tag_last_active_categories[0] = '\0';
            break;
          }
          case SPACE_NODE: {
            SpaceNode *snode = reinterpret_cast<SpaceNode *>(&sl);
            snode->tabs_state.tag_last_active_categories[0] = '\0';
            break;
          }
          case SPACE_IMAGE: {
            SpaceImage *sima = reinterpret_cast<SpaceImage *>(&sl);
            sima->tabs_state.tag_last_active_categories[0] = '\0';
            break;
          }
          default:
            break;
        }
      }
    }
  }
}

/* Migrate the legacy ``tag_last_active_categories`` packed string into the structured
 * #CategoryTabsState.last_active_categories array. The packed format is '\n'-separated
 * "tags:category" records (the tag key may contain ';' but never ':' or '\n'). */
static void do_versions_structure_last_active_categories(CategoryTabsState &state)
{
  state.last_active_num = 0;
  const char *cursor = state.tag_last_active_categories;
  while (*cursor != '\0' && state.last_active_num < CATEGORY_LAST_ACTIVE_MAX) {
    const char *record_end = cursor;
    while (*record_end != '\0' && *record_end != '\n') {
      record_end++;
    }
    /* The first ':' separates the tag key from the category id. */
    const char *colon = cursor;
    while (colon < record_end && *colon != ':') {
      colon++;
    }
    if (colon < record_end) {
      CategoryLastActive &slot = state.last_active_categories[state.last_active_num];
      int tags_ncpy = int(colon - cursor) + 1;
      if (tags_ncpy > int(sizeof(slot.tags))) {
        tags_ncpy = int(sizeof(slot.tags));
      }
      BLI_strncpy(slot.tags, cursor, tags_ncpy);
      int cat_ncpy = int(record_end - (colon + 1)) + 1;
      if (cat_ncpy > int(sizeof(slot.category))) {
        cat_ncpy = int(sizeof(slot.category));
      }
      BLI_strncpy(slot.category, colon + 1, cat_ncpy);
      state.last_active_num++;
    }
    cursor = (*record_end == '\n') ? record_end + 1 : record_end;
  }
  /* The packed string is no longer used at runtime. */
  state.tag_last_active_categories[0] = '\0';
}

static void do_versions_structure_tag_category_memory(Main *bmain)
{
  for (bScreen &screen : bmain->screens) {
    for (ScrArea &area : screen.areabase) {
      for (SpaceLink &sl : area.spacedata) {
        switch (sl.spacetype) {
          case SPACE_VIEW3D:
            do_versions_structure_last_active_categories(
                reinterpret_cast<View3D *>(&sl)->tabs_state);
            break;
          case SPACE_PROPERTIES:
            do_versions_structure_last_active_categories(
                reinterpret_cast<SpaceProperties *>(&sl)->tabs_state);
            break;
          case SPACE_NODE:
            do_versions_structure_last_active_categories(
                reinterpret_cast<SpaceNode *>(&sl)->tabs_state);
            break;
          case SPACE_IMAGE:
            do_versions_structure_last_active_categories(
                reinterpret_cast<SpaceImage *>(&sl)->tabs_state);
            break;
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

  /* Before the ensure below, which no longer covers Properties: drop the region that earlier
   * versions of this file added there. */
  do_versions_remove_properties_tag_bar_region(bmain);

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

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 17)) {
    for (Material &materials : bmain->materials) {
      if (materials.gp_style != nullptr) {
        materials.gp_style->placement_mode = GP_MATERIAL_PLACEMENT_COUNT;
        materials.gp_style->placement_count = 1;
        materials.gp_style->placement_density = 10.0f;
        materials.gp_style->placement_radius_spacing = 100.0f;
      }
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 18)) {
    for (Scene &scene : bmain->scenes) {
      if (scene.toolsettings->sculpt) {
        Sculpt &sculpt = *scene.toolsettings->sculpt;
        MeshAutomaskingSettings *settings = MEM_new<MeshAutomaskingSettings>(__func__);
        settings->flags = sculpt.automasking_flags;
        settings->boundary_edges_propagation_steps =
            sculpt.automasking_boundary_edges_propagation_steps;
        settings->cavity_blur_steps = sculpt.automasking_cavity_blur_steps;
        settings->cavity_factor = sculpt.automasking_cavity_factor;
        settings->start_normal_limit = sculpt.automasking_start_normal_limit;
        settings->start_normal_falloff = sculpt.automasking_start_normal_falloff;
        settings->view_normal_limit = sculpt.automasking_view_normal_limit;
        settings->view_normal_falloff = sculpt.automasking_view_normal_falloff;
        settings->cavity_curve = BKE_curvemapping_copy(sculpt.automasking_cavity_curve);
        settings->cavity_curve_op = BKE_curvemapping_copy(sculpt.automasking_cavity_curve_op);

        scene.toolsettings->sculpt->paint.mesh_automasking_settings = settings;
      }
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 19)) {
    for (bNodeTree &tree : bmain->nodetrees) {
      sanitize_node_tree_interface_socket_identifiers(tree);
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 20)) {
    for (Brush &brush : bmain->brushes) {
      if (brush.ob_mode != OB_MODE_SCULPT) {
        continue;
      }

      brush.mesh_automasking_settings = MEM_new<MeshAutomaskingSettings>(__func__);
      brush.mesh_automasking_settings->flags = brush.automasking_flags;
      brush.mesh_automasking_settings->boundary_edges_propagation_steps =
          brush.automasking_boundary_edges_propagation_steps;
      brush.mesh_automasking_settings->cavity_blur_steps = brush.automasking_cavity_blur_steps;
      brush.mesh_automasking_settings->cavity_factor = brush.automasking_cavity_factor;
      brush.mesh_automasking_settings->start_normal_falloff =
          brush.automasking_start_normal_falloff;
      brush.mesh_automasking_settings->start_normal_limit = brush.automasking_start_normal_limit;
      brush.mesh_automasking_settings->view_normal_falloff = brush.automasking_view_normal_falloff;
      brush.mesh_automasking_settings->view_normal_limit = brush.automasking_view_normal_limit;
      brush.mesh_automasking_settings->cavity_curve = BKE_curvemapping_copy(
          brush.automasking_cavity_curve);
      brush.mesh_automasking_settings->cavity_curve_op = nullptr;
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 21)) {
    for (Material &materials : bmain->materials) {
      if (materials.gp_style != nullptr) {
        materials.gp_style->random_size_factor = 0.0f;
        materials.gp_style->random_strength_factor = 0.0f;
        materials.gp_style->random_rotation_factor = 0.0f;
        materials.gp_style->random_hue_factor = 0.0f;
        materials.gp_style->random_saturation_factor = 0.0f;
        materials.gp_style->random_value_factor = 0.0f;
        materials.gp_style->random_noise_scale = 1.0f;
      }
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 22)) {
    version_strip_modifier_show_preview_flag(*bmain);
  }

  /* The ID member of the Viewer node is no longer initialized to the Viewer Image, so clear that
   * member. */
  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 23)) {
    FOREACH_NODETREE_BEGIN (bmain, node_tree, id) {
      if (node_tree->type == NTREE_COMPOSIT) {
        for (bNode &node : node_tree->nodes) {
          if (node.type_legacy == CMP_NODE_VIEWER) {
            node.id = nullptr;
          }
        }
      }
    }
    FOREACH_NODETREE_END;
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 24)) {
    FOREACH_NODETREE_BEGIN (bmain, node_tree, id) {
      if (node_tree->type == NTREE_SHADER) {
        for (bNode &node : node_tree->nodes) {
          if (node.type_legacy == SH_NODE_RAYCAST && node.storage == nullptr) {
            node.storage = MEM_new<NodeShaderRaycast>(__func__);
          }
        }
      }
    }
    FOREACH_NODETREE_END;
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 25)) {
    for (bScreen &screen : bmain->screens) {
      for (ScrArea &area : screen.areabase) {
        for (SpaceLink &space : area.spacedata) {
          if (space.spacetype == SPACE_OUTLINER) {
            SpaceOutliner *space_outliner = reinterpret_cast<SpaceOutliner *>(&space);
            space_outliner->flag |= SO_SCROLL_TO_ACTIVE;
          }
        }
      }
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 26)) {
    FOREACH_NODETREE_BEGIN (bmain, tree, id) {
      if (tree->type != NTREE_GEOMETRY) {
        continue;
      }
      for (bNode &node : tree->nodes) {
        switch (node.type_legacy) {
          case FN_NODE_COMPARE:
          case FN_NODE_RANDOM_VALUE: {
            version_socket_identifier_suffixes_for_dynamic_types(node.inputs, "_");
            version_socket_identifier_suffixes_for_dynamic_types(node.outputs, "_");
            break;
          }
        }
      }
    }
    FOREACH_NODETREE_END;
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 28)) {
    version_text_strip_space_line(*bmain);
    version_compositor_effect_initialized(*bmain);
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 29)) {
    for (bScreen &screen : bmain->screens) {
      for (ScrArea &area : screen.areabase) {
        for (SpaceLink &sl : area.spacedata) {
          if (sl.spacetype != SPACE_SEQ) {
            continue;
          }
          ListBaseT<ARegion> *regionbase = (&sl == area.spacedata.first) ? &area.regionbase :
                                                                           &sl.regionbase;
          ARegion *scrubbing_region = do_versions_add_region_if_not_found(
              regionbase, RGN_TYPE_SCRUBBING, "Scrubbing Region", RGN_TYPE_FOOTER);
          if (scrubbing_region) {
            scrubbing_region->alignment = RGN_ALIGN_BOTTOM | RGN_STACK_ON_PREV |
                                          RGN_ALIGN_HIDE_WITH_PREV;
          }
        }
      }
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 30)) {
    enable_compositor_nodes_is_strip_modifier(*bmain);
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 31)) {
    for (Mesh &mesh : bmain->meshes) {
      if (mesh.attributes().contains(".uv_seam")) {
        mesh.attributes_for_write().rename(".uv_seam", "uv_seam");
      }
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 34)) {
    FOREACH_NODETREE_BEGIN (bmain, ntree, id) {
      if (ntree->type == NTREE_COMPOSIT) {
        versioning_replace_legacy_compositor_switch_node(ntree);
      }
    }
    FOREACH_NODETREE_END;
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 35)) {
    for (Object &object : bmain->objects) {
      object.parent_bone_head_tail_factor = 1.0;
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 37)) {
    version_text_strip_abs_space_line(*bmain);
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 38)) {
    for (Brush &brush : bmain->brushes) {
      if (brush.gpencil_settings != nullptr) {
        brush.gpencil_settings->fill_gap_factor = 0.4f;
        brush.gpencil_settings->flag |= GP_BRUSH_FILL_INTERNAL_GAPS;
      }
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 39)) {
    for (bScreen &screen : bmain->screens) {
      for (ScrArea &area : screen.areabase) {
        for (SpaceLink &sl : area.spacedata) {
          if (sl.spacetype == SPACE_SEQ) {
            SpaceSeq *sseq = reinterpret_cast<SpaceSeq *>(&sl);
            sseq->preview_overlay.flag |= SEQ_PREVIEW_SHOW_COMPOSITION_GUIDES;
            float default_col[4] = {0.5f, 0.5f, 0.5f, 1.0f};
            copy_v4_v4(sseq->preview_overlay.composition_guide_color, default_col);
          }
        }
      }
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 40)) {
    for (wmWindowManager &wm : bmain->wm) {
      wm.xr.session_settings.viewfinder_enabled = false;
      wm.xr.session_settings.viewfinder_crosshair_enabled = true;

      wm.xr.session_settings.viewfinder_hand = XR_VIEWFINDER_HAND_RIGHT;
      wm.xr.session_settings.viewfinder_scale = 1.0f;

      wm.xr.session_settings.viewfinder_passepartout_overscan = 0.5f;
      wm.xr.session_settings.viewfinder_passepartout_opacity = 0.5f;
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 41)) {
    version_solid_color_width_height_defaults(*bmain);
  }

  /* Fix the fact that previously, making a linked data local and/or clearing a liboverride would
   * not properly flag some sub-data like modifiers or constraints as local. */
  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 43)) {
    for (ID &id : MainAllIDsIterator{*bmain}) {
      if (!ID_IS_LINKED(&id) && !ID_IS_OVERRIDE_LIBRARY(&id)) {
        BKE_lib_override_flag_subdata_local(id);
      }
    }
  }

  /* The compositor previously did not support default inputs for group nodes, but some built-in
   * nodes had the position field default type for some inputs, so node groups would gain it as a
   * default type through some operators. Later, the default inputs were supported for group nodes,
   * though position field were not supported in the compositor, so it would assert. To fix this,
   * we reset any position field default input to the default value. */
  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 44)) {
    FOREACH_NODETREE_BEGIN (bmain, node_tree, id) {
      if (node_tree->type == NTREE_COMPOSIT) {
        node_tree->ensure_interface_cache();
        for (bNodeTreeInterfaceSocket *input : node_tree->interface_inputs()) {
          if (input->default_input == NODE_DEFAULT_INPUT_POSITION_FIELD) {
            input->default_input = NODE_DEFAULT_INPUT_VALUE;
          }
        }
      }
    }
    FOREACH_NODETREE_END;
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 45)) {
    /* #Brush.drag_kind is new; back-fill it from the fields it classifies (sculpt_brush_type /
     * stroke_method / cloth_deform_type) so multi-object sculpt strokes with brushes saved by
     * older files get the same world-space drag mirroring as brushes created after this version
     * (see Architecture_Refactoring_Analysis.md 3.5). */
    for (Brush &brush : bmain->brushes) {
      BKE_brush_drag_kind_update(&brush);
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 46)) {
    /* Symmetry overlays are new, older files store zeroes for them. Match the defaults of a
     * newly created viewport so the feature is not silently invisible in existing files. */
    for (bScreen &screen : bmain->screens) {
      for (ScrArea &area : screen.areabase) {
        for (SpaceLink &sl : area.spacedata) {
          if (sl.spacetype == SPACE_VIEW3D) {
            View3D *v3d = reinterpret_cast<View3D *>(&sl);
            /* Contours on by default; translucent symmetry plane stays off. */
            v3d->overlay.symmetry_flag |= V3D_OVERLAY_SYMMETRY_SCULPT_CONTOUR |
                                          V3D_OVERLAY_SYMMETRY_WEIGHT_PAINT_CONTOUR |
                                          V3D_OVERLAY_SYMMETRY_VERTEX_PAINT_CONTOUR |
                                          V3D_OVERLAY_SYMMETRY_TEXTURE_PAINT_CONTOUR |
                                          V3D_OVERLAY_SYMMETRY_EDIT_MESH_CONTOUR;
            v3d->overlay.sculpt_symmetry_plane_opacity = 0.03f;
            v3d->overlay.sculpt_symmetry_contour_thickness = 3.0f;
          }
        }
      }
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 47)) {
    /* The sculpt layer mask overlay is on by default, and its bit is new: every file predating it
     * stored a zero there, which would otherwise read as "the user turned it off". Set once, so a
     * later deliberate toggle survives. */
    for (bScreen &screen : bmain->screens) {
      for (ScrArea &area : screen.areabase) {
        for (SpaceLink &sl : area.spacedata) {
          if (sl.spacetype == SPACE_VIEW3D) {
            View3D *v3d = reinterpret_cast<View3D *>(&sl);
            v3d->overlay.flag |= V3D_OVERLAY_SCULPT_SHOW_LAYER_MASK;
            v3d->overlay.sculpt_mode_layer_mask_opacity = 0.75f;
          }
        }
      }
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 71)) {
    do_versions_fix_category_tabs_zoom_in_spaces(bmain);
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 72)) {
    do_versions_structure_tag_category_memory(bmain);
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 70)) {
    for (bScreen &screen : bmain->screens) {
      for (ScrArea &area : screen.areabase) {
        for (SpaceLink &sl : area.spacedata) {
          if (sl.spacetype != SPACE_FILE) {
            continue;
          }
          ListBaseT<ARegion> *regionbase = (&sl == area.spacedata.first) ? &area.regionbase :
                                                                           &sl.regionbase;
          ARegion *tool_header = do_versions_add_region_if_not_found(
              regionbase, RGN_TYPE_TOOL_HEADER, "Asset Browser Tool Header", RGN_TYPE_HEADER);
          if (tool_header) {
            tool_header->alignment = RGN_ALIGN_TOP;
          }
        }
      }
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 48)) {
    /* The two layer-preview settings are new members, so a file predating them reads zeros: the
     * overlay would come up with a zero threshold and a zero opacity, and the user would have to
     * find both sliders before it showed anything. The flag itself is deliberately left alone — a
     * new overlay should not switch itself on in an existing file. */
    for (Scene &scene : bmain->scenes) {
      if (scene.toolsettings != nullptr && scene.toolsettings->sculpt != nullptr) {
        scene.toolsettings->sculpt->sculpt_layer_preview_threshold = 0.01f;
        scene.toolsettings->sculpt->sculpt_layer_preview_opacity = 0.75f;
      }
    }
  }

  /* NOTE: no versioning translates sculpt layers written before the layer tree migration, which
   * removed the flat `Mesh::sculpt_layers` list such code walked. The old list is not a member of
   * the current SDNA, so a pre-migration file's layers are never read back at all: such a file
   * opens with no sculpt layers. That is the accepted outcome of the no-backward-compatibility
   * decision, not a defect. */

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 49)) {
    bke::sculpt_layers::sculpt_layers_after_lib_link_fixups(*bmain);
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 50)) {
    /* #Brush.face_set_color and #Brush.face_set_secondary_color are new; files predating them read
     * zeroes. Development builds also shipped interim defaults that should converge on the current
     * Face Set color. */
    static const float face_set_color_default[3] = {
        144.0f / 255.0f, 178.0f / 255.0f, 218.0f / 255.0f};
    for (Brush &brush : bmain->brushes) {
      if ((brush.ob_mode & OB_MODE_SCULPT) == 0) {
        continue;
      }
      if (face_set_brush_color_is_uninitialized(brush.face_set_color)) {
        copy_v3_v3(brush.face_set_color, face_set_color_default);
      }
      if (face_set_brush_color_is_uninitialized(brush.face_set_secondary_color)) {
        copy_v3_v3(brush.face_set_secondary_color, face_set_color_default);
      }
    }
  }

  /* Poly Paint (material attribute painting) landed as a single squashed commit in this fork, so
   * none of the intermediate dev-build subversions this feature was originally staged across need
   * to be individually distinguishable any more - only "before this feature existed" vs. "at or
   * after its final shape" matters. All of its versioning steps therefore run under one gate, in
   * their original relative order. */
  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 58)) {
    version_material_paint_channel_defaults(*bmain);
    version_material_paint_base_color_blend_from_brush(*bmain);
    version_material_paint_channel_height_defaults(*bmain);
    version_material_paint_channel_source_mtex_defaults(*bmain);
    version_material_paint_channel_image_size_defaults(*bmain);
    version_material_paint_channel_alpha_ao_emission_defaults(*bmain);
    version_material_paint_channel_visibility_defaults(*bmain);
    version_material_paint_channel_shader_visibility_defaults(*bmain);
    version_material_paint_brush_sync_defaults(*bmain);
    version_material_paint_channel_visibility_fix_zeros(*bmain);
    version_material_paint_channel_visibility_per_paint(*bmain);
  }

  /* Canvas rotation was added to the Image Editor. Files written before it have the new
   * #SpaceImage.rotation_pivot zero-filled rather than centered, which would rotate about the
   * image corner. */
  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 59)) {
    for (bScreen &screen : bmain->screens) {
      for (ScrArea &area : screen.areabase) {
        for (SpaceLink &sl : area.spacedata) {
          if (sl.spacetype == SPACE_IMAGE) {
            SpaceImage *sima = reinterpret_cast<SpaceImage *>(&sl);
            sima->rotation = 0.0f;
            copy_v2_fl(sima->rotation_pivot, 0.5f);
          }
        }
      }
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 60)) {
    /* Members missing from an old file's SDNA are zeroed by blenloader, never left with garbage.
     * Zero happens to be the intended default for `gradient_type`, `gradient_repeat` and
     * `gradient_blend_mode`, but it is out of range or wrong for the rest: `gradient_opacity`
     * would be fully transparent, `warp_grid_size` would fall outside the RNA range [2, 10], and
     * the embedded #ColorBand needs an explicit runtime init to hold any stops. Assign the whole
     * block so the DNA defaults and the versioned values cannot drift apart. */
    for (Scene &scene : bmain->scenes) {
      ImagePaintSettings &imapaint = scene.toolsettings->imapaint;
      imapaint.gradient_type = IMAGE_PAINT_GRADIENT_LINEAR;
      imapaint.gradient_repeat = IMAGE_PAINT_GRADIENT_REPEAT_NONE;
      imapaint.gradient_blend_mode = 0;
      imapaint.gradient_opacity = 1.0f;
      imapaint.warp_grid_size = 4;
      imapaint.warp_interpolation = IMAGE_PAINT_WARP_INTERP_LINEAR;
      BKE_colorband_init(&imapaint.gradient_colorband, true);
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 61)) {
    /* Initialize the vertex paint channel output flags for existing brushes. */
    for (Brush &brush : bmain->brushes) {
      brush.vertex_paint_channel_flag = (BRUSH_VPAINT_CHANNEL_R | BRUSH_VPAINT_CHANNEL_G |
                                         BRUSH_VPAINT_CHANNEL_B);
    }

    /* Initialize the vertex paint channel display flags for the 3D viewport overlay. */
    for (bScreen &screen : bmain->screens) {
      for (ScrArea &area : screen.areabase) {
        for (SpaceLink &sl : area.spacedata) {
          if (sl.spacetype == SPACE_VIEW3D) {
            View3D *v3d = reinterpret_cast<View3D *>(&sl);
            if (v3d->overlay.vertex_paint_channel_flag == 0) {
              v3d->overlay.vertex_paint_channel_flag = V3D_OVERLAY_VPAINT_SHOW_RGB_MASK;
            }
          }
        }
      }
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 62)) {
    const Sculpt defaults = {};
    for (Scene &scene : bmain->scenes) {
      if (Sculpt *sculpt = scene.toolsettings->sculpt) {
        sculpt->paint_curve_show_radius_handles = defaults.paint_curve_show_radius_handles;
        sculpt->paint_curve_radius_display_mode = defaults.paint_curve_radius_display_mode;
      }
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 63)) {
    /* Curve Patch and Roll are sculpt-only, and Curve Patch is further restricted to the brushes
     * #supports_curve_patch allows. A brush that stored either outside those bounds would show a
     * blank stroke method in the UI, since the enum item is no longer offered.
     *
     * Since the spec rollout (2026-08-19), Roll and Curve Patch are also exposed in the Image
     * Editor (Paint mode Texture2D). Curve Patch there is further restricted to Draw brushes
     * via `image_curve_patch_session_begin()`; that's enforced at runtime, not at version
     * downgrade time -- brushes that had it set in a texture-paint slot before the spec wrote
     * 2D support stay selected here so the user sees Curve Patch stay armed. */
    for (Brush &brush : bmain->brushes) {
      if (!ELEM(brush.stroke_method, BRUSH_STROKE_CURVE_PATCH, BRUSH_STROKE_ROLL)) {
        continue;
      }
      const bool is_sculpt_brush = (brush.ob_mode & OB_MODE_SCULPT) != 0;
      const bool is_image_paint_brush = (brush.ob_mode & OB_MODE_TEXTURE_PAINT) != 0;
      const bool keep_sculpt = is_sculpt_brush && (brush.stroke_method == BRUSH_STROKE_ROLL ||
                                                   bke::brush::supports_curve_patch(brush));
      const bool keep_image_paint = is_image_paint_brush &&
                                    ((brush.stroke_method == BRUSH_STROKE_ROLL) ||
                                     (brush.stroke_method == BRUSH_STROKE_CURVE_PATCH &&
                                      brush.image_brush_type == IMAGE_PAINT_BRUSH_TYPE_DRAW));
      if (!keep_sculpt && !keep_image_paint) {
        brush.stroke_method = BRUSH_STROKE_SPACE;
      }
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 64)) {
    /* The Curve Patch brush settings were added with non-zero defaults, which a file written
     * before they existed cannot carry -- every member reads back as zero. `length_repeat` doubles
     * as the sentinel for "this brush predates the feature": the UI clamps it to 1..64, so no file
     * that knew about the settings can store 0. */
    const Brush defaults = {};
    for (Brush &brush : bmain->brushes) {
      if (brush.curve_patch.length_repeat != 0) {
        continue;
      }
      brush.curve_patch.length_repeat = defaults.curve_patch.length_repeat;
      brush.curve_patch.end_falloff_percent = defaults.curve_patch.end_falloff_percent;
      brush.curve_patch.cap_start_length = defaults.curve_patch.cap_start_length;
      brush.curve_patch.cap_end_length = defaults.curve_patch.cap_end_length;
      brush.roll_pressure_scale = defaults.roll_pressure_scale;
    }

    /* Paint curves used to keep their control points in a screen-space array of their own. The
     * bezier geometry is authoritative now, so convert what the reader loaded and drop the legacy
     * array. */
    for (PaintCurve &paint_curve : bmain->paintcurves) {
      const bool had_legacy_points = paint_curve.points != nullptr && paint_curve.tot_points > 0;
      BKE_paint_curve_legacy_points_convert(paint_curve);
      /* Converted points stay in screen space, which is what a false #PaintCurve::use_3d_space
       * means -- promoting them needs a viewport, so it is left to the user. A curve that had
       * nothing to convert is empty and gets the modern default instead. Both flags default to 1
       * and would otherwise load as 0. */
      paint_curve.use_3d_space = had_legacy_points ? 0 : 1;
      paint_curve.show_radius_handles = 1;
    }
  }

  /* NOTE: An earlier revision of this feature zeroed `mtex.random_angle` on Curve Patch
   * brushes, because STAMPS mode read the amount without the #MTEX_ANGLE_RANDOM switch that
   * gates it everywhere else.
   * The gate now lives in `curve_patch_params_from_brush()`, so no stored data needs changing --
   * and rewriting the amount would have destroyed a deliberate user setting. */

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 65)) {
    /* Paint curves kept their selection in an attribute of their own, packed as one `int8_t` per
     * point (bit 0 left handle, bit 1 control point, bit 2 right handle). They use the same
     * `.selection*` attributes as every other curves editor now.
     *
     * Note the inverted default: the old attribute was absent when nothing was selected, but
     * an absent `.selection` means everything IS selected. So a curve is converted whether or
     * not it carried the legacy attribute -- writing all-false where it had none is what preserves
     * "nothing selected" for those files. */
    for (PaintCurve &paint_curve : bmain->paintcurves) {
      bke::CurvesGeometry &geometry = paint_curve.geometry.wrap();
      if (geometry.points_num() == 0) {
        continue;
      }
      bke::MutableAttributeAccessor attributes = geometry.attributes_for_write();
      const VArray<int8_t> legacy = *attributes.lookup_or_default<int8_t>(
          "paintcurve_selection", bke::AttrDomain::Point, int8_t(0));

      const std::array<StringRef, 3> names = {
          ".selection_handle_left", ".selection", ".selection_handle_right"};
      for (const int bit : IndexRange(3)) {
        bke::SpanAttributeWriter<bool> selection =
            attributes.lookup_or_add_for_write_only_span<bool>(names[bit], bke::AttrDomain::Point);
        for (const int point : geometry.points_range()) {
          selection.span[point] = (legacy[point] & (1 << bit)) != 0;
        }
        selection.finish();
      }
      attributes.remove("paintcurve_selection");
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 73)) {
    version_material_paint_source_defaults(*bmain);
  }

  /**
   * Always bump subversion in BKE_blender_version.h when adding versioning
   * code here, and wrap it inside a MAIN_VERSION_FILE_ATLEAST check.
   *
   * \note Keep this message at the bottom of the function.
   */
}

}  // namespace blender
