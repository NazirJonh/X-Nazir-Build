/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Operators managing the mesh attributes painted by Material Paint (Poly Paint). They work in
 * Object and Sculpt mode, not only in Edit Mode.
 */

#include <string>

#include "BKE_attribute.h"
#include "BKE_attribute.hh"
#include "BKE_context.hh"
#include "BKE_mesh.hh"
#include "BKE_paint.hh"
#include "BKE_report.hh"

#include "DEG_depsgraph.hh"

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_screen.hh"

#include "BLT_translation.hh"

#include "paint_material_attribute.hh" /* own include */

namespace blender::ed::sculpt_paint {

/* -------------------------------------------------------------------- */
/** \name Material Attribute Operators (Poly Paint)
 * \{ */

/**
 * Resolves the attribute name the operator should act on.
 *
 * The fixed channels take their name from the channel descriptor table, so callers (and the UI)
 * never have to repeat the names. Only the Custom channel reads the `name` property.
 */
static std::string material_attribute_name_get(bContext *C, wmOperator *op)
{
  const eMaterialPaintChannel channel = eMaterialPaintChannel(RNA_enum_get(op->ptr, "channel"));
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(channel);
  if (info.attribute_name != nullptr) {
    return info.attribute_name;
  }

  char name[MAX_NAME];
  RNA_string_get(op->ptr, "name", name);
  if (name[0] != '\0') {
    return name;
  }

  /* Fall back to the name configured for the scene, which is what painting will use. */
  const Scene *scene = CTX_data_scene(C);
  if (scene && scene->toolsettings) {
    return BKE_paint_material_channel_attribute_name(scene->toolsettings->paint_mode, channel);
  }
  return "";
}

static Mesh *material_attribute_mesh_get(bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  if (!ob || ob->type != OB_MESH) {
    return nullptr;
  }
  return id_cast<Mesh *>(ob->data);
}

static void material_attribute_changed_notify(bContext *C, Mesh &mesh)
{
  Object *ob = CTX_data_active_object(C);
  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, &mesh);
}

static wmOperatorStatus material_attribute_add_exec(bContext *C, wmOperator *op)
{
  Mesh *mesh = material_attribute_mesh_get(C);
  if (mesh == nullptr) {
    return OPERATOR_CANCELLED;
  }

  const eMaterialPaintChannel channel = eMaterialPaintChannel(RNA_enum_get(op->ptr, "channel"));
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(channel);

  if (!info.supports_vertex_paint) {
    /* Normal/Height/Emission are texture-map-only channels: creating a vertex attribute for them
     * would just leave dead data the vertex canvas never writes. */
    BKE_reportf(op->reports,
               RPT_ERROR,
               "%s channel has no per-vertex storage",
               IFACE_(info.ui_name));
    return OPERATOR_CANCELLED;
  }

  const std::string attr_name = material_attribute_name_get(C, op);
  bool created = false;
  const MaterialPaintAttributeStatus status =
      info.is_color ? BKE_paint_mesh_material_color_attribute_ensure(*mesh, channel, &created) :
                      BKE_paint_mesh_material_attribute_ensure(*mesh, attr_name, &created);

  if (status != MaterialPaintAttributeStatus::Ok) {
    BKE_reportf(op->reports,
                RPT_ERROR,
                "%s: %s",
                attr_name.c_str(),
                TIP_(BKE_paint_material_attribute_status_message(status)));
    return OPERATOR_CANCELLED;
  }

  if (!created) {
    BKE_reportf(op->reports, RPT_INFO, "Attribute '%s' already exists", attr_name.c_str());
    return OPERATOR_CANCELLED;
  }

  material_attribute_changed_notify(C, *mesh);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus material_attribute_add_invoke(bContext *C,
                                                      wmOperator *op,
                                                      const wmEvent *event)
{
  /* The Custom channel needs a name; the fixed channels know theirs and can run directly. */
  const eMaterialPaintChannel channel = eMaterialPaintChannel(RNA_enum_get(op->ptr, "channel"));
  if (BKE_paint_material_channel_info(channel).attribute_name == nullptr &&
      !RNA_struct_property_is_set(op->ptr, "name"))
  {
    return WM_operator_props_popup_confirm(C, op, event);
  }
  return material_attribute_add_exec(C, op);
}

static wmOperatorStatus material_attribute_remove_exec(bContext *C, wmOperator *op)
{
  Mesh *mesh = material_attribute_mesh_get(C);
  if (mesh == nullptr) {
    return OPERATOR_CANCELLED;
  }

  const eMaterialPaintChannel channel = eMaterialPaintChannel(RNA_enum_get(op->ptr, "channel"));
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(channel);

  const std::string attr_name = material_attribute_name_get(C, op);
  if (attr_name.empty()) {
    BKE_report(op->reports, RPT_ERROR, "No attribute name given");
    return OPERATOR_CANCELLED;
  }

  bke::MutableAttributeAccessor attrs = mesh->attributes_for_write();
  const std::optional<bke::AttributeMetaData> meta_data = attrs.lookup_meta_data(attr_name);
  if (!meta_data) {
    BKE_reportf(op->reports, RPT_ERROR, "Attribute '%s' does not exist", attr_name.c_str());
    return OPERATOR_CANCELLED;
  }

  /* Refuse to remove an attribute that does not match what this channel would have created:
   * a Custom name colliding with an unrelated geometry-nodes attribute, or a fixed channel whose
   * attribute was repurposed to a different type, must not be silently deleted by this operator. */
  const bool domain_ok = info.is_color ? ELEM(meta_data->domain,
                                             bke::AttrDomain::Point,
                                             bke::AttrDomain::Corner) :
                                         meta_data->domain == bke::AttrDomain::Point;
  const bool type_ok = info.is_color ?
                           ELEM(meta_data->data_type,
                                bke::AttrType::ColorFloat,
                                bke::AttrType::ColorByte) :
                           meta_data->data_type == bke::AttrType::Float;
  if (!domain_ok || !type_ok) {
    BKE_reportf(op->reports,
               RPT_ERROR,
               "Attribute '%s' is not a %s material paint attribute",
               attr_name.c_str(),
               info.is_color ? "color" : "scalar");
    return OPERATOR_CANCELLED;
  }

  /* Removing the active/default color attribute out from under the mesh would leave those
   * references dangling; clear them first the same way #BKE_id_attributes_active_color_set
   * updates them, rather than leaving stale names Undo and the draw engines have to guard
   * against. */
  if (info.is_color) {
    if (BKE_id_attributes_active_color_name(&mesh->id) == StringRef(attr_name)) {
      BKE_id_attributes_active_color_clear(&mesh->id);
    }
    if (BKE_id_attributes_default_color_name(&mesh->id) == StringRef(attr_name)) {
      BKE_id_attributes_default_color_set(&mesh->id, std::nullopt);
    }
  }

  if (!attrs.remove(attr_name)) {
    BKE_reportf(op->reports, RPT_ERROR, "Failed to remove attribute '%s'", attr_name.c_str());
    return OPERATOR_CANCELLED;
  }

  material_attribute_changed_notify(C, *mesh);
  return OPERATOR_FINISHED;
}

/** \} */

}  // namespace blender::ed::sculpt_paint

void blender::ed::sculpt_paint::PAINT_OT_material_attribute_add(wmOperatorType *ot)
{
  using namespace blender::ed::sculpt_paint;
  ot->name = "Add Material Attribute";
  ot->idname = "PAINT_OT_material_attribute_add";
  ot->description = "Add the mesh attribute a material paint channel is stored in";

  ot->exec = material_attribute_add_exec;
  ot->invoke = material_attribute_add_invoke;
  ot->poll = ED_operator_object_active_editable_mesh;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ot->prop = RNA_def_enum(ot->srna,
                          "channel",
                          rna_enum_material_paint_channel_items,
                          PAINT_MATERIAL_CHANNEL_METALLIC,
                          "Channel",
                          "Material paint channel to add the attribute for");
  RNA_def_string(ot->srna,
                 "name",
                 nullptr,
                 MAX_NAME,
                 "Name",
                 "Attribute name, used by the Custom channel only");
}

void blender::ed::sculpt_paint::PAINT_OT_material_attribute_remove(wmOperatorType *ot)
{
  using namespace blender::ed::sculpt_paint;
  ot->name = "Remove Material Attribute";
  ot->idname = "PAINT_OT_material_attribute_remove";
  ot->description = "Remove the mesh attribute a material paint channel is stored in";

  ot->exec = material_attribute_remove_exec;
  ot->poll = ED_operator_object_active_editable_mesh;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ot->prop = RNA_def_enum(ot->srna,
                          "channel",
                          rna_enum_material_paint_channel_items,
                          PAINT_MATERIAL_CHANNEL_METALLIC,
                          "Channel",
                          "Material paint channel to remove the attribute of");
  RNA_def_string(ot->srna,
                 "name",
                 nullptr,
                 MAX_NAME,
                 "Name",
                 "Attribute name, used by the Custom channel only");
}
