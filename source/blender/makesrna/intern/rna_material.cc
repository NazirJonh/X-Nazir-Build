/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup RNA
 */

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cstdlib>

#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_scene_types.h"

#include "BLI_math_rotation.h"

#include "BLT_translation.hh"

#include "BKE_customdata.hh"
#include "BKE_paint.hh"

#include "RNA_define.hh"
#include "RNA_enum_types.hh"

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

/** How a row blends over what is below it. The internal Normal Combine is not offered: the Normal
 * channel forces it, and #BKE_paint_layers_set_blend refuses it. Shared by the definition and the
 * item callback of a mask correction, which drops the modes that read hue. */
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
    {MA_PAINT_LAYER_BLEND_LINEAR_LIGHT, "LINEAR_LIGHT", 0, "Linear Light", "Linear light blend"},
    {MA_PAINT_LAYER_BLEND_HUE, "HUE", 0, "Hue", "Hue blend"},
    {MA_PAINT_LAYER_BLEND_SATURATION, "SATURATION", 0, "Saturation", "Saturation blend"},
    {MA_PAINT_LAYER_BLEND_COLOR, "COLOR", 0, "Color", "Color blend"},
    {MA_PAINT_LAYER_BLEND_VALUE, "VALUE", 0, "Value", "Value blend"},
    {0, nullptr, 0, nullptr, nullptr},
};

/* Sizes a layered material's maps can take. 8K is left out for now: a stack creates one map per
 * channel per layer, so a size that large is not offered for a whole material. Shared by the
 * material's RNA and #MATERIAL_OT_new_layered's dialog. */
const EnumPropertyItem rna_enum_material_paint_layers_image_size_items[] = {
    {0,
     "SCENE",
     0,
     "Scene Default",
     "Follow the scene's New Channel Image Size (what files saved before this setting use)"},
    {PAINT_NEW_CHANNEL_IMAGE_SIZE_256, "SIZE_256", 0, "256 (256 x 256)", "256 x 256"},
    {PAINT_NEW_CHANNEL_IMAGE_SIZE_512, "SIZE_512", 0, "512 (512 x 512)", "512 x 512"},
    {PAINT_NEW_CHANNEL_IMAGE_SIZE_1K, "SIZE_1K", 0, "1K (1024 x 1024)", "1024 x 1024"},
    {PAINT_NEW_CHANNEL_IMAGE_SIZE_2K, "SIZE_2K", 0, "2K (2048 x 2048)", "2048 x 2048"},
    {PAINT_NEW_CHANNEL_IMAGE_SIZE_4K, "SIZE_4K", 0, "4K (4096 x 4096)", "4096 x 4096"},
    {0, nullptr, 0, nullptr, nullptr},
};

}

#ifdef RNA_RUNTIME

#  include "MEM_guardedalloc.h"

#  include "DNA_gpencil_legacy_types.h"
#  include "DNA_meshdata_types.h"
#  include "DNA_node_types.h"
#  include "DNA_object_types.h"
#  include "DNA_screen_types.h"
#  include "DNA_space_types.h"

#  include <fmt/format.h>
#  include <optional>
#  include <string>

#  include "BLI_listbase.h"
#  include "BLI_math_vector.h"
#  include "BLI_string_utf8.h"
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
#  include "BKE_paint_layers.hh"
#  include "BKE_paint_layers_edit.hh"
#  include "BKE_paint_layers_target.hh"
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

#  include "RNA_prototypes.hh"

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

static void rna_Material_paint_layers_begin(CollectionPropertyIterator *iter, PointerRNA *ptr)
{
  Material *ma = static_cast<Material *>(ptr->data);
  rna_iterator_listbase_begin(iter, ptr, &ma->paint_layers, nullptr);
}

static PointerRNA rna_Material_paint_layers_active_get(PointerRNA *ptr)
{
  Material *ma = static_cast<Material *>(ptr->data);
  MaterialPaintLayer *layer = BKE_paint_layers_active_layer_get(*ma);
  if (layer == nullptr) {
    return PointerRNA_NULL;
  }
  return RNA_pointer_create_with_parent(*ptr, RNA_MaterialPaintLayer, layer);
}

/** The active mask correction, or null when the active row is a layer, a correction or the base. */
static PointerRNA rna_Material_paint_layers_active_mask_item_get(PointerRNA *ptr)
{
  Material *ma = static_cast<Material *>(ptr->data);
  MaterialPaintLayer *item = BKE_paint_layers_active_mask_item_get(*ma);
  if (item == nullptr) {
    return PointerRNA_NULL;
  }
  return RNA_pointer_create_with_parent(*ptr, RNA_MaterialPaintLayer, item);
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

/**
 * Whether \a layer is a row of \a ma's stack, judged by address: a stale pointer (a row freed
 * since the caller held it, or one of another material) must never be dereferenced.
 */
static bool rna_paint_layer_in_stack(const Material &ma, const MaterialPaintLayer *layer)
{
  bool found = false;
  BKE_paint_layers_foreach(
      ma, [&](const MaterialPaintLayer &row, const MaterialPaintLayer * /*parent*/) -> bool {
        found = &row == layer;
        return !found;
      });
  return found;
}

/**
 * Paste a copy of the content correction or mask item \a source onto \a target. The source may
 * belong to another material, which is how a preset or another material's correction is handed
 * over; images are copied, never shared.
 */
static MaterialPaintLayer *rna_Material_paint_layers_paste_correction(Material *ma,
                                                                      Main *bmain,
                                                                      ReportList *reports,
                                                                      PointerRNA *source_ptr,
                                                                      PointerRNA *target_ptr)
{
  const MaterialPaintLayer *source = (source_ptr != nullptr) ?
                                         static_cast<const MaterialPaintLayer *>(source_ptr->data) :
                                         nullptr;
  MaterialPaintLayer *target = (target_ptr != nullptr) ?
                                   static_cast<MaterialPaintLayer *>(target_ptr->data) :
                                   nullptr;
  if (source == nullptr || target == nullptr || !rna_paint_layer_in_stack(*ma, target)) {
    BKE_report(reports, RPT_ERROR, "Pasting a correction needs a target row of this material");
    return nullptr;
  }
  MaterialPaintLayer *copy = BKE_paint_layers_correction_paste(*bmain, *ma, *source, target);
  if (copy == nullptr) {
    BKE_report(reports, RPT_ERROR, "Cannot paste this correction onto that layer");
    return nullptr;
  }
  WM_main_add_notifier(NC_MATERIAL | ND_SHADING, &ma->id);
  return copy;
}

static MaterialPaintLayer *rna_Material_paint_layers_new(Material *ma,
                                                         Main *bmain,
                                                         ReportList *reports,
                                                         const char *name,
                                                         PointerRNA *anchor_ptr,
                                                         int place,
                                                         bool make_active,
                                                         int source,
                                                         bool create_maps)
{
  /* The authoring policy is the shared BKE one (#BKE_paint_layers_add_with_policy): the same
   * default names and channels every other authoring path gets. An empty name means the unique
   * default. */
  PaintLayerAddParams params;
  switch (eMaterialPaintLayerSource(source)) {
    case MA_PAINT_LAYER_SOURCE_CONSTANT:
      params.kind = PaintLayerAddKind::Fill;
      break;
    case MA_PAINT_LAYER_SOURCE_STACK:
      params.kind = PaintLayerAddKind::Folder;
      break;
    default:
      params.kind = PaintLayerAddKind::Paint;
      break;
  }
  params.name = (name[0] != '\0') ? name : nullptr;
  /* An unset optional pointer reaches the callback as null, not as a null #PointerRNA. */
  params.anchor = (anchor_ptr != nullptr) ? static_cast<MaterialPaintLayer *>(anchor_ptr->data) :
                                            nullptr;
  if (params.anchor != nullptr && !rna_paint_layer_in_stack(*ma, params.anchor)) {
    BKE_report(reports, RPT_ERROR, "The anchor is not a paint layer of this material");
    return nullptr;
  }
  params.place = PaintLayerPlace(place);
  MaterialPaintLayer *layer = BKE_paint_layers_add_with_policy(*ma, params);
  if (layer == nullptr) {
    BKE_report(reports, RPT_ERROR, "Cannot add a paint layer here");
    return nullptr;
  }
  /* A fresh row becomes the cursor, mirroring what the UI does when it adds one, unless the caller
   * builds a stack unattended and passes make_active=False to leave the cursor alone. */
  if (make_active) {
    BKE_paint_layers_active_set(*ma, layer->marker);
  }
  /* Opt in: a script that builds a stack unattended would otherwise allocate every map of every
   * layer. The maps are made for the active row, so the cursor visits the new row and goes back. */
  if (create_maps && layer->source == MA_PAINT_LAYER_SOURCE_IMAGE) {
    const MaterialPaintLayer *previous = BKE_paint_layers_active_layer_get(*ma);
    const bUUID previous_marker = previous != nullptr ? previous->marker : bUUID{};
    BKE_paint_layers_active_set(*ma, layer->marker);
    BKE_paint_layers_active_maps_ensure(*bmain, *ma, PAINT_NEW_CHANNEL_IMAGE_SIZE_4K);
    if (!make_active) {
      BKE_paint_layers_active_set(*ma, previous_marker);
    }
  }
  /* BKE only tags DEG; the editors need the WM notifier too. */
  WM_main_add_notifier(NC_MATERIAL | ND_SHADING, &ma->id);
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

static void rna_Material_paint_layers_move(Material *ma,
                                           ReportList *reports,
                                           PointerRNA *layer_ptr,
                                           PointerRNA *anchor_ptr,
                                           int place)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(layer_ptr->data);
  MaterialPaintLayer *anchor = (anchor_ptr != nullptr) ?
                                   static_cast<MaterialPaintLayer *>(anchor_ptr->data) :
                                   nullptr;
  if (!BKE_paint_layers_move(*ma, layer, anchor, PaintLayerPlace(place))) {
    BKE_report(reports, RPT_ERROR, "Cannot move the paint layer relative to the given anchor");
    return;
  }
  WM_main_add_notifier(NC_MATERIAL | ND_SHADING, &ma->id);
}

static void rna_Material_paint_layers_reorder(Material *ma,
                                              ReportList *reports,
                                              PointerRNA *layer_ptr,
                                              int index)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(layer_ptr->data);
  if (!BKE_paint_layers_reorder(*ma, layer, index)) {
    BKE_report(reports, RPT_ERROR, "Cannot reorder a paint layer that is not part of the material");
    return;
  }
  WM_main_add_notifier(NC_MATERIAL | ND_SHADING, &ma->id);
}

/**
 * RNA carries no list arguments, so a group is named by its members pairwise: the first row plus an
 * optional second. Larger selections are grouped by the Outliner's operator, which calls the BKE
 * API directly with the full span.
 */
static MaterialPaintLayer *rna_Material_paint_layers_group(Material *ma,
                                                           ReportList *reports,
                                                           PointerRNA *layer_ptr,
                                                           PointerRNA *with_ptr)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(layer_ptr->data);
  MaterialPaintLayer *with = (with_ptr != nullptr) ?
                                 static_cast<MaterialPaintLayer *>(with_ptr->data) :
                                 nullptr;
  if (!rna_paint_layer_in_stack(*ma, layer) ||
      (with != nullptr && !rna_paint_layer_in_stack(*ma, with)))
  {
    BKE_report(reports, RPT_ERROR, "The paint layers are not part of this material");
    return nullptr;
  }
  MaterialPaintLayer *members[2] = {layer, with};
  MaterialPaintLayer *folder = BKE_paint_layers_group(
      *ma, Span<MaterialPaintLayer *>(members, (with != nullptr) ? 2 : 1));
  if (folder == nullptr) {
    BKE_report(reports, RPT_ERROR, "Cannot group paint layers of different lists");
    return nullptr;
  }
  WM_main_add_notifier(NC_MATERIAL | ND_SHADING, &ma->id);
  return folder;
}

static void rna_Material_paint_layers_ungroup(Material *ma,
                                              ReportList *reports,
                                              PointerRNA *folder_ptr)
{
  MaterialPaintLayer *folder = static_cast<MaterialPaintLayer *>(folder_ptr->data);
  if (!rna_paint_layer_in_stack(*ma, folder) || !BKE_paint_layers_ungroup(*ma, folder)) {
    BKE_report(reports, RPT_ERROR, "Cannot ungroup this paint layer");
    return;
  }
  folder_ptr->invalidate();
  WM_main_add_notifier(NC_MATERIAL | ND_SHADING, &ma->id);
}

static MaterialPaintLayer *rna_Material_paint_layers_merge_down(Material *ma,
                                                                ReportList *reports,
                                                                PointerRNA *upper_ptr,
                                                                PointerRNA *lower_ptr)
{
  MaterialPaintLayer *upper = static_cast<MaterialPaintLayer *>(upper_ptr->data);
  MaterialPaintLayer *lower = static_cast<MaterialPaintLayer *>(lower_ptr->data);
  if (!rna_paint_layer_in_stack(*ma, upper) || !rna_paint_layer_in_stack(*ma, lower)) {
    BKE_report(reports, RPT_ERROR, "Merged rows must belong to this material");
    return nullptr;
  }
  MaterialPaintLayer *folder = BKE_paint_layers_edit_merge_down(*ma, *upper, *lower);
  if (folder == nullptr) {
    BKE_reportf(reports,
                RPT_ERROR,
                "Cannot merge paint layer '%s' down into '%s'",
                upper->name,
                lower->name);
    return nullptr;
  }
  WM_main_add_notifier(NC_MATERIAL | ND_SHADING, &ma->id);
  return folder;
}

static bool rna_Material_is_layered_get(PointerRNA *ptr)
{
  return BKE_paint_layers_is_layered(*id_cast<Material *>(ptr->owner_id));
}

/* The effective set, not the raw field: an unauthored field reads the derived set. */
static int rna_Material_paint_layers_channels_get(PointerRNA *ptr)
{
  return BKE_paint_layers_channel_set_mask_get(*id_cast<Material *>(ptr->owner_id));
}

static void rna_Material_paint_layers_channels_set(PointerRNA *ptr, int value)
{
  Material *ma = id_cast<Material *>(ptr->owner_id);
  BKE_paint_layers_channel_set_mask_set(*ma, uint16_t(value));
}

static bool rna_Material_paint_layers_channel_in_set(Material *ma, int channel)
{
  return BKE_paint_layers_channel_in_set(*ma, eMaterialPaintChannel(channel));
}

/** Every layer-pointer setter goes back to the material that owns the row, which the pointer's
 * #owner_id carries. */
static Material *rna_paint_layer_material(PointerRNA *ptr, MaterialPaintLayer *layer)
{
  Material *ma = reinterpret_cast<Material *>(ptr->owner_id);
  if (ma == nullptr) {
    return nullptr;
  }
  /* A row of the stack at any depth, an item of one row's mask or one of its corrections: judged by
   * address, so a stale pointer is never dereferenced. */
  bool owned = false;
  BKE_paint_layers_foreach(
      static_cast<const Material &>(*ma),
      [&](const MaterialPaintLayer &row, const MaterialPaintLayer * /*parent*/) -> bool {
        owned = &row == layer || BLI_findindex(&row.mask_stack, layer) != -1 ||
                BLI_findindex(&row.effects, layer) != -1;
        return !owned;
      });
  return owned ? ma : nullptr;
}

static IDProperty **rna_MaterialPaintLayer_system_idprops(PointerRNA *ptr)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  return &layer->system_properties;
}

static void rna_MaterialPaintLayer_name_set(PointerRNA *ptr, const char *value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    BKE_paint_layers_rename(*ma, layer, value);
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

/**
 * A mask correction blends a grey, which has no hue, so the modes that read one are not offered
 * there. A value one already holds still reads and draws; only the choice is narrowed.
 */
static const EnumPropertyItem *rna_MaterialPaintLayer_blend_itemf(bContext * /*C*/,
                                                                  PointerRNA *ptr,
                                                                  PropertyRNA * /*prop*/,
                                                                  bool *r_free)
{
  const MaterialPaintLayer *layer = (ptr != nullptr) ?
                                        static_cast<const MaterialPaintLayer *>(ptr->data) :
                                        nullptr;
  if (layer == nullptr || layer->role != MA_PAINT_LAYER_ROLE_MASK_ITEM) {
    return rna_enum_material_paint_layer_blend_items;
  }
  EnumPropertyItem *items = nullptr;
  int items_num = 0;
  for (const EnumPropertyItem *item = rna_enum_material_paint_layer_blend_items;
       item->identifier != nullptr;
       item++)
  {
    if (ELEM(item->value,
             MA_PAINT_LAYER_BLEND_HUE,
             MA_PAINT_LAYER_BLEND_SATURATION,
             MA_PAINT_LAYER_BLEND_COLOR,
             MA_PAINT_LAYER_BLEND_VALUE))
    {
      continue;
    }
    RNA_enum_item_add(&items, &items_num, item);
  }
  RNA_enum_item_end(&items, &items_num);
  *r_free = true;
  return items;
}

static int rna_MaterialPaintLayer_source_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return layer->source;
}

static void rna_MaterialPaintLayer_source_set(PointerRNA *ptr, int value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    BKE_paint_layers_set_source(*ma, layer, eMaterialPaintLayerSource(value));
  }
}

static void rna_MaterialPaintLayer_fill_color_get(PointerRNA *ptr, float *value)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  /* A Fill keeps its colour in the Base-Color record; the single reader answers for it. */
  BKE_paint_layers_base_color_get(*layer, value);
}

static void rna_MaterialPaintLayer_fill_color_set(PointerRNA *ptr, const float *value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    BKE_paint_layers_set_fill_color(*ma, layer, value);
  }
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

/**
 * The path of \a layer below \a prefix, searching \a list and every folder under it: a row is
 * `<prefix>[i]`, a folder's rows `<prefix>[i].children[j]` and a mask item
 * `<row path>.mask_items[k]`, a content correction `<row path>.effects[k]`.
 */
static bool rna_paint_layer_path_find(const ListBaseT<MaterialPaintLayer> &list,
                                      const MaterialPaintLayer *layer,
                                      const std::string &prefix,
                                      std::string &r_path)
{
  int index = 0;
  for (const MaterialPaintLayer &row : list) {
    const std::string row_path = fmt::format("{}[{}]", prefix, index);
    if (&row == layer) {
      r_path = row_path;
      return true;
    }
    const int item_index = BLI_findindex(&row.mask_stack, layer);
    if (item_index != -1) {
      r_path = fmt::format("{}.mask_items[{}]", row_path, item_index);
      return true;
    }
    const int effect_index = BLI_findindex(&row.effects, layer);
    if (effect_index != -1) {
      r_path = fmt::format("{}.effects[{}]", row_path, effect_index);
      return true;
    }
    if (BKE_paint_layers_is_folder(row) &&
        rna_paint_layer_path_find(row.children, layer, row_path + ".children", r_path))
    {
      return true;
    }
    index++;
  }
  return false;
}

static std::optional<std::string> rna_MaterialPaintLayer_path(const PointerRNA *ptr)
{
  const Material *ma = reinterpret_cast<const Material *>(ptr->owner_id);
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  std::string path;
  if (rna_paint_layer_path_find(ma->paint_layers, layer, "paint_layers", path)) {
    return path;
  }
  return std::nullopt;
}

/** The material and row a layer pointer stands for, or null when the pointer is stale. */
static Material *rna_MaterialPaintLayer_owner(PointerRNA ptr, MaterialPaintLayer **r_layer)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr.data);
  Material *ma = rna_paint_layer_material(&ptr, layer);
  if (ma != nullptr) {
    *r_layer = layer;
  }
  return ma;
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
      DEG_id_tag_update(&ma->id, ID_RECALC_SHADING);
      WM_main_add_notifier(NC_MATERIAL | ND_SHADING, &ma->id);
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
      DEG_id_tag_update(&ma->id, ID_RECALC_SHADING);
      WM_main_add_notifier(NC_MATERIAL | ND_SHADING, &ma->id);
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
      /* The toggle must redraw the viewport and the panels at once, like the other mutators. */
      DEG_id_tag_update(&ma->id, ID_RECALC_SHADING);
      WM_main_add_notifier(NC_MATERIAL | ND_SHADING, &ma->id);
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

/** One row of the computed `MaterialPaintLayer.issues` collection, a snapshot of a description
 * problem; the array is built on iteration and owned by the iterator. */
struct MaterialPaintLayerIssueItem {
  int code;
  int channel;
  char layer_marker[UUID_STRING_SIZE];
  char correction_marker[UUID_STRING_SIZE];
  char text[256];
};

static void rna_MaterialPaintLayer_issues_begin(CollectionPropertyIterator *iter, PointerRNA *ptr)
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
  rna_iterator_array_begin(
      iter, ptr, array, sizeof(MaterialPaintLayerIssueItem), count, array != nullptr, nullptr);
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
  const MaterialPaintLayerIssueItem *item = rna_MaterialPaintLayerIssue(ptr);
  BLI_strncpy(value, item->text, sizeof(item->text));
}

static int rna_MaterialPaintLayerIssue_text_length(PointerRNA *ptr)
{
  return int(strlen(rna_MaterialPaintLayerIssue(ptr)->text));
}

/** The constant coverage of a mask item: the mean of its Base Color record, one for any other. */
static float rna_MaterialPaintLayer_mask_value_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *item = static_cast<const MaterialPaintLayer *>(ptr->data);
  return (item->role == MA_PAINT_LAYER_ROLE_MASK_ITEM) ? BKE_paint_layers_mask_constant(*item) :
                                                         1.0f;
}

static void rna_MaterialPaintLayer_mask_value_set(PointerRNA *ptr, const float value)
{
  MaterialPaintLayer *item = static_cast<MaterialPaintLayer *>(ptr->data);
  if (item->role != MA_PAINT_LAYER_ROLE_MASK_ITEM) {
    return;
  }
  if (Material *ma = rna_paint_layer_material(ptr, item)) {
    const float gray = std::clamp(value, 0.0f, 1.0f);
    const float rgba[4] = {gray, gray, gray, 1.0f};
    BKE_paint_layers_channel_set_value(*ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, rgba);
  }
}

/**
 * The constant coverage as a grey swatch, so a mask correction's flat value is picked the way a
 * Fill layer's colour is. A write keeps only the mean of what was picked: the mask reads one grey.
 */
static void rna_MaterialPaintLayer_mask_color_get(PointerRNA *ptr, float *values)
{
  const MaterialPaintLayer *item = static_cast<const MaterialPaintLayer *>(ptr->data);
  const float gray = (item->role == MA_PAINT_LAYER_ROLE_MASK_ITEM) ?
                         BKE_paint_layers_mask_constant(*item) :
                         1.0f;
  values[0] = values[1] = values[2] = gray;
}

static void rna_MaterialPaintLayer_mask_color_set(PointerRNA *ptr, const float *values)
{
  rna_MaterialPaintLayer_mask_value_set(ptr, (values[0] + values[1] + values[2]) / 3.0f);
}

static void rna_MaterialPaintLayer_mask_items_begin(CollectionPropertyIterator *iter,
                                                    PointerRNA *ptr)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  rna_iterator_listbase_begin(iter, ptr, &layer->mask_stack, nullptr);
}

static void rna_MaterialPaintLayer_effects_begin(CollectionPropertyIterator *iter, PointerRNA *ptr)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  rna_iterator_listbase_begin(iter, ptr, &layer->effects, nullptr);
}

static MaterialPaintLayer *rna_MaterialPaintLayer_effect_add(PointerRNA ptr,
                                                             ReportList *reports,
                                                             const char *name,
                                                             const int source)
{
  if (!ELEM(source, MA_PAINT_LAYER_SOURCE_IMAGE, MA_PAINT_LAYER_SOURCE_CONSTANT)) {
    BKE_report(reports,
               RPT_ERROR,
               "A correction is a painted map (Image) or a flat colour (Constant)");
    return nullptr;
  }
  MaterialPaintLayer *layer = nullptr;
  if (Material *ma = rna_MaterialPaintLayer_owner(ptr, &layer)) {
    /* The policy numbers an empty name among this layer's own corrections. */
    PaintLayerAddParams params;
    params.kind = (source == MA_PAINT_LAYER_SOURCE_CONSTANT) ? PaintLayerAddKind::CorrectionFill :
                                                               PaintLayerAddKind::Correction;
    params.anchor = layer;
    params.name = (name[0] != '\0') ? name : nullptr;
    if (MaterialPaintLayer *effect = BKE_paint_layers_add_with_policy(*ma, params)) {
      return effect;
    }
  }
  BKE_report(reports, RPT_ERROR, "Only a layer of the stack takes a correction");
  return nullptr;
}

static MaterialPaintLayer *rna_MaterialPaintLayer_mask_item_add(PointerRNA ptr,
                                                                ReportList *reports,
                                                                const char *name,
                                                                const int source)
{
  if (!ELEM(source, MA_PAINT_LAYER_SOURCE_IMAGE, MA_PAINT_LAYER_SOURCE_CONSTANT)) {
    BKE_report(reports,
               RPT_ERROR,
               "A mask correction is a painted map (Image) or a flat grey (Constant)");
    return nullptr;
  }
  MaterialPaintLayer *layer = nullptr;
  if (Material *ma = rna_MaterialPaintLayer_owner(ptr, &layer)) {
    /* The policy makes the base mask first when the layer has none, and numbers an empty name among
     * this layer's own mask items. */
    PaintLayerAddParams params;
    params.kind = (source == MA_PAINT_LAYER_SOURCE_CONSTANT) ?
                      PaintLayerAddKind::MaskCorrectionFill :
                      PaintLayerAddKind::MaskCorrection;
    params.anchor = layer;
    params.name = (name[0] != '\0') ? name : nullptr;
    if (MaterialPaintLayer *item = BKE_paint_layers_add_with_policy(*ma, params)) {
      return item;
    }
  }
  BKE_report(reports, RPT_ERROR, "Only a layer of the stack takes a mask correction");
  return nullptr;
}

static void rna_MaterialPaintLayer_children_begin(CollectionPropertyIterator *iter,
                                                  PointerRNA *ptr)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  rna_iterator_listbase_begin(iter, ptr, &layer->children, nullptr);
}

static int rna_MaterialPaintLayer_children_length(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return BLI_listbase_count(&layer->children);
}

static MaterialPaintLayer *rna_MaterialPaintLayer_mask_add(PointerRNA ptr,
                                                           ReportList *reports,
                                                           const float value)
{
  MaterialPaintLayer *layer = nullptr;
  if (Material *ma = rna_MaterialPaintLayer_owner(ptr, &layer)) {
    if (MaterialPaintLayer *item = BKE_paint_layers_mask_add(*ma, layer, value)) {
      return item;
    }
    /* A mask item is resolved by the owner lookup too, but only a row of the stack takes a mask. */
    BKE_report(reports,
               RPT_ERROR,
               (BKE_paint_layers_mask_base(*layer) != nullptr) ?
                   "Cannot add a mask to this paint layer: it has one already" :
                   "Cannot add a mask to this paint layer: it is not a row of the stack");
    return nullptr;
  }
  BKE_report(reports, RPT_ERROR, "Paint layer is not part of this material");
  return nullptr;
}

static void rna_MaterialPaintLayer_mask_remove(PointerRNA ptr, ReportList *reports)
{
  MaterialPaintLayer *layer = nullptr;
  if (Material *ma = rna_MaterialPaintLayer_owner(ptr, &layer)) {
    if (BKE_paint_layers_mask_remove(*ma, layer)) {
      return;
    }
  }
  BKE_report(reports, RPT_ERROR, "This paint layer has no mask");
}

static int rna_MaterialPaintLayer_channels_length(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return layer->channels_num;
}

/**
 * The layer whose channel record is \a channel, or null when it is not in \a ma's stack. A row's
 * mask items are searched too: their records are addressed like any row's.
 */
static MaterialPaintLayer *rna_paint_layer_by_channel(Material &ma,
                                                      const MaterialPaintLayerChannel *channel)
{
  MaterialPaintLayer *found = nullptr;
  BKE_paint_layers_foreach(
      ma, [&](MaterialPaintLayer &layer, MaterialPaintLayer * /*parent*/) -> bool {
        if (channel >= layer.channels && channel < layer.channels + layer.channels_num) {
          found = &layer;
          return false;
        }
        for (MaterialPaintLayer &item : layer.mask_stack) {
          if (channel >= item.channels && channel < item.channels + item.channels_num) {
            found = &item;
            return false;
          }
        }
        for (MaterialPaintLayer &effect : layer.effects) {
          if (channel >= effect.channels && channel < effect.channels + effect.channels_num) {
            found = &effect;
            return false;
          }
        }
        return true;
      });
  return found;
}

/** The owning material of a channel pointer, or null when it is not in its stack. */
static Material *rna_paint_layer_sub_owner(PointerRNA *ptr, MaterialPaintLayer **r_layer)
{
  Material *ma = reinterpret_cast<Material *>(ptr->owner_id);
  if (ma == nullptr) {
    return nullptr;
  }
  *r_layer = rna_paint_layer_by_channel(
      *ma, static_cast<const MaterialPaintLayerChannel *>(ptr->data));
  return (*r_layer != nullptr) ? ma : nullptr;
}

/** Path of a record below its material, so a property of it can be addressed (eyedropper). */
static std::optional<std::string> rna_MaterialPaintLayerChannel_path(const PointerRNA *ptr)
{
  const Material *ma = reinterpret_cast<const Material *>(ptr->owner_id);
  const MaterialPaintLayerChannel *record = static_cast<const MaterialPaintLayerChannel *>(
      ptr->data);
  if (ma == nullptr) {
    return std::nullopt;
  }
  /* The row (or mask item) that holds the record, then the record's index in it. */
  const MaterialPaintLayer *owner = nullptr;
  BKE_paint_layers_foreach(
      static_cast<const Material &>(*ma),
      [&](const MaterialPaintLayer &layer, const MaterialPaintLayer * /*parent*/) -> bool {
        if (record >= layer.channels && record < layer.channels + layer.channels_num) {
          owner = &layer;
          return false;
        }
        for (const MaterialPaintLayer &item : layer.mask_stack) {
          if (record >= item.channels && record < item.channels + item.channels_num) {
            owner = &item;
            return false;
          }
        }
        for (const MaterialPaintLayer &effect : layer.effects) {
          if (record >= effect.channels && record < effect.channels + effect.channels_num) {
            owner = &effect;
            return false;
          }
        }
        return true;
      });
  std::string path;
  if (owner == nullptr ||
      !rna_paint_layer_path_find(ma->paint_layers, owner, "paint_layers", path))
  {
    return std::nullopt;
  }
  return fmt::format("{}.channels[{}]", path, int(record - owner->channels));
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
        *ma,
        layer,
        eMaterialPaintChannel(rna_MaterialPaintLayerChannel_channel_get(ptr)),
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
        *ma,
        layer,
        eMaterialPaintChannel(rna_MaterialPaintLayerChannel_channel_get(ptr)),
        value);
  }
}

/**
 * The colour channels' constant without its alpha, as the brush channel's colour reads. Alpha is
 * coverage in the record (zero until the constant is written), not part of what the user picks, so
 * a write keeps a live alpha and makes a transparent record opaque.
 */
static void rna_MaterialPaintLayerChannel_color_get(PointerRNA *ptr, float *values)
{
  const MaterialPaintLayerChannel *record = static_cast<const MaterialPaintLayerChannel *>(
      ptr->data);
  copy_v3_v3(values, record->value);
}

static void rna_MaterialPaintLayerChannel_color_set(PointerRNA *ptr, const float *values)
{
  MaterialPaintLayer *layer = nullptr;
  if (Material *ma = rna_paint_layer_sub_owner(ptr, &layer)) {
    const MaterialPaintLayerChannel *record = static_cast<const MaterialPaintLayerChannel *>(
        ptr->data);
    const float rgba[4] = {
        values[0], values[1], values[2], record->value[3] > 0.0f ? record->value[3] : 1.0f};
    BKE_paint_layers_channel_set_value(
        *ma,
        layer,
        eMaterialPaintChannel(rna_MaterialPaintLayerChannel_channel_get(ptr)),
        rgba);
  }
}

/** Whether \a channel's constant is a single scalar (stored in `value[0]`) or a colour. Mirrors the
 * brush channel's own test, so the shared layer widget reads a layer channel as a brush one. */
static bool rna_MaterialPaintLayerChannel_is_scalar_channel(const eMaterialPaintChannel channel)
{
  return ELEM(channel,
              PAINT_MATERIAL_CHANNEL_METALLIC,
              PAINT_MATERIAL_CHANNEL_ROUGHNESS,
              PAINT_MATERIAL_CHANNEL_SPECULAR,
              PAINT_MATERIAL_CHANNEL_HEIGHT,
              PAINT_MATERIAL_CHANNEL_ALPHA,
              PAINT_MATERIAL_CHANNEL_AO,
              PAINT_MATERIAL_CHANNEL_CUSTOM);
}

/**
 * The scalar channels store their constant in `value[0]`; Base Color and Normal are colours and use
 * the `value` array directly. This is the one float a scalar channel's UI edits, routed through
 * #BKE_paint_layers_channel_set_value so the value-only / topology tags stay in one place.
 */
static float rna_MaterialPaintLayerChannel_value_scalar_get(PointerRNA *ptr)
{
  const MaterialPaintLayerChannel *record = static_cast<const MaterialPaintLayerChannel *>(
      ptr->data);
  return record->value[0];
}

/** The channel's own range (0..1, Height -1..1), the same one the brush channel's value uses. */
static void rna_MaterialPaintLayerChannel_value_scalar_range(
    PointerRNA *ptr, float *min, float *max, float *softmin, float *softmax)
{
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(rna_MaterialPaintLayerChannel_channel_get(ptr)));
  *min = *softmin = info.value_min;
  *max = *softmax = info.value_max;
}

static void rna_MaterialPaintLayerChannel_value_scalar_set(PointerRNA *ptr, float value)
{
  MaterialPaintLayer *layer = nullptr;
  if (Material *ma = rna_paint_layer_sub_owner(ptr, &layer)) {
    const eMaterialPaintChannel channel = eMaterialPaintChannel(
        rna_MaterialPaintLayerChannel_channel_get(ptr));
    const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(channel);
    float rgba[4];
    const MaterialPaintLayerChannel *record = static_cast<const MaterialPaintLayerChannel *>(
        ptr->data);
    copy_v4_v4(rgba, record->value);
    rgba[0] = std::clamp(value, info.value_min, info.value_max);
    BKE_paint_layers_channel_set_value(*ma, layer, channel, rgba);
  }
}

/**
 * Whether the channel takes part. Mirrors #BrushMaterialPaintChannel.use so one widget can read a
 * brush channel and a layer channel the same way: on for ENABLED, off for DISABLED and ABSENT.
 * Writing goes through #BKE_paint_layers_channel_set_enabled, which owns the tags and rebuild; a
 * switched-off record keeps its value and map, so re-enabling restores the channel untouched.
 */
static bool rna_MaterialPaintLayerChannel_use_get(PointerRNA *ptr)
{
  const MaterialPaintLayerChannel *record = static_cast<const MaterialPaintLayerChannel *>(
      ptr->data);
  return record->state == MA_PAINT_LAYER_CHANNEL_ENABLED;
}

static void rna_MaterialPaintLayerChannel_use_set(PointerRNA *ptr, bool value)
{
  MaterialPaintLayer *layer = nullptr;
  if (Material *ma = rna_paint_layer_sub_owner(ptr, &layer)) {
    BKE_paint_layers_channel_set_enabled(
        *ma, layer, eMaterialPaintChannel(rna_MaterialPaintLayerChannel_channel_get(ptr)), value);
  }
}

static bool rna_MaterialPaintLayerChannel_invert_get(PointerRNA *ptr)
{
  const MaterialPaintLayerChannel *record = static_cast<const MaterialPaintLayerChannel *>(
      ptr->data);
  return (record->flag & MA_PAINT_LAYER_CHANNEL_INVERT) != 0;
}

static void rna_MaterialPaintLayerChannel_invert_set(PointerRNA *ptr, bool value)
{
  MaterialPaintLayer *layer = nullptr;
  if (Material *ma = rna_paint_layer_sub_owner(ptr, &layer)) {
    BKE_paint_layers_channel_set_invert(
        *ma, layer, eMaterialPaintChannel(rna_MaterialPaintLayerChannel_channel_get(ptr)), value);
  }
}

/**
 * The row whose #MaterialPaintLayer::channel_settings holds \a settings, or null when it is not in
 * \a ma's stack. A row's mask items and corrections are searched too: their settings are addressed
 * like any row's.
 */
static MaterialPaintLayer *rna_paint_layer_by_settings(
    Material &ma, const MaterialPaintLayerChannelSettings *settings)
{
  const auto holds = [&](const MaterialPaintLayer &row) {
    return settings >= row.channel_settings &&
           settings < row.channel_settings + PAINT_MATERIAL_CHANNEL_NUM;
  };
  MaterialPaintLayer *found = nullptr;
  BKE_paint_layers_foreach(
      ma, [&](MaterialPaintLayer &layer, MaterialPaintLayer * /*parent*/) -> bool {
        if (holds(layer)) {
          found = &layer;
          return false;
        }
        for (MaterialPaintLayer &item : layer.mask_stack) {
          if (holds(item)) {
            found = &item;
            return false;
          }
        }
        for (MaterialPaintLayer &effect : layer.effects) {
          if (holds(effect)) {
            found = &effect;
            return false;
          }
        }
        return true;
      });
  return found;
}

static int rna_MaterialPaintLayerChannelSettings_channel_get(PointerRNA *ptr)
{
  Material *ma = reinterpret_cast<Material *>(ptr->owner_id);
  const MaterialPaintLayerChannelSettings *settings =
      static_cast<const MaterialPaintLayerChannelSettings *>(ptr->data);
  if (ma != nullptr) {
    if (const MaterialPaintLayer *row = rna_paint_layer_by_settings(*ma, settings)) {
      return int(settings - row->channel_settings);
    }
  }
  return 0;
}

static float rna_MaterialPaintLayerChannelSettings_opacity_get(PointerRNA *ptr)
{
  const MaterialPaintLayerChannelSettings *settings =
      static_cast<const MaterialPaintLayerChannelSettings *>(ptr->data);
  return settings->opacity * 100.0f;
}

static void rna_MaterialPaintLayerChannelSettings_opacity_set(PointerRNA *ptr, float value)
{
  Material *ma = reinterpret_cast<Material *>(ptr->owner_id);
  if (ma == nullptr) {
    return;
  }
  const MaterialPaintLayerChannelSettings *settings =
      static_cast<const MaterialPaintLayerChannelSettings *>(ptr->data);
  if (MaterialPaintLayer *row = rna_paint_layer_by_settings(*ma, settings)) {
    BKE_paint_layers_channel_opacity_set(
        *ma, *row, int(settings - row->channel_settings), value / 100.0f);
  }
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

/**
 * A scalar channel's value as the grayscale colour of its gradient ramp, so the shared channel
 * widget draws the same swatch for a layer as for a brush. Colour channels (Base Color, Normal,
 * Emission) read black and ignore writes, as the widget never sends them here.
 */
static void rna_MaterialPaintLayerChannel_value_color_get(PointerRNA *ptr, float *values)
{
  const MaterialPaintLayerChannel *record = static_cast<const MaterialPaintLayerChannel *>(
      ptr->data);
  const eMaterialPaintChannel channel = eMaterialPaintChannel(
      rna_MaterialPaintLayerChannel_channel_get(ptr));
  if (!rna_MaterialPaintLayerChannel_is_scalar_channel(channel)) {
    zero_v3(values);
    return;
  }
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(channel);
  const float t = BKE_paint_material_t_from_value(info.value_min, info.value_max, record->value[0]);
  BKE_paint_material_value_gradient_color(info.value_min, info.value_max, t, values);
}

static void rna_MaterialPaintLayerChannel_value_color_set(PointerRNA *ptr, const float *values)
{
  MaterialPaintLayer *layer = nullptr;
  if (Material *ma = rna_paint_layer_sub_owner(ptr, &layer)) {
    const eMaterialPaintChannel channel = eMaterialPaintChannel(
        rna_MaterialPaintLayerChannel_channel_get(ptr));
    if (!rna_MaterialPaintLayerChannel_is_scalar_channel(channel)) {
      return;
    }
    const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(channel);
    const float gray = (values[0] + values[1] + values[2]) / 3.0f;
    const float t = BKE_paint_material_t_from_value(info.value_min, info.value_max, gray);
    float rgba[4];
    const MaterialPaintLayerChannel *record = static_cast<const MaterialPaintLayerChannel *>(
        ptr->data);
    copy_v4_v4(rgba, record->value);
    rgba[0] = BKE_paint_material_value_from_t(info.value_min, info.value_max, t);
    BKE_paint_layers_channel_set_value(*ma, layer, channel, rgba);
  }
}

/** \} */

}  // namespace blender

#else

namespace blender {

/* Values match #eMaterialPaintLayerChannelState. */
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

/* Values must match #PaintLayerPlace (a runtime-only type, unavailable in the definition pass). */
static const EnumPropertyItem rna_enum_material_paint_layer_place_items[] = {
    {0, "ABOVE", 0, "Above", "Directly above the anchor"},
    {1, "BELOW", 0, "Below", "Directly below the anchor"},
    {2, "INTO", 0, "Into", "Inside the anchor folder, on top of what it holds"},
    {0, nullptr, 0, nullptr, nullptr},
};

/** What a row paints with: a painted map, a flat Fill colour, or the layers a folder holds. */
static const EnumPropertyItem rna_enum_material_paint_layer_source_items[] = {
    {MA_PAINT_LAYER_SOURCE_IMAGE, "IMAGE", 0, "Image", "A painted map"},
    {MA_PAINT_LAYER_SOURCE_CONSTANT, "CONSTANT", 0, "Constant", "A flat colour"},
    {MA_PAINT_LAYER_SOURCE_STACK, "STACK", 0, "Stack", "A nested stack of layers (a folder)"},
    {0, nullptr, 0, nullptr, nullptr},
};

/* Values must match #PaintLayersIssueCode. */
static const EnumPropertyItem rna_enum_material_paint_layer_issue_code_items[] = {
    {0,
     "FILL_CORRECTION_ON_NORMAL",
     0,
     "Fill Correction on Normal",
     "A flat value is not a normal, so the correction is skipped in Normal until it holds a map"},
    {1,
     "FOLDER_HAS_MAPS",
     0,
     "Folder Has Maps",
     "A folder takes part through its layers; a map on the folder itself is ignored"},
    {2,
     "NON_FOLDER_HAS_CHILDREN",
     0,
     "Non-Folder Has Children",
     "Only a folder takes part through its children; the stray nesting is ignored"},
    {3,
     "KIND_NOT_ALLOWED",
     0,
     "Kind Not Allowed",
     "The row's kind is unknown to this build or not allowed in the row's role"},
    {0, nullptr, 0, nullptr, nullptr},
};

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

static void rna_def_material_paint_layer(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;
  FunctionRNA *func;
  PropertyRNA *parm;

  srna = RNA_def_struct(brna, "MaterialPaintLayer", nullptr);
  RNA_def_struct_sdna(srna, "MaterialPaintLayer");
  RNA_def_struct_path_func(srna, "rna_MaterialPaintLayer_path");
  RNA_def_struct_system_idprops_func(srna, "rna_MaterialPaintLayer_system_idprops");
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

  prop = RNA_def_property(srna, "opacity", PROP_FLOAT, PROP_PERCENTAGE);
  RNA_def_property_float_funcs(
      prop, "rna_MaterialPaintLayer_opacity_get", "rna_MaterialPaintLayer_opacity_set", nullptr);
  RNA_def_property_range(prop, 0.0f, 100.0f);
  RNA_def_property_ui_text(prop, "Opacity", "Opacity of the paint layer, in percent");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "blend_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_material_paint_layer_blend_items);
  RNA_def_property_enum_funcs(
      prop,
      "rna_MaterialPaintLayer_blend_get",
      "rna_MaterialPaintLayer_blend_set",
      "rna_MaterialPaintLayer_blend_itemf");
  RNA_def_property_ui_text(prop, "Blend Type", "How the layer blends over what is below it");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "source", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_material_paint_layer_source_items);
  RNA_def_property_enum_funcs(
      prop, "rna_MaterialPaintLayer_source_get", "rna_MaterialPaintLayer_source_set", nullptr);
  RNA_def_property_ui_text(prop, "Source", "What the layer reads its values from");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "fill_color", PROP_FLOAT, PROP_COLOR);
  RNA_def_property_array(prop, 4);
  RNA_def_property_float_funcs(prop,
                               "rna_MaterialPaintLayer_fill_color_get",
                               "rna_MaterialPaintLayer_fill_color_set",
                               nullptr);
  RNA_def_property_ui_text(prop, "Fill Color", "Constant color of a Fill layer or Fill correction");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "enabled", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(
      prop, "rna_MaterialPaintLayer_enabled_get", "rna_MaterialPaintLayer_enabled_set");
  RNA_def_property_ui_text(prop, "Enabled", "Whether the layer takes part in the stack");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

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
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    nullptr);
  RNA_def_property_ui_text(
      prop, "Channel Settings", "Per-channel opacity of the row, one entry per paint channel");

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
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Children", "Nested rows of this folder layer, bottom to top");

  prop = RNA_def_property(srna, "issues", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_struct_type(prop, "MaterialPaintLayerIssue");
  RNA_def_property_collection_funcs(prop,
                                    "rna_MaterialPaintLayer_issues_begin",
                                    "rna_iterator_array_next",
                                    "rna_iterator_array_end",
                                    "rna_iterator_array_get",
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop, "Issues", "Description problems on this layer that the stack silently ignores");

  prop = RNA_def_property(srna, "mask_items", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_struct_type(prop, "MaterialPaintLayer");
  RNA_def_property_collection_funcs(prop,
                                    "rna_MaterialPaintLayer_mask_items_begin",
                                    "rna_iterator_listbase_next",
                                    "rna_iterator_listbase_end",
                                    "rna_iterator_listbase_get",
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop, "Mask Items", "The coverage items laid over the layer, bottom to top; the first is the "
                          "base mask");

  prop = RNA_def_property(srna, "effects", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_struct_type(prop, "MaterialPaintLayer");
  RNA_def_property_collection_funcs(prop,
                                    "rna_MaterialPaintLayer_effects_begin",
                                    "rna_iterator_listbase_next",
                                    "rna_iterator_listbase_end",
                                    "rna_iterator_listbase_get",
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop,
                           "Corrections",
                           "The content corrections laid over the layer's colour, bottom to top");

  prop = RNA_def_property(srna, "is_mask_base", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", MA_PAINT_LAYER_MASK_BASE);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Is Mask Base", "Whether this is the base mask item of a layer");

  prop = RNA_def_property(srna, "mask_value", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_float_funcs(prop,
                               "rna_MaterialPaintLayer_mask_value_get",
                               "rna_MaterialPaintLayer_mask_value_set",
                               nullptr);
  RNA_def_property_ui_text(
      prop,
      "Mask Value",
      "The constant coverage of a mask item while it has no map: one shows the layer, zero hides it");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "mask_color", PROP_FLOAT, PROP_COLOR);
  RNA_def_property_array(prop, 3);
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_float_funcs(prop,
                               "rna_MaterialPaintLayer_mask_color_get",
                               "rna_MaterialPaintLayer_mask_color_set",
                               nullptr);
  RNA_def_property_ui_text(
      prop,
      "Mask Color",
      "The constant coverage of a mask item as a grey swatch: white shows the layer, black hides it");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  func = RNA_def_function(srna, "mask_add", "rna_MaterialPaintLayer_mask_add");
  RNA_def_function_ui_description(
      func, "Give the layer a base mask of a constant value; a stroke in Mask mode paints it");
  RNA_def_function_flag(func, FUNC_SELF_AS_RNA | FUNC_USE_REPORTS);
  parm = RNA_def_float(func, "value", 1.0f, 0.0f, 1.0f, "Value", "Constant coverage", 0.0f, 1.0f);
  parm = RNA_def_pointer(func, "item", "MaterialPaintLayer", "", "The new mask item");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "effect_add", "rna_MaterialPaintLayer_effect_add");
  RNA_def_function_ui_description(
      func,
      "Add a content correction: a map laid over the layer's colour, painted into like a layer, or "
      "with the Constant source one flat colour (see fill_color) laid at the correction's opacity");
  RNA_def_function_flag(func, FUNC_SELF_AS_RNA | FUNC_USE_REPORTS);
  RNA_def_string(func,
                 "name",
                 nullptr,
                 MAX_NAME,
                 "Name",
                 "Name of the new correction, empty for the unique default");
  /* Last, and optional: existing callers pass the name alone. */
  RNA_def_enum(func,
               "source",
               rna_enum_material_paint_layer_source_items,
               MA_PAINT_LAYER_SOURCE_IMAGE,
               "Source",
               "Image for a painted map, Constant for a flat colour");
  parm = RNA_def_pointer(func, "effect", "MaterialPaintLayer", "", "The new correction");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "mask_item_add", "rna_MaterialPaintLayer_mask_item_add");
  RNA_def_function_ui_description(
      func,
      "Add a mask correction over the layer's mask (made white first when there is none): a grey "
      "map painted in Mask mode, or with the Constant source a flat grey (see mask_value), laid by "
      "the correction's own blend and opacity");
  RNA_def_function_flag(func, FUNC_SELF_AS_RNA | FUNC_USE_REPORTS);
  RNA_def_string(func,
                 "name",
                 nullptr,
                 MAX_NAME,
                 "Name",
                 "Name of the new correction, empty for the unique default");
  RNA_def_enum(func,
               "source",
               rna_enum_material_paint_layer_source_items,
               MA_PAINT_LAYER_SOURCE_IMAGE,
               "Source",
               "Image for a painted map, Constant for a flat grey");
  parm = RNA_def_pointer(func, "item", "MaterialPaintLayer", "", "The new mask correction");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "mask_remove", "rna_MaterialPaintLayer_mask_remove");
  RNA_def_function_ui_description(func, "Remove the layer's mask");
  RNA_def_function_flag(func, FUNC_SELF_AS_RNA | FUNC_USE_REPORTS);

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

/** The overrides of one (row, channel) pair; every row has one per channel, records or not. */
static void rna_def_material_paint_layer_channel_settings(BlenderRNA *brna)
{
  StructRNA *srna = RNA_def_struct(brna, "MaterialPaintLayerChannelSettings", nullptr);
  RNA_def_struct_sdna(srna, "MaterialPaintLayerChannelSettings");
  RNA_def_struct_ui_text(srna,
                         "Paint Layer Channel Settings",
                         "The opacity a paint layer row is laid with in one paint channel");

  PropertyRNA *prop = RNA_def_property(srna, "channel", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_funcs(
      prop, "rna_MaterialPaintLayerChannelSettings_channel_get", nullptr, nullptr);
  RNA_def_property_enum_items(prop, rna_enum_material_paint_channel_items);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Channel", "The paint channel these settings are for");

  prop = RNA_def_property(srna, "opacity", PROP_FLOAT, PROP_PERCENTAGE);
  RNA_def_property_float_funcs(prop,
                               "rna_MaterialPaintLayerChannelSettings_opacity_get",
                               "rna_MaterialPaintLayerChannelSettings_opacity_set",
                               nullptr);
  RNA_def_property_range(prop, 0.0f, 100.0f);
  RNA_def_property_ui_text(
      prop, "Opacity", "Opacity of the row in this channel, applied over the row's own");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");
}

static void rna_def_material_paint_layer_channel(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "MaterialPaintLayerChannel", nullptr);
  RNA_def_struct_sdna(srna, "MaterialPaintLayerChannel");
  RNA_def_struct_path_func(srna, "rna_MaterialPaintLayerChannel_path");
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

  prop = RNA_def_property(srna, "color", PROP_FLOAT, PROP_COLOR);
  RNA_def_property_array(prop, 3);
  RNA_def_property_float_funcs(prop,
                               "rna_MaterialPaintLayerChannel_color_get",
                               "rna_MaterialPaintLayerChannel_color_set",
                               nullptr);
  RNA_def_property_ui_text(prop, "Color", "The colour channels' constant, without alpha");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  /* Read-only in the sense of the shared widget: it reads a layer channel exactly as it reads a
   * brush channel (on for ENABLED). The setter routes through BKE_paint_layers_channel_set_enabled. */
  prop = RNA_def_property(srna, "use", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(
      prop, "rna_MaterialPaintLayerChannel_use_get", "rna_MaterialPaintLayerChannel_use_set");
  RNA_def_property_ui_text(prop, "Enabled", "Whether the channel takes part");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "invert", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(
      prop, "rna_MaterialPaintLayerChannel_invert_get", "rna_MaterialPaintLayerChannel_invert_set");
  RNA_def_property_ui_text(
      prop, "Invert", "Read the map's grey (or the flat grey) of a mask correction as 1 - value");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  /* The scalar channels' single value, stored in `value[0]`; colour channels use `value`. */
  prop = RNA_def_property(srna, "value_scalar", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_funcs(prop,
                               "rna_MaterialPaintLayerChannel_value_scalar_get",
                               "rna_MaterialPaintLayerChannel_value_scalar_set",
                               "rna_MaterialPaintLayerChannel_value_scalar_range");
  RNA_def_property_ui_range(prop, 0.0f, 1.0f, 0.01, 3);
  RNA_def_property_ui_text(prop, "Value", "The scalar channel's constant value");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  /* The scalar channels' grayscale swatch, matching the brush channel's value_color. */
  prop = RNA_def_property(srna, "value_color", PROP_FLOAT, PROP_COLOR);
  RNA_def_property_array(prop, 3);
  RNA_def_property_float_funcs(prop,
                               "rna_MaterialPaintLayerChannel_value_color_get",
                               "rna_MaterialPaintLayerChannel_value_color_set",
                               nullptr);
  RNA_def_property_ui_text(prop, "Value Color", "The scalar channel's value as a gradient swatch");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");
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
  RNA_def_function_ui_description(func, "Add a paint layer relative to an anchor row");
  RNA_def_function_flag(func, FUNC_USE_MAIN | FUNC_USE_REPORTS);
  RNA_def_string(func,
                 "name",
                 nullptr,
                 MAX_NAME,
                 "Name",
                 "Name of the new layer, empty for the unique default");
  parm = RNA_def_pointer(
      func, "anchor", "MaterialPaintLayer", "", "Anchor row, or none for the top of the stack");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_RNAPTR);
  RNA_def_parameter_clear_flags(parm, PROP_THICK_WRAP, ParameterFlag(0));
  parm = RNA_def_enum(func,
                      "place",
                      rna_enum_material_paint_layer_place_items,
                      0,
                      "Place",
                      "Where to put the layer relative to the anchor");
  RNA_def_boolean(
      func, "make_active", true, "Make Active", "Move the active-layer cursor onto the new row");
  /* Last, and optional: existing callers pass the earlier parameters by position or keyword. */
  RNA_def_enum(func,
               "source",
               rna_enum_material_paint_layer_source_items,
               MA_PAINT_LAYER_SOURCE_IMAGE,
               "Source",
               "Source of the new layer");
  RNA_def_boolean(func,
                  "create_maps",
                  false,
                  "Create Maps",
                  "Create a map for every channel of the material's set now, at the material's "
                  "image size, instead of on the first stroke");
  parm = RNA_def_pointer(func, "layer", "MaterialPaintLayer", "", "The newly created paint layer");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "paste_correction", "rna_Material_paint_layers_paste_correction");
  RNA_def_function_ui_description(
      func,
      "Paste a copy of a content correction or a mask correction (of this or another material) onto "
      "a layer");
  RNA_def_function_flag(func, FUNC_USE_MAIN | FUNC_USE_REPORTS);
  parm = RNA_def_pointer(func,
                         "source",
                         "MaterialPaintLayer",
                         "Correction",
                         "The correction or mask item to copy, possibly of another material");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED | PARM_RNAPTR);
  RNA_def_parameter_clear_flags(parm, PROP_THICK_WRAP, ParameterFlag(0));
  parm = RNA_def_pointer(func,
                         "target",
                         "MaterialPaintLayer",
                         "Layer",
                         "The layer of this material to paste the copy onto");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED | PARM_RNAPTR);
  RNA_def_parameter_clear_flags(parm, PROP_THICK_WRAP, ParameterFlag(0));
  parm = RNA_def_pointer(func, "copy", "MaterialPaintLayer", "", "The pasted copy");
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

  func = RNA_def_function(srna, "move", "rna_Material_paint_layers_move");
  RNA_def_function_ui_description(func, "Move a paint layer relative to an anchor");
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
  RNA_def_function_ui_description(func, "Move a paint layer to a position in the stack");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  parm = RNA_def_pointer(
      func, "layer", "MaterialPaintLayer", "Paint Layer", "The layer to reorder");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED | PARM_RNAPTR);
  RNA_def_parameter_clear_flags(parm, PROP_THICK_WRAP, ParameterFlag(0));
  RNA_def_int(func, "index", 0, 0, INT_MAX, "Index", "Position, bottom to top", 0, INT_MAX);

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

  func = RNA_def_function(srna, "merge_down", "rna_Material_paint_layers_merge_down");
  RNA_def_function_ui_description(
      func, "Group two neighbouring paint layers into a new folder, which takes their place");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  parm = RNA_def_pointer(
      func, "upper", "MaterialPaintLayer", "Paint Layer", "The upper of the two layers");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED | PARM_RNAPTR);
  RNA_def_parameter_clear_flags(parm, PROP_THICK_WRAP, ParameterFlag(0));
  parm = RNA_def_pointer(
      func, "lower", "MaterialPaintLayer", "Paint Layer", "The lower of the two layers");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED | PARM_RNAPTR);
  RNA_def_parameter_clear_flags(parm, PROP_THICK_WRAP, ParameterFlag(0));
  parm = RNA_def_pointer(func, "folder", "MaterialPaintLayer", "", "The new folder");
  RNA_def_function_return(func, parm);

  prop = RNA_def_property(srna, "active", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "MaterialPaintLayer");
  RNA_def_property_pointer_funcs(prop,
                                 "rna_Material_paint_layers_active_get",
                                 "rna_Material_paint_layers_active_set",
                                 nullptr,
                                 nullptr);
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Active Layer", "Active paint layer of the material");

  /* `active` answers the item's row, so the tab needs the item itself to draw its own controls. */
  prop = RNA_def_property(srna, "active_mask_item", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "MaterialPaintLayer");
  RNA_def_property_pointer_funcs(
      prop, "rna_Material_paint_layers_active_mask_item_get", nullptr, nullptr, nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop, "Active Mask Correction", "Active mask correction of the active layer, if one is active");
}

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
  RNA_def_property_ui_text(prop, "Paint Layers", "The material's paint layer stack, bottom to top");
  rna_def_material_paint_layers(brna, prop);

  prop = RNA_def_property(srna, "is_layered", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop, "rna_Material_is_layered_get", nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop, "Layered Material", "The material's stack is a DNA description of its paint layers");

  prop = RNA_def_property(srna, "paint_layers_image_size", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "paint_layers_image_size");
  RNA_def_property_enum_items(prop, rna_enum_material_paint_layers_image_size_items);
  RNA_def_property_enum_default(prop, PAINT_NEW_CHANNEL_IMAGE_SIZE_4K);
  RNA_def_property_ui_text(prop,
                           "Image Size",
                           "Width and height of the maps created for this material's layers, "
                           "masks and fills; existing maps keep their size");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, nullptr);

  prop = RNA_def_property(srna, "paint_layers_channels", PROP_ENUM, PROP_NONE);
  RNA_def_property_flag(prop, PROP_ENUM_FLAG);
  RNA_def_property_enum_items(prop, rna_enum_material_paint_channel_flag_items);
  RNA_def_property_enum_funcs(prop,
                              "rna_Material_paint_layers_channels_get",
                              "rna_Material_paint_layers_channels_set",
                              nullptr);
  RNA_def_property_ui_text(
      prop,
      "Channels",
      "The material's global paint channel set; while unset it is derived from the layer records "
      "and the build default. Base Color is always in the set");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, nullptr);

  {
    FunctionRNA *func = RNA_def_function(
        srna, "paint_layers_channel_in_set", "rna_Material_paint_layers_channel_in_set");
    RNA_def_function_ui_description(
        func, "Whether a channel is in the material's global paint channel set");
    PropertyRNA *parm = RNA_def_enum(func,
                                     "channel",
                                     rna_enum_material_paint_channel_items,
                                     PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                     "Channel",
                                     "Channel to test");
    RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
    parm = RNA_def_boolean(func, "result", false, "In Set", "Whether the channel is in the set");
    RNA_def_function_return(func, parm);
  }

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
  rna_def_material_paint_layer_issue(brna);
  rna_def_material_paint_layer(brna);
  rna_def_material_paint_layer_channel(brna);
  rna_def_material_paint_layer_channel_settings(brna);

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
