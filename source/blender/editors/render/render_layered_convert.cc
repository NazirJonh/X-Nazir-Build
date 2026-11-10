/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edrend
 *
 * Turns a material the user built by hand into a layered one, either by keeping its node graph as
 * the stack's bottom or by baking its channels into a Fill layer, plus the revert that hands the
 * original graph back. The channel dialog and its checkboxes are shared with the New Layered
 * operator.
 */

#include <iterator>
#include <string>

#include "BLI_listbase.h"
#include "BLI_listbase_iterator.hh"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_vector.hh"

#include "DNA_ID.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BLT_translation.hh"

#include "BKE_context.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_object.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_source_graph.hh"
#include "BKE_paint_layers_target.hh"
#include "BKE_paint_material_resolve.hh"
#include "BKE_paint_material_sync.hh"
#include "BKE_report.hh"

#include "ED_material_bake.hh"
#include "ED_object.hh"
#include "ED_paint_layers_bake.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"
#include "RNA_prototypes.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"

#include "render_intern.hh" /* own include */

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Convert Material to Layered Operator
 *
 * A material the user built by hand becomes a layered one, two ways. The stack can composite over
 * the graph the way it stands -- the graph's nodes are kept untouched and its channels become the
 * chains' bottoms -- or the graph can be baked down into maps that a Fill layer holds. Both share
 * the channel dialog of #MATERIAL_OT_new_layered: the channels the stack works with, ticked with
 * what the material's graph really feeds.
 * \{ */

/** What the conversion does to the material's own node graph. */
enum MaterialLayeredConvertMode {
  /** Bake every channel the graph feeds into a map; a Fill layer holds them. */
  LAYERED_CONVERT_BAKE = 0,
  /** Keep the graph: the stack composites over the channels it feeds, painting lays over it. */
  LAYERED_CONVERT_GRAPH = 1,
};

static const EnumPropertyItem material_layered_convert_mode_items[] = {
    {LAYERED_CONVERT_BAKE,
     "BAKE",
     0,
     "Bake to Fill Layer",
     "Bake every channel the material's graph feeds into a map and put them into a Fill layer. "
     "The graph's nodes are kept in the tree for Revert to Plain Material"},
    {LAYERED_CONVERT_GRAPH,
     "GRAPH",
     0,
     "Keep Node Graph",
     "Keep the material's node graph as it is: the stack receives the channels it feeds (Base "
     "Color, Roughness and the rest) and paints over them. Painting never edits the graph's nodes"},
    {0, nullptr, 0, nullptr, nullptr},
};

/** The material the conversion works on: the context's, or the active object's active one. */
static Material *layered_convert_material_get(bContext *C)
{
  if (Material *ma = static_cast<Material *>(
          CTX_data_pointer_get_type(C, "material", RNA_Material).data))
  {
    return ma;
  }
  if (Object *ob = ed::object::context_object(C)) {
    return BKE_object_material_get(ob, ob->actcol);
  }
  return nullptr;
}

static bool layered_convert_poll(bContext *C)
{
  Material *ma = layered_convert_material_get(C);
  if (ma == nullptr || ma->nodetree == nullptr || BKE_paint_layers_is_layered(*ma)) {
    return false;
  }
  return ID_IS_EDITABLE(&ma->id) && !ID_IS_OVERRIDE_LIBRARY(&ma->id);
}

/**
 * The channels the material's graph really feeds: a linked input, a plain map on it, or the Normal
 * chain the resolver unwinds. A value left on an unlinked input is not detected -- it survives the
 * conversion where it is, on the Principled, and the dialog offers the channel anyway.
 */
static PaintChannelMask layered_convert_channels_detect(const MaterialSourceResolve &resolve)
{
  PaintChannelMask mask = 0;
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    if (info.socket_name == nullptr) {
      /* Custom, Height and AO have no Principled input the graph could feed. */
      continue;
    }
    const ChannelResolution resolution = resolve.channels[int(info.channel)];
    if (resolution == ChannelResolution::Image || resolution == ChannelResolution::Baked) {
      mask |= uint32_t(1) << int(info.channel);
    }
  }
  /* Base Color is what a stack is for; offer it even to a material that never linked it. */
  mask |= uint32_t(1) << PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  return mask;
}

/**
 * Whether the author's graph feeds the Principled Normal, so the Normal and Height checkboxes must
 * stay off in GRAPH mode. Adopting that chain under the stack would apply its tangent transform a
 * second time; the user's Normal Map and Bump stay on the Principled instead.
 */
static bool layered_convert_normal_is_fed(const MaterialSourceResolve &resolve)
{
  const ChannelResolution resolution = resolve.channels[PAINT_MATERIAL_CHANNEL_NORMAL];
  return resolution == ChannelResolution::Image || resolution == ChannelResolution::Baked;
}

static int layered_principled_count(const bNodeTree &tree, Set<const bNodeTree *> &visited)
{
  if (!visited.add(&tree)) {
    return 0;
  }
  int count = 0;
  for (const bNode &node : tree.nodes) {
    if (node.type_legacy == SH_NODE_BSDF_PRINCIPLED) {
      count++;
      continue;
    }
    if (node.is_group() && node.id != nullptr && GS(node.id->name) == ID_NT) {
      count += layered_principled_count(*id_cast<bNodeTree *>(node.id), visited);
    }
  }
  return count;
}

/**
 * A conversion edits one Principled in the material's own tree. A second one (a Mix Shader, say)
 * is outside the stack's reach, and a Principled inside a group belongs to a definition this
 * material does not own. Refusing with a reason is safer than silently converting half a material.
 */
static bool layered_convert_principled_validate(wmOperator *op, Material &ma)
{
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  Vector<const bNode *> group_path;
  const bNode *principled = BKE_paint_material_principled_find(ma, reason, &group_path);
  if (principled == nullptr) {
    BKE_report(op->reports,
               RPT_ERROR,
               RPT_("The material has no Principled BSDF for the stack to wire its channels into"));
    return false;
  }
  if (!group_path.is_empty()) {
    BKE_report(op->reports,
               RPT_ERROR,
               RPT_("The Principled BSDF is inside a node group; the stack can only convert the "
                    "material's own tree"));
    return false;
  }
  Set<const bNodeTree *> visited;
  if (layered_principled_count(*ma.nodetree, visited) > 1) {
    BKE_report(op->reports,
               RPT_ERROR,
               RPT_("The material uses more than one Principled BSDF; the stack can only convert "
                    "one"));
    return false;
  }
  return true;
}

static void layered_convert_preserved_links_report(wmOperator *op,
                                                   Material &ma,
                                                   const PaintChannelMask baked_mask)
{
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  const bNode *principled = BKE_paint_material_principled_find(ma, reason);
  if (principled == nullptr) {
    return;
  }
  ma.nodetree->ensure_topology_cache();
  std::string names;
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    if (info.channel == PAINT_MATERIAL_CHANNEL_CUSTOM ||
        info.channel == PAINT_MATERIAL_CHANNEL_AO ||
        info.socket_name == nullptr ||
        (baked_mask & (uint32_t(1) << int(info.channel))) != 0)
    {
      continue;
    }
    bNodeSocket *target = bke::node_find_socket(
        const_cast<bNode &>(*principled),
        SOCK_IN,
        UString::from_ptr_noinline(info.socket_name));
    if (target == nullptr || target->directly_linked_links().is_empty()) {
      continue;
    }
    if (!names.empty()) {
      names += ", ";
    }
    names += IFACE_(info.ui_name);
  }
  if (!names.empty()) {
    char message[512];
    SNPRINTF(message, "Kept the graph's Principled links: %s", names.c_str());
    BKE_report(op->reports, RPT_INFO, message);
  }
}

static wmOperatorStatus layered_convert_invoke(bContext *C,
                                               wmOperator *op,
                                               const wmEvent * /*event*/)
{
  if (Material *ma = layered_convert_material_get(C)) {
    /* The resolver walks the whole graph; the dialog and its redraws read what it found here, so
     * it runs once per invoke and not once per checkbox or redraw. */
    const MaterialSourceResolve resolve = BKE_paint_material_source_resolve(ma);
    const PaintChannelMask detected = layered_convert_channels_detect(resolve);
    const bool graph_mode = RNA_enum_get(op->ptr, "mode") != LAYERED_CONVERT_BAKE;
    const bool normal_fed = graph_mode && layered_convert_normal_is_fed(resolve);
    RNA_boolean_set(op->ptr, "normal_chain_fed", normal_fed);
    for (const LayeredChannelOption &entry : layered_channel_entries()) {
      const bool checked = (detected & (uint32_t(1) << int(entry.channel))) != 0;
      /* The author's Normal chain stays untouched in GRAPH mode: keeping these two off by default
       * is what stops the stack from trying to route a second normal transform over it. */
      const bool blocked = normal_fed &&
                           ELEM(entry.channel,
                                PAINT_MATERIAL_CHANNEL_NORMAL,
                                PAINT_MATERIAL_CHANNEL_HEIGHT);
      RNA_boolean_set(op->ptr, entry.prop_name, checked && !blocked);
    }
  }
  /* The dialog is the one place the blocking bake is announced (see the BAKE exec); exec without
   * it -- a Python call -- reads this back false and warns from there. */
  RNA_boolean_set(op->ptr, "dialog_confirmed", true);
  return WM_operator_props_dialog_popup(C, op, 320, std::nullopt, IFACE_("Convert"));
}

static void layered_convert_ui(bContext *C, wmOperator *op)
{
  ui::Layout &col = op->layout->column(false);
  col.prop(op->ptr, "mode", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  col.separator();
  layered_channels_ui(C, op);
  const bool graph_mode = RNA_enum_get(op->ptr, "mode") != LAYERED_CONVERT_BAKE;
  if (graph_mode && RNA_boolean_get(op->ptr, "normal_chain_fed")) {
    col.label(IFACE_("Normal/Height stay on your node graph; the stack will not adopt them"),
              ICON_INFO);
  }
  if (!graph_mode) {
    col.separator();
    col.prop(op->ptr, "image_size", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col.label(IFACE_("Baking blocks the interface until the maps are rendered"), ICON_INFO);
  }
}

/**
 * The finish the conversion shares with the other stack entries: the base row is active, its maps
 * exist, and a brush in a paint mode points at the material without another opt-in.
 */
static void layered_convert_paint_ready(bContext *C,
                                        Material &ma,
                                        const bUUID base_marker,
                                        Object *ob)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  BKE_paint_layers_active_set(ma, base_marker);
  if (scene != nullptr) {
    int &cursor = scene->toolsettings->paint_mode.stack_layer_channel;
    cursor = BKE_paint_layers_channel_clamp_to_set(ma, cursor);
  }
  BKE_paint_layers_active_maps_ensure(
      *bmain,
      ma,
      scene ? scene->toolsettings->paint_mode.new_channel_image_size :
              int(PAINT_NEW_CHANNEL_IMAGE_SIZE_4K));
  if (Paint *paint = BKE_paint_get_active_from_context(C)) {
    if (BKE_paint_material_brush_auto_ensure(scene, paint, ob)) {
      WM_event_add_notifier(C, NC_BRUSH | NA_EDITED, BKE_paint_brush(paint));
    }
  }
}

/** Keep the graph: the stack receives the graph's channels and paints over them. */
static wmOperatorStatus layered_convert_graph_exec(bContext *C, wmOperator *op, Material &ma)
{
  Main *bmain = CTX_data_main(C);
  const PaintChannelMask mask = layered_channel_mask_from_op(op);
  if (mask == 0) {
    BKE_report(op->reports, RPT_ERROR, RPT_("Select at least one channel for the stack"));
    return OPERATOR_CANCELLED;
  }

  /* The whole conversion -- the set, the mode, the recorded links, the base row and the stack's
   * regeneration, with a rollback when the base row cannot be added -- is one kernel function; the
   * operator brings the context, the reports and the notifiers. */
  PaintLayersConvertReport report;
  if (!BKE_paint_layers_convert_from_graph(*bmain, ma, mask, &report)) {
    BKE_report(op->reports, RPT_ERROR, RPT_("The base layer could not be added to the stack"));
    return OPERATOR_CANCELLED;
  }
  for (const eMaterialPaintChannel channel : report.kept_channels) {
    const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(channel);
    char message[256];
    SNPRINTF(message,
             "The graph's %s chain stays connected; the stack's row will not replace it",
             IFACE_(info.ui_name));
    BKE_report(op->reports, RPT_WARNING, message);
  }

  Object *ob = ed::object::context_object(C);
  layered_convert_paint_ready(C, ma, report.base_layer_marker, ob);

  if (report.regenerate_failed) {
    BKE_report(op->reports,
               RPT_WARNING,
               RPT_("The stack was created but its tree will be generated on the next update"));
  }

  WM_event_add_notifier(C, NC_MATERIAL | NA_EDITED, &ma.id);
  if (ob != nullptr) {
    WM_event_add_notifier(C, NC_OBJECT | ND_OB_SHADING, ob);
  }
  BKE_report(
      op->reports,
      RPT_INFO,
      RPT_("Converted; the stack composites over the material's node graph, which is kept for "
           "Revert to Plain Material"));
  return OPERATOR_FINISHED;
}

/** Bake the graph's channels into maps, flatten them into a Fill layer, and let the generated
 * stack take the tree over. The whole material mutation -- the set, the size, the recorded links,
 * the Fill row and the maps, with one rollback when any step fails -- is the kernel function's;
 * the operator brings the bake, the context, the reports and the notifiers. */
static wmOperatorStatus layered_convert_bake_exec(bContext *C, wmOperator *op, Material &ma)
{
  Main *bmain = CTX_data_main(C);
  const PaintChannelMask requested = layered_channel_mask_from_op(op);
  if (requested == 0) {
    BKE_report(op->reports, RPT_ERROR, RPT_("Select at least one channel to bake"));
    return OPERATOR_CANCELLED;
  }

  Vector<ed::material_bake::BakeTargetSpec> targets;
  for (const LayeredChannelOption &entry : layered_channel_entries()) {
    const eMaterialPaintChannel channel = entry.channel;
    const bool tracked = BKE_paint_material_channel_info(channel).socket_name != nullptr;
    if ((requested & (uint32_t(1) << int(channel))) == 0 || !tracked) {
      continue;
    }
    targets.append(ed::material_bake::BakeTargetSpec{channel, nullptr, false});
  }
  if (targets.is_empty()) {
    BKE_report(op->reports,
               RPT_ERROR,
               RPT_("None of the selected channels can be baked from the material"));
    return OPERATOR_CANCELLED;
  }

  const int size = int(RNA_enum_get(op->ptr, "image_size"));
  ed::material_bake::MaterialBakeToImagesParams params;
  params.material = &ma;
  params.targets = targets;
  params.size = size;
  /* A conversion is a one-shot edit, not a background job: the material is rebuilt from the maps
   * in this very operator, so the pixels have to be here when it continues. The dialog says that
   * this blocks the interface; a Python call lands here without one, so the warning is sent from
   * here for it. */
  if (!RNA_boolean_get(op->ptr, "dialog_confirmed")) {
    BKE_report(op->reports,
               RPT_WARNING,
               RPT_("Baking blocks the interface until the maps are rendered"));
  }
  params.blocking = true;
  const ed::material_bake::MaterialBakeToImagesResult bake =
      ed::material_bake::material_bake_to_images(*bmain, nullptr, nullptr, params);
  if (!bake.ok || bake.created.is_empty() ||
      bake.created_channels.size() != bake.created.size())
  {
    /* The bake's own leftover: the maps it made have no owner yet, and nothing was written. */
    for (Image *image : bake.created) {
      BKE_id_delete(bmain, image);
    }
    BKE_report(op->reports,
               RPT_ERROR,
               RPT_("Nothing was baked: the material has no channel the bake can resolve"));
    return OPERATOR_CANCELLED;
  }

  PaintLayersConvertBakeReport report;
  if (!BKE_paint_layers_convert_from_bake(
          *bmain, ma, bake.created_channels, bake.created, int16_t(size), &report))
  {
    /* The kernel rolled everything back: the maps are deleted, the material reads as it was found.
     * What failed is named; nothing else of the conversion remains. */
    switch (report.failure) {
      case PaintLayersConvertBakeReport::Failure::FillAdd: {
        BKE_report(op->reports,
                   RPT_ERROR,
                   RPT_("The Fill layer could not be added to the stack"));
        break;
      }
      case PaintLayersConvertBakeReport::Failure::MapLink: {
        BKE_report(op->reports,
                   RPT_ERROR,
                   RPT_("The baked maps could not be linked into the Fill layer"));
        break;
      }
      case PaintLayersConvertBakeReport::Failure::NotEditable: {
        BKE_report(op->reports,
                   RPT_ERROR,
                   RPT_("The material cannot be edited here; nothing was converted"));
        break;
      }
      case PaintLayersConvertBakeReport::Failure::ChannelSet:
      case PaintLayersConvertBakeReport::Failure::None: {
        BKE_report(op->reports, RPT_ERROR, RPT_("The baked maps could not form a channel set"));
        break;
      }
    }
    return OPERATOR_CANCELLED;
  }

  PaintChannelMask baked_mask = 0;
  for (const eMaterialPaintChannel channel : bake.created_channels) {
    baked_mask |= uint32_t(1) << int(channel);
  }

  /* The original graph is deliberately not deleted. Packaging it into a node group needs the node
   * editor's group operator, which BKE cannot call; the fallback the design allows is to leave its
   * nodes untouched. The stack only takes over the baked channels' Principled links, so the rest
   * of the graph -- Displacement, Volume, Emission, a second branch -- keeps rendering. Revert
   * puts the recorded links back. */
  Object *ob = ed::object::context_object(C);
  layered_convert_paint_ready(C, ma, report.fill_marker, ob);

  WM_event_add_notifier(C, NC_MATERIAL | NA_EDITED, &ma.id);
  if (ob != nullptr) {
    WM_event_add_notifier(C, NC_OBJECT | ND_OB_SHADING, ob);
  }

  for (const eMaterialPaintChannel channel : bake.skipped_unavailable) {
    const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(channel);
    char message[256];
    SNPRINTF(message,
             RPT_("Channel %s could not be baked and keeps its Principled value"),
             IFACE_(info.ui_name));
    BKE_report(op->reports, RPT_WARNING, message);
  }
  layered_convert_preserved_links_report(op, ma, baked_mask);

  char message[256];
  SNPRINTF(message,
           RPT_("Baked %d of %d selected channel(s) into a Fill layer; the graph is kept for "
                "Revert"),
           int(bake.created.size()),
           int(targets.size()));
  BKE_report(op->reports, RPT_INFO, message);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus layered_convert_exec(bContext *C, wmOperator *op)
{
  Material *ma = layered_convert_material_get(C);
  if (ma == nullptr || ma->nodetree == nullptr) {
    return OPERATOR_CANCELLED;
  }
  if (!layered_convert_principled_validate(op, *ma)) {
    return OPERATOR_CANCELLED;
  }
  if (RNA_enum_get(op->ptr, "mode") == LAYERED_CONVERT_BAKE) {
    return layered_convert_bake_exec(C, op, *ma);
  }
  return layered_convert_graph_exec(C, op, *ma);
}

void MATERIAL_OT_layered_convert(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Convert to Layered Material";
  ot->idname = "MATERIAL_OT_layered_convert";
  ot->description =
      "Turn the material's own node graph into the base of a paint-layer stack, or bake it down "
      "into a Fill layer";

  /* API callbacks. */
  ot->invoke = layered_convert_invoke;
  ot->exec = layered_convert_exec;
  ot->ui = layered_convert_ui;
  ot->poll = layered_convert_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_enum(ot->srna,
               "mode",
               material_layered_convert_mode_items,
               LAYERED_CONVERT_GRAPH,
               "Mode",
               "What happens to the material's node graph");

  /* Found once in invoke and read back by the dialog's redraws: the resolver walks the whole
   * graph, and a redraw must not. */
  PropertyRNA *prop = RNA_def_boolean(
      ot->srna, "normal_chain_fed", false, "Normal Chain Fed", "");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);

  /* Set by the invoke, which is where the blocking bake is announced; exec without the dialog
   * (a Python call) reads it false and warns from there. */
  prop = RNA_def_boolean(ot->srna, "dialog_confirmed", false, "Dialog Confirmed", "");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);

  layered_channels_rna_define(ot, 0, "Let the stack work with this channel of the material");

  RNA_def_enum(ot->srna,
               "image_size",
               rna_enum_material_paint_layers_image_size_items,
               PAINT_NEW_CHANNEL_IMAGE_SIZE_2K,
               "Image Size",
               "Width and height of every map the bake creates");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Revert to Plain Material
 * \{ */

static bool layered_revert_poll(bContext *C)
{
  Material *ma = layered_convert_material_get(C);
  if (ma == nullptr || !BKE_paint_layers_is_layered(*ma)) {
    return false;
  }
  return ID_IS_EDITABLE(&ma->id) && !ID_IS_OVERRIDE_LIBRARY(&ma->id) &&
         BKE_paint_layers_source_graph_can_release(*ma);
}

static wmOperatorStatus layered_revert_exec(bContext *C, wmOperator *op)
{
  Material *ma = layered_convert_material_get(C);
  if (ma == nullptr || ma->nodetree == nullptr) {
    return OPERATOR_CANCELLED;
  }
  Main *bmain = CTX_data_main(C);

  PaintLayersRevertReport report;
  if (!BKE_paint_layers_revert_to_plain(*bmain, *ma, &report)) {
    return OPERATOR_CANCELLED;
  }
  for (const eMaterialPaintChannel channel : report.lost_channels) {
    const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(channel);
    char message[256];
    SNPRINTF(message,
             "The recorded source of %s is gone (renamed or deleted); its default value is back",
             IFACE_(info.ui_name));
    BKE_report(op->reports, RPT_WARNING, message);
  }

  WM_event_add_notifier(C, NC_MATERIAL | NA_EDITED, &ma->id);
  if (Object *ob = ed::object::context_object(C)) {
    WM_event_add_notifier(C, NC_OBJECT | ND_OB_SHADING, ob);
  }
  BKE_report(op->reports,
             RPT_INFO,
             RPT_("Reverted to a plain material; the original Principled links are back"));
  return OPERATOR_FINISHED;
}

void MATERIAL_OT_layered_revert(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Revert to Plain Material";
  ot->idname = "MATERIAL_OT_layered_revert";
  ot->description =
      "Remove the generated stack and put the material's original Principled links back";

  /* API callbacks. */
  ot->exec = layered_revert_exec;
  ot->poll = layered_revert_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Update Source-Graph Cache
 *
 * The manual hand of the source-graph cache: the debounced path re-bakes a material the user is
 * actually looking at, this one starts the same bake the moment it is asked, whatever the
 * active-material policy says about the window it runs in.
 * \{ */

static bool layered_source_cache_update_poll(bContext *C)
{
  Material *ma = layered_convert_material_get(C);
  if (ma == nullptr || !BKE_paint_layers_is_layered(*ma) ||
      (ma->paint_layers_flag & MA_PAINT_LAYERS_SOURCE_GRAPH) == 0)
  {
    return false;
  }
  return ID_IS_EDITABLE(&ma->id) && !ID_IS_OVERRIDE_LIBRARY(&ma->id);
}

static wmOperatorStatus layered_source_cache_update_exec(bContext *C, wmOperator *op)
{
  Material *ma = layered_convert_material_get(C);
  Main *bmain = CTX_data_main(C);
  wmWindowManager *wm = CTX_wm_manager(C);
  wmWindow *win = CTX_wm_window(C);
  if (ma == nullptr || bmain == nullptr || wm == nullptr) {
    return OPERATOR_CANCELLED;
  }
  if (win == nullptr) {
    /* #paint_layers_source_cache_bake_start refuses a bake it has no window to key the job on. */
    BKE_report(op->reports, RPT_ERROR, IFACE_("No window to start the cache bake from"));
    return OPERATOR_CANCELLED;
  }

  /* The checks follow the same order #paint_layers_source_cache_bake_start works through, so
   * every status it can reach is named truthfully here. */
  if (!BKE_paint_layers_source_cache_stale_get(*ma)) {
    BKE_report(op->reports, RPT_INFO, IFACE_("The source-graph cache is up to date"));
    return OPERATOR_FINISHED;
  }
  PaintLayersSourceChannelFeed feeds[PAINT_MATERIAL_CHANNEL_NUM];
  if (BKE_paint_layers_source_graph_feeds_find(
          *ma, MutableSpan<PaintLayersSourceChannelFeed>(feeds, PAINT_MATERIAL_CHANNEL_NUM)) == 0)
  {
    /* The same inline release the debounced path makes: a graph that feeds nothing has no cache
     * to bake, and the maps it no longer fills go at once. */
    ed::material_bake::paint_layers_source_cache_bake_start(*wm, win, *bmain, *ma);
    BKE_report(op->reports,
               RPT_WARNING,
               IFACE_("The graph feeds no linked channel; the cache was released"));
    return OPERATOR_FINISHED;
  }
  if (!ed::material_bake::paint_layers_source_cache_bake_start(*wm, win, *bmain, *ma)) {
    /* The only refusal left at this point: the material's own cache bake is already running. */
    BKE_report(op->reports, RPT_INFO, IFACE_("The source-graph cache bake is already running"));
    return OPERATOR_FINISHED;
  }
  BKE_report(op->reports, RPT_INFO, IFACE_("The source-graph cache bake has started"));
  /* KNOWN LIMITATION, the one the auto path carries too: the cache is rendered on a UV quad, so a
   * channel whose chain reads the object or the camera bakes the quad's answer, and only the
   * render itself keeps reading the real surface. */
  if (BKE_paint_layers_source_graph_object_dependent(*ma)) {
    BKE_report(op->reports,
               RPT_WARNING,
               IFACE_("The cache bakes a UV quad's answer: nodes that read the object or the "
                      "camera (Geometry, Object Info, Texture Coordinate, Camera Data, Light "
                      "Path) may differ from the render"));
  }
  return OPERATOR_FINISHED;
}

void MATERIAL_OT_source_cache_update(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Update Source-Graph Cache";
  ot->idname = "MATERIAL_OT_source_cache_update";
  ot->description =
      "Re-bake the CPU cache of the material's own node graph now: a converted material's "
      "composite reads the graph through these maps, and this starts their bake instead of "
      "waiting out the pause that follows an edit";

  /* API callbacks. */
  ot->exec = layered_source_cache_update_exec;
  ot->poll = layered_source_cache_update_poll;

  /* flags */
  /* No #OPTYPE_UNDO: the operator only starts a background job, and the one edit it can make
   * before that lands (releasing a cache the graph no longer fills) is cache bookkeeping of the
   * same kind the debounced path does outside any undo step -- the user's graph and maps are
   * never touched. */
  ot->flag = OPTYPE_REGISTER;
}

/** \} */

}  // namespace blender
