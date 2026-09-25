/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup RNA
 */

#include <cfloat>
#include <climits>
#include <cstdlib>
#include <string>

#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_scene_types.h"

#include "BLI_listbase.h"
#include "BLI_math_rotation.h"
#include "BLI_rect.h"
#include "BLI_string.h"

#include "BLT_translation.hh"

#include "BKE_customdata.hh"

#include "RNA_define.hh"
#include "RNA_enum_types.hh"
#include "RNA_prototypes.hh"

#include "rna_internal.hh"

#include "WM_api.hh"
#include "WM_types.hh"

namespace blender {

const EnumPropertyItem rna_enum_ramp_blend_items[] = {
    {MA_RAMP_BLEND, "MIX", 0, "Mix", ""},
    RNA_ENUM_ITEM_SEPR,
    {MA_RAMP_DARK, "DARKEN", 0, "Darken", ""},
    {MA_RAMP_MULT, "MULTIPLY", 0, "Multiply", ""},
    {MA_RAMP_BURN, "BURN", 0, "Color Burn", ""},
    RNA_ENUM_ITEM_SEPR,
    {MA_RAMP_LIGHT, "LIGHTEN", 0, "Lighten", ""},
    {MA_RAMP_SCREEN, "SCREEN", 0, "Screen", ""},
    {MA_RAMP_DODGE, "DODGE", 0, "Color Dodge", ""},
    {MA_RAMP_ADD, "ADD", 0, "Add", ""},
    RNA_ENUM_ITEM_SEPR,
    {MA_RAMP_OVERLAY, "OVERLAY", 0, "Overlay", ""},
    {MA_RAMP_SOFT, "SOFT_LIGHT", 0, "Soft Light", ""},
    {MA_RAMP_LINEAR, "LINEAR_LIGHT", 0, "Linear Light", ""},
    RNA_ENUM_ITEM_SEPR,
    {MA_RAMP_DIFF, "DIFFERENCE", 0, "Difference", ""},
    {MA_RAMP_EXCLUSION, "EXCLUSION", 0, "Exclusion", ""},
    {MA_RAMP_SUB, "SUBTRACT", 0, "Subtract", ""},
    {MA_RAMP_DIV, "DIVIDE", 0, "Divide", ""},
    RNA_ENUM_ITEM_SEPR,
    {MA_RAMP_HUE, "HUE", 0, "Hue", ""},
    {MA_RAMP_SAT, "SATURATION", 0, "Saturation", ""},
    {MA_RAMP_COLOR, "COLOR", 0, "Color", ""},
    {MA_RAMP_VAL, "VALUE", 0, "Value", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

}

#ifdef RNA_RUNTIME

#  include "MEM_guardedalloc.h"

#  include "DNA_gpencil_legacy_types.h"
#  include "DNA_image_types.h"
#  include "DNA_meshdata_types.h"
#  include "DNA_node_types.h"
#  include "DNA_object_types.h"
#  include "DNA_screen_types.h"
#  include "DNA_space_types.h"

#  include "BLI_listbase.h"
#  include "BLI_string_utf8.h"
#  include "BLI_index_range.hh"
#  include "BLI_math_vector.h"
#  include "BLI_span.hh"
#  include "BLI_uuid.h"

#  include "BKE_attribute.h"
#  include "BKE_attribute.hh"
#  include "BKE_colorband.hh"
#  include "BKE_context.hh"
#  include "BKE_editmesh.hh"
#  include "BKE_gpencil_legacy.h"
#  include "BKE_grease_pencil.hh"
#  include "BKE_lib_id.hh"
#  include "BKE_main.hh"
#  include "BKE_material.hh"
#  include "BKE_mesh.hh"
#  include "BKE_mesh_types.hh"
#  include "BKE_node.hh"
#  include "BKE_paint.hh"
#  include "BKE_mesh_maps.hh"
#  include "BKE_paint_layers.hh"
#  include "BKE_paint_layers_composite.hh"
#  include "BKE_paint_layers_generate.hh"
#  include "BKE_report.hh"
#  include "BKE_scene.hh"
#  include "BKE_texture.h"
#  include "BKE_workspace.hh"

#  include "DEG_depsgraph.hh"
#  include "DEG_depsgraph_build.hh"

#  include "ED_gpencil_legacy.hh"
#  include "ED_image.hh"
#  include "ED_node.hh"
#  include "ED_screen.hh"

namespace blender {

static void rna_Material_update(Main * /*bmain*/, Scene * /*scene*/, PointerRNA *ptr)
{
  Material *ma = id_cast<Material *>(ptr->owner_id);

  DEG_id_tag_update(&ma->id, ID_RECALC_SHADING);
  WM_main_add_notifier(NC_MATERIAL | ND_SHADING, ma);
}

static void rna_Material_update_previews(Main * /*bmain*/, Scene * /*scene*/, PointerRNA *ptr)
{
  Material *ma = id_cast<Material *>(ptr->owner_id);

  WM_main_add_notifier(NC_MATERIAL | ND_SHADING_PREVIEW, ma);
}

static void rna_MaterialGpencil_update(Main *bmain, Scene *scene, PointerRNA *ptr)
{
  Material *ma = id_cast<Material *>(ptr->owner_id);
  rna_Material_update(bmain, scene, ptr);

  /* Need set all caches as dirty. */
  for (Object *ob = static_cast<Object *>(bmain->objects.first); ob;
       ob = static_cast<Object *>(ob->id.next))
  {
    if (ob->type == OB_GREASE_PENCIL) {
      GreasePencil &grease_pencil = *id_cast<GreasePencil *>(ob->data);
      DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
    }
  }

  WM_main_add_notifier(NC_GPENCIL | ND_DATA, ma);
}

static void rna_MaterialLineArt_update(Main * /*bmain*/, Scene * /*scene*/, PointerRNA *ptr)
{
  Material *ma = id_cast<Material *>(ptr->owner_id);
  /* Need to tag geometry for line art modifier updates. */
  DEG_id_tag_update(&ma->id, ID_RECALC_GEOMETRY);
  WM_main_add_notifier(NC_MATERIAL | ND_SHADING_DRAW, ma);
}

static std::optional<std::string> rna_MaterialLineArt_path(const PointerRNA * /*ptr*/)
{
  return "lineart";
}

static void rna_Material_draw_update(Main * /*bmain*/, Scene * /*scene*/, PointerRNA *ptr)
{
  Material *ma = id_cast<Material *>(ptr->owner_id);

  DEG_id_tag_update(&ma->id, ID_RECALC_SHADING);
  WM_main_add_notifier(NC_MATERIAL | ND_SHADING_DRAW, ma);
}

static void rna_Material_texpaint_begin(CollectionPropertyIterator *iter, PointerRNA *ptr)
{
  Material *ma = static_cast<Material *>(ptr->data);
  rna_iterator_array_begin(iter,
                           ptr,
                           static_cast<void *>(ma->texpaintslot),
                           sizeof(TexPaintSlot),
                           ma->tot_slots,
                           0,
                           nullptr);
}

static void rna_Material_active_paint_texture_index_update(bContext *C, PointerRNA *ptr)
{
  Main *bmain = CTX_data_main(C);
  Material *ma = id_cast<Material *>(ptr->owner_id);

  if (paint_layers_is_layered(*ma)) {
    /* The slot index maps to a channel through a fixed table (an empty slot still names its
     * channel), so the chosen entry drives the stroke's channel. */
    Scene *scene = CTX_data_scene(C);
    if (scene != nullptr) {
      PaintModeSettings &paint_mode = scene->toolsettings->paint_mode;
      const int slot_index = ma->paint_active_slot;
      const int channel = BKE_paint_layers_texpaint_slot_channel(&paint_mode, slot_index);
      if (channel >= 0) {
        paint_mode.active_layer_channel = channel;
      }
      if (ma->texpaintslot != nullptr && slot_index >= 0 && slot_index < ma->tot_slots) {
        Image *image = ma->texpaintslot[slot_index].ima;
        if (image != nullptr) {
          ED_space_image_sync(bmain, image, false);
        }
      }
    }
    return;
  }

  if (ma->nodetree) {
    std::pair<bNodeTree *, bNode *> found = BKE_texpaint_slot_material_find_node(
        ma, ma->paint_active_slot);

    if (found.second) {
      BLI_assert(found.first != nullptr);
      bke::node_set_active(*found.first, *found.second);
      /* Tag nodetree for viewport update (if node is found in a nested group). */
      if (ma->nodetree != found.first) {
        DEG_id_tag_update(&found.first->id, ID_RECALC_SYNC_TO_EVAL);
      }
    }
  }

  if (ma->texpaintslot && (ma->tot_slots > ma->paint_active_slot)) {
    TexPaintSlot *slot = &ma->texpaintslot[ma->paint_active_slot];
    Image *image = slot->ima;
    if (image) {
      ED_space_image_sync(bmain, image, false);
    }

    /* For compatibility reasons with vertex paint we activate the color attribute. */
    if (const char *name = slot->attribute_name) {
      Object *ob = CTX_data_active_object(C);
      if (ob != nullptr && ob->type == OB_MESH) {
        Mesh *mesh = id_cast<Mesh *>(ob->data);
        if (mesh->runtime->edit_mesh) {
          if (const BMDataLayerLookup attr = BM_data_layer_lookup(*mesh->runtime->edit_mesh->bm,
                                                                  name))
          {
            BKE_id_attributes_active_color_set(&mesh->id, name);
          }
        }
        else {
          const bke::AttributeAccessor attributes = mesh->attributes();
          if (bke::mesh::is_color_attribute(attributes.lookup_meta_data(name))) {
            BKE_id_attributes_active_color_set(&mesh->id, name);
          }
        }
        DEG_id_tag_update(&ob->id, 0);
        WM_main_add_notifier(NC_GEOM | ND_DATA, &ob->id);
      }
    }
  }

  DEG_id_tag_update(&ma->id, 0);
  WM_main_add_notifier(NC_MATERIAL | ND_SHADING, ma);
}

static int rna_Material_blend_method_get(PointerRNA *ptr)
{
  Material *material = id_cast<Material *>(ptr->owner_id);
  switch (material->surface_render_method) {
    case MA_SURFACE_METHOD_DEFERRED:
      return MA_BM_HASHED;
    case MA_SURFACE_METHOD_FORWARD:
      return MA_BM_BLEND;
  }
  return MA_BM_HASHED;
}

static void rna_Material_blend_method_set(PointerRNA *ptr, int new_blend_method)
{
  Material *material = id_cast<Material *>(ptr->owner_id);
  switch (new_blend_method) {
    case MA_BM_SOLID:
    case MA_BM_CLIP:
    case MA_BM_HASHED:
      material->surface_render_method = MA_SURFACE_METHOD_DEFERRED;
      break;
    case MA_BM_BLEND:
      material->surface_render_method = MA_SURFACE_METHOD_FORWARD;
      break;
  }
}

static void rna_Material_render_method_set(PointerRNA *ptr, int new_render_method)
{
  Material *material = id_cast<Material *>(ptr->owner_id);
  material->surface_render_method = eMaterial_SurfaceRenderMethod(new_render_method);

  /* Still sets the legacy property for forward compatibility. */
  switch (new_render_method) {
    case MA_SURFACE_METHOD_DEFERRED:
      material->blend_method = MA_BM_HASHED;
      break;
    case MA_SURFACE_METHOD_FORWARD:
      material->blend_method = MA_BM_BLEND;
      break;
  }
}
static void rna_Material_transparent_shadow_set(PointerRNA *ptr, bool new_value)
{
  Material *material = id_cast<Material *>(ptr->owner_id);
  SET_FLAG_FROM_TEST(material->blend_flag, new_value, MA_BL_TRANSPARENT_SHADOW);
  /* Still sets the legacy property for forward compatibility. */
  material->blend_shadow = new_value ? MA_BS_HASHED : MA_BS_SOLID;
}

static bool rna_Material_use_nodes_get(PointerRNA * /*ptr*/)
{
  /* #use_nodes is deprecated. All materials now use nodes. */
  return true;
}

static void rna_Material_use_nodes_set(PointerRNA * /*ptr*/, bool /*new_value*/)
{
  /* #use_nodes is deprecated. Setting the property has no effect.
   * Note: Users will get a warning through the RNA deprecation warning, so no need to log a
   * warning here. */
  return;
}

MTex *rna_mtex_texture_slots_add(ID *self_id, bContext *C, ReportList *reports)
{
  MTex *mtex = BKE_texture_mtex_add_id(self_id, -1);
  if (mtex == nullptr) {
    BKE_reportf(reports, RPT_ERROR, "Maximum number of textures added %d", MAX_MTEX);
    return nullptr;
  }

  /* for redraw only */
  WM_event_add_notifier(C, NC_TEXTURE, CTX_data_scene(C));

  return mtex;
}

MTex *rna_mtex_texture_slots_create(ID *self_id, bContext *C, ReportList *reports, int index)
{
  MTex *mtex;

  if (index < 0 || index >= MAX_MTEX) {
    BKE_reportf(reports, RPT_ERROR, "Index %d is invalid", index);
    return nullptr;
  }

  mtex = BKE_texture_mtex_add_id(self_id, index);

  /* for redraw only */
  WM_event_add_notifier(C, NC_TEXTURE, CTX_data_scene(C));

  return mtex;
}

void rna_mtex_texture_slots_clear(ID *self_id, bContext *C, ReportList *reports, int index)
{
  MTex **mtex_ar;
  short act;

  give_active_mtex(self_id, &mtex_ar, &act);

  if (mtex_ar == nullptr) {
    BKE_report(reports, RPT_ERROR, "Mtex not found for this type");
    return;
  }

  if (index < 0 || index >= MAX_MTEX) {
    BKE_reportf(reports, RPT_ERROR, "Index %d is invalid", index);
    return;
  }

  if (mtex_ar[index]) {
    id_us_min(id_cast<ID *>(mtex_ar[index]->tex));
    MEM_delete(mtex_ar[index]);
    mtex_ar[index] = nullptr;
    DEG_id_tag_update(self_id, 0);
  }

  /* for redraw only */
  WM_event_add_notifier(C, NC_TEXTURE, CTX_data_scene(C));
}

static void rna_TexPaintSlot_uv_layer_get(PointerRNA *ptr, char *value)
{
  TexPaintSlot *data = static_cast<TexPaintSlot *>(ptr->data);

  if (data->uvname != nullptr) {
    strcpy(value, data->uvname);
  }
  else {
    value[0] = '\0';
  }
}

static int rna_TexPaintSlot_uv_layer_length(PointerRNA *ptr)
{
  TexPaintSlot *data = static_cast<TexPaintSlot *>(ptr->data);
  return data->uvname == nullptr ? 0 : strlen(data->uvname);
}

static void rna_TexPaintSlot_uv_layer_set(PointerRNA *ptr, const char *value)
{
  TexPaintSlot *data = static_cast<TexPaintSlot *>(ptr->data);

  if (data->uvname != nullptr) {
    BLI_strncpy_utf8(data->uvname, value, MAX_CUSTOMDATA_LAYER_NAME_NO_PREFIX);
  }
}

/**
 * The name of an empty slot of a *layered* material: the channel the slot will paint, since the
 * slot table is fixed and an empty slot has no image to name. False for every other case -- a
 * non-layered material, or a slot that has an image or an attribute to name itself by.
 */
static bool rna_TexPaintSlot_layered_empty_name(PointerRNA *ptr, std::string &r_name)
{
  TexPaintSlot *data = static_cast<TexPaintSlot *>(ptr->data);
  if (data->ima != nullptr || data->attribute_name != nullptr) {
    return false;
  }
  Material *ma = id_cast<Material *>(ptr->owner_id);
  if (ma == nullptr || !paint_layers_is_layered(*ma) || ma->texpaintslot == nullptr) {
    return false;
  }
  const int slot_index = int(data - ma->texpaintslot);
  const char *channel_name = nullptr;
  if (ma->tot_slots == 1) {
    /* The single slot a MASK-mode layered material has. */
    channel_name = IFACE_("Mask");
  }
  else {
    const int channel = BKE_paint_layers_texpaint_slot_channel_content(slot_index);
    if (channel >= 0) {
      channel_name = IFACE_(BKE_paint_material_channel_info(eMaterialPaintChannel(channel)).ui_name);
    }
  }
  if (channel_name == nullptr) {
    return false;
  }
  r_name = std::string(channel_name) + " \xe2\x80\x94 no map";
  return true;
}

static void rna_TexPaintSlot_name_get(PointerRNA *ptr, char *value)
{
  TexPaintSlot *data = static_cast<TexPaintSlot *>(ptr->data);

  if (data->ima != nullptr) {
    strcpy(value, data->ima->id.name + 2);
    return;
  }

  if (data->attribute_name != nullptr) {
    strcpy(value, data->attribute_name);
    return;
  }

  std::string empty_name;
  if (rna_TexPaintSlot_layered_empty_name(ptr, empty_name)) {
    BLI_strncpy(value, empty_name.c_str(), MAX_NAME);
    return;
  }

  value[0] = '\0';
}

static int rna_TexPaintSlot_name_length(PointerRNA *ptr)
{
  TexPaintSlot *data = static_cast<TexPaintSlot *>(ptr->data);
  if (data->ima != nullptr) {
    return strlen(data->ima->id.name) - 2;
  }
  if (data->attribute_name != nullptr) {
    return strlen(data->attribute_name);
  }

  std::string empty_name;
  if (rna_TexPaintSlot_layered_empty_name(ptr, empty_name)) {
    return int(empty_name.size());
  }

  return 0;
}

static int rna_TexPaintSlot_icon_get(PointerRNA *ptr)
{
  TexPaintSlot *data = static_cast<TexPaintSlot *>(ptr->data);
  if (data->ima != nullptr) {
    return ICON_IMAGE;
  }
  if (data->attribute_name != nullptr) {
    return ICON_COLOR;
  }

  return ICON_NONE;
}

static bool rna_is_grease_pencil_get(PointerRNA *ptr)
{
  Material *ma = static_cast<Material *>(ptr->data);
  if (ma->gp_style != nullptr) {
    return true;
  }

  return false;
}

static std::optional<std::string> rna_GpencilColorData_path(const PointerRNA * /*ptr*/)
{
  return "grease_pencil";
}

static bool rna_GpencilColorData_is_stroke_visible_get(PointerRNA *ptr)
{
  MaterialGPencilStyle *pcolor = static_cast<MaterialGPencilStyle *>(ptr->data);
  return (pcolor->stroke_rgba[3] > GPENCIL_ALPHA_OPACITY_THRESH);
}

static bool rna_GpencilColorData_is_fill_visible_get(PointerRNA *ptr)
{
  MaterialGPencilStyle *pcolor = static_cast<MaterialGPencilStyle *>(ptr->data);
  return ((pcolor->fill_rgba[3] > GPENCIL_ALPHA_OPACITY_THRESH) || (pcolor->fill_style > 0));
}

static void rna_GpencilColorData_stroke_image_set(PointerRNA *ptr,
                                                  PointerRNA value,
                                                  ReportList * /*reports*/)
{
  MaterialGPencilStyle *pcolor = static_cast<MaterialGPencilStyle *>(ptr->data);
  ID *id = static_cast<ID *>(value.data);

  id_us_plus(id);
  pcolor->sima = id_cast<Image *>(id);
}

static void rna_GpencilColorData_fill_image_set(PointerRNA *ptr,
                                                PointerRNA value,
                                                ReportList * /*reports*/)
{
  MaterialGPencilStyle *pcolor = static_cast<MaterialGPencilStyle *>(ptr->data);
  ID *id = static_cast<ID *>(value.data);

  id_us_plus(id);
  pcolor->ima = id_cast<Image *>(id);
}

/* -------------------------------------------------------------------- */
/** \name Paint Layer
 * \{ */

/** The collection iterates the top level of the stack; nested folders are reached through find. */
static void rna_Material_paint_layers_begin(CollectionPropertyIterator *iter, PointerRNA *ptr)
{
  Material *ma = static_cast<Material *>(ptr->data);
  rna_iterator_listbase_begin(iter, ptr, &ma->paint_layers, nullptr);
}

static PointerRNA rna_Material_paint_layers_active_get(PointerRNA *ptr)
{
  Material *ma = static_cast<Material *>(ptr->data);
  const bUUID marker = BKE_paint_layers_active_get(*ma);
  if (BLI_uuid_is_nil(marker)) {
    return PointerRNA_NULL;
  }
  MaterialPaintLayer *layer = BKE_paint_layers_find(*ma, marker);
  if (layer == nullptr) {
    return PointerRNA_NULL;
  }
  return RNA_pointer_create_with_parent(*ptr, RNA_MaterialPaintLayer, layer);
}

static void rna_Material_paint_layers_active_set(PointerRNA *ptr,
                                                 PointerRNA value,
                                                 ReportList *reports)
{
  Material *ma = static_cast<Material *>(ptr->data);
  if (value.data == nullptr) {
    BKE_paint_layers_active_set(*ma, {});
    return;
  }
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(value.data);
  if (BKE_paint_layers_find(*ma, layer->marker) != layer) {
    BKE_reportf(reports,
                RPT_ERROR,
                "Material '%s' does not contain the given paint layer",
                ma->id.name + 2);
    return;
  }
  BKE_paint_layers_active_set(*ma, layer->marker);
}

static MaterialPaintLayer *rna_Material_paint_layers_new(Material *ma,
                                                         int source,
                                                         const char *name)
{
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, eMaterialPaintLayerSource(source), name, nullptr, PaintLayerPlace::Above);
  if (layer != nullptr) {
    /* The default channel set a freshly authored Paint or Fill row takes part in. */
    BKE_paint_layers_default_channels_apply(*ma, *layer);
    /* A fresh row becomes the cursor, mirroring what the UI does when it adds one. */
    BKE_paint_layers_active_set(*ma, layer->marker);
    /* BKE only tags DEG; the Outliner and the Layer Material tab need the WM notifier too. */
    WM_main_add_notifier(NC_MATERIAL | ND_SHADING, &ma->id);
  }
  return layer;
}

static void rna_Material_paint_layers_remove(Material *ma,
                                             ReportList *reports,
                                             PointerRNA *layer_ptr)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(layer_ptr->data);
  if (!BKE_paint_layers_remove(*ma, layer)) {
    BKE_reportf(reports,
                RPT_ERROR,
                "Material '%s' does not contain the given paint layer",
                ma->id.name + 2);
    return;
  }
  layer_ptr->invalidate();
  WM_main_add_notifier(NC_MATERIAL | ND_SHADING, &ma->id);
}

static MaterialPaintLayer *rna_Material_paint_layers_find(Material *ma, const char *marker)
{
  bUUID uuid;
  if (!BLI_uuid_parse_string(&uuid, marker)) {
    return nullptr;
  }
  return BKE_paint_layers_find(*ma, uuid);
}

static bool rna_Material_paint_layers_composite(Material *ma,
                                                ReportList *reports,
                                                int channel,
                                                PointerRNA *image_ptr)
{
  Image *image = static_cast<Image *>(image_ptr->data);
  if (image == nullptr) {
    BKE_report(reports, RPT_ERROR, "No destination image");
    return false;
  }
  return BKE_paint_layers_composite_image(*ma, channel, *image, nullptr, reports);
}

static void rna_Material_paint_layers_regenerate(Material *ma, Main *bmain)
{
  if (bmain == nullptr || ma == nullptr) {
    return;
  }
  BKE_paint_layers_regenerate(*bmain, *ma);
}

static bool rna_Material_is_layered_get(PointerRNA *ptr)
{
  return paint_layers_is_layered(*id_cast<Material *>(ptr->owner_id));
}

static bool rna_Material_paint_layers_tree_is_stale_get(PointerRNA *ptr)
{
  Material *ma = id_cast<Material *>(ptr->owner_id);
  if (!paint_layers_is_layered(*ma)) {
    return false;
  }
  /* The generated tree is out of step when it is missing altogether or the description asked for
   * a rebuild; an unlocked material can sit in this state until the user regenerates. */
  return ma->paint_layers_tree == nullptr ||
         (ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN) != 0;
}

static bool rna_Material_paint_layers_locked_get(PointerRNA *ptr)
{
  return (id_cast<Material *>(ptr->owner_id)->paint_layers_flag & MA_PAINT_LAYERS_LOCKED) != 0;
}

static void rna_Material_paint_layers_locked_set(PointerRNA *ptr, bool value)
{
  Material *ma = id_cast<Material *>(ptr->owner_id);
  SET_FLAG_FROM_TEST(ma->paint_layers_flag, value, MA_PAINT_LAYERS_LOCKED);
  /* Locking hands the tree back to the generator, which rebuilds it and overwrites whatever manual
   * edits were made. Unlocking only stops the generator: the tree is left as it is until the user
   * asks for a Regenerate. */
  if (value) {
    ma->paint_layers_flag |= MA_PAINT_LAYERS_REGEN;
  }
  DEG_id_tag_update(&ma->id, ID_RECALC_SHADING);
}

/** Every layer-pointer setter goes back to the material that owns the row, which the pointer's
 * #owner_id carries however deep the row is nested. */
static Material *rna_paint_layer_material(PointerRNA *ptr, MaterialPaintLayer *layer)
{
  Material *ma = reinterpret_cast<Material *>(ptr->owner_id);
  if (ma == nullptr || BKE_paint_layers_find(*ma, layer->marker) != layer) {
    return nullptr;
  }
  return ma;
}

static void rna_Material_paint_layers_move(Material *ma,
                                           ReportList *reports,
                                           PointerRNA *layer_ptr,
                                           PointerRNA *anchor_ptr,
                                           int place)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(layer_ptr->data);
  MaterialPaintLayer *anchor = static_cast<MaterialPaintLayer *>(anchor_ptr->data);
  if (!BKE_paint_layers_move(*ma, layer, anchor, PaintLayerPlace(place))) {
    BKE_reportf(reports,
                RPT_ERROR,
                "Cannot move paint layer '%s' relative to '%s'",
                layer->name,
                anchor != nullptr ? anchor->name : "the top of the stack");
  }
}

static void rna_Material_paint_layers_reorder(Material *ma,
                                              ReportList *reports,
                                              PointerRNA *layer_ptr,
                                              int index)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(layer_ptr->data);
  if (!BKE_paint_layers_reorder(*ma, layer, index)) {
    BKE_reportf(reports, RPT_ERROR, "Cannot reorder paint layer '%s'", layer->name);
  }
}

static MaterialPaintLayer *rna_Material_paint_layers_duplicate(Material *ma,
                                                               Main *bmain,
                                                               ReportList *reports,
                                                               PointerRNA *layer_ptr)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(layer_ptr->data);
  MaterialPaintLayer *copy = BKE_paint_layers_duplicate(*bmain, *ma, layer);
  if (copy == nullptr) {
    BKE_reportf(reports, RPT_ERROR, "Cannot duplicate paint layer '%s'", layer->name);
    return nullptr;
  }
  return copy;
}

/**
 * RNA carries no list arguments, so a group is named by its members pairwise: the first row plus
 * an optional second. Larger selections are grouped by the UI operator, which calls the BKE API
 * directly with the full span.
 */
static MaterialPaintLayer *rna_Material_paint_layers_group(Material *ma,
                                                           ReportList *reports,
                                                           PointerRNA *layer_ptr,
                                                           PointerRNA *with_ptr)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(layer_ptr->data);
  MaterialPaintLayer *with = static_cast<MaterialPaintLayer *>(with_ptr->data);
  MaterialPaintLayer *folder = BKE_paint_layers_group(*ma, with != nullptr ?
                                                               Span<MaterialPaintLayer *>{layer,
                                                                                          with} :
                                                               Span<MaterialPaintLayer *>{layer});
  if (folder == nullptr) {
    BKE_report(reports, RPT_ERROR, "Cannot group paint layers of different lists");
    return nullptr;
  }
  return folder;
}

static void rna_Material_paint_layers_ungroup(Material *ma,
                                              ReportList *reports,
                                              PointerRNA *folder_ptr)
{
  MaterialPaintLayer *folder = static_cast<MaterialPaintLayer *>(folder_ptr->data);
  if (!BKE_paint_layers_ungroup(*ma, folder)) {
    BKE_report(reports, RPT_ERROR, "Cannot ungroup this paint layer");
    return;
  }
  folder_ptr->invalidate();
}

static void rna_MaterialPaintLayer_name_set(PointerRNA *ptr, const char *value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    BKE_paint_layers_rename(*ma, layer, value);
  }
}

static int rna_MaterialPaintLayer_blend_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return layer->blend;
}

static void rna_MaterialPaintLayer_blend_set(PointerRNA *ptr, int value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    BKE_paint_layers_set_blend(*ma, layer, eMaterialPaintLayerBlend(value));
  }
}

static float rna_MaterialPaintLayer_opacity_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return layer->opacity * 100.0f;
}

static void rna_MaterialPaintLayer_opacity_set(PointerRNA *ptr, float value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    BKE_paint_layers_set_opacity(*ma, layer, value / 100.0f);
  }
}

static void rna_MaterialPaintLayer_fill_color_get(PointerRNA *ptr, float *value)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  copy_v4_v4(value, layer->fill_color);
}

static void rna_MaterialPaintLayer_fill_color_set(PointerRNA *ptr, const float *value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    BKE_paint_layers_set_fill_color(*ma, layer, value);
  }
}

static int rna_MaterialPaintLayer_color_tag_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return layer->color_tag;
}

static void rna_MaterialPaintLayer_color_tag_set(PointerRNA *ptr, int value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    BKE_paint_layers_set_color_tag(*ma, layer, int8_t(value));
  }
}

static bool rna_MaterialPaintLayer_enabled_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return (layer->flag & MA_PAINT_LAYER_ENABLED) != 0;
}

static void rna_MaterialPaintLayer_enabled_set(PointerRNA *ptr, bool value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    BKE_paint_layers_set_enabled(*ma, layer, value);
  }
}

static void rna_MaterialPaintLayer_custom_group_set(PointerRNA *ptr,
                                                    PointerRNA value,
                                                    ReportList * /*reports*/)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    BKE_paint_layers_set_custom_group(
        *ma, layer, static_cast<bNodeTree *>(value.data));
  }
}

static void rna_MaterialPaintLayer_material_set(PointerRNA *ptr,
                                               PointerRNA value,
                                               ReportList * /*reports*/)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    BKE_paint_layers_set_material(*ma, layer, static_cast<Material *>(value.data));
  }
}

static int rna_MaterialPaintLayer_source_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return layer->source;
}

/*
 * A stack Layer's source is changed by conversion, through #MaterialPaintLayer.source_change(),
 * which #BKE_paint_layers_source_change restricts to Image/Constant and to a Layer row -- never by
 * assigning this property. A correction's source, on the other hand, has always been a plain
 * assignment (the old `effect` property); #BKE_paint_layers_correction_source_set keeps that
 * shape. #rna_MaterialPaintLayer_source_editable reports the property as editable for a
 * correction only, so a Layer row's own widget greys out instead of silently doing nothing; the
 * setter still defers the actual restriction to the BKE call, which is what a script that bypasses
 * `RNA_property_editable()` actually runs into.
 */
static int rna_MaterialPaintLayer_source_editable(const PointerRNA *ptr, const char **r_info)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  if (BKE_paint_layers_role(*layer) == PaintLayerRole::Layer) {
    if (r_info) {
      *r_info = N_(
          "A stack layer's source is changed by conversion, through source_change(), not by "
          "assignment");
    }
    return 0;
  }
  return PROP_EDITABLE;
}

static void rna_MaterialPaintLayer_source_set(PointerRNA *ptr, int value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    BKE_paint_layers_correction_source_set(*ma, layer, value);
  }
}

static int rna_MaterialPaintLayer_mesh_map_type_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return layer->mesh_map_type;
}

static int rna_MaterialPaintLayer_mesh_map_type_editable(const PointerRNA *ptr,
                                                        const char **r_info)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  if (layer->source == MA_PAINT_LAYER_SOURCE_MESH_MAP) {
    return PROP_EDITABLE;
  }
  if (r_info) {
    *r_info = N_("Only a Mesh Map row has a map type; change its source first");
  }
  return 0;
}

static void rna_MaterialPaintLayer_mesh_map_type_set(PointerRNA *ptr, int value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    BKE_paint_layers_mesh_map_type_set(*ma, layer, int8_t(value));
  }
}

static int rna_MaterialPaintLayer_role_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return layer->role;
}

static void rna_MaterialPaintLayer_role_set(PointerRNA *ptr, int value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    BKE_paint_layers_role_set(*ma, layer, value);
  }
}

static Material *rna_MaterialPaintLayer_owner(PointerRNA ptr, MaterialPaintLayer **r_layer);

static MaterialPaintLayer *rna_MaterialPaintLayer_correction_add(PointerRNA ptr,
                                                                 ReportList *reports,
                                                                 int role,
                                                                 int source,
                                                                 const char *name)
{
  MaterialPaintLayer *layer = nullptr;
  if (Material *ma = rna_MaterialPaintLayer_owner(ptr, &layer)) {
    MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
        *ma, layer, role, source, name);
    if (correction != nullptr) {
      WM_main_add_notifier(NC_MATERIAL | ND_SHADING, &ma->id);
      return correction;
    }
  }
  BKE_report(reports, RPT_ERROR, "Cannot add a correction to this paint layer");
  return nullptr;
}

static void rna_MaterialPaintLayer_children_begin(CollectionPropertyIterator *iter,
                                                  PointerRNA *ptr)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  rna_iterator_listbase_begin(iter, ptr, &layer->children, nullptr);
}

static void rna_MaterialPaintLayer_effects_begin(CollectionPropertyIterator *iter,
                                                 PointerRNA *ptr)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  rna_iterator_listbase_begin(iter, ptr, &layer->effects, nullptr);
}

static void rna_MaterialPaintLayer_mask_stack_begin(CollectionPropertyIterator *iter,
                                                    PointerRNA *ptr)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  rna_iterator_listbase_begin(iter, ptr, &layer->mask_stack, nullptr);
}

/** The owning material of the layer a #FUNC_SELF_AS_RNA callback points at. */
static Material *rna_MaterialPaintLayer_owner(PointerRNA ptr, MaterialPaintLayer **r_layer)
{
  Material *ma = reinterpret_cast<Material *>(ptr.owner_id);
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr.data);
  if (ma == nullptr || layer == nullptr || BKE_paint_layers_find(*ma, layer->marker) != layer) {
    return nullptr;
  }
  *r_layer = layer;
  return ma;
}

static int rna_MaterialPaintLayer_bake_mode_get(PointerRNA *ptr)
{
  return BKE_paint_layers_bake_mode_get(
      *static_cast<const MaterialPaintLayer *>(ptr->data));
}

static void rna_MaterialPaintLayer_bake_mode_set(PointerRNA *ptr, int value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  Material *ma = rna_MaterialPaintLayer_owner(*ptr, &layer);
  if (ma != nullptr && BKE_paint_layers_bake_mode_set(*ma, *layer, value)) {
    WM_main_add_notifier(NC_MATERIAL | ND_SHADING, &ma->id);
  }
}

static int rna_MaterialPaintLayer_bake_size_get(PointerRNA *ptr)
{
  return BKE_paint_layers_bake_size_get(
      *static_cast<const MaterialPaintLayer *>(ptr->data));
}

static void rna_MaterialPaintLayer_bake_size_set(PointerRNA *ptr, int value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  Material *ma = rna_MaterialPaintLayer_owner(*ptr, &layer);
  if (ma != nullptr && BKE_paint_layers_bake_size_set(*ma, *layer, value)) {
    WM_main_add_notifier(NC_MATERIAL | ND_SHADING, &ma->id);
  }
}

static bool rna_MaterialPaintLayer_bake_is_valid_get(PointerRNA *ptr)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  Material *ma = rna_MaterialPaintLayer_owner(*ptr, &layer);
  return ma != nullptr && BKE_paint_layers_bake_is_valid(*ma, *layer);
}

/* Read-only observation of #BKE_paint_layers_material_live_status: the panel and the Outliner
 * share this one answer instead of reimplementing the mode and bake predicates. */
static int rna_MaterialPaintLayer_live_status_get(PointerRNA *ptr)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  Material *ma = rna_MaterialPaintLayer_owner(*ptr, &layer);
  if (ma == nullptr) {
    return int(PaintLayerMaterialLiveStatus::Baked);
  }
  return int(BKE_paint_layers_material_live_status(*ma, *layer));
}

static void rna_MaterialPaintLayer_live_status_refusal_reason_get(PointerRNA *ptr, char *value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  Material *ma = rna_MaterialPaintLayer_owner(*ptr, &layer);
  PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::None;
  if (ma != nullptr) {
    BKE_paint_layers_material_live_status(*ma, *layer, &refusal);
  }
  if (refusal == PaintLayersSourceGroupRefusal::None) {
    value[0] = '\0';
    return;
  }
  BLI_strncpy(value,
              BKE_paint_layers_source_group_refusal_name(refusal),
              sizeof(char) * 64);
}

static int rna_MaterialPaintLayer_live_status_refusal_reason_length(PointerRNA *ptr)
{
  char value[64];
  rna_MaterialPaintLayer_live_status_refusal_reason_get(ptr, value);
  return int(strlen(value));
}

static void rna_MaterialPaintLayer_bake_request(PointerRNA ptr)
{
  MaterialPaintLayer *layer = nullptr;
  if (rna_MaterialPaintLayer_owner(ptr, &layer) != nullptr) {
    BKE_paint_layers_bake_request(*layer);
  }
}

static void rna_MaterialPaintLayer_bake_clear(PointerRNA ptr)
{
  MaterialPaintLayer *layer = nullptr;
  if (Material *ma = rna_MaterialPaintLayer_owner(ptr, &layer)) {
    BKE_paint_layers_bake_clear(*ma, *layer);
    WM_main_add_notifier(NC_MATERIAL | ND_SHADING, &ma->id);
  }
}

static MaterialPaintLayer *rna_MaterialPaintLayer_mask_add(PointerRNA ptr,
                                                           ReportList *reports,
                                                           float value)
{
  MaterialPaintLayer *layer = nullptr;
  if (Material *ma = rna_MaterialPaintLayer_owner(ptr, &layer)) {
    MaterialPaintLayer *item = BKE_paint_layers_mask_add(*ma, layer, value);
    if (item != nullptr) {
      WM_main_add_notifier(NC_MATERIAL | ND_SHADING, &ma->id);
      return item;
    }
  }
  BKE_report(reports, RPT_ERROR, "Cannot add a mask to this paint layer");
  return nullptr;
}

static MaterialPaintLayerChannel *rna_MaterialPaintLayer_channel_add(PointerRNA ptr,
                                                                     ReportList *reports,
                                                                     int channel)
{
  MaterialPaintLayer *layer = nullptr;
  if (Material *ma = rna_MaterialPaintLayer_owner(ptr, &layer)) {
    MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
        *ma, layer, eMaterialPaintChannel(channel));
    if (record != nullptr) {
      return record;
    }
  }
  BKE_report(reports, RPT_ERROR, "Cannot add a channel to this paint layer");
  return nullptr;
}

static void rna_MaterialPaintLayer_channel_remove(PointerRNA ptr,
                                                  ReportList *reports,
                                                  int channel)
{
  MaterialPaintLayer *layer = nullptr;
  if (Material *ma = rna_MaterialPaintLayer_owner(ptr, &layer)) {
    if (BKE_paint_layers_channel_remove(*ma, layer, eMaterialPaintChannel(channel))) {
      return;
    }
  }
  BKE_report(reports, RPT_ERROR, "This paint layer has no such channel");
}

static void rna_MaterialPaintLayer_channel_set_enabled(PointerRNA ptr,
                                                       ReportList *reports,
                                                       int channel,
                                                       bool enabled)
{
  MaterialPaintLayer *layer = nullptr;
  if (Material *ma = rna_MaterialPaintLayer_owner(ptr, &layer)) {
    if (BKE_paint_layers_channel_set_enabled(
            *ma, layer, eMaterialPaintChannel(channel), enabled))
    {
      return;
    }
  }
  BKE_report(reports, RPT_ERROR, "This paint layer has no such channel");
}

static void rna_MaterialPaintLayer_channels_begin(CollectionPropertyIterator *iter,
                                                  PointerRNA *ptr)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  rna_iterator_array_begin(iter,
                           ptr,
                           layer->channels,
                           sizeof(MaterialPaintLayerChannel),
                           layer->channels_num,
                           false,
                           nullptr);
}

static void rna_MaterialPaintLayer_channel_settings_begin(CollectionPropertyIterator *iter,
                                                          PointerRNA *ptr)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  rna_iterator_array_begin(iter,
                           ptr,
                           layer->channel_settings,
                           sizeof(MaterialPaintLayerChannelSettings),
                           PAINT_MATERIAL_CHANNEL_NUM,
                           false,
                           nullptr);
}

static int rna_MaterialPaintLayer_channel_settings_length(PointerRNA * /*ptr*/)
{
  return PAINT_MATERIAL_CHANNEL_NUM;
}

/** The layer whose channel record is \a channel, searching the whole stack. */
static MaterialPaintLayer *rna_paint_layer_by_channel(ListBase &list,
                                                      const MaterialPaintLayerChannel *channel)
{
  for (MaterialPaintLayer &layer : *reinterpret_cast<ListBaseT<MaterialPaintLayer> *>(&list)) {
    for (const int i : IndexRange(layer.channels_num)) {
      if (&layer.channels[i] == channel) {
        return &layer;
      }
    }
    if (MaterialPaintLayer *found = rna_paint_layer_by_channel(layer.children, channel)) {
      return found;
    }
    /* A correction keeps its channel records in `effects`/`mask_stack`, not in `children`; without
     * this walk a correction's map or state is invisible to its own RNA pointer. */
    if (MaterialPaintLayer *found = rna_paint_layer_by_channel(layer.effects, channel)) {
      return found;
    }
    if (MaterialPaintLayer *found = rna_paint_layer_by_channel(layer.mask_stack, channel)) {
      return found;
    }
  }
  return nullptr;
}

/** The layer whose fixed \a settings array contains \a settings, searching the whole stack. */
static MaterialPaintLayer *rna_paint_layer_by_settings(
    ListBase &list, const MaterialPaintLayerChannelSettings *settings)
{
  for (MaterialPaintLayer &layer : *reinterpret_cast<ListBaseT<MaterialPaintLayer> *>(&list)) {
    if (settings >= &layer.channel_settings[0] &&
        settings < &layer.channel_settings[PAINT_MATERIAL_CHANNEL_NUM])
    {
      return &layer;
    }
    if (MaterialPaintLayer *found = rna_paint_layer_by_settings(layer.children, settings)) {
      return found;
    }
    /* A correction's per (row, channel) settings live on the correction, which is an element of its
     * parent's `effects`/`mask_stack`; missing this walk made its opacity and blend sliders no-ops. */
    if (MaterialPaintLayer *found = rna_paint_layer_by_settings(layer.effects, settings)) {
      return found;
    }
    if (MaterialPaintLayer *found = rna_paint_layer_by_settings(layer.mask_stack, settings)) {
      return found;
    }
  }
  return nullptr;
}

/** The owning material of a mask or channel pointer, or null when it is not in its stack. */
static Material *rna_paint_layer_sub_owner(PointerRNA *ptr, MaterialPaintLayer **r_layer)
{
  Material *ma = reinterpret_cast<Material *>(ptr->owner_id);
  if (ma == nullptr) {
    return nullptr;
  }
  *r_layer = nullptr;
  /* A channel pointer and a channel-settings pointer are both small structs; the RNA struct type
   * is the only thing that tells them apart, so each lookup is tried in turn and the first hit
   * wins. */
  if (MaterialPaintLayerChannel *channel = static_cast<MaterialPaintLayerChannel *>(ptr->data)) {
    *r_layer = rna_paint_layer_by_channel(ma->paint_layers, channel);
  }
  if (*r_layer == nullptr) {
    if (MaterialPaintLayerChannelSettings *settings =
            static_cast<MaterialPaintLayerChannelSettings *>(ptr->data))
    {
      *r_layer = rna_paint_layer_by_settings(ma->paint_layers, settings);
    }
  }
  if (*r_layer == nullptr) {
    return nullptr;
  }
  return ma;
}

static int rna_MaterialPaintLayerChannel_channel_get(PointerRNA *ptr)
{
  const MaterialPaintLayerChannel *record = static_cast<const MaterialPaintLayerChannel *>(
      ptr->data);
  return record->channel;
}

static int rna_MaterialPaintLayerChannel_state_get(PointerRNA *ptr)
{
  const MaterialPaintLayerChannel *record = static_cast<const MaterialPaintLayerChannel *>(
      ptr->data);
  return record->state;
}

static void rna_MaterialPaintLayerChannel_image_set(PointerRNA *ptr,
                                                    PointerRNA value,
                                                    ReportList * /*reports*/)
{
  MaterialPaintLayer *layer = nullptr;
  if (Material *ma = rna_paint_layer_sub_owner(ptr, &layer)) {
    BKE_paint_layers_channel_set_image(
        *ma, layer, eMaterialPaintChannel(rna_MaterialPaintLayerChannel_channel_get(ptr)),
        static_cast<Image *>(value.data));
  }
}

static void rna_MaterialPaintLayerChannel_value_get(PointerRNA *ptr, float *value)
{
  const MaterialPaintLayerChannel *record = static_cast<const MaterialPaintLayerChannel *>(
      ptr->data);
  copy_v4_v4(value, record->value);
}

static void rna_MaterialPaintLayerChannel_value_set(PointerRNA *ptr, const float *value)
{
  MaterialPaintLayer *layer = nullptr;
  if (Material *ma = rna_paint_layer_sub_owner(ptr, &layer)) {
    BKE_paint_layers_channel_set_value(
        *ma, layer, eMaterialPaintChannel(rna_MaterialPaintLayerChannel_channel_get(ptr)),
        value);
  }
}

static int rna_MaterialPaintLayerChannelSettings_channel_get(PointerRNA *ptr)
{
  MaterialPaintLayer *layer = nullptr;
  const MaterialPaintLayerChannelSettings *settings =
      static_cast<const MaterialPaintLayerChannelSettings *>(ptr->data);
  if (rna_paint_layer_sub_owner(ptr, &layer) && layer != nullptr && settings != nullptr) {
    return int(settings - layer->channel_settings);
  }
  return 0;
}

static int rna_MaterialPaintLayerChannelSettings_blend_get(PointerRNA *ptr)
{
  const MaterialPaintLayerChannelSettings *settings =
      static_cast<const MaterialPaintLayerChannelSettings *>(ptr->data);
  return settings->blend;
}

static void rna_MaterialPaintLayerChannelSettings_blend_set(PointerRNA *ptr, int value)
{
  const MaterialPaintLayerChannelSettings *settings =
      static_cast<const MaterialPaintLayerChannelSettings *>(ptr->data);
  MaterialPaintLayer *layer = nullptr;
  if (Material *ma = rna_paint_layer_sub_owner(ptr, &layer)) {
    BKE_paint_layers_channel_blend_set(*ma, *layer, int(settings - layer->channel_settings), value);
  }
}

static float rna_MaterialPaintLayerChannelSettings_opacity_get(PointerRNA *ptr)
{
  const MaterialPaintLayerChannelSettings *settings =
      static_cast<const MaterialPaintLayerChannelSettings *>(ptr->data);
  return settings->opacity * 100.0f;
}

static void rna_MaterialPaintLayerChannelSettings_opacity_set(PointerRNA *ptr, float value)
{
  const MaterialPaintLayerChannelSettings *settings =
      static_cast<const MaterialPaintLayerChannelSettings *>(ptr->data);
  MaterialPaintLayer *layer = nullptr;
  if (Material *ma = rna_paint_layer_sub_owner(ptr, &layer)) {
    BKE_paint_layers_channel_opacity_set(
        *ma, *layer, int(settings - layer->channel_settings), value / 100.0f);
  }
}

static int rna_MaterialPaintLayer_children_length(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return BLI_listbase_count(&layer->children);
}

static int rna_MaterialPaintLayer_effects_length(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return BLI_listbase_count(&layer->effects);
}

static int rna_MaterialPaintLayer_mask_stack_length(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return BLI_listbase_count(&layer->mask_stack);
}

/** One row of the computed `MaterialPaintLayer.issues` collection, a snapshot of a description
 * problem; the array is built on iteration and owned by the iterator. */
struct MaterialPaintLayerIssueItem {
  int code;
  int channel;
  char layer_marker[UUID_STRING_SIZE];
  char correction_marker[UUID_STRING_SIZE];
  char text[256];
};

static void rna_MaterialPaintLayer_issues_begin(CollectionPropertyIterator *iter,
                                                PointerRNA *ptr)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  Material *ma = reinterpret_cast<Material *>(ptr->owner_id);
  MaterialPaintLayerIssueItem *array = nullptr;
  int64_t count = 0;
  if (ma != nullptr && layer != nullptr && BKE_paint_layers_find(*ma, layer->marker) == layer) {
    Vector<PaintLayersIssue> issues;
    BKE_paint_layers_issues_get(*ma, issues);
    for (const PaintLayersIssue &issue : issues) {
      if (BLI_uuid_equal(issue.layer, layer->marker)) {
        count++;
      }
    }
    if (count > 0) {
      array = MEM_new_array_uninitialized<MaterialPaintLayerIssueItem>(count, __func__);
      int64_t i = 0;
      for (const PaintLayersIssue &issue : issues) {
        if (!BLI_uuid_equal(issue.layer, layer->marker)) {
          continue;
        }
        MaterialPaintLayerIssueItem &item = array[i++];
        item.code = int(issue.code);
        item.channel = issue.channel;
        BLI_uuid_format(item.layer_marker, issue.layer);
        BLI_uuid_format(item.correction_marker, issue.correction);
        STRNCPY(item.text, issue.text != nullptr ? issue.text : "");
      }
    }
  }
  rna_iterator_array_begin(iter,
                           ptr,
                           array,
                           sizeof(MaterialPaintLayerIssueItem),
                           count,
                           array != nullptr,
                           nullptr);
}

static MaterialPaintLayerIssueItem *rna_MaterialPaintLayerIssue(PointerRNA *ptr)
{
  return static_cast<MaterialPaintLayerIssueItem *>(ptr->data);
}

static int rna_MaterialPaintLayerIssue_code_get(PointerRNA *ptr)
{
  return rna_MaterialPaintLayerIssue(ptr)->code;
}

static int rna_MaterialPaintLayerIssue_channel_get(PointerRNA *ptr)
{
  return rna_MaterialPaintLayerIssue(ptr)->channel;
}

static void rna_MaterialPaintLayerIssue_layer_marker_get(PointerRNA *ptr, char *value)
{
  BLI_strncpy(value, rna_MaterialPaintLayerIssue(ptr)->layer_marker, UUID_STRING_SIZE);
}

static int rna_MaterialPaintLayerIssue_layer_marker_length(PointerRNA *ptr)
{
  return int(strlen(rna_MaterialPaintLayerIssue(ptr)->layer_marker));
}

static void rna_MaterialPaintLayerIssue_correction_marker_get(PointerRNA *ptr, char *value)
{
  BLI_strncpy(value, rna_MaterialPaintLayerIssue(ptr)->correction_marker, UUID_STRING_SIZE);
}

static int rna_MaterialPaintLayerIssue_correction_marker_length(PointerRNA *ptr)
{
  return int(strlen(rna_MaterialPaintLayerIssue(ptr)->correction_marker));
}

static void rna_MaterialPaintLayerIssue_text_get(PointerRNA *ptr, char *value)
{
  BLI_strncpy(value, rna_MaterialPaintLayerIssue(ptr)->text, sizeof(rna_MaterialPaintLayerIssue(ptr)->text));
}

static int rna_MaterialPaintLayerIssue_text_length(PointerRNA *ptr)
{
  return int(strlen(rna_MaterialPaintLayerIssue(ptr)->text));
}

/** One row of the computed `MaterialPaintLayer.custom_channels` collection. */
struct MaterialPaintLayerCustomChannelItem {
  int channel;
};

static void rna_MaterialPaintLayer_custom_channels_begin(CollectionPropertyIterator *iter,
                                                         PointerRNA *ptr)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  Vector<int> channels;
  if (layer != nullptr) {
    BKE_paint_layers_custom_channels_get(*layer, channels);
  }
  MaterialPaintLayerCustomChannelItem *array = nullptr;
  if (!channels.is_empty()) {
    array = MEM_new_array_uninitialized<MaterialPaintLayerCustomChannelItem>(
        channels.size(), __func__);
    for (const int64_t i : channels.index_range()) {
      array[i].channel = channels[i];
    }
  }
  rna_iterator_array_begin(iter,
                           ptr,
                           array,
                           sizeof(MaterialPaintLayerCustomChannelItem),
                           channels.size(),
                           array != nullptr,
                           nullptr);
}

static int rna_MaterialPaintLayerCustomChannel_channel_get(PointerRNA *ptr)
{
  return static_cast<MaterialPaintLayerCustomChannelItem *>(ptr->data)->channel;
}

static int rna_MaterialPaintLayer_channels_length(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return layer->channels_num;
}

static void rna_MaterialPaintLayer_source_change(PointerRNA ptr, ReportList *reports, int source)
{
  MaterialPaintLayer *layer = nullptr;
  if (Material *ma = rna_MaterialPaintLayer_owner(ptr, &layer)) {
    if (BKE_paint_layers_source_change(*ma, layer, int8_t(source))) {
      WM_main_add_notifier(NC_MATERIAL | ND_SHADING, &ma->id);
      return;
    }
  }
  BKE_report(reports, RPT_ERROR, "Cannot convert this paint layer to the given source");
}

/* The IDProperty group of the layer, exposed as a PropertyGroup like the nodes modifier does. */
static IDProperty **rna_MaterialPaintLayerProperties_idprops(PointerRNA *ptr)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  return &layer->properties;
}

static StructRNA *rna_MaterialPaintLayerProperties_refine(PointerRNA * /*ptr*/)
{
  return RNA_PropertyGroup;
}

static PointerRNA rna_MaterialPaintLayer_properties_get(PointerRNA *ptr)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  return RNA_pointer_create_with_parent(*ptr, RNA_MaterialPaintLayerProperties, layer);
}

static void rna_MaterialPaintLayer_marker_get(PointerRNA *ptr, char *value)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  BLI_uuid_format(value, layer->marker);
}

static int rna_MaterialPaintLayer_marker_length(PointerRNA * /*ptr*/)
{
  return UUID_STRING_SIZE - 1;
}

static std::optional<std::string> rna_MaterialPaintLayer_path(const PointerRNA *ptr)
{
  const Material *ma = reinterpret_cast<const Material *>(ptr->owner_id);
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  /* Only the top level is addressable by path; nested rows resolve through find(). */
  const int index = BLI_findindex(&ma->paint_layers, layer);
  if (index == -1) {
    return std::nullopt;
  }
  return fmt::format("paint_layers[{}]", index);
}

/**
 * The path of a #MaterialPaintLayerChannelSettings entry: the row's own path plus the fixed channel
 * index. Built from the settings pointer arithmetic so a key can address a pair that has no channel
 * record yet.
 */
static std::optional<std::string> rna_MaterialPaintLayerChannelSettings_path(const PointerRNA *ptr)
{
  const Material *ma = reinterpret_cast<const Material *>(ptr->owner_id);
  const MaterialPaintLayerChannelSettings *settings =
      static_cast<const MaterialPaintLayerChannelSettings *>(ptr->data);
  if (ma == nullptr || settings == nullptr) {
    return std::nullopt;
  }
  MaterialPaintLayer *layer = rna_paint_layer_by_settings(
      const_cast<ListBase &>(ma->paint_layers), settings);
  if (layer == nullptr) {
    return std::nullopt;
  }
  const int index = BLI_findindex(&ma->paint_layers, layer);
  if (index == -1) {
    return std::nullopt;
  }
  return fmt::format(
      "paint_layers[{}].channel_settings[{}]", index, int(settings - layer->channel_settings));
}

/* -------------------------------------------------------------------- */
/** \name Mesh map RNA helpers
 * \{ */

static void rna_Material_mesh_map_slots_begin(CollectionPropertyIterator *iter, PointerRNA *ptr)
{
  Material *ma = id_cast<Material *>(ptr->owner_id);
  rna_iterator_listbase_begin(iter, ptr, &ma->mesh_map_slots, nullptr);
}

static int rna_Material_mesh_map_slots_length(PointerRNA *ptr)
{
  Material *ma = id_cast<Material *>(ptr->owner_id);
  return BLI_listbase_count(&ma->mesh_map_slots);
}

static MaterialMeshMapSlot *rna_Material_mesh_map_slots_ensure(Material *ma, int type)
{
  return BKE_mesh_maps_slot_ensure(*ma, int8_t(type));
}

static MaterialMeshMapSlot *rna_Material_mesh_map_slots_find(Material *ma, int type)
{
  return BKE_mesh_maps_slot_find(*ma, int8_t(type));
}

static int rna_MaterialMeshMapSlot_type_get(PointerRNA *ptr)
{
  return static_cast<const MaterialMeshMapSlot *>(ptr->data)->type;
}

static void rna_MaterialMeshMapSlot_image_set(PointerRNA *ptr, PointerRNA value, ReportList *)
{
  MaterialMeshMapSlot *slot = static_cast<MaterialMeshMapSlot *>(ptr->data);
  Material *ma = id_cast<Material *>(ptr->owner_id);
  BKE_mesh_maps_slot_image_set(*ma, slot->type, static_cast<Image *>(value.data));
}

static PointerRNA rna_Material_mesh_map_settings_get(PointerRNA *ptr)
{
  Material *ma = id_cast<Material *>(ptr->owner_id);
  return RNA_pointer_create_with_parent(
      *ptr, RNA_MaterialMeshMapSettings, &ma->mesh_map_settings);
}

/** \} */

}  // namespace blender

#else

namespace blender {

static const EnumPropertyItem rna_enum_material_paint_layer_source_items[] = {
    {MA_PAINT_LAYER_SOURCE_IMAGE, "IMAGE", 0, "Image", "A painted map"},
    {MA_PAINT_LAYER_SOURCE_CONSTANT, "CONSTANT", 0, "Constant", "A flat colour"},
    {MA_PAINT_LAYER_SOURCE_MATERIAL,
     "MATERIAL",
     0,
     "Material",
     "Another material's channels, baked"},
    {MA_PAINT_LAYER_SOURCE_NODE_GROUP,
     "NODE_GROUP",
     0,
     "Node Group",
     "A user's node group"},
    {MA_PAINT_LAYER_SOURCE_STACK, "STACK", 0, "Stack", "A nested stack of layers (a folder)"},
    {MA_PAINT_LAYER_SOURCE_MESH_MAP,
     "MESH_MAP",
     0,
     "Mesh Map",
     "A geometry map of the object, read from the material's shared atlas"},
    {0, nullptr, 0, nullptr, nullptr},
};

/* The mesh map types exposed to the UI. THICKNESS and POSITION have DNA values but no bake side
 * yet, so they are not offered. */
static const EnumPropertyItem rna_enum_material_mesh_map_type_items[] = {
    {MA_MESH_MAP_AO, "AO", 0, "Ambient Occlusion", "Ambient occlusion map"},
    {MA_MESH_MAP_CURVATURE, "CURVATURE", 0, "Curvature", "Curvature map"},
    {MA_MESH_MAP_NORMAL_WORLD, "NORMAL_WORLD", 0, "Normal (World)", "World-space normal map"},
    {MA_MESH_MAP_NORMAL_OBJECT,
     "NORMAL_OBJECT",
     0,
     "Normal (Object)",
     "Object-space normal map"},
    {MA_MESH_MAP_ID_OBJECT, "ID_OBJECT", 0, "Object ID", "Object index map"},
    {MA_MESH_MAP_ID_MATERIAL, "ID_MATERIAL", 0, "Material ID", "Material index map"},
    {MA_MESH_MAP_EDGE, "EDGE", 0, "Edge", "Edge/bevel map"},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem rna_enum_material_paint_layer_blend_items[] = {
    {MA_PAINT_LAYER_BLEND_MIX, "MIX", 0, "Mix", "Mix blend"},
    {MA_PAINT_LAYER_BLEND_MULTIPLY, "MULTIPLY", 0, "Multiply", "Multiply blend"},
    {MA_PAINT_LAYER_BLEND_OVERLAY, "OVERLAY", 0, "Overlay", "Overlay blend"},
    {MA_PAINT_LAYER_BLEND_ADD, "ADD", 0, "Add", "Add blend"},
    {MA_PAINT_LAYER_BLEND_DARKEN, "DARKEN", 0, "Darken", "Darken blend"},
    {MA_PAINT_LAYER_BLEND_BURN, "BURN", 0, "Color Burn", "Color burn blend"},
    {MA_PAINT_LAYER_BLEND_LIGHTEN, "LIGHTEN", 0, "Lighten", "Lighten blend"},
    {MA_PAINT_LAYER_BLEND_SCREEN, "SCREEN", 0, "Screen", "Screen blend"},
    {MA_PAINT_LAYER_BLEND_DODGE, "DODGE", 0, "Color Dodge", "Color dodge blend"},
    {MA_PAINT_LAYER_BLEND_SUBTRACT, "SUBTRACT", 0, "Subtract", "Subtract blend"},
    {MA_PAINT_LAYER_BLEND_DIVIDE, "DIVIDE", 0, "Divide", "Divide blend"},
    {MA_PAINT_LAYER_BLEND_DIFFERENCE, "DIFFERENCE", 0, "Difference", "Difference blend"},
    {MA_PAINT_LAYER_BLEND_EXCLUSION, "EXCLUSION", 0, "Exclusion", "Exclusion blend"},
    {MA_PAINT_LAYER_BLEND_SOFT_LIGHT, "SOFT_LIGHT", 0, "Soft Light", "Soft light blend"},
    {MA_PAINT_LAYER_BLEND_LINEAR_LIGHT,
     "LINEAR_LIGHT",
     0,
     "Linear Light",
     "Linear light blend"},
    {MA_PAINT_LAYER_BLEND_HUE, "HUE", 0, "Hue", "Hue blend"},
    {MA_PAINT_LAYER_BLEND_SATURATION, "SATURATION", 0, "Saturation", "Saturation blend"},
    {MA_PAINT_LAYER_BLEND_COLOR, "COLOR", 0, "Color", "Color blend"},
    {MA_PAINT_LAYER_BLEND_VALUE, "VALUE", 0, "Value", "Value blend"},
    /* MA_PAINT_LAYER_BLEND_NORMAL_COMBINE is internal: the Normal channel forces it, so it is not
     * offered as a choice, and #BKE_paint_layers_set_blend refuses it. */
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem rna_enum_material_paint_layer_channel_state_items[] = {
    {MA_PAINT_LAYER_CHANNEL_ABSENT, "ABSENT", 0, "Absent", "No map: the channel carries no map"},
    {MA_PAINT_LAYER_CHANNEL_ENABLED, "ENABLED", 0, "Enabled", "A map feeds the channel"},
    {MA_PAINT_LAYER_CHANNEL_DISABLED,
     "DISABLED",
     0,
     "Disabled",
     "The map is kept but switched off"},
    {0, nullptr, 0, nullptr, nullptr},
};

/* The channel's own blend. Kept apart from #rna_enum_material_paint_layer_blend_items so the row
 * never offers Inherit, which only a channel override can meaningfully be. */
static const EnumPropertyItem rna_enum_material_paint_layer_channel_blend_items[] = {
    {-1, "INHERIT", 0, "Inherit", "Use the row's blend mode"},
    {MA_PAINT_LAYER_BLEND_MIX, "MIX", 0, "Mix", "Mix blend"},
    {MA_PAINT_LAYER_BLEND_MULTIPLY, "MULTIPLY", 0, "Multiply", "Multiply blend"},
    {MA_PAINT_LAYER_BLEND_OVERLAY, "OVERLAY", 0, "Overlay", "Overlay blend"},
    {MA_PAINT_LAYER_BLEND_ADD, "ADD", 0, "Add", "Add blend"},
    {MA_PAINT_LAYER_BLEND_DARKEN, "DARKEN", 0, "Darken", "Darken blend"},
    {MA_PAINT_LAYER_BLEND_BURN, "BURN", 0, "Color Burn", "Color burn blend"},
    {MA_PAINT_LAYER_BLEND_LIGHTEN, "LIGHTEN", 0, "Lighten", "Lighten blend"},
    {MA_PAINT_LAYER_BLEND_SCREEN, "SCREEN", 0, "Screen", "Screen blend"},
    {MA_PAINT_LAYER_BLEND_DODGE, "DODGE", 0, "Color Dodge", "Color dodge blend"},
    {MA_PAINT_LAYER_BLEND_SUBTRACT, "SUBTRACT", 0, "Subtract", "Subtract blend"},
    {MA_PAINT_LAYER_BLEND_DIVIDE, "DIVIDE", 0, "Divide", "Divide blend"},
    {MA_PAINT_LAYER_BLEND_DIFFERENCE, "DIFFERENCE", 0, "Difference", "Difference blend"},
    {MA_PAINT_LAYER_BLEND_EXCLUSION, "EXCLUSION", 0, "Exclusion", "Exclusion blend"},
    {MA_PAINT_LAYER_BLEND_SOFT_LIGHT, "SOFT_LIGHT", 0, "Soft Light", "Soft light blend"},
    {MA_PAINT_LAYER_BLEND_LINEAR_LIGHT,
     "LINEAR_LIGHT",
     0,
     "Linear Light",
     "Linear light blend"},
    {MA_PAINT_LAYER_BLEND_HUE, "HUE", 0, "Hue", "Hue blend"},
    {MA_PAINT_LAYER_BLEND_SATURATION, "SATURATION", 0, "Saturation", "Saturation blend"},
    {MA_PAINT_LAYER_BLEND_COLOR, "COLOR", 0, "Color", "Color blend"},
    {MA_PAINT_LAYER_BLEND_VALUE, "VALUE", 0, "Value", "Value blend"},
    {0, nullptr, 0, nullptr, nullptr},
};

/* Values must match #PaintLayerPlace (a runtime-only type, unavailable in the definition pass). */
static const EnumPropertyItem rna_enum_material_paint_layer_place_items[] = {
    {0, "ABOVE", 0, "Above", "Directly above the anchor"},
    {1, "BELOW", 0, "Below", "Directly below the anchor"},
    {2, "INTO", 0, "Into", "Inside the anchor folder"},
    {0, nullptr, 0, nullptr, nullptr},
};

/* Values must match #PaintLayersIssueCode. */
static const EnumPropertyItem rna_enum_material_paint_layer_issue_code_items[] = {
    {0,
     "FILL_CORRECTION_ON_NORMAL",
     0,
     "Fill Correction on Normal",
     "A constant normal has no meaning, so the correction is skipped in the Normal channel"},
    {1,
     "FOLDER_HAS_MAPS",
     0,
     "Folder Has Maps",
     "A folder takes part through its children; a map on the folder itself is ignored"},
    {2,
     "NON_FOLDER_HAS_CHILDREN",
     0,
     "Non-Folder Has Children",
     "Only a folder takes part through its children; the stray nesting is ignored"},
    {0, nullptr, 0, nullptr, nullptr},
};

/* Values match #eMaterialPaintLayerRole. */
static const EnumPropertyItem rna_enum_material_paint_layer_role_items[] = {
    {MA_PAINT_LAYER_ROLE_LAYER, "LAYER", 0, "Layer", "A stack member"},
    {MA_PAINT_LAYER_ROLE_EFFECT, "EFFECT", 0, "Effect", "Adjusts what the row below paints"},
    {MA_PAINT_LAYER_ROLE_MASK_ITEM, "MASK_ITEM", 0, "Mask Item", "Limits where the row applies"},
    {0, nullptr, 0, nullptr, nullptr},
};

/* Values match #PaintLayerMaterialLiveStatus. */
static const EnumPropertyItem rna_enum_material_paint_layer_live_status_items[] = {
    {0, "LIVE", 0, "Live", "The row shows its source material live"},
    {1, "BAKING", 0, "Baking", "The row is live while its bake is being rendered"},
    {2, "BAKED", 0, "Baked", "The row shows its baked maps"},
    {3,
     "REFUSED",
     0,
     "Refused",
     "The live source could not be wrapped; the row shows its baked maps instead"},
    {0, nullptr, 0, nullptr, nullptr},
};

/* Values match #eMaterialPaintLayerBakeMode. */
static const EnumPropertyItem rna_enum_material_paint_layer_bake_mode_items[] = {
    {MA_PAINT_LAYER_BAKE_AUTO,
     "AUTO",
     0,
     "Auto",
     "Bake the heavy inactive subgraphs; the active row is evaluated live"},
    {MA_PAINT_LAYER_BAKE_ALWAYS,
     "ALWAYS",
     0,
     "Always",
     "Always bake; show the row as stale until the bake is current"},
    {MA_PAINT_LAYER_BAKE_NEVER, "NEVER", 0, "Never", "Never bake; evaluate the row live"},
    {0, nullptr, 0, nullptr, nullptr},
};

static void rna_def_material_paint_layer(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;
  FunctionRNA *func;
  PropertyRNA *parm;

  srna = RNA_def_struct(brna, "MaterialPaintLayer", nullptr);
  RNA_def_struct_sdna(srna, "MaterialPaintLayer");
  RNA_def_struct_path_func(srna, "rna_MaterialPaintLayer_path");
  RNA_def_struct_ui_text(srna, "Paint Layer", "One row of a layered material's stack");

  prop = RNA_def_property(srna, "name", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, nullptr, "name");
  RNA_def_property_string_maxlength(prop, MAX_NAME);
  RNA_def_property_string_funcs(prop, nullptr, nullptr, "rna_MaterialPaintLayer_name_set");
  RNA_def_property_ui_text(prop, "Name", "Name of the paint layer");
  RNA_def_struct_name_property(srna, prop);
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  /* Read-only: the marker is the row's identity and is assigned when the row is created. */
  prop = RNA_def_property(srna, "marker", PROP_STRING, PROP_NONE);
  RNA_def_property_string_funcs(prop,
                                "rna_MaterialPaintLayer_marker_get",
                                "rna_MaterialPaintLayer_marker_length",
                                nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Marker", "Stable identity of the paint layer");

  /* A stack Layer's source is changed by conversion, through source_change(), never by assigning
   * this property -- #rna_MaterialPaintLayer_source_editable reports it as not editable for that
   * case, so a UI widget greys out rather than doing nothing. A correction's source is a plain,
   * always-editable setting (the old `effect` property); #rna_MaterialPaintLayer_source_set
   * defers the actual restriction to BKE either way. */
  prop = RNA_def_property(srna, "source", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_material_paint_layer_source_items);
  RNA_def_property_enum_funcs(
      prop, "rna_MaterialPaintLayer_source_get", "rna_MaterialPaintLayer_source_set", nullptr);
  RNA_def_property_editable_func(prop, "rna_MaterialPaintLayer_source_editable");
  RNA_def_property_ui_text(prop, "Source", "What the layer reads its values from");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  /* Which geometry map a MESH_MAP row reads. Editable only for a MESH_MAP row; the BKE setter
   * refuses any other source, so a script that bypasses the editable flag changes nothing. */
  prop = RNA_def_property(srna, "mesh_map_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_material_mesh_map_type_items);
  RNA_def_property_enum_funcs(prop,
                              "rna_MaterialPaintLayer_mesh_map_type_get",
                              "rna_MaterialPaintLayer_mesh_map_type_set",
                              nullptr);
  RNA_def_property_editable_func(prop, "rna_MaterialPaintLayer_mesh_map_type_editable");
  RNA_def_property_ui_text(prop, "Mesh Map Type", "Which geometry map a Mesh Map row reads");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "blend_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_material_paint_layer_blend_items);
  RNA_def_property_enum_funcs(
      prop, "rna_MaterialPaintLayer_blend_get", "rna_MaterialPaintLayer_blend_set", nullptr);
  RNA_def_property_ui_text(prop, "Blend Type", "How the layer blends over what is below it");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "opacity", PROP_FLOAT, PROP_PERCENTAGE);
  RNA_def_property_float_funcs(
      prop, "rna_MaterialPaintLayer_opacity_get", "rna_MaterialPaintLayer_opacity_set", nullptr);
  RNA_def_property_range(prop, 0.0f, 100.0f);
  RNA_def_property_ui_text(prop, "Opacity", "Opacity of the paint layer, in percent");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "enabled", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(
      prop, "rna_MaterialPaintLayer_enabled_get", "rna_MaterialPaintLayer_enabled_set");
  RNA_def_property_ui_text(prop, "Enabled", "Whether the layer takes part in the stack");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "color_tag", PROP_INT, PROP_NONE);
  RNA_def_property_int_funcs(prop,
                             "rna_MaterialPaintLayer_color_tag_get",
                             "rna_MaterialPaintLayer_color_tag_set",
                             nullptr);
  RNA_def_property_ui_text(prop, "Color Tag", "Display color tag, interpreted by the UI only");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  /* Meaningful for a correction row only; harmless to read on a stack Layer, which is always
   * #MA_PAINT_LAYER_ROLE_LAYER. Setting it to Layer is refused by #BKE_paint_layers_role_set. */
  prop = RNA_def_property(srna, "role", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_material_paint_layer_role_items);
  RNA_def_property_enum_funcs(
      prop, "rna_MaterialPaintLayer_role_get", "rna_MaterialPaintLayer_role_set", nullptr);
  RNA_def_property_ui_text(prop, "Role", "The row's structural place in its owner");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "fill_color", PROP_FLOAT, PROP_COLOR);
  RNA_def_property_array(prop, 4);
  RNA_def_property_float_funcs(prop,
                               "rna_MaterialPaintLayer_fill_color_get",
                               "rna_MaterialPaintLayer_fill_color_set",
                               nullptr);
  RNA_def_property_ui_text(prop, "Fill Color", "Constant color of a Fill layer");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "custom_group", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, nullptr, "custom_group");
  RNA_def_property_struct_type(prop, "NodeTree");
  RNA_def_property_pointer_funcs(
      prop, nullptr, "rna_MaterialPaintLayer_custom_group_set", nullptr, nullptr);
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Custom Group", "Node group backing a Node Group source layer");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "material", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, nullptr, "material");
  RNA_def_property_struct_type(prop, "Material");
  RNA_def_property_pointer_funcs(
      prop, nullptr, "rna_MaterialPaintLayer_material_set", nullptr, nullptr);
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Source Material", "Material a Material layer is built from");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "properties", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "PropertyGroup");
  RNA_def_property_pointer_funcs(
      prop, "rna_MaterialPaintLayer_properties_get", nullptr, nullptr, nullptr);
  RNA_def_property_ui_text(prop, "Properties", "Custom group input values and add-on data");

  prop = RNA_def_property(srna, "children", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_struct_type(prop, "MaterialPaintLayer");
  RNA_def_property_collection_funcs(prop,
                                    "rna_MaterialPaintLayer_children_begin",
                                    "rna_iterator_listbase_next",
                                    "rna_iterator_listbase_end",
                                    "rna_iterator_listbase_get",
                                    "rna_MaterialPaintLayer_children_length",
                                    nullptr,
                                    nullptr,
                                    nullptr);
  RNA_def_property_ui_text(prop, "Children", "Nested rows of this folder layer");

  prop = RNA_def_property(srna, "effects", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_struct_type(prop, "MaterialPaintLayer");
  RNA_def_property_collection_funcs(prop,
                                    "rna_MaterialPaintLayer_effects_begin",
                                    "rna_iterator_listbase_next",
                                    "rna_iterator_listbase_end",
                                    "rna_iterator_listbase_get",
                                    "rna_MaterialPaintLayer_effects_length",
                                    nullptr,
                                    nullptr,
                                    nullptr);
  RNA_def_property_ui_text(prop, "Effects", "Effects that adjust what this layer paints with");

  prop = RNA_def_property(srna, "mask_stack", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_struct_type(prop, "MaterialPaintLayer");
  RNA_def_property_collection_funcs(prop,
                                    "rna_MaterialPaintLayer_mask_stack_begin",
                                    "rna_iterator_listbase_next",
                                    "rna_iterator_listbase_end",
                                    "rna_iterator_listbase_get",
                                    "rna_MaterialPaintLayer_mask_stack_length",
                                    nullptr,
                                    nullptr,
                                    nullptr);
  RNA_def_property_ui_text(prop, "Mask Stack", "Mask items that limit where this layer applies");

  prop = RNA_def_property(srna, "channels", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_struct_type(prop, "MaterialPaintLayerChannel");
  RNA_def_property_collection_funcs(prop,
                                    "rna_MaterialPaintLayer_channels_begin",
                                    "rna_iterator_array_next",
                                    "rna_iterator_array_end",
                                    "rna_iterator_array_get",
                                    "rna_MaterialPaintLayer_channels_length",
                                    nullptr,
                                    nullptr,
                                    nullptr);
  RNA_def_property_ui_text(prop, "Channels", "Per-channel maps and values of the layer");

  prop = RNA_def_property(srna, "channel_settings", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_struct_type(prop, "MaterialPaintLayerChannelSettings");
  RNA_def_property_collection_funcs(prop,
                                    "rna_MaterialPaintLayer_channel_settings_begin",
                                    "rna_iterator_array_next",
                                    "rna_iterator_array_end",
                                    "rna_iterator_array_get",
                                    "rna_MaterialPaintLayer_channel_settings_length",
                                    nullptr,
                                    nullptr,
                                    nullptr);
  RNA_def_property_ui_text(prop,
                           "Channel Settings",
                           "Per (row, channel) blend and opacity; exists for every channel, "
                           "whether or not it has a map");

  prop = RNA_def_property(srna, "issues", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_struct_type(prop, "MaterialPaintLayerIssue");
  RNA_def_property_collection_funcs(prop,
                                    "rna_MaterialPaintLayer_issues_begin",
                                    "rna_iterator_array_next",
                                    "rna_iterator_array_end",
                                    "rna_iterator_array_dereference_get",
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop, "Issues", "Description problems on this layer that the stack silently ignores");

  prop = RNA_def_property(srna, "custom_channels", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_struct_type(prop, "MaterialPaintLayerCustomChannel");
  RNA_def_property_collection_funcs(prop,
                                    "rna_MaterialPaintLayer_custom_channels_begin",
                                    "rna_iterator_array_next",
                                    "rna_iterator_array_end",
                                    "rna_iterator_array_dereference_get",
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop,
                           "Custom Channels",
                           "Channels a Custom layer's group declares through its COLOR outputs");

  prop = RNA_def_property(srna, "bake_mode", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_material_paint_layer_bake_mode_items);
  RNA_def_property_enum_funcs(prop,
                              "rna_MaterialPaintLayer_bake_mode_get",
                              "rna_MaterialPaintLayer_bake_mode_set",
                              nullptr);
  RNA_def_property_ui_text(prop, "Bake Mode", "When the row's subtree is baked into a cache");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "bake_size", PROP_INT, PROP_NONE);
  RNA_def_property_int_funcs(prop,
                             "rna_MaterialPaintLayer_bake_size_get",
                             "rna_MaterialPaintLayer_bake_size_set",
                             nullptr);
  RNA_def_property_range(prop, 0, 16384);
  RNA_def_property_ui_text(prop, "Bake Size", "Square side the row's baked maps are created at");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "bake_is_valid", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop, "rna_MaterialPaintLayer_bake_is_valid_get", nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop, "Bake Is Valid", "Whether the stored bake still matches the row's description");

  /* Read-only: computed from the row's mode, bake readiness and wrapper refusal. */
  prop = RNA_def_property(srna, "live_status", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_material_paint_layer_live_status_items);
  RNA_def_property_enum_funcs(prop, "rna_MaterialPaintLayer_live_status_get", nullptr, nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop,
      "Live Status",
      "Whether a Material row shows its source live, is waiting for its bake, "
      "shows its baked maps, or was refused a live wrapper");

  prop = RNA_def_property(srna, "live_status_refusal_reason", PROP_STRING, PROP_NONE);
  RNA_def_property_string_funcs(prop,
                                "rna_MaterialPaintLayer_live_status_refusal_reason_get",
                                "rna_MaterialPaintLayer_live_status_refusal_reason_length",
                                nullptr);
  RNA_def_property_string_maxlength(prop, 64);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop,
                           "Live Status Refusal Reason",
                           "Why a Refused Material row shows its baked maps instead of its source");

  func = RNA_def_function(srna, "bake_request", "rna_MaterialPaintLayer_bake_request");
  RNA_def_function_ui_description(
      func, "Mark the row's bake stale so the planner re-bakes it");
  RNA_def_function_flag(func, FUNC_SELF_AS_RNA);

  func = RNA_def_function(srna, "bake_clear", "rna_MaterialPaintLayer_bake_clear");
  RNA_def_function_ui_description(func, "Drop the row's baked cache");
  RNA_def_function_flag(func, FUNC_SELF_AS_RNA);

  func = RNA_def_function(srna, "source_change", "rna_MaterialPaintLayer_source_change");
  RNA_def_function_ui_description(
      func, "Convert the layer between Image and Constant, converting what the two disagree on");
  RNA_def_function_flag(func, FUNC_SELF_AS_RNA | FUNC_USE_REPORTS);
  parm = RNA_def_enum(func,
                      "source",
                      rna_enum_material_paint_layer_source_items,
                      MA_PAINT_LAYER_SOURCE_IMAGE,
                      "Source",
                      "The source to convert to");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);

  func = RNA_def_function(srna, "correction_add", "rna_MaterialPaintLayer_correction_add");
  RNA_def_function_ui_description(func, "Add a correction row under this paint layer");
  RNA_def_function_flag(func, FUNC_SELF_AS_RNA | FUNC_USE_REPORTS);
  parm = RNA_def_enum(func,
                      "role",
                      rna_enum_material_paint_layer_role_items,
                      MA_PAINT_LAYER_ROLE_EFFECT,
                      "Role",
                      "Which part of the parent the correction adjusts");
  parm = RNA_def_enum(func,
                      "source",
                      rna_enum_material_paint_layer_source_items,
                      MA_PAINT_LAYER_SOURCE_IMAGE,
                      "Source",
                      "What the correction applies");
  RNA_def_string(
      func, "name", "Correction", MAX_NAME, "Name", "Name of the new correction");
  parm = RNA_def_pointer(
      func, "correction", "MaterialPaintLayer", "", "The newly created correction");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "mask_add", "rna_MaterialPaintLayer_mask_add");
  RNA_def_function_ui_description(func, "Add a constant mask item to the bottom of the mask stack");
  RNA_def_function_flag(func, FUNC_SELF_AS_RNA | FUNC_USE_REPORTS);
  RNA_def_float(func, "value", 1.0f, 0.0f, 1.0f, "Value", "Constant mask strength", 0.0f, 1.0f);
  parm = RNA_def_pointer(func, "mask", "MaterialPaintLayer", "", "The new mask item");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "channel_add", "rna_MaterialPaintLayer_channel_add");
  RNA_def_function_ui_description(func, "Add a channel record to the layer");
  RNA_def_function_flag(func, FUNC_SELF_AS_RNA | FUNC_USE_REPORTS);
  parm = RNA_def_enum(func,
                      "channel",
                      rna_enum_material_paint_channel_items,
                      PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                      "Channel",
                      "The channel to add");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_pointer(func, "record", "MaterialPaintLayerChannel", "", "The channel record");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "channel_remove", "rna_MaterialPaintLayer_channel_remove");
  RNA_def_function_ui_description(func, "Remove a channel record from the layer");
  RNA_def_function_flag(func, FUNC_SELF_AS_RNA | FUNC_USE_REPORTS);
  parm = RNA_def_enum(func,
                      "channel",
                      rna_enum_material_paint_channel_items,
                      PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                      "Channel",
                      "The channel to remove");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);

  func = RNA_def_function(srna, "channel_set_enabled", "rna_MaterialPaintLayer_channel_set_enabled");
  RNA_def_function_ui_description(func, "Switch a channel of the layer on or off");
  RNA_def_function_flag(func, FUNC_SELF_AS_RNA | FUNC_USE_REPORTS);
  parm = RNA_def_enum(func,
                      "channel",
                      rna_enum_material_paint_channel_items,
                      PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                      "Channel",
                      "The channel to switch");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  RNA_def_boolean(func, "enabled", true, "Enabled", "Whether the channel takes part");
}

static void rna_def_material_paint_layer_channel(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "MaterialPaintLayerChannel", nullptr);
  RNA_def_struct_sdna(srna, "MaterialPaintLayerChannel");
  RNA_def_struct_ui_text(srna, "Paint Layer Channel", "One channel record of a paint layer");

  /* Read-only: the record is keyed by its channel; the key is set when the record is added. */
  prop = RNA_def_property(srna, "channel", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_funcs(prop, "rna_MaterialPaintLayerChannel_channel_get", nullptr, nullptr);
  RNA_def_property_enum_items(prop, rna_enum_material_paint_channel_items);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Channel", "The paint channel the record stands for");

  /* Read-only: the state changes through the owning layer's channel_set_enabled(). */
  prop = RNA_def_property(srna, "state", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_funcs(prop, "rna_MaterialPaintLayerChannel_state_get", nullptr, nullptr);
  RNA_def_property_enum_items(prop, rna_enum_material_paint_layer_channel_state_items);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "State", "Whether the channel's map is live");

  prop = RNA_def_property(srna, "image", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, nullptr, "image");
  RNA_def_property_struct_type(prop, "Image");
  RNA_def_property_pointer_funcs(
      prop, nullptr, "rna_MaterialPaintLayerChannel_image_set", nullptr, nullptr);
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Image", "The channel's map, or none for a constant");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "value", PROP_FLOAT, PROP_COLOR);
  RNA_def_property_array(prop, 4);
  RNA_def_property_float_funcs(prop,
                               "rna_MaterialPaintLayerChannel_value_get",
                               "rna_MaterialPaintLayerChannel_value_set",
                               nullptr);
  RNA_def_property_ui_text(prop, "Value", "Constant value used while the channel has no map");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");
}

static void rna_def_material_paint_layer_channel_settings(BlenderRNA *brna)
{
  PropertyRNA *prop;
  StructRNA *srna = RNA_def_struct(brna, "MaterialPaintLayerChannelSettings", nullptr);
  RNA_def_struct_sdna(srna, "MaterialPaintLayerChannelSettings");
  RNA_def_struct_path_func(srna, "rna_MaterialPaintLayerChannelSettings_path");
  RNA_def_struct_ui_text(
      srna, "Paint Layer Channel Settings", "Blend and opacity of one (row, channel) pair");

  /* Read-only: the entry is keyed by its position in the layer's fixed array. */
  prop = RNA_def_property(srna, "channel", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_funcs(
      prop, "rna_MaterialPaintLayerChannelSettings_channel_get", nullptr, nullptr);
  RNA_def_property_enum_items(prop, rna_enum_material_paint_channel_items);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Channel", "The paint channel the entry stands for");

  prop = RNA_def_property(srna, "blend_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_funcs(prop,
                              "rna_MaterialPaintLayerChannelSettings_blend_get",
                              "rna_MaterialPaintLayerChannelSettings_blend_set",
                              nullptr);
  RNA_def_property_enum_items(prop, rna_enum_material_paint_layer_channel_blend_items);
  RNA_def_property_ui_text(prop,
                           "Blend Mode",
                           "The channel's own blend, or Inherit to use the row's; the Normal "
                           "channel has no blend of its own");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "opacity", PROP_FLOAT, PROP_PERCENTAGE);
  RNA_def_property_float_funcs(prop,
                               "rna_MaterialPaintLayerChannelSettings_opacity_get",
                               "rna_MaterialPaintLayerChannelSettings_opacity_set",
                               nullptr);
  RNA_def_property_range(prop, 0.0f, 100.0f);
  RNA_def_property_ui_text(prop, "Opacity", "Multiplied by the row's opacity");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");
}

static void rna_def_material_paint_layer_custom_channel(BlenderRNA *brna)
{
  StructRNA *srna = RNA_def_struct(brna, "MaterialPaintLayerCustomChannel", nullptr);
  RNA_def_struct_ui_text(
      srna, "Paint Layer Custom Channel", "A channel a Custom layer's group declares");

  PropertyRNA *prop = RNA_def_property(srna, "channel", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_funcs(
      prop, "rna_MaterialPaintLayerCustomChannel_channel_get", nullptr, nullptr);
  RNA_def_property_enum_items(prop, rna_enum_material_paint_channel_items);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Channel", "The paint channel the group declares");
}

static void rna_def_material_paint_layer_issue(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "MaterialPaintLayerIssue", nullptr);
  RNA_def_struct_ui_text(
      srna, "Paint Layer Issue", "A description problem on a paint layer, computed on read");

  prop = RNA_def_property(srna, "code", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_funcs(prop, "rna_MaterialPaintLayerIssue_code_get", nullptr, nullptr);
  RNA_def_property_enum_items(prop, rna_enum_material_paint_layer_issue_code_items);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Code", "What kind of problem this is");

  prop = RNA_def_property(srna, "channel", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_funcs(prop, "rna_MaterialPaintLayerIssue_channel_get", nullptr, nullptr);
  RNA_def_property_enum_items(prop, rna_enum_material_paint_channel_items);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Channel", "Channel the issue is about");

  prop = RNA_def_property(srna, "layer_marker", PROP_STRING, PROP_NONE);
  RNA_def_property_string_funcs(prop,
                                "rna_MaterialPaintLayerIssue_layer_marker_get",
                                "rna_MaterialPaintLayerIssue_layer_marker_length",
                                nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Layer Marker", "Marker of the layer the issue is about");

  prop = RNA_def_property(srna, "correction_marker", PROP_STRING, PROP_NONE);
  RNA_def_property_string_funcs(prop,
                                "rna_MaterialPaintLayerIssue_correction_marker_get",
                                "rna_MaterialPaintLayerIssue_correction_marker_length",
                                nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop, "Correction Marker", "Marker of the correction the issue is about, or empty");

  prop = RNA_def_property(srna, "text", PROP_STRING, PROP_NONE);
  RNA_def_property_string_funcs(prop,
                                "rna_MaterialPaintLayerIssue_text_get",
                                "rna_MaterialPaintLayerIssue_text_length",
                                nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Text", "Human-readable description of the issue");
}

static void rna_def_material_paint_layer_properties(BlenderRNA *brna)
{
  StructRNA *srna;

  srna = RNA_def_struct(brna, "MaterialPaintLayerProperties", nullptr);
  RNA_def_struct_ui_text(
      srna, "Paint Layer Properties", "Custom group input values and add-on data of a layer");
  RNA_def_struct_refine_func(srna, "rna_MaterialPaintLayerProperties_refine");
  RNA_def_struct_system_idprops_func(srna, "rna_MaterialPaintLayerProperties_idprops");
}

static void rna_def_material_paint_layers(BlenderRNA *brna, PropertyRNA *cprop)
{
  StructRNA *srna;
  PropertyRNA *prop;
  FunctionRNA *func;
  PropertyRNA *parm;

  RNA_def_property_srna(cprop, "MaterialPaintLayers");
  srna = RNA_def_struct(brna, "MaterialPaintLayers", nullptr);
  RNA_def_struct_sdna(srna, "Material");
  RNA_def_struct_ui_text(srna, "Paint Layers", "Collection of a material's paint layers");

  /* The functions operate on the owning #Material, so every structural change can go through the
   * BKE description API rather than writing DNA from RNA. */
  func = RNA_def_function(srna, "new", "rna_Material_paint_layers_new");
  RNA_def_function_ui_description(func, "Add a paint layer on top of the stack");
  parm = RNA_def_enum(func,
                      "source",
                      rna_enum_material_paint_layer_source_items,
                      MA_PAINT_LAYER_SOURCE_IMAGE,
                      "Source",
                      "Source of the new layer");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  RNA_def_string(func, "name", "Layer", MAX_NAME, "Name", "Name of the new layer");
  parm = RNA_def_pointer(func, "layer", "MaterialPaintLayer", "", "The newly created paint layer");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "remove", "rna_Material_paint_layers_remove");
  RNA_def_function_ui_description(func, "Remove a paint layer");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  parm = RNA_def_pointer(
      func, "layer", "MaterialPaintLayer", "Paint Layer", "The paint layer to remove");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED | PARM_RNAPTR);
  RNA_def_parameter_clear_flags(parm, PROP_THICK_WRAP, ParameterFlag(0));

  func = RNA_def_function(srna, "find", "rna_Material_paint_layers_find");
  RNA_def_function_ui_description(func, "Find a paint layer by its marker");
  parm = RNA_def_string(func, "marker", nullptr, 0, "Marker", "Marker of the layer to find");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_pointer(
      func, "layer", "MaterialPaintLayer", "", "The layer carrying the given marker");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "composite", "rna_Material_paint_layers_composite");
  RNA_def_function_ui_description(
      func, "Composite one channel of the stack into an image on the CPU");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  parm = RNA_def_enum(func,
                      "channel",
                      rna_enum_material_paint_channel_items,
                      PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                      "Channel",
                      "Channel to composite");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_pointer(func, "image", "Image", "Image", "Destination image");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED | PARM_RNAPTR);
  RNA_def_parameter_clear_flags(parm, PROP_THICK_WRAP, ParameterFlag(0));
  parm = RNA_def_boolean(func, "result", false, "", "Whether the channel was composited");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "move", "rna_Material_paint_layers_move");
  RNA_def_function_ui_description(
      func, "Move a paint layer, with everything nested under it, relative to an anchor");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  parm = RNA_def_pointer(func, "layer", "MaterialPaintLayer", "Paint Layer", "The layer to move");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED | PARM_RNAPTR);
  RNA_def_parameter_clear_flags(parm, PROP_THICK_WRAP, ParameterFlag(0));
  parm = RNA_def_pointer(
      func, "anchor", "MaterialPaintLayer", "", "The anchor, or none for the top of the stack");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_RNAPTR);
  RNA_def_parameter_clear_flags(parm, PROP_THICK_WRAP, ParameterFlag(0));
  parm = RNA_def_enum(func,
                      "place",
                      rna_enum_material_paint_layer_place_items,
                      0,
                      "Place",
                      "Where to put the layer relative to the anchor");

  func = RNA_def_function(srna, "reorder", "rna_Material_paint_layers_reorder");
  RNA_def_function_ui_description(
      func, "Move a paint layer within its own list, keeping its nesting");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  parm = RNA_def_pointer(
      func, "layer", "MaterialPaintLayer", "Paint Layer", "The layer to reorder");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED | PARM_RNAPTR);
  RNA_def_parameter_clear_flags(parm, PROP_THICK_WRAP, ParameterFlag(0));
  RNA_def_int(func, "index", 0, 0, INT_MAX, "Index", "Sibling position, bottom to top", 0, INT_MAX);

  func = RNA_def_function(srna, "duplicate", "rna_Material_paint_layers_duplicate");
  RNA_def_function_ui_description(
      func, "Deep-copy a paint layer branch into a row directly above it");
  RNA_def_function_flag(func, FUNC_USE_MAIN | FUNC_USE_REPORTS);
  parm = RNA_def_pointer(
      func, "layer", "MaterialPaintLayer", "Paint Layer", "The layer to duplicate");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED | PARM_RNAPTR);
  RNA_def_parameter_clear_flags(parm, PROP_THICK_WRAP, ParameterFlag(0));
  parm = RNA_def_pointer(func, "copy", "MaterialPaintLayer", "", "The copy");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "group", "rna_Material_paint_layers_group");
  RNA_def_function_ui_description(func, "Fold paint layers into a new folder layer");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  parm = RNA_def_pointer(func, "layer", "MaterialPaintLayer", "Paint Layer", "A layer to group");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED | PARM_RNAPTR);
  RNA_def_parameter_clear_flags(parm, PROP_THICK_WRAP, ParameterFlag(0));
  parm = RNA_def_pointer(
      func, "with_layer", "MaterialPaintLayer", "", "A second layer to group, or none");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_RNAPTR);
  RNA_def_parameter_clear_flags(parm, PROP_THICK_WRAP, ParameterFlag(0));
  parm = RNA_def_pointer(func, "folder", "MaterialPaintLayer", "", "The new folder");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "ungroup", "rna_Material_paint_layers_ungroup");
  RNA_def_function_ui_description(func, "Lift the rows of a folder and free the folder");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  parm = RNA_def_pointer(func, "folder", "MaterialPaintLayer", "Paint Layer", "The folder");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED | PARM_RNAPTR);
  RNA_def_parameter_clear_flags(parm, PROP_THICK_WRAP, ParameterFlag(0));

  func = RNA_def_function(srna, "regenerate", "rna_Material_paint_layers_regenerate");
  RNA_def_function_ui_description(
      func, "Rebuild the material's generated node tree from its layer description");
  RNA_def_function_flag(func, FUNC_USE_MAIN);

  prop = RNA_def_property(srna, "active", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "MaterialPaintLayer");
  RNA_def_property_pointer_funcs(prop,
                                 "rna_Material_paint_layers_active_get",
                                 "rna_Material_paint_layers_active_set",
                                 nullptr,
                                 nullptr);
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Active Layer", "Active paint layer of the material");
}

/* The mesh map RNA helpers that call BKE live in the runtime half (the definition pass must not
 * depend on BKE); only the registration below is here. */

static void rna_def_material_mesh_maps(BlenderRNA *brna, StructRNA *srna)
{
  StructRNA *coll_srna;
  PropertyRNA *prop;
  FunctionRNA *func;
  PropertyRNA *parm;

  prop = RNA_def_property(srna, "mesh_map_slots", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_struct_type(prop, "MaterialMeshMapSlot");
  RNA_def_property_collection_funcs(prop,
                                    "rna_Material_mesh_map_slots_begin",
                                    "rna_iterator_listbase_next",
                                    "rna_iterator_listbase_end",
                                    "rna_iterator_listbase_get",
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    nullptr);
  RNA_def_property_ui_text(
      prop, "Mesh Map Slots", "The material's shared mesh map atlases, one per map type");

  RNA_def_property_srna(prop, "MaterialMeshMapSlots");
  coll_srna = RNA_def_struct(brna, "MaterialMeshMapSlots", nullptr);
  RNA_def_struct_sdna(coll_srna, "Material");
  RNA_def_struct_ui_text(coll_srna, "Mesh Map Slots", "Collection of mesh map atlas slots");

  func = RNA_def_function(coll_srna, "ensure", "rna_Material_mesh_map_slots_ensure");
  RNA_def_function_ui_description(func, "Get the slot for a map type, creating it if absent");
  parm = RNA_def_enum(func,
                      "type",
                      rna_enum_material_mesh_map_type_items,
                      MA_MESH_MAP_AO,
                      "Type",
                      "The mesh map type");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_pointer(
      func, "slot", "MaterialMeshMapSlot", "", "The mesh map slot");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(coll_srna, "find", "rna_Material_mesh_map_slots_find");
  RNA_def_function_ui_description(func, "Get the slot for a map type, or none");
  parm = RNA_def_enum(func,
                      "type",
                      rna_enum_material_mesh_map_type_items,
                      MA_MESH_MAP_AO,
                      "Type",
                      "The mesh map type");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_pointer(
      func, "slot", "MaterialMeshMapSlot", "", "The mesh map slot, or none");
  RNA_def_function_return(func, parm);

  /* slot */
  srna = RNA_def_struct(brna, "MaterialMeshMapSlot", nullptr);
  RNA_def_struct_sdna(srna, "MaterialMeshMapSlot");
  RNA_def_struct_ui_text(srna, "Mesh Map Slot", "One shared mesh map atlas");

  prop = RNA_def_property(srna, "type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_material_mesh_map_type_items);
  RNA_def_property_enum_funcs(prop, "rna_MaterialMeshMapSlot_type_get", nullptr, nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Type", "The mesh map this slot holds");

  prop = RNA_def_property(srna, "image", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "Image");
  RNA_def_property_pointer_sdna(prop, nullptr, "image");
  RNA_def_property_pointer_funcs(prop, nullptr, "rna_MaterialMeshMapSlot_image_set", nullptr, nullptr);
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Image", "The shared atlas for this map type, or none");
  /* The atlas is a map a MESH_MAP row reads, so changing it is a structural edit: the generated
   * tree has to be rebuilt. Same update a channel map's setter uses. */
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  /* settings */
  prop = RNA_def_property(srna, "mesh_map_settings", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "MaterialMeshMapSettings");
  RNA_def_property_pointer_funcs(prop, "rna_Material_mesh_map_settings_get", nullptr, nullptr, nullptr);
  RNA_def_property_ui_text(
      prop, "Mesh Map Settings", "Baking and viewport settings shared by the mesh maps");

  srna = RNA_def_struct(brna, "MaterialMeshMapSettings", nullptr);
  RNA_def_struct_sdna(srna, "MaterialMeshMapSettings");
  RNA_def_struct_ui_text(srna, "Mesh Map Settings", "Baking and viewport settings of mesh maps");

  prop = RNA_def_property(srna, "resolution", PROP_INT, PROP_NONE);
  RNA_def_property_int_sdna(prop, nullptr, "resolution");
  RNA_def_property_range(prop, 16, 16384);
  RNA_def_property_ui_text(prop, "Resolution", "Square side the atlas is allocated at");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "samples", PROP_INT, PROP_NONE);
  RNA_def_property_int_sdna(prop, nullptr, "samples");
  RNA_def_property_range(prop, 1, 4096);
  RNA_def_property_ui_text(prop, "Samples", "Cycles samples the mesh map bake renders with");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "use_denoise", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "use_denoise", 1);
  RNA_def_property_ui_text(prop, "Denoise", "Ask Cycles to denoise the mesh map bake");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "margin", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, nullptr, "margin");
  RNA_def_property_range(prop, 0.0f, 256.0f);
  RNA_def_property_ui_text(prop, "Margin", "UV margin in pixels applied while merging a bake");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "ao_distance", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, nullptr, "ao_distance");
  RNA_def_property_range(prop, 0.0f, FLT_MAX);
  RNA_def_property_ui_text(prop, "AO Distance", "Ambient occlusion ray distance; zero is automatic");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "edge_radius", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, nullptr, "edge_radius");
  RNA_def_property_range(prop, 0.0f, FLT_MAX);
  RNA_def_property_ui_text(prop, "Edge Radius", "Edge/bevel ray radius in object-space units");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");
}

/** \} */

static void rna_def_material_display(StructRNA *srna)
{
  PropertyRNA *prop;

  prop = RNA_def_property(srna, "diffuse_color", PROP_FLOAT, PROP_COLOR);
  RNA_def_property_float_sdna(prop, nullptr, "r");
  RNA_def_property_array(prop, 4);
  RNA_def_property_override_flag(prop, PROPOVERRIDE_OVERRIDABLE_LIBRARY);
  RNA_def_property_ui_text(prop, "Diffuse Color", "Diffuse color of the material");
  /* See #82514 for details, for now re-define defaults here. Keep in sync with
   * #DNA_material_defaults.h */
  static const float diffuse_color_default[4] = {0.8f, 0.8f, 0.8f, 1.0f};
  RNA_def_property_float_array_default(prop, diffuse_color_default);
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");

  prop = RNA_def_property(srna, "specular_color", PROP_FLOAT, PROP_COLOR);
  RNA_def_property_float_sdna(prop, nullptr, "specr");
  RNA_def_property_array(prop, 3);
  RNA_def_property_override_flag(prop, PROPOVERRIDE_OVERRIDABLE_LIBRARY);
  RNA_def_property_ui_text(prop, "Specular Color", "Specular color of the material");
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");

  prop = RNA_def_property(srna, "roughness", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "roughness");
  RNA_def_property_range(prop, 0, 1);
  RNA_def_property_override_flag(prop, PROPOVERRIDE_OVERRIDABLE_LIBRARY);
  RNA_def_property_ui_text(prop, "Roughness", "Roughness of the material");
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");

  prop = RNA_def_property(srna, "specular_intensity", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "spec");
  RNA_def_property_range(prop, 0, 1);
  RNA_def_property_override_flag(prop, PROPOVERRIDE_OVERRIDABLE_LIBRARY);
  RNA_def_property_ui_text(prop, "Specular", "How intense (bright) the specular reflection is");
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");

  prop = RNA_def_property(srna, "metallic", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "metallic");
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_override_flag(prop, PROPOVERRIDE_OVERRIDABLE_LIBRARY);
  RNA_def_property_ui_text(prop, "Metallic", "Amount of mirror reflection for raytrace");
  RNA_def_property_update(prop, 0, "rna_Material_update");

  /* Freestyle line color */
  prop = RNA_def_property(srna, "line_color", PROP_FLOAT, PROP_COLOR);
  RNA_def_property_float_sdna(prop, nullptr, "line_col");
  RNA_def_property_array(prop, 4);
  RNA_def_property_ui_text(prop, "Line Color", "Line color used for Freestyle line rendering");
  RNA_def_property_update(prop, 0, "rna_Material_update");

  prop = RNA_def_property(srna, "line_priority", PROP_INT, PROP_NONE);
  RNA_def_property_int_sdna(prop, nullptr, "line_priority");
  RNA_def_property_range(prop, 0, 32767);
  RNA_def_property_ui_text(
      prop, "Line Priority", "The line color of a higher priority is used at material boundaries");
  RNA_def_property_update(prop, 0, "rna_Material_update");
}

static void rna_def_material_greasepencil(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  /* mode type styles */
  static const EnumPropertyItem gpcolordata_mode_types_items[] = {
      {GP_MATERIAL_MODE_LINE, "LINE", 0, "Line", "Draw strokes using a continuous line"},
      {GP_MATERIAL_MODE_DOT, "DOTS", 0, "Dots", "Draw strokes using separated dots"},
      {GP_MATERIAL_MODE_SQUARE, "BOX", 0, "Squares", "Draw strokes using separated squares"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  /* stroke styles */
  static const EnumPropertyItem stroke_style_items[] = {
      {GP_MATERIAL_STROKE_STYLE_SOLID, "SOLID", 0, "Solid", "Draw strokes with solid color"},
      {GP_MATERIAL_STROKE_STYLE_TEXTURE, "TEXTURE", 0, "Texture", "Draw strokes using texture"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  /* fill styles */
  static const EnumPropertyItem fill_style_items[] = {
      {GP_MATERIAL_FILL_STYLE_SOLID, "SOLID", 0, "Solid", "Fill area with solid color"},
      {GP_MATERIAL_FILL_STYLE_GRADIENT,
       "GRADIENT",
       0,
       "Gradient",
       "Fill area with gradient color"},
      {GP_MATERIAL_FILL_STYLE_TEXTURE, "TEXTURE", 0, "Texture", "Fill area with image texture"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem fill_gradient_items[] = {
      {GP_MATERIAL_GRADIENT_LINEAR, "LINEAR", 0, "Linear", "Fill area with gradient color"},
      {GP_MATERIAL_GRADIENT_RADIAL, "RADIAL", 0, "Radial", "Fill area with radial gradient"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem alignment_draw_items[] = {
      {GP_MATERIAL_FOLLOW_PATH,
       "PATH",
       0,
       "Path",
       "Follow stroke drawing path and object rotation"},
      {GP_MATERIAL_FOLLOW_OBJ, "OBJECT", 0, "Object", "Follow object rotation only"},
      {GP_MATERIAL_FOLLOW_FIXED,
       "FIXED",
       0,
       "Fixed",
       "Do not follow drawing path or object rotation and keeps aligned with viewport"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static EnumPropertyItem placement_mode_items[] = {
      {GP_MATERIAL_PLACEMENT_COUNT,
       "COUNT",
       0,
       "Count",
       "Place dots evenly along each segment of the stroke"},
      {GP_MATERIAL_PLACEMENT_RADIUS,
       "RADIUS",
       0,
       "Radius",
       "Place dots evenly with respect to radius"},
      {GP_MATERIAL_PLACEMENT_DENSITY,
       "DENSITY",
       0,
       "Density",
       "Place dots evenly along the length of the stroke"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  srna = RNA_def_struct(brna, "MaterialGPencilStyle", nullptr);
  RNA_def_struct_sdna(srna, "MaterialGPencilStyle");
  RNA_def_struct_ui_text(srna, "Grease Pencil Color", "");
  RNA_def_struct_path_func(srna, "rna_GpencilColorData_path");

  prop = RNA_def_property(srna, "color", PROP_FLOAT, PROP_COLOR);
  RNA_def_property_float_sdna(prop, nullptr, "stroke_rgba");
  RNA_def_property_array(prop, 4);
  RNA_def_property_range(prop, 0.0f, FLT_MAX);
  RNA_def_property_ui_range(prop, 0.0f, 1.0f, 0.1f, 3);
  RNA_def_property_ui_text(prop, "Color", "");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");

  /* Fill Drawing Color */
  prop = RNA_def_property(srna, "fill_color", PROP_FLOAT, PROP_COLOR);
  RNA_def_property_float_sdna(prop, nullptr, "fill_rgba");
  RNA_def_property_array(prop, 4);
  RNA_def_property_range(prop, 0.0f, FLT_MAX);
  RNA_def_property_ui_range(prop, 0.0f, 1.0f, 0.1f, 3);
  RNA_def_property_ui_text(prop, "Fill Color", "Color for filling region bounded by each stroke");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");

  /* Secondary Drawing Color */
  prop = RNA_def_property(srna, "mix_color", PROP_FLOAT, PROP_COLOR);
  RNA_def_property_float_sdna(prop, nullptr, "mix_rgba");
  RNA_def_property_array(prop, 4);
  RNA_def_property_range(prop, 0.0f, FLT_MAX);
  RNA_def_property_ui_range(prop, 0.0f, 1.0f, 0.1f, 3);
  RNA_def_property_ui_text(prop, "Mix Color", "Color for mixing with primary filling color");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");

  /* Mix factor */
  prop = RNA_def_property(srna, "mix_factor", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "mix_factor");
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_text(prop, "Mix", "Mix Factor");
  RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_ID_GPENCIL);
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");

  /* Stroke Mix factor */
  prop = RNA_def_property(srna, "mix_stroke_factor", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "mix_stroke_factor");
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_text(prop, "Mix", "Mix Stroke Factor");
  RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_ID_GPENCIL);
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");

  /* Texture angle */
  prop = RNA_def_property(srna, "texture_angle", PROP_FLOAT, PROP_ANGLE);
  RNA_def_property_float_sdna(prop, nullptr, "texture_angle");
  RNA_def_property_ui_text(prop, "Angle", "Texture Orientation Angle");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");

  /* Scale factor for texture */
  prop = RNA_def_property(srna, "texture_scale", PROP_FLOAT, PROP_COORDS);
  RNA_def_property_float_sdna(prop, nullptr, "texture_scale");
  RNA_def_property_array(prop, 2);
  RNA_def_property_ui_text(prop, "Scale", "Scale Factor for Texture");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");

  /* Shift factor to move texture in 2d space */
  prop = RNA_def_property(srna, "texture_offset", PROP_FLOAT, PROP_COORDS);
  RNA_def_property_float_sdna(prop, nullptr, "texture_offset");
  RNA_def_property_array(prop, 2);
  RNA_def_property_ui_text(prop, "Offset", "Shift Texture in 2d Space");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");

  /* texture pixsize factor (used for UV along the stroke) */
  prop = RNA_def_property(srna, "pixel_size", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, nullptr, "texture_pixsize");
  RNA_def_property_range(prop, 1, 5000);
  RNA_def_property_ui_text(prop, "UV Factor", "Texture Pixel Size factor along the stroke");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");

  /* Flags */
  prop = RNA_def_property(srna, "hide", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", GP_MATERIAL_HIDE);
  RNA_def_property_ui_icon(prop, ICON_HIDE_OFF, -1);
  RNA_def_property_ui_text(prop, "Hide", "Set color Visibility");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");

  prop = RNA_def_property(srna, "lock", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", GP_MATERIAL_LOCKED);
  RNA_def_property_ui_icon(prop, ICON_UNLOCKED, 1);
  RNA_def_property_ui_text(
      prop, "Locked", "Protect color from further editing and/or frame changes");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");

  prop = RNA_def_property(srna, "ghost", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", GP_MATERIAL_HIDE_ONIONSKIN);
  RNA_def_property_ui_icon(prop, ICON_GHOST_ENABLED, 0);
  RNA_def_property_ui_text(
      prop, "Show in Ghosts", "Display strokes using this color when showing onion skins");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");

  prop = RNA_def_property(srna, "texture_clamp", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", GP_MATERIAL_TEX_CLAMP);
  RNA_def_property_ui_text(prop, "Clamp", "Do not repeat texture and clamp to one instance only");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");

  prop = RNA_def_property(srna, "flip", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", GP_MATERIAL_FLIP_FILL);
  RNA_def_property_ui_text(prop, "Flip", "Flip filling colors");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");

  prop = RNA_def_property(srna, "use_overlap_strokes", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", GP_MATERIAL_DISABLE_STENCIL);
  RNA_def_property_ui_text(
      prop, "Self Overlap", "Disable stencil and overlap self intersections with alpha materials");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");

  prop = RNA_def_property(srna, "use_stroke_holdout", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", GP_MATERIAL_IS_STROKE_HOLDOUT);
  RNA_def_property_ui_text(
      prop, "Holdout", "Remove the color from underneath this stroke by using it as a mask");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");

  prop = RNA_def_property(srna, "use_fill_holdout", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", GP_MATERIAL_IS_FILL_HOLDOUT);
  RNA_def_property_ui_text(
      prop, "Holdout", "Remove the color from underneath this stroke by using it as a mask");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");

  prop = RNA_def_property(srna, "show_stroke", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", GP_MATERIAL_STROKE_SHOW);
  RNA_def_property_ui_text(prop, "Show Stroke", "Show stroke lines of this material");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");
  RNA_def_property_deprecated(
      prop, "Unused but kept for compatibility with older versions of Blender.", 510, 600);

  prop = RNA_def_property(srna, "show_fill", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", GP_MATERIAL_FILL_SHOW);
  RNA_def_property_ui_text(prop, "Show Fill", "Show stroke fills of this material");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");
  RNA_def_property_deprecated(
      prop, "Unused but kept for compatibility with older versions of Blender.", 510, 600);

  /* Mode to align Dots and Boxes to drawing path and object rotation */
  prop = RNA_def_property(srna, "alignment_mode", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_bitflag_sdna(prop, nullptr, "alignment_mode");
  RNA_def_property_enum_items(prop, alignment_draw_items);
  RNA_def_property_ui_text(
      prop, "Alignment", "Defines how align Dots and Boxes with drawing path and object rotation");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");

  /* Rotation of texture for Dots or Strokes. */
  prop = RNA_def_property(srna, "alignment_rotation", PROP_FLOAT, PROP_ANGLE);
  RNA_def_property_float_sdna(prop, nullptr, "alignment_rotation");
  RNA_def_property_float_default(prop, 0.0f);
  RNA_def_property_range(prop, -DEG2RADF(90.0f), DEG2RADF(90.0f));
  RNA_def_property_ui_range(prop, -DEG2RADF(90.0f), DEG2RADF(90.0f), 10, 3);
  RNA_def_property_ui_text(
      prop, "Rotation", "Additional rotation applied to dots and square texture of strokes");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");

  /* Placement mode for Dots and Squares. */
  prop = RNA_def_property(srna, "placement_mode", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_bitflag_sdna(prop, nullptr, "placement_mode");
  RNA_def_property_enum_items(prop, placement_mode_items);
  RNA_def_property_enum_default(prop, GP_MATERIAL_PLACEMENT_RADIUS);
  RNA_def_property_ui_text(
      prop, "Placement", "Defines how Dots or Squares are placed along strokes");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");

  /* Placement count. */
  prop = RNA_def_property(srna, "placement_count", PROP_INT, PROP_NONE);
  RNA_def_property_int_sdna(prop, nullptr, "placement_count");
  RNA_def_property_range(prop, 1, INT_MAX);
  RNA_def_property_ui_text(prop, "Count", "Number of dots placed per segment");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");

  /* Placement radius factor. */
  prop = RNA_def_property(srna, "placement_radius_spacing", PROP_FLOAT, PROP_PERCENTAGE);
  RNA_def_property_float_sdna(prop, nullptr, "placement_radius_spacing");
  RNA_def_property_float_default(prop, 100.0f);
  RNA_def_property_range(prop, 0.0f, FLT_MAX);
  RNA_def_property_ui_range(prop, 1.0f, 200.0f, 10, 0);
  RNA_def_property_ui_text(
      prop, "Spacing", "Spacing between dots as a percentage of the diameter");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");

  /* Placement density. */
  prop = RNA_def_property(srna, "placement_density", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, nullptr, "placement_density");
  RNA_def_property_float_default(prop, 10.0f);
  RNA_def_property_range(prop, 0.0f, FLT_MAX);
  RNA_def_property_ui_text(prop, "Density", "Density of dots along the stroke");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");

  /* Use Randomization. */
  prop = RNA_def_property(srna, "use_randomization", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", GP_MATERIAL_USE_DOTS_RANDOMIZATION);
  RNA_def_property_ui_text(prop, "Randomization", "Use material randomization");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");

  /* Random Size. */
  prop = RNA_def_property(srna, "random_size_factor", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, nullptr, "random_size_factor");
  RNA_def_property_float_default(prop, 0.0f);
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_text(prop, "Size", "Randomize the size");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");

  /* Random Strength. */
  prop = RNA_def_property(srna, "random_strength_factor", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, nullptr, "random_strength_factor");
  RNA_def_property_float_default(prop, 0.0f);
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_text(prop, "Strength", "Randomize strength");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");

  /* Random Rotation. */
  prop = RNA_def_property(srna, "random_rotation_factor", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, nullptr, "random_rotation_factor");
  RNA_def_property_float_default(prop, 0.0f);
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_text(prop, "Rotation", "Randomize texture rotation");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");

  /* Random Color Hue. */
  prop = RNA_def_property(srna, "random_hue_factor", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, nullptr, "random_hue_factor");
  RNA_def_property_float_default(prop, 0.0f);
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_text(prop, "Hue", "Randomize color hue");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");

  /* Random Color Saturation. */
  prop = RNA_def_property(srna, "random_saturation_factor", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, nullptr, "random_saturation_factor");
  RNA_def_property_float_default(prop, 0.0f);
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_text(prop, "Saturation", "Randomize color saturation");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");

  /* Random Color Value. */
  prop = RNA_def_property(srna, "random_value_factor", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, nullptr, "random_value_factor");
  RNA_def_property_float_default(prop, 0.0f);
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_text(prop, "Value", "Randomize color value");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");

  /* Random Noise Scale. */
  prop = RNA_def_property(srna, "random_noise_scale", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, nullptr, "random_noise_scale");
  RNA_def_property_float_default(prop, 1.0f);
  RNA_def_property_range(prop, 0.0f, FLT_MAX);
  RNA_def_property_ui_range(prop, 0.0f, 2.0f, 0.1f, 3);
  RNA_def_property_ui_text(prop, "Noise Scale", "Scale the noise frequency");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");

  /* pass index for future compositing and editing tools */
  prop = RNA_def_property(srna, "pass_index", PROP_INT, PROP_UNSIGNED);
  RNA_def_property_int_sdna(prop, nullptr, "index");
  RNA_def_property_ui_text(prop, "Pass Index", "Index number for the \"Color Index\" pass");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");

  /* mode type */
  prop = RNA_def_property(srna, "mode", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_bitflag_sdna(prop, nullptr, "mode");
  RNA_def_property_enum_items(prop, gpcolordata_mode_types_items);
  RNA_def_property_ui_text(prop, "Line Type", "Select line type for strokes");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");

  /* stroke style */
  prop = RNA_def_property(srna, "stroke_style", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_bitflag_sdna(prop, nullptr, "stroke_style");
  RNA_def_property_enum_items(prop, stroke_style_items);
  RNA_def_property_ui_text(prop, "Stroke Style", "Select style used to draw strokes");
  RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_ID_GPENCIL);
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");

  /* stroke image texture */
  prop = RNA_def_property(srna, "stroke_image", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, nullptr, "sima");
  RNA_def_property_pointer_funcs(
      prop, nullptr, "rna_GpencilColorData_stroke_image_set", nullptr, nullptr);
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_override_flag(prop, PROPOVERRIDE_OVERRIDABLE_LIBRARY);
  RNA_def_property_ui_text(prop, "Image", "");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");

  /* fill style */
  prop = RNA_def_property(srna, "fill_style", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_bitflag_sdna(prop, nullptr, "fill_style");
  RNA_def_property_enum_items(prop, fill_style_items);
  RNA_def_property_ui_text(prop, "Fill Style", "Select style used to fill strokes");
  RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_ID_GPENCIL);
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");

  /* gradient type */
  prop = RNA_def_property(srna, "gradient_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_bitflag_sdna(prop, nullptr, "gradient_type");
  RNA_def_property_enum_items(prop, fill_gradient_items);
  RNA_def_property_ui_text(prop, "Gradient Type", "Select type of gradient used to fill strokes");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");

  /* fill image texture */
  prop = RNA_def_property(srna, "fill_image", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, nullptr, "ima");
  RNA_def_property_pointer_funcs(
      prop, nullptr, "rna_GpencilColorData_fill_image_set", nullptr, nullptr);
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_override_flag(prop, PROPOVERRIDE_OVERRIDABLE_LIBRARY);
  RNA_def_property_ui_text(prop, "Image", "");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialGpencil_update");

  /* Read-only state props (for simpler UI code) */
  prop = RNA_def_property(srna, "is_stroke_visible", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop, "rna_GpencilColorData_is_stroke_visible_get", nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop, "Is Stroke Visible", "True when opacity of stroke is set high enough to be visible");

  prop = RNA_def_property(srna, "is_fill_visible", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop, "rna_GpencilColorData_is_fill_visible_get", nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop, "Is Fill Visible", "True when opacity of fill is set high enough to be visible");
}
static void rna_def_material_lineart(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "MaterialLineArt", nullptr);
  RNA_def_struct_sdna(srna, "MaterialLineArt");
  RNA_def_struct_ui_text(srna, "Material Line Art", "");
  RNA_def_struct_path_func(srna, "rna_MaterialLineArt_path");

  prop = RNA_def_property(srna, "use_material_mask", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_default(prop, false);
  RNA_def_property_boolean_sdna(prop, nullptr, "flags", LRT_MATERIAL_MASK_ENABLED);
  RNA_def_property_ui_text(
      prop, "Use Material Mask", "Use material masks to filter out occluded strokes");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialLineArt_update");

  prop = RNA_def_property(srna, "use_material_mask_bits", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_default(prop, false);
  RNA_def_property_boolean_bitset_array_sdna(prop, nullptr, "material_mask_bits", 1 << 0, 8);
  RNA_def_property_ui_text(prop, "Mask", "");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialLineArt_update");

  prop = RNA_def_property(srna, "mat_occlusion", PROP_INT, PROP_NONE);
  RNA_def_property_ui_range(prop, 0.0f, 5.0f, 1.0f, 1);
  RNA_def_property_ui_text(
      prop,
      "Effectiveness",
      "Faces with this material will behave as if it has set number of layers in occlusion");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialLineArt_update");

  prop = RNA_def_property(srna, "intersection_priority", PROP_INT, PROP_NONE);
  RNA_def_property_range(prop, 0, 255);
  RNA_def_property_ui_text(prop,
                           "Intersection Priority",
                           "The intersection line will be included into the object with the "
                           "higher intersection priority value");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialLineArt_update");

  prop = RNA_def_property(srna, "use_intersection_priority_override", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_default(prop, false);
  RNA_def_property_boolean_sdna(prop, nullptr, "flags", LRT_MATERIAL_CUSTOM_INTERSECTION_PRIORITY);
  RNA_def_property_ui_text(prop,
                           "Use Intersection Priority",
                           "Override object and collection intersection priority value");
  RNA_def_property_update(prop, NC_GPENCIL | ND_SHADING, "rna_MaterialLineArt_update");
}

void RNA_def_material(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  /* Render Preview Types */
  static const EnumPropertyItem preview_type_items[] = {
      {MA_FLAT, "FLAT", ICON_MATPLANE, "Flat", "Flat XY plane"},
      {MA_SPHERE, "SPHERE", ICON_MATSPHERE, "Sphere", "Sphere"},
      {MA_CUBE, "CUBE", ICON_MATCUBE, "Cube", "Cube"},
      {MA_HAIR, "HAIR", ICON_CURVES, "Hair", "Hair strands"},
      {MA_SHADERBALL, "SHADERBALL", ICON_MATSHADERBALL, "Shader Ball", "Shader ball"},
      {MA_CLOTH, "CLOTH", ICON_MATCLOTH, "Cloth", "Cloth"},
      {MA_FLUID, "FLUID", ICON_MATFLUID, "Fluid", "Fluid"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem prop_eevee_volume_isect_method_items[] = {
      {MA_VOLUME_ISECT_FAST,
       "FAST",
       0,
       "Fast",
       "Each face is considered as a medium interface. Gives correct results for manifold "
       "geometry that contains no inner parts."},
      {MA_VOLUME_ISECT_ACCURATE,
       "ACCURATE",
       0,
       "Accurate",
       "Faces are considered as medium interface only when they have different consecutive "
       "facing. Gives correct results as long as the max ray depth is not exceeded. Have "
       "significant memory overhead compared to the fast method."},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem prop_eevee_thickness_method_items[] = {
      {MA_THICKNESS_SPHERE,
       "SPHERE",
       0,
       "Sphere",
       "Approximate the object as a sphere whose diameter is equal to the thickness defined by "
       "the node tree"},
      {MA_THICKNESS_SLAB,
       "SLAB",
       0,
       "Slab",
       "Approximate the object as an infinite slab of thickness defined by the node tree"},
      {0, nullptr, 0, nullptr, nullptr},
  };

#  if 1 /* Delete this section once we remove old eevee. */
  static const EnumPropertyItem prop_eevee_blend_items[] = {
      {MA_BM_SOLID, "OPAQUE", 0, "Opaque", "Render surface without transparency"},
      {MA_BM_CLIP,
       "CLIP",
       0,
       "Alpha Clip",
       "Use the alpha threshold to clip the visibility (binary visibility)"},
      {MA_BM_HASHED,
       "HASHED",
       0,
       "Alpha Hashed",
       "Use noise to dither the binary visibility (works well with multi-samples)"},
      {MA_BM_BLEND,
       "BLEND",
       0,
       "Alpha Blend",
       "Render polygon transparent, depending on alpha channel of the texture"},
      {0, nullptr, 0, nullptr, nullptr},
  };
#  endif

  static const EnumPropertyItem prop_eevee_surface_render_method_items[] = {
      {MA_SURFACE_METHOD_DEFERRED,
       "DITHERED",
       0,
       "Dithered",
       "Allows for grayscale hashed transparency, and compatible with render passes and "
       "raytracing. Also known as deferred rendering."},
      {MA_SURFACE_METHOD_FORWARD,
       "BLENDED",
       0,
       "Blended",
       "Allows for colored transparency, but incompatible with render passes and raytracing. Also "
       "known as forward rendering."},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem prop_displacement_method_items[] = {
      {MA_DISPLACEMENT_BUMP,
       "BUMP",
       0,
       "Bump Only",
       "Bump mapping to simulate the appearance of displacement"},
      {MA_DISPLACEMENT_DISPLACE,
       "DISPLACEMENT",
       0,
       "Displacement Only",
       "Use true displacement of surface only, requires fine subdivision"},
      {MA_DISPLACEMENT_BOTH,
       "BOTH",
       0,
       "Displacement and Bump",
       "Combination of true displacement and bump mapping for finer detail"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  srna = RNA_def_struct(brna, "Material", "ID");
  RNA_def_struct_ui_text(
      srna,
      "Material",
      "Material data-block to define the appearance of geometric objects for rendering");
  RNA_def_struct_ui_icon(srna, ICON_MATERIAL_DATA);

  prop = RNA_def_property(srna, "surface_render_method", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, prop_eevee_surface_render_method_items);
  RNA_def_property_ui_text(prop,
                           "Surface Render Method",
                           "Controls the blending and the compatibility with certain features");
  /* Setter function for forward compatibility. */
  RNA_def_property_enum_funcs(prop, nullptr, "rna_Material_render_method_set", nullptr);
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");

  prop = RNA_def_property(srna, "displacement_method", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, prop_displacement_method_items);
  RNA_def_property_ui_text(prop, "Displacement Method", "Method to use for the displacement");
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");

#  if 1 /* Delete this section once we remove old eevee. */
  /* Blending (only Eevee for now) */
  prop = RNA_def_property(srna, "blend_method", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, prop_eevee_blend_items);
  RNA_def_property_ui_text(
      prop,
      "Blend Mode",
      "Blend Mode for Transparent Faces (Deprecated: use 'surface_render_method')");
  RNA_def_property_enum_funcs(
      prop, "rna_Material_blend_method_get", "rna_Material_blend_method_set", nullptr);
  RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_ID_MATERIAL);
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");

  prop = RNA_def_property(srna, "alpha_threshold", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_range(prop, 0, 1);
  RNA_def_property_ui_text(prop,
                           "Clip Threshold",
                           "A pixel is rendered only if its alpha value is above this threshold");
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");
#  endif

  prop = RNA_def_property(srna, "use_transparency_overlap", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_negative_sdna(prop, nullptr, "blend_flag", MA_BL_HIDE_BACKFACE);
  RNA_def_property_ui_text(prop,
                           "Use Transparency Overlap",
                           "Render multiple transparent layers "
                           "(may introduce transparency sorting problems)");

#  if 1 /* This should be deleted in Blender 4.5 */
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");
  prop = RNA_def_property(srna, "show_transparent_back", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_negative_sdna(prop, nullptr, "blend_flag", MA_BL_HIDE_BACKFACE);
  RNA_def_property_ui_text(
      prop,
      "Show Backface",
      "Render multiple transparent layers "
      "(may introduce transparency sorting problems) (Deprecated: use 'use_tranparency_overlap')");
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");
#  endif

  prop = RNA_def_property(srna, "use_backface_culling", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "blend_flag", MA_BL_CULL_BACKFACE);
  RNA_def_property_ui_text(
      prop, "Backface Culling", "Use back face culling to hide the back side of faces");
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");

  prop = RNA_def_property(srna, "use_backface_culling_shadow", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "blend_flag", MA_BL_CULL_BACKFACE_SHADOW);
  RNA_def_property_ui_text(
      prop, "Shadow Backface Culling", "Use back face culling when casting shadows");
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");

  prop = RNA_def_property(srna, "use_backface_culling_lightprobe_volume", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_negative_sdna(
      prop, nullptr, "blend_flag", MA_BL_LIGHTPROBE_VOLUME_DOUBLE_SIDED);
  RNA_def_property_ui_text(
      prop,
      "Light Probe Volume Backface Culling",
      "Consider material single sided for light probe volume capture. "
      "Additionally helps rejecting probes inside the object to avoid light leaks.");
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");

  prop = RNA_def_property(srna, "use_transparent_shadow", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "blend_flag", MA_BL_TRANSPARENT_SHADOW);
  RNA_def_property_boolean_funcs(prop, nullptr, "rna_Material_transparent_shadow_set");
  RNA_def_property_ui_text(
      prop,
      "Transparent Shadows",
      "Use transparent shadows for this material if it contains a Transparent BSDF, "
      "disabling will render faster but not give accurate shadows");
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");

  prop = RNA_def_property(srna, "use_raytrace_refraction", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "blend_flag", MA_BL_SS_REFRACTION);
  RNA_def_property_ui_text(
      prop,
      "Raytrace Transmission",
      "Use raytracing to determine transmitted color instead of using only light probes. "
      "This prevents the surface from contributing to the lighting of surfaces not using this "
      "setting.");
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");

#  if 1 /* This should be deleted in Blender 4.5 */
  prop = RNA_def_property(srna, "use_screen_refraction", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "blend_flag", MA_BL_SS_REFRACTION);
  RNA_def_property_ui_text(
      prop,
      "Raytrace Transmission",
      "Use raytracing to determine transmitted color instead of using only light probes. "
      "This prevents the surface from contributing to the lighting of surfaces not using this "
      "setting. Deprecated: use 'use_raytrace_refraction'.");
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");

  prop = RNA_def_property(srna, "use_sss_translucency", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "blend_flag", MA_BL_TRANSLUCENCY);
  RNA_def_property_ui_text(
      prop, "Subsurface Translucency", "Add translucency effect to subsurface (Deprecated)");
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");

  prop = RNA_def_property(srna, "refraction_depth", PROP_FLOAT, PROP_DISTANCE);
  RNA_def_property_float_sdna(prop, nullptr, "refract_depth");
  RNA_def_property_range(prop, 0.0f, FLT_MAX);
  RNA_def_property_ui_text(prop,
                           "Refraction Depth",
                           "Approximate the thickness of the object to compute two refraction "
                           "events (0 is disabled) (Deprecated)");
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");
#  endif

  prop = RNA_def_property(srna, "thickness_mode", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, prop_eevee_thickness_method_items);
  RNA_def_property_ui_text(prop,
                           "Thickness Mode",
                           "Approximation used to model the light interactions inside the object");
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");

  prop = RNA_def_property(srna, "use_thickness_from_shadow", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "blend_flag", MA_BL_THICKNESS_FROM_SHADOW);
  RNA_def_property_ui_text(prop,
                           "Thickness From Shadow",
                           "Use the shadow maps from shadow casting lights "
                           "to refine the thickness defined by the material node tree");
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");

  prop = RNA_def_property(srna, "volume_intersection_method", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, prop_eevee_volume_isect_method_items);
  RNA_def_property_ui_text(
      prop,
      "Volume Intersection Method",
      "Determines which inner part of the mesh will produce volumetric effect");
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");

  prop = RNA_def_property(srna, "max_vertex_displacement", PROP_FLOAT, PROP_DISTANCE);
  RNA_def_property_float_sdna(prop, nullptr, "inflate_bounds");
  RNA_def_property_range(prop, 0.0f, FLT_MAX);
  RNA_def_property_ui_text(prop,
                           "Max Vertex Displacement",
                           "The max distance a vertex can be displaced. "
                           "Displacements over this threshold may cause visibility issues.");
  RNA_def_property_update(prop, 0, "rna_Material_draw_update");

  /* For Preview Render */
  prop = RNA_def_property(srna, "preview_render_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "pr_type");
  RNA_def_property_enum_items(prop, preview_type_items);
  RNA_def_property_ui_text(prop, "Preview Render Type", "Type of preview render");
  RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_ID_MATERIAL);
  RNA_def_property_update(prop, 0, "rna_Material_update_previews");

  prop = RNA_def_property(srna, "use_preview_world", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "pr_flag", MA_PREVIEW_WORLD);
  RNA_def_property_ui_text(
      prop, "Preview World", "Use the current world background to light the preview render");
  RNA_def_property_update(prop, 0, "rna_Material_update_previews");

  prop = RNA_def_property(srna, "pass_index", PROP_INT, PROP_UNSIGNED);
  RNA_def_property_int_sdna(prop, nullptr, "index");
  RNA_def_property_ui_text(
      prop, "Pass Index", "Index number for the \"Material Index\" render pass");
  RNA_def_property_update(prop, NC_OBJECT, "rna_Material_update");

  /* nodetree */
  prop = RNA_def_property(srna, "node_tree", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, nullptr, "nodetree");
  RNA_def_property_clear_flag(prop, PROP_PTR_NO_OWNERSHIP);
  RNA_def_property_override_flag(prop, PROPOVERRIDE_OVERRIDABLE_LIBRARY);
  RNA_def_property_ui_text(prop, "Node Tree", "Node tree for node based materials");

  prop = RNA_def_property(srna, "use_nodes", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "use_nodes", 1);
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_ui_text(prop, "Use Nodes", "Use shader nodes to render the material");
  RNA_def_property_boolean_funcs(prop, "rna_Material_use_nodes_get", "rna_Material_use_nodes_set");
  RNA_def_property_deprecated(prop,
                              "Unused but kept for compatibility reasons. Setting the property "
                              "has no effect, and getting it always returns True.",
                              500,
                              600);

  /* paint layers */
  prop = RNA_def_property(srna, "paint_layers", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_struct_type(prop, "MaterialPaintLayer");
  RNA_def_property_collection_funcs(prop,
                                    "rna_Material_paint_layers_begin",
                                    "rna_iterator_listbase_next",
                                    "rna_iterator_listbase_end",
                                    "rna_iterator_listbase_get",
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    nullptr);
  RNA_def_property_ui_text(
      prop, "Paint Layers", "The material's paint layer stack, bottom to top");
  rna_def_material_paint_layers(brna, prop);

  /* mesh maps */
  rna_def_material_mesh_maps(brna, srna);

  prop = RNA_def_property(srna, "is_layered", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop, "rna_Material_is_layered_get", nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop,
      "Layered Material",
      "The material's stack is a DNA description and its node tree is generated from it");

  prop = RNA_def_property(srna, "paint_layers_tree_is_stale", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop, "rna_Material_paint_layers_tree_is_stale_get", nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop,
      "Tree Is Stale",
      "The generated node tree does not match the layer description and needs a Regenerate");

  prop = RNA_def_property(srna, "paint_layers_locked", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop,
                                 "rna_Material_paint_layers_locked_get",
                                 "rna_Material_paint_layers_locked_set");
  RNA_def_property_ui_text(
      prop,
      "Locked",
      "The generator owns the node tree and overwrites manual edits; unlock for debugging, then "
      "Regenerate explicitly");

  /* common */
  rna_def_animdata_common(srna);
  rna_def_texpaint_slots(brna, srna);
  rna_def_material_display(srna);

  /* grease pencil */
  prop = RNA_def_property(srna, "grease_pencil", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, nullptr, "gp_style");
  RNA_def_property_ui_text(
      prop, "Grease Pencil Settings", "Grease Pencil color settings for material");

  prop = RNA_def_property(srna, "is_grease_pencil", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop, "rna_is_grease_pencil_get", nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop, "Is Grease Pencil", "True if this material has Grease Pencil data");

  /* line art */
  prop = RNA_def_property(srna, "lineart", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, nullptr, "lineart");
  RNA_def_property_ui_text(prop, "Line Art Settings", "Line Art settings for material");

  rna_def_material_greasepencil(brna);
  rna_def_material_lineart(brna);
  /* The issue and custom-channel structs are referenced by name from the layer, so define them
   * first. */
  rna_def_material_paint_layer_issue(brna);
  rna_def_material_paint_layer_custom_channel(brna);
  rna_def_material_paint_layer(brna);
  rna_def_material_paint_layer_channel(brna);
  rna_def_material_paint_layer_channel_settings(brna);
  rna_def_material_paint_layer_properties(brna);

  RNA_api_material(srna);
}

static void rna_def_texture_slots(BlenderRNA *brna,
                                  PropertyRNA *cprop,
                                  const char *structname,
                                  const char *structname_slots)
{
  StructRNA *srna;

  FunctionRNA *func;
  PropertyRNA *parm;

  RNA_def_property_srna(cprop, structname_slots);
  srna = RNA_def_struct(brna, structname_slots, nullptr);
  RNA_def_struct_sdna(srna, "ID");
  RNA_def_struct_ui_text(srna, "Texture Slots", "Collection of texture slots");

  /* functions */
  func = RNA_def_function(srna, "add", "rna_mtex_texture_slots_add");
  RNA_def_function_flag(func,
                        FUNC_USE_SELF_ID | FUNC_NO_SELF | FUNC_USE_CONTEXT | FUNC_USE_REPORTS);
  parm = RNA_def_pointer(func, "mtex", structname, "", "The newly initialized mtex");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "create", "rna_mtex_texture_slots_create");
  RNA_def_function_flag(func,
                        FUNC_USE_SELF_ID | FUNC_NO_SELF | FUNC_USE_CONTEXT | FUNC_USE_REPORTS);
  parm = RNA_def_int(
      func, "index", 0, 0, INT_MAX, "Index", "Slot index to initialize", 0, INT_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_pointer(func, "mtex", structname, "", "The newly initialized mtex");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "clear", "rna_mtex_texture_slots_clear");
  RNA_def_function_flag(func,
                        FUNC_USE_SELF_ID | FUNC_NO_SELF | FUNC_USE_CONTEXT | FUNC_USE_REPORTS);
  parm = RNA_def_int(func, "index", 0, 0, INT_MAX, "Index", "Slot index to clear", 0, INT_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
}

void rna_def_mtex_common(BlenderRNA *brna,
                         StructRNA *srna,
                         const char *begin,
                         const char *activeget,
                         const char *activeset,
                         const char *activeeditable,
                         const char *structname,
                         const char *structname_slots,
                         const char *update,
                         const char *update_index)
{
  PropertyRNA *prop;

  /* mtex */
  prop = RNA_def_property(srna, "texture_slots", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_struct_type(prop, structname);
  RNA_def_property_collection_funcs(prop,
                                    begin,
                                    "rna_iterator_array_next",
                                    "rna_iterator_array_end",
                                    "rna_iterator_array_dereference_get",
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    nullptr);
  RNA_def_property_ui_text(
      prop, "Textures", "Texture slots defining the mapping and influence of textures");
  rna_def_texture_slots(brna, prop, structname, structname_slots);

  prop = RNA_def_property(srna, "active_texture", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "Texture");
  RNA_def_property_flag(prop, PROP_EDITABLE | PROP_ID_REFCOUNT);
  if (activeeditable) {
    RNA_def_property_editable_func(prop, activeeditable);
  }
  RNA_def_property_pointer_funcs(prop, activeget, activeset, nullptr, nullptr);
  RNA_def_property_ui_text(prop, "Active Texture", "Active texture slot being displayed");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING_LINKS, update);

  prop = RNA_def_property(srna, "active_texture_index", PROP_INT, PROP_UNSIGNED);
  RNA_def_property_int_sdna(prop, nullptr, "texact");
  RNA_def_property_range(prop, 0, MAX_MTEX - 1);
  RNA_def_property_ui_text(prop, "Active Texture Index", "Index of active texture slot");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING_LINKS, update_index);
}

static void rna_def_tex_slot(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "TexPaintSlot", nullptr);
  RNA_def_struct_ui_text(
      srna, "Texture Paint Slot", "Slot that contains information about texture painting");

  prop = RNA_def_property(srna, "name", PROP_STRING, PROP_NONE);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_string_funcs(
      prop, "rna_TexPaintSlot_name_get", "rna_TexPaintSlot_name_length", nullptr);
  RNA_def_property_ui_text(prop, "Name", "Name of the slot");
  RNA_def_struct_name_property(srna, prop);

  prop = RNA_def_property(srna, "icon_value", PROP_INT, PROP_NONE);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_int_funcs(prop, "rna_TexPaintSlot_icon_get", nullptr, nullptr);
  RNA_def_property_ui_text(prop, "Icon", "Paint slot icon");

  prop = RNA_def_property(srna, "uv_layer", PROP_STRING, PROP_NONE);
  RNA_def_property_string_maxlength(
      prop, MAX_CUSTOMDATA_LAYER_NAME_NO_PREFIX); /* Else it uses the pointer size! */
  RNA_def_property_string_sdna(prop, nullptr, "uvname");
  RNA_def_property_string_funcs(prop,
                                "rna_TexPaintSlot_uv_layer_get",
                                "rna_TexPaintSlot_uv_layer_length",
                                "rna_TexPaintSlot_uv_layer_set");
  RNA_def_property_ui_text(prop, "UV Map", "Name of UV map");
  RNA_def_property_update(prop, NC_GEOM | ND_DATA, "rna_Material_update");

  prop = RNA_def_property(srna, "is_valid", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "valid", 1);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Valid", "Slot has a valid image and UV map");
}

void rna_def_texpaint_slots(BlenderRNA *brna, StructRNA *srna)
{
  PropertyRNA *prop;

  rna_def_tex_slot(brna);

  /* mtex */
  prop = RNA_def_property(srna, "texture_paint_images", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_collection_sdna(prop, nullptr, "texpaintslot", nullptr);
  RNA_def_property_collection_funcs(prop,
                                    "rna_Material_texpaint_begin",
                                    "rna_iterator_array_next",
                                    "rna_iterator_array_end",
                                    "rna_iterator_array_dereference_get",
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    nullptr);
  RNA_def_property_struct_type(prop, "Image");
  RNA_def_property_ui_text(
      prop, "Texture Slot Images", "Texture images used for texture painting");

  prop = RNA_def_property(srna, "texture_paint_slots", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_collection_funcs(prop,
                                    "rna_Material_texpaint_begin",
                                    "rna_iterator_array_next",
                                    "rna_iterator_array_end",
                                    "rna_iterator_array_get",
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    nullptr);
  RNA_def_property_struct_type(prop, "TexPaintSlot");
  RNA_def_property_ui_text(
      prop, "Texture Slots", "Texture slots defining the mapping and influence of textures");

  prop = RNA_def_property(srna, "paint_active_slot", PROP_INT, PROP_UNSIGNED);
  RNA_def_property_range(prop, 0, SHRT_MAX);
  RNA_def_property_ui_text(
      prop, "Active Paint Texture Index", "Index of active texture paint slot");
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_update(
      prop, NC_MATERIAL | ND_SHADING_LINKS, "rna_Material_active_paint_texture_index_update");

  prop = RNA_def_property(srna, "paint_clone_slot", PROP_INT, PROP_UNSIGNED);
  RNA_def_property_range(prop, 0, SHRT_MAX);
  RNA_def_property_ui_text(prop, "Clone Paint Texture Index", "Index of clone texture paint slot");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING_LINKS, nullptr);
}

}  // namespace blender

#endif
