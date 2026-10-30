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
#include <mutex>
#include <string>
#include <unordered_set>

#include "DNA_brush_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_paint_vector_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "BLI_math_gradient.hh"
#include "BLI_math_rotation.h"

#include "BLT_translation.hh"

#include "BKE_customdata.hh"
#include "BKE_library.hh"
#include "BKE_paint.hh"
/* Outside #RNA_RUNTIME: the tile range, Height scale and response limit constants are used by the
 * RNA definitions too. */
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_object_mask.hh"

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

/* The mapping projection. Only UV exists so far; the enum is stored from the start so a second
 * space never renumbers files. */
static const EnumPropertyItem rna_enum_material_paint_layer_mapping_space_items[] = {
    {MA_PAINT_LAYER_MAPPING_SPACE_UV, "UV", 0, "UV", "Project with the stack UV map"},
    {0, nullptr, 0, nullptr, nullptr},
};

/* What the row's maps do outside the unit UV square after the mapping. */
static const EnumPropertyItem rna_enum_material_paint_layer_mapping_wrap_items[] = {
    {MA_PAINT_LAYER_MAPPING_WRAP_REPEAT, "REPEAT", 0, "Repeat", "Repeat along both axes"},
    {MA_PAINT_LAYER_MAPPING_WRAP_REPEAT_X,
     "REPEAT_X",
     0,
     "Repeat Horizontally",
     "Repeat along U; transparent outside the UV square along V"},
    {MA_PAINT_LAYER_MAPPING_WRAP_REPEAT_Y,
     "REPEAT_Y",
     0,
     "Repeat Vertically",
     "Repeat along V; transparent outside the UV square along U"},
    {MA_PAINT_LAYER_MAPPING_WRAP_CLIP,
     "CLIP",
     0,
     "Clip",
     "One copy; transparent outside the UV square on both axes"},
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

/**
 * The per (row, channel) blend: -1 is the row's own, the rest are the user-facing blends. The
 * Normal channel offers its own pair instead, through the item callback; the internal Normal
 * Combine and Replace are never offered on the other channels.
 */
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
    {MA_PAINT_LAYER_BLEND_LINEAR_LIGHT, "LINEAR_LIGHT", 0, "Linear Light", "Linear light blend"},
    {MA_PAINT_LAYER_BLEND_HUE, "HUE", 0, "Hue", "Hue blend"},
    {MA_PAINT_LAYER_BLEND_SATURATION, "SATURATION", 0, "Saturation", "Saturation blend"},
    {MA_PAINT_LAYER_BLEND_COLOR, "COLOR", 0, "Color", "Color blend"},
    {MA_PAINT_LAYER_BLEND_VALUE, "VALUE", 0, "Value", "Value blend"},
    {0, nullptr, 0, nullptr, nullptr},
};

/**
 * The Height channel's own blend: how a row's Height record is laid over the height below it. Every
 * mode works on the distance from the neutral level, so a neutral record changes nothing whatever
 * the mode; the values are not the colour blends' codes. Inherit follows the row's blend: Mix, Add
 * and Subtract keep their meaning, any other blend reads as Mix. Only the Height channel offers
 * them.
 */
static const EnumPropertyItem rna_enum_material_paint_layer_height_blend_items[] = {
    {MA_PAINT_LAYER_HEIGHT_BLEND_INHERIT,
     "INHERIT",
     0,
     "Inherit",
     "Follows the row's blend; Mix, Add and Subtract keep their meaning around the neutral level, "
     "any other blend reads as Mix"},
    {MA_PAINT_LAYER_HEIGHT_BLEND_MIX,
     "MIX",
     0,
     "Mix",
     "Blend between the height below and the record, by the row's factor"},
    {MA_PAINT_LAYER_HEIGHT_BLEND_ADD,
     "ADD",
     0,
     "Add",
     "Add the record's distance from the neutral level to the height below"},
    {MA_PAINT_LAYER_HEIGHT_BLEND_SUBTRACT,
     "SUBTRACT",
     0,
     "Subtract",
     "Subtract the record's distance from the neutral level from the height below"},
    {MA_PAINT_LAYER_HEIGHT_BLEND_COMPARE,
     "COMPARE",
     0,
     "Compare",
     "The greater height wins; where the row won, its alpha covers its other channels"},
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

static const EnumPropertyItem rna_enum_material_paint_layers_preview_size_items[] = {
    {MA_PAINT_PREVIEW_SIZE_AUTO,
     "AUTO",
     0,
     "Auto",
     "A quarter of the layer's size, 256 pixels at the least"},
    {MA_PAINT_PREVIEW_SIZE_256, "256", 0, "256", ""},
    {MA_PAINT_PREVIEW_SIZE_512, "512", 0, "512", ""},
    {MA_PAINT_PREVIEW_SIZE_1024, "1024", 0, "1024", ""},
    {MA_PAINT_PREVIEW_SIZE_2048, "2048", 0, "2048", ""},
    {MA_PAINT_PREVIEW_SIZE_4096, "4096", 0, "4096", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem rna_enum_material_paint_layers_preview_delay_items[] = {
    {MA_PAINT_PREVIEW_DELAY_5S, "5", 0, "5 s", ""},
    {MA_PAINT_PREVIEW_DELAY_10S, "10", 0, "10 s", ""},
    {MA_PAINT_PREVIEW_DELAY_15S, "15", 0, "15 s", ""},
    {MA_PAINT_PREVIEW_DELAY_25S, "25", 0, "25 s", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem rna_enum_material_paint_layers_composite_device_items[] = {
    {MA_PAINT_COMPOSITE_GPU,
     "GPU",
     0,
     "GPU",
     "Shade the Image Editor preview with compute shaders (falls back to the CPU for rows they "
     "cannot lay)"},
    {MA_PAINT_COMPOSITE_CPU, "CPU", 0, "CPU", "Shade the Image Editor preview on the CPU"},
    {0, nullptr, 0, nullptr, nullptr},
};

/* The display tags of a layer group. Outside the #RNA_RUNTIME split, unlike the property definition
 * that uses it: the `#else` branch below is compiled only into the makesrna generator, so a
 * definition placed there never reaches bf_rna -- and #OUTLINER_OT_stack_layer_color_tag_set links
 * against this table too. Values are #eMaterialPaintLayerColorTag. */
const EnumPropertyItem rna_enum_material_paint_layer_color_items[] = {
    {MA_PAINT_LAYER_COLOR_NONE, "NONE", ICON_X, "None", "Remove the group's color tag"},
    {MA_PAINT_LAYER_COLOR_01, "COLOR_01", ICON_COLLECTION_COLOR_01, "Color 01", ""},
    {MA_PAINT_LAYER_COLOR_02, "COLOR_02", ICON_COLLECTION_COLOR_02, "Color 02", ""},
    {MA_PAINT_LAYER_COLOR_03, "COLOR_03", ICON_COLLECTION_COLOR_03, "Color 03", ""},
    {MA_PAINT_LAYER_COLOR_04, "COLOR_04", ICON_COLLECTION_COLOR_04, "Color 04", ""},
    {MA_PAINT_LAYER_COLOR_05, "COLOR_05", ICON_COLLECTION_COLOR_05, "Color 05", ""},
    {MA_PAINT_LAYER_COLOR_06, "COLOR_06", ICON_COLLECTION_COLOR_06, "Color 06", ""},
    {MA_PAINT_LAYER_COLOR_07, "COLOR_07", ICON_COLLECTION_COLOR_07, "Color 07", ""},
    {MA_PAINT_LAYER_COLOR_08, "COLOR_08", ICON_COLLECTION_COLOR_08, "Color 08", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

/* Fallback for makesrna, which requires a static item list even when the runtime itemf builds the
 * real, filtered list. The itemf replaces it in the UI. Kept outside #ifdef RNA_RUNTIME because the
 * property definition is generated in the other branch. */
static const EnumPropertyItem rna_enum_material_paint_layer_anchor_point_target_items[] = {
    {0, "NONE", 0, "None", "This reader lays nothing"},
    {0, nullptr, 0, nullptr, nullptr},
};

}

#ifdef RNA_RUNTIME

#  include "MEM_guardedalloc.h"

#  include "DNA_gpencil_legacy_types.h"
#  include "DNA_meshdata_types.h"
#  include "DNA_node_types.h"
#  include "DNA_object_types.h"

#  include "ED_image.hh"
#  include "DNA_screen_types.h"
#  include "DNA_space_types.h"

#  include <fmt/format.h>
#  include <mutex>
#  include <optional>
#  include <string>
#  include <unordered_set>

#  include "BLI_listbase.h"
#  include "BLI_math_vector.h"
#  include "BLI_string_utf8.h"
#  include "BLI_uuid.h"

#  include "BKE_attribute.h"
#  include "BKE_attribute.hh"
#  include "BKE_brush.hh"
#  include "BKE_colorband.hh"
#  include "BKE_context.hh"
#  include "BKE_editmesh.hh"
#  include "BKE_global.hh"
#  include "BKE_gpencil_legacy.h"
#  include "BKE_idprop.hh"
#  include "BKE_grease_pencil.hh"
#  include "BKE_lib_id.hh"
#  include "BKE_main.hh"
#  include "BKE_material.hh"
#  include "BKE_mesh.hh"
#  include "BKE_mesh_types.hh"
#  include "BKE_node.hh"
#  include "BKE_paint.hh"
#  include "BKE_paint_layers.hh"
#  include "BKE_paint_layers_asset.hh"
#  include "BKE_paint_layers_gradient.hh"
#  include "BKE_paint_layers_composite.hh"
#  include "BKE_paint_layers_edit.hh"
#  include "BKE_paint_layers_generate.hh"
#  include "BKE_paint_layers_target.hh"
#  include "BKE_paint_material_composite.hh"
#  include "BKE_report.hh"
#  include "BKE_scene.hh"
#  include "BKE_texture.h"
#  include "BKE_workspace.hh"

#  include "BLI_string.h"

#  include "DEG_depsgraph.hh"
#  include "DEG_depsgraph_build.hh"

#  include "ED_gpencil_legacy.hh"
#  include "ED_image.hh"
#  include "ED_node.hh"
#  include "ED_outliner_stack.hh"
#  include "ED_paint.hh"
#  include "ED_screen.hh"
#  include "ED_undo.hh"

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

/** The curve container of a Curve Patch row's payload, as an ID pointer. */
static PointerRNA rna_MaterialPaintLayerCurvePatch_vectors_get(PointerRNA *ptr)
{
  const MaterialPaintLayerCurvePatch *payload = static_cast<const MaterialPaintLayerCurvePatch *>(
      ptr->data);
  if (payload == nullptr || payload->vectors == nullptr) {
    return PointerRNA_NULL;
  }
  return RNA_id_pointer_create(&payload->vectors->id);
}

/** The owned brush snapshot of a Curve Patch row, as an ID pointer (read-only in RNA). */
static PointerRNA rna_MaterialPaintLayerCurvePatch_brush_get(PointerRNA *ptr)
{
  const MaterialPaintLayerCurvePatch *payload = static_cast<const MaterialPaintLayerCurvePatch *>(
      ptr->data);
  if (payload == nullptr || payload->brush == nullptr) {
    return PointerRNA_NULL;
  }
  return RNA_id_pointer_create(&payload->brush->id);
}

/** Only a brush a Curve Patch can stamp with is offered (and accepted from a drop); the effects'
 * own private snapshots are not sources, so one effect cannot adopt another's state by accident. */
static bool rna_MaterialPaintLayerCurvePatch_brush_poll(PointerRNA * /*ptr*/, const PointerRNA value)
{
  const Brush *brush = static_cast<const Brush *>(value.data);
  return brush != nullptr && bke::brush::supports_curve_patch(*brush) &&
         !STRPREFIX(brush->id.name + 2, ".CP Brush ");
}

/** Assigning a brush gives the effect a fresh private duplicate of it (the effect never shares a
 * brush with the user's library), so a drop of a brush asset onto the field is the whole gesture. */
static void rna_MaterialPaintLayerCurvePatch_brush_set(PointerRNA *ptr,
                                                       PointerRNA value,
                                                       ReportList *reports)
{
  MaterialPaintLayerCurvePatch *payload = static_cast<MaterialPaintLayerCurvePatch *>(ptr->data);
  Material *ma = (ptr->owner_id != nullptr && GS(ptr->owner_id->name) == ID_MA) ?
                     id_cast<Material *>(ptr->owner_id) :
                     nullptr;
  const Brush *source = static_cast<const Brush *>(value.data);
  Main *bmain = G_MAIN;
  if (payload == nullptr || ma == nullptr || source == nullptr || bmain == nullptr ||
      source == payload->brush)
  {
    return;
  }
  if (!rna_MaterialPaintLayerCurvePatch_brush_poll(ptr, value)) {
    BKE_report(reports, RPT_WARNING, "That brush cannot stamp a Curve Patch");
    return;
  }
  /* The previous snapshot is freed by the replacement: refuse while a session holds it as the
   * active brush, which would leave the modal editing through a freed brush. */
  for (const Scene &scene : bmain->scenes) {
    if (scene.toolsettings != nullptr && scene.toolsettings->sculpt != nullptr &&
        scene.toolsettings->sculpt->paint.brush == payload->brush)
    {
      BKE_report(reports, RPT_WARNING, "Finish the Curve Patch session before changing its brush");
      return;
    }
  }
  MaterialPaintLayer *owner = nullptr;
  BKE_paint_layers_foreach(*ma, [&](MaterialPaintLayer &layer, MaterialPaintLayer * /*parent*/) {
    for (MaterialPaintLayer &effect : layer.effects) {
      if (effect.curve_patch == payload) {
        owner = &effect;
        return false;
      }
    }
    return true;
  });
  if (owner == nullptr || !BKE_paint_layers_curve_patch_brush_set(*ma, *bmain, *owner, *source)) {
    BKE_report(reports, RPT_ERROR, "Could not assign that brush to the Curve Patch");
    return;
  }
  WM_main_add_notifier(NC_BRUSH | NA_EDITED, nullptr);
  WM_main_add_notifier(NC_MATERIAL | ND_SHADING, &ma->id);
}

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
                                                         bool create_maps,
                                                         Material *source_material)
{
  /* The authoring policy is the shared BKE one (#BKE_paint_layers_add_with_policy): the same
   * default names and channels every other authoring path gets. An empty name means the unique
   * default. */
  PaintLayerAddParams params;
  switch (eMaterialPaintLayerSource(source)) {
    case MA_PAINT_LAYER_SOURCE_CONSTANT:
      params.kind = PaintLayerAddKind::Fill;
      break;
    case MA_PAINT_LAYER_SOURCE_GENERATOR:
      params.kind = PaintLayerAddKind::Gradient;
      break;
    case MA_PAINT_LAYER_SOURCE_STACK:
      params.kind = PaintLayerAddKind::Folder;
      break;
    case MA_PAINT_LAYER_SOURCE_MATERIAL:
      if (source_material == nullptr) {
        BKE_report(reports, RPT_ERROR, "A Material layer needs the material it reads");
        return nullptr;
      }
      params.kind = PaintLayerAddKind::Material;
      params.source = source_material;
      /* Each Material row reads a material of its own, so its settings are its own. */
      params.bmain = bmain;
      break;
    case MA_PAINT_LAYER_SOURCE_CURVE_PATCH:
      /* A Curve Patch is only ever an effect of a layer, never a layer of its own. */
      BKE_report(reports, RPT_ERROR, "A Curve Patch can only be added as an effect of a layer");
      return nullptr;
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
    BKE_report(reports,
               RPT_ERROR,
               params.kind == PaintLayerAddKind::Material ?
                   "Cannot add a layer reading that material here: it is this material, or it "
                   "already reads this one" :
                   "Cannot add a paint layer here");
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

/* The ID Browser popover assigns a data-block to a pointer property of an ID, and an operator has
 * none, so "add a Material layer" is the assignment of this write-only property: reading it always
 * answers none, setting it adds a row that reads the picked material above the active row. */
static PointerRNA rna_Material_paint_layer_add_material_get(PointerRNA * /*ptr*/)
{
  return RNA_id_pointer_create(nullptr);
}

/* The data-block the write-only "add" properties record between their setter and their update. The
 * row is not added in the setter: RNA's debug check requires a setter whose getter reads none to
 * leave the user count of the assigned ID untouched, and a new row or mask takes a user. One store
 * for the whole family, so one reset point; a setter and its update run back to back, so the slots
 * never interleave. */
struct PaintLayerAddPending {
  Material *material = nullptr;
  Image *image = nullptr;
};
static PaintLayerAddPending rna_paint_layer_add_pending;

static void rna_Material_paint_layer_add_material_set(PointerRNA * /*ptr*/,
                                                      PointerRNA value,
                                                      ReportList * /*reports*/)
{
  /* One store serves the whole family: clear it before writing, so a setter whose update never
   * runs (a `bpy` write with no context, say) cannot leave its value for the next update. */
  rna_paint_layer_add_pending = {};
  rna_paint_layer_add_pending.material = static_cast<Material *>(value.data);
}

static bool rna_Material_paint_layer_add_material_poll(PointerRNA *ptr, PointerRNA value)
{
  const Material *ma = id_cast<const Material *>(ptr->owner_id);
  const Material *source = static_cast<const Material *>(value.data);
  return ma == nullptr || source == nullptr || !BKE_paint_layers_material_depends_on(*source, *ma);
}

/* Adds the row (or the correction on the active row) for the material the setter recorded. The
 * popover assignment pushes no undo step of its own. */
static void rna_Material_paint_layer_add_material_do(bContext *C,
                                                     PointerRNA *ptr,
                                                     const PaintLayerAddKind kind)
{
  Material *source = rna_paint_layer_add_pending.material;
  rna_paint_layer_add_pending = {};
  Material *ma = id_cast<Material *>(ptr->owner_id);
  if (source == nullptr || ma == nullptr) {
    return;
  }
  ReportList *reports = CTX_wm_reports(C);
  if (!BKE_paint_layers_is_layered(*ma)) {
    BKE_report(reports, RPT_ERROR, "The material is not a layered material");
    return;
  }
  const bool correction = kind != PaintLayerAddKind::Material;
  const bool mask = kind == PaintLayerAddKind::MaskCorrectionMaterial;
  PaintLayerAddParams params;
  params.kind = kind;
  params.source = source;
  params.bmain = CTX_data_main(C);
  params.anchor = BKE_paint_layers_active_layer_get(*ma);
  MaterialPaintLayer *layer = BKE_paint_layers_add_with_policy(*ma, params);
  if (layer == nullptr) {
    BKE_report(reports,
               RPT_ERROR,
               correction ? "That material cannot be read here: there is no layer to correct, or "
                            "it is this material, or it already reads it" :
                            "That material cannot be read here: it is this material, or it "
                            "already reads it");
    return;
  }
  BKE_paint_layers_active_set(*ma, layer->marker);
  /* This path adds the row without going through the Stack Layers Add operator, so the Outliner
   * has to be told the row it now has is the active one -- otherwise the rows it cached before the
   * Add keep the old row lit. */
  ed::outliner::outliner_stack_row_added_sync(*C, ma->id, layer->marker);
  ED_undo_push(C,
               mask ? "Add Mask Material Correction" :
                      (correction ? "Add Material Correction" : "Add Material Layer"));
  WM_main_add_notifier(NC_MATERIAL | ND_SHADING, ptr->owner_id);
}

static void rna_Material_paint_layer_add_material_update(bContext *C, PointerRNA *ptr)
{
  rna_Material_paint_layer_add_material_do(C, ptr, PaintLayerAddKind::Material);
}

static void rna_Material_paint_layer_add_material_correction_update(bContext *C, PointerRNA *ptr)
{
  rna_Material_paint_layer_add_material_do(C, ptr, PaintLayerAddKind::CorrectionMaterial);
}

static void rna_Material_paint_layer_add_material_mask_correction_update(bContext *C,
                                                                        PointerRNA *ptr)
{
  rna_Material_paint_layer_add_material_do(C, ptr, PaintLayerAddKind::MaskCorrectionMaterial);
}

/* The same write-only assignment for a mask that reads an image: the ID Browser of the Add Mask
 * menu assigns the image here, and the update makes the mask that reads it. */
static PointerRNA rna_Material_paint_layer_add_image_mask_get(PointerRNA * /*ptr*/)
{
  return RNA_id_pointer_create(nullptr);
}

/* The image picked by the last assignment, consumed by the update that follows it. Kept out of the
 * setter for the reason #rna_paint_layer_add_pending is: the mask takes a user. */
static void rna_Material_paint_layer_add_image_mask_set(PointerRNA * /*ptr*/,
                                                        PointerRNA value,
                                                        ReportList * /*reports*/)
{
  /* See #rna_Material_paint_layer_add_material_set for the reset. */
  rna_paint_layer_add_pending = {};
  rna_paint_layer_add_pending.image = static_cast<Image *>(value.data);
}

static bool rna_Material_paint_layer_add_image_mask_poll(PointerRNA * /*ptr*/, PointerRNA value)
{
  return value.data != nullptr;
}

/* Gives the active layer a mask that reads the image the setter recorded. The popover assignment
 * pushes no undo step of its own. */
static void rna_Material_paint_layer_add_image_mask_update(bContext *C, PointerRNA *ptr)
{
  Image *image = rna_paint_layer_add_pending.image;
  rna_paint_layer_add_pending = {};
  Material *ma = id_cast<Material *>(ptr->owner_id);
  if (image == nullptr || ma == nullptr) {
    return;
  }
  MaterialPaintLayer *layer = BKE_paint_layers_active_layer_get(*ma);
  if (layer == nullptr) {
    BKE_report(CTX_wm_reports(C), RPT_WARNING, "Select a layer to give an image mask to");
    return;
  }
  /* The mask, its activation, the paint target and the Outliner sync are all
   * #ed::outliner::outliner_stack_row_mask_add_image's. */
  MaterialPaintLayer *mask = ed::outliner::outliner_stack_row_mask_add_image(
      *C, *ma, *layer, *image);
  if (mask == nullptr) {
    return;
  }
  ED_undo_push(C, "Add Image Mask");
  WM_main_add_notifier(NC_MATERIAL | ND_SHADING, ptr->owner_id);
}

static void rna_Material_paint_layers_remove(Material *ma,
                                             Main *bmain,
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
  /* The row is gone, so records naming its marker are orphans: drop them now instead of carrying
   * them to the next save. */
  BKE_paint_layer_object_masks_cleanup_all(*bmain);
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

static bool rna_Material_paint_layers_composite(Material *ma,
                                                ReportList *reports,
                                                const int channel,
                                                PointerRNA *image_ptr)
{
  Image *image = static_cast<Image *>(image_ptr->data);
  if (image == nullptr) {
    BKE_report(reports, RPT_ERROR, "No destination image");
    return false;
  }
  /* An RNA function is always reached like `exec`: there is no modal loop it could go into, so a
   * stale result is refused rather than waited out. A caller without a window manager (background
   * mode) is never refused: nothing there ever makes the result fresh.
   *
   * The freshness test is the BKE-only #BKE_paint_layers_is_stale, deliberately: RNA may not reach
   * into the editor for a window manager. That is enough because the editor stamps
   * #MA_PAINT_LAYERS_BAKE_SCHEDULED before every render it starts for the material, so the mark,
   * not a `WM_jobs_test`, keeps this answer in step with `ED_paint_layers_stale_or_pending`. */
  const bool headless = G_MAIN == nullptr || G_MAIN->wm.first == nullptr;
  if (BKE_paint_layers_bake_gate_decide(BKE_paint_layers_is_stale(*ma),
                                        /*is_invoke=*/false,
                                        headless) == PaintLayersBakeGateAction::Refuse)
  {
    BKE_report(reports,
               RPT_ERROR,
               "Paint layers bake is not up to date; wait until Material.paint_layers_is_stale is "
               "False (see bpy.ops.material.paint_layers_bake_now)");
    return false;
  }
  return BKE_paint_layers_composite_image(*ma, channel, *image, nullptr, reports);
}

static bool rna_Material_is_layered_get(PointerRNA *ptr)
{
  return BKE_paint_layers_is_layered(*id_cast<Material *>(ptr->owner_id));
}

static bool rna_Material_paint_layers_is_stale_get(PointerRNA *ptr)
{
  /* BKE-only signal: it cannot see a render the editor has started but not yet marked, which an
   * RNA getter could only learn from a window manager it may not reach. */
  return BKE_paint_layers_is_stale(*id_cast<Material *>(ptr->owner_id));
}

static bool rna_Material_smart_asset_instance_get(PointerRNA *ptr)
{
  return BKE_paint_layers_asset_instances_any(*id_cast<Material *>(ptr->owner_id));
}

/* The effective set, not the raw field: an unauthored field reads the derived set. */
static int rna_Material_paint_layers_channels_get(PointerRNA *ptr)
{
  return BKE_paint_layers_channel_set_mask_get(*id_cast<Material *>(ptr->owner_id));
}

static void rna_Material_paint_layers_channels_set(PointerRNA *ptr, int value)
{
  Material *ma = id_cast<Material *>(ptr->owner_id);
  /* Ignore empty sets: the set must have at least one channel. */
  if (uint16_t(value) == 0) {
    return;
  }
  BKE_paint_layers_channel_set_mask_set(*ma, uint16_t(value));
}

/**
 * Move Image Editors off a composite pass the set no longer holds: the Composite menu stops
 * listing it, and a selector resting on a missing item reads blank while the canvas keeps showing
 * the empty pass.
 */
static void rna_Material_paint_layers_channels_update(Main *bmain,
                                                      Scene * /*scene*/,
                                                      PointerRNA *ptr)
{
  Material *ma = id_cast<Material *>(ptr->owner_id);
  if (bmain != nullptr && BKE_paint_layers_is_layered(*ma)) {
    for (bScreen &screen : bmain->screens) {
      for (ScrArea &area : screen.areabase) {
        for (SpaceLink &space : area.spacedata) {
          if (space.spacetype != SPACE_IMAGE) {
            continue;
          }
          SpaceImage &sima = reinterpret_cast<SpaceImage &>(space);
          /* Only passes that are channels: Combined and Mask are not part of the set. */
          if ((sima.flag & SI_PAINT_COMPOSITE_MODE) == 0 ||
              sima.material_paint_pass >= PAINT_MATERIAL_CHANNEL_NUM ||
              ED_space_image_composite_material_get(bmain, sima.image) != ma)
          {
            continue;
          }
          sima.material_paint_pass = BKE_paint_layers_channel_clamp_to_set(
              *ma, sima.material_paint_pass);
        }
      }
    }
    /* The 3D Viewport's pass may have lost its channel too; the sync resets it to Combined. */
    BKE_paint_layers_view_pass_sync(bmain, *ma);
  }
  WM_main_add_notifier(NC_MATERIAL | ND_SHADING, nullptr);
}

/** Identifier of a 3D Viewport view pass item. Stable literals: an enum identifier reaches Python
 * as-is and is copied by reference. */
static const char *rna_Material_paint_layers_view_pass_identifier(const int pass)
{
  switch (pass) {
    case PAINT_LAYER_PASS_COMBINED:
      return "COMPOSITE_COMBINED";
    case PAINT_MATERIAL_CHANNEL_BASE_COLOR:
      return "COMPOSITE_BASE_COLOR";
    case PAINT_MATERIAL_CHANNEL_METALLIC:
      return "COMPOSITE_METALLIC";
    case PAINT_MATERIAL_CHANNEL_ROUGHNESS:
      return "COMPOSITE_ROUGHNESS";
    case PAINT_MATERIAL_CHANNEL_SPECULAR:
      return "COMPOSITE_SPECULAR";
    case PAINT_MATERIAL_CHANNEL_NORMAL:
      return "COMPOSITE_NORMAL";
    case PAINT_MATERIAL_CHANNEL_HEIGHT:
      return "COMPOSITE_HEIGHT";
    case PAINT_MATERIAL_CHANNEL_AO:
      return "COMPOSITE_AO";
    case PAINT_MATERIAL_CHANNEL_ALPHA:
      return "COMPOSITE_ALPHA";
    case PAINT_MATERIAL_CHANNEL_EMISSION:
      return "COMPOSITE_EMISSION";
    case PAINT_LAYER_MAP_MASK:
      return "COMPOSITE_MASK";
    default:
      return "COMPOSITE_COMBINED";
  }
}

/**
 * The passes the 3D Viewport can show for this material: Combined, then every channel of its paint
 * channel set that has a layer stack. A pass without a stack has nothing to show, so it is left
 * out rather than offered and silently falling back.
 */
static const EnumPropertyItem *rna_Material_paint_layers_view_pass_itemf(bContext *C,
                                                                        PointerRNA *ptr,
                                                                        PropertyRNA * /*prop*/,
                                                                        bool *r_free)
{
  Material *ma = id_cast<Material *>(ptr->owner_id);
  EnumPropertyItem *item = nullptr;
  int totitem = 0;

  EnumPropertyItem combined{};
  combined.value = PAINT_LAYER_PASS_COMBINED;
  combined.identifier = "COMPOSITE_COMBINED";
  combined.name = "Combined";
  combined.description = "Shade every channel together, as the material does";
  combined.icon = ICON_SHADING_RENDERED;
  RNA_enum_item_add(&item, &totitem, &combined);

  if (ma != nullptr && BKE_paint_layers_is_layered(*ma)) {
    for (const int pass : BKE_paint_layers_view_passes()) {
      if (pass == PAINT_LAYER_PASS_COMBINED || !BKE_paint_layers_view_pass_available(*ma, pass)) {
        continue;
      }
      const bool is_mask = pass == PAINT_LAYER_MAP_MASK;
      EnumPropertyItem pass_item{};
      pass_item.value = pass;
      pass_item.identifier = rna_Material_paint_layers_view_pass_identifier(pass);
      pass_item.name = is_mask ? "Mask" :
                                BKE_paint_material_channel_info(eMaterialPaintChannel(pass)).ui_name;
      pass_item.description = is_mask ? "Show the active layer's mask" :
                                        "Show only this channel while painting";
      pass_item.icon = is_mask ? ICON_MOD_MASK : ICON_NODE_COMPOSITING;
      RNA_enum_item_add(&item, &totitem, &pass_item);
    }

    /* Last, after a separator: the checkbox that keeps the pass equal with the Image Editor. An
     * action item, so Ctrl-Wheel steps over it. */
    EnumPropertyItem separator{};
    separator.identifier = "";
    RNA_enum_item_add(&item, &totitem, &separator);

    Main *bmain = (C != nullptr) ? CTX_data_main(C) : G_MAIN;
    EnumPropertyItem sync{};
    sync.value = RNA_ENUM_VALUE_ACTION;
    sync.identifier = "SYNC_VIEW_PASS";
    sync.name = "Sync Views";
    sync.description = "Show the same paint pass in the 3D Viewport and in the Image Editor";
    sync.icon = (bmain != nullptr && ED_paint_view_pass_sync_get(*bmain)) ? ICON_CHECKBOX_HLT :
                                                                            ICON_CHECKBOX_DEHLT;
    RNA_enum_item_add(&item, &totitem, &sync);
  }

  RNA_enum_item_end(&item, &totitem);
  *r_free = true;
  return item;
}

static int rna_Material_paint_layers_view_pass_get(PointerRNA *ptr)
{
  const Material *ma = id_cast<Material *>(ptr->owner_id);
  return (ma != nullptr) ? BKE_paint_layers_view_pass_get(*ma) : PAINT_LAYER_PASS_COMBINED;
}

static void rna_Material_paint_layers_view_pass_set(PointerRNA *ptr, int value)
{
  if (value == RNA_ENUM_VALUE_ACTION) {
    ED_paint_view_pass_sync_toggle(*G_MAIN);
    return;
  }
  Material *ma = id_cast<Material *>(ptr->owner_id);
  if (ma != nullptr) {
    BKE_paint_layers_view_pass_set(*ma, value);
  }
}

static void rna_Material_paint_layers_view_pass_update(Main *bmain,
                                                       Scene * /*scene*/,
                                                       PointerRNA *ptr)
{
  Material *ma = id_cast<Material *>(ptr->owner_id);
  Main *bmain_resolved = (bmain != nullptr) ? bmain : G_MAIN;
  if (ma != nullptr && bmain_resolved != nullptr) {
    /* The selector belongs to the active material, but a Sculpt Mode stroke can span several
     * selected objects: the editor gives every one of them the same pass and notifies. */
    ED_paint_material_view_pass_apply(*bmain_resolved, *ma);
  }
}

static float rna_Material_paint_layers_bump_strength_get(PointerRNA *ptr)
{
  const Material *ma = id_cast<Material *>(ptr->owner_id);
  return (ma != nullptr) ? BKE_paint_layers_bump_strength(*ma) : 1.0f;
}

static bool rna_Material_paint_layers_has_bump_get(PointerRNA *ptr)
{
  const Material *ma = id_cast<Material *>(ptr->owner_id);
  return (ma != nullptr) && BKE_paint_layers_has_bump(*ma);
}

static bool rna_Material_paint_layers_has_normal_map_get(PointerRNA *ptr)
{
  const Material *ma = id_cast<Material *>(ptr->owner_id);
  return (ma != nullptr) && BKE_paint_layers_has_normal_map(*ma);
}

static void rna_Material_paint_layers_bump_strength_set(PointerRNA *ptr, float value)
{
  if (Material *ma = id_cast<Material *>(ptr->owner_id)) {
    BKE_paint_layers_bump_strength_set(*ma, value);
  }
}

static float rna_Material_paint_layers_bump_distance_get(PointerRNA *ptr)
{
  const Material *ma = id_cast<Material *>(ptr->owner_id);
  return (ma != nullptr) ? BKE_paint_layers_bump_distance(*ma) : 1.0f;
}

static void rna_Material_paint_layers_bump_distance_set(PointerRNA *ptr, float value)
{
  if (Material *ma = id_cast<Material *>(ptr->owner_id)) {
    BKE_paint_layers_bump_distance_set(*ma, value);
  }
}

static float rna_Material_paint_layers_normal_strength_get(PointerRNA *ptr)
{
  const Material *ma = id_cast<Material *>(ptr->owner_id);
  return (ma != nullptr) ? BKE_paint_layers_normal_strength(*ma) : 1.0f;
}

static void rna_Material_paint_layers_normal_strength_set(PointerRNA *ptr, float value)
{
  if (Material *ma = id_cast<Material *>(ptr->owner_id)) {
    BKE_paint_layers_normal_strength_set(*ma, value);
  }
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

static bool rna_MaterialPaintLayer_locked_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return BKE_paint_layers_is_locked(*layer);
}

static void rna_MaterialPaintLayer_locked_set(PointerRNA *ptr, bool value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    BKE_paint_layers_set_locked(*ma, layer, value);
  }
}

static void rna_MaterialPaintLayer_locked_update(Main * /*bmain*/,
                                                 Scene * /*scene*/,
                                                 PointerRNA *ptr)
{
  /* Only the UI shows the lock: a redraw, unlike #rna_Material_update, which re-evaluates the
   * shading. */
  Material *ma = id_cast<Material *>(ptr->owner_id);
  WM_main_add_notifier(NC_MATERIAL | ND_SHADING_DRAW, ma);
}

static int rna_MaterialPaintLayer_color_tag_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  /* A file written by a build that knew fewer values, or garbage in what used to be padding,
   * reads as "no tag" rather than as an enum value RNA cannot name. */
  if (layer->color_tag < MA_PAINT_LAYER_COLOR_NONE ||
      layer->color_tag >= MA_PAINT_LAYER_COLOR_TOT)
  {
    return MA_PAINT_LAYER_COLOR_NONE;
  }
  return layer->color_tag;
}

static void rna_MaterialPaintLayer_color_tag_set(PointerRNA *ptr, int value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    BKE_paint_layers_set_color_tag(*ma, layer, value);
  }
}

/**
 * Whether the row keeps a UV mapping at all for the UI to edit: everything the row may carry --
 * a Fill with a map in at least one channel or through its mask, a Material row whose source reads
 * the active UVs -- plus a Fill that has no map yet, whose mapping belongs to the row and not to
 * the map it happens to read. The panel shows the Mapping block and the gizmo resolves a row only
 * while this is true; the graph and the CPU ask #BKE_paint_layers_mapping_remaps instead.
 */
static bool rna_MaterialPaintLayer_mapping_available_get(PointerRNA *ptr)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  Material *ma = rna_paint_layer_material(ptr, layer);
  return ma != nullptr && BKE_paint_layers_mapping_available(*ma, *layer);
}

static bool rna_MaterialPaintLayer_mapping_applies_get(PointerRNA *ptr)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  Material *ma = rna_paint_layer_material(ptr, layer);
  return ma != nullptr && BKE_paint_layers_mapping_applies(*ma, *layer);
}

static bool rna_MaterialPaintLayer_mapping_scale_lock_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return BKE_paint_layers_mapping_scale_lock_get(*layer);
}

static void rna_MaterialPaintLayer_mapping_scale_lock_set(PointerRNA *ptr, bool value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    /* The raw toggle: locking joins the axes, which is a scale change and may slide the tile. The
     * UI's Lock (``tile_scale_lock``) keeps the tile in place. */
    BKE_paint_layers_mapping_set_scale_lock(*ma, layer, value);
  }
}

static bool rna_MaterialPaintLayer_mapping_enabled_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return BKE_paint_layers_mapping_enabled_get(*layer);
}

static void rna_MaterialPaintLayer_mapping_enabled_set(PointerRNA *ptr, bool value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    /* Refused while the row can carry no mapping at all; the editable callback explains why. */
    BKE_paint_layers_mapping_set_enabled(*ma, layer, value);
  }
}

static int rna_MaterialPaintLayer_mapping_enabled_editable(const PointerRNA *ptr,
                                                           const char **r_info)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  PointerRNA ptr_mutable = *ptr;
  Material *ma = rna_paint_layer_material(&ptr_mutable, layer);
  if (ma == nullptr) {
    return 0;
  }
  /* An enabled mapping stays switchable off even after its map is gone. */
  if (BKE_paint_layers_mapping_enabled_get(*layer) ||
      BKE_paint_layers_mapping_available(*ma, *layer))
  {
    return PROP_EDITABLE;
  }
  if (r_info != nullptr) {
    *r_info = N_("This row's source takes no UV mapping");
  }
  return 0;
}

static int rna_MaterialPaintLayer_mapping_space_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return layer->mapping.space;
}

static void rna_MaterialPaintLayer_mapping_offset_get(PointerRNA *ptr, float *value)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  copy_v2_v2(value, layer->mapping.offset);
}

static void rna_MaterialPaintLayer_mapping_offset_set(PointerRNA *ptr, const float *value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    BKE_paint_layers_mapping_set_offset(*ma, layer, value);
  }
}

static void rna_MaterialPaintLayer_mapping_scale_get(PointerRNA *ptr, float *value)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  /* Old files read back as zeroes; show the normalized value every consumer uses. */
  value[0] = BKE_paint_layers_mapping_scale_normalize(layer->mapping.scale[0]);
  value[1] = BKE_paint_layers_mapping_scale_normalize(layer->mapping.scale[1]);
}

static void rna_MaterialPaintLayer_mapping_scale_set(PointerRNA *ptr, const float *value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    /* The raw value: the stored offset is applied after the scale, so writing it alone slides the
     * tile across the canvas. This is the data's own meaning, for scripts and drivers; the UI
     * edits the tile the gizmo draws through ``tile_scale``, which follows the offset itself. */
    BKE_paint_layers_mapping_set_scale(*ma, layer, value);
  }
}

static float rna_MaterialPaintLayer_mapping_rotation_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return layer->mapping.rotation;
}

static void rna_MaterialPaintLayer_mapping_rotation_set(PointerRNA *ptr, float value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    /* The raw value: writing it alone turns the tile around the canvas origin. The UI turns it
     * around its centre through ``tile_rotation``. */
    BKE_paint_layers_mapping_set_rotation(*ma, layer, value);
  }
}

static void rna_MaterialPaintLayer_mapping_tile_center_get(PointerRNA *ptr, float *value)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  BKE_paint_layers_mapping_tile_center(*layer, value);
}

static void rna_MaterialPaintLayer_mapping_tile_scale_get(PointerRNA *ptr, float *value)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  value[0] = BKE_paint_layers_mapping_scale_normalize(layer->mapping.scale[0]);
  value[1] = BKE_paint_layers_mapping_scale_normalize(layer->mapping.scale[1]);
}

static void rna_MaterialPaintLayer_mapping_tile_scale_set(PointerRNA *ptr, const float *value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    BKE_paint_layers_mapping_set_scale_holding_tile(*ma, layer, value);
  }
}

static float rna_MaterialPaintLayer_mapping_tile_rotation_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return layer->mapping.rotation;
}

static void rna_MaterialPaintLayer_mapping_tile_rotation_set(PointerRNA *ptr, float value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    BKE_paint_layers_mapping_set_rotation_holding_tile(*ma, layer, value);
  }
}

static bool rna_MaterialPaintLayer_mapping_tile_scale_lock_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return BKE_paint_layers_mapping_scale_lock_get(*layer);
}

static void rna_MaterialPaintLayer_mapping_tile_scale_lock_set(PointerRNA *ptr, bool value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    BKE_paint_layers_mapping_set_scale_lock_holding_tile(*ma, layer, value);
  }
}

static int rna_MaterialPaintLayer_mapping_wrap_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return BKE_paint_layers_mapping_wrap_get(*layer);
}

static void rna_MaterialPaintLayer_mapping_wrap_set(PointerRNA *ptr, int value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    BKE_paint_layers_mapping_set_wrap(*ma, layer, int16_t(value));
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

static void rna_MaterialPaintLayer_material_set(PointerRNA *ptr,
                                                PointerRNA value,
                                                ReportList *reports)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  Material *ma = rna_paint_layer_material(ptr, layer);
  if (ma == nullptr) {
    return;
  }
  Material *source = static_cast<Material *>(value.data);
  if (source == nullptr) {
    BKE_report(reports, RPT_ERROR, "A Material layer needs the material it reads");
    return;
  }
  if (!BKE_paint_layers_set_material(*ma, layer, source)) {
    BKE_report(reports,
               RPT_ERROR,
               "That material cannot be read here: it is this material, or it already reads it");
  }
}

static bool rna_MaterialPaintLayer_material_poll(PointerRNA *ptr, PointerRNA value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  const Material *owner = rna_paint_layer_material(ptr, layer);
  const Material *source = static_cast<const Material *>(value.data);
  return owner == nullptr || source == nullptr ||
         !BKE_paint_layers_material_depends_on(*source, *owner);
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

static int rna_MaterialPaintLayer_mask_source_channel_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  const int channel = BKE_paint_layers_mask_source_channel_get(*layer);
  return channel >= 0 ? channel : int(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
}

static void rna_MaterialPaintLayer_mask_source_channel_set(PointerRNA *ptr, const int value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  Material *ma = id_cast<Material *>(ptr->owner_id);
  if (ma != nullptr) {
    BKE_paint_layers_mask_source_channel_set(*ma, layer, value);
  }
}

static void rna_MaterialPaintLayer_row_result_source_get(PointerRNA *ptr, char *value)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  if (BLI_uuid_is_nil(layer->row_result_source)) {
    value[0] = '\0';
    return;
  }
  BLI_uuid_format(value, layer->row_result_source);
}

static int rna_MaterialPaintLayer_row_result_source_length(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return BLI_uuid_is_nil(layer->row_result_source) ? 0 : UUID_STRING_SIZE - 1;
}

/* The marker of the Anchor Point a reader lays. Empty clears the reference. Setting checks the
 * row's kind and ownership through the BKE setter, so a plain row cannot acquire one. */
static void rna_MaterialPaintLayer_anchor_point_source_get(PointerRNA *ptr, char *value)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  const bUUID marker = BKE_paint_layers_anchor_point_source_get(*layer);
  if (BLI_uuid_is_nil(marker)) {
    value[0] = '\0';
    return;
  }
  BLI_uuid_format(value, marker);
}

static int rna_MaterialPaintLayer_anchor_point_source_length(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return BLI_uuid_is_nil(BKE_paint_layers_anchor_point_source_get(*layer)) ? 0 :
                                                                            UUID_STRING_SIZE - 1;
}

static void rna_MaterialPaintLayer_anchor_point_source_set(PointerRNA *ptr, const char *value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  Material *ma = rna_paint_layer_material(ptr, layer);
  if (ma == nullptr) {
    return;
  }
  bUUID marker = BLI_uuid_nil();
  if (value != nullptr && value[0] != '\0' && !BLI_uuid_parse_string(&marker, value)) {
    return;
  }
  BKE_paint_layers_anchor_point_source_set(*ma, layer, marker);
}

struct RNAAnchorPointTargetEntry {
  bUUID marker;
  std::string label;
};

/** The points this reader may read, in the order the stack lists them. */
static void rna_anchor_point_targets_get(const Material &ma,
                                         const MaterialPaintLayer &reader,
                                         Vector<RNAAnchorPointTargetEntry> &r_entries)
{
  if (!BKE_paint_layers_reads_anchor_point(reader)) {
    return;
  }
  BKE_paint_layers_foreach(
      ma, [&](const MaterialPaintLayer &row, const MaterialPaintLayer * /*parent*/) -> bool {
        const auto consider = [&](const MaterialPaintLayer &candidate,
                                  const PaintLayerAnchorPointTarget target) {
          if (!BKE_paint_layers_is_anchor_point(candidate) ||
              !paint_layer_anchor_point_source_valid(ma, reader, candidate.marker))
          {
            return;
          }
          RNAAnchorPointTargetEntry entry;
          entry.marker = candidate.marker;
          entry.label = fmt::format(
              "{} ({}, {})",
              candidate.name[0] != '\0' ? candidate.name : "Anchor Point",
              row.name[0] != '\0' ? row.name : "Layer",
              target == PaintLayerAnchorPointTarget::Mask ? "Mask" : "Content");
          r_entries.append(std::move(entry));
        };
        for (const MaterialPaintLayer &effect : row.effects) {
          consider(effect, PaintLayerAnchorPointTarget::Content);
        }
        for (const MaterialPaintLayer &item : row.mask_stack) {
          consider(item, PaintLayerAnchorPointTarget::Mask);
        }
        return true;
      });
}

/**
 * A string that outlives the enum items built from it. The items are copied shallowly and the popup
 * reads them after the callback returned, so a local string would dangle. The set is node based, so
 * a stored string never moves; the number of distinct labels stays small (point and layer names).
 */
static const char *rna_anchor_point_string_intern(const std::string &text)
{
  static std::unordered_set<std::string> pool;
  static std::mutex mutex;
  std::lock_guard<std::mutex> lock(mutex);
  return pool.insert(text).first->c_str();
}

static const EnumPropertyItem *rna_MaterialPaintLayer_anchor_point_target_itemf(
    bContext * /*C*/, PointerRNA *ptr, PropertyRNA * /*prop*/, bool *r_free)
{
  *r_free = true;
  EnumPropertyItem *items = nullptr;
  int items_num = 0;
  EnumPropertyItem none = {};
  none.value = 0;
  none.identifier = "NONE";
  none.name = N_("None");
  none.description = N_("This reader lays nothing");
  RNA_enum_item_add(&items, &items_num, &none);

  MaterialPaintLayer *reader = static_cast<MaterialPaintLayer *>(ptr->data);
  Material *ma = rna_paint_layer_material(ptr, reader);
  if (ma != nullptr && BKE_paint_layers_reads_anchor_point(*reader)) {
    Vector<RNAAnchorPointTargetEntry> entries;
    rna_anchor_point_targets_get(*ma, *reader, entries);
    for (const int i : entries.index_range()) {
      EnumPropertyItem item = {};
      item.value = i + 1;
      item.identifier = rna_anchor_point_string_intern(fmt::format("ANCHOR_{}", i + 1));
      item.name = rna_anchor_point_string_intern(entries[i].label);
      item.description = N_("Lay the slice this Anchor Point publishes");
      RNA_enum_item_add(&items, &items_num, &item);
    }
  }
  RNA_enum_item_end(&items, &items_num);
  return items;
}

static int rna_MaterialPaintLayer_anchor_point_target_get(PointerRNA *ptr)
{
  MaterialPaintLayer *reader = static_cast<MaterialPaintLayer *>(ptr->data);
  Material *ma = rna_paint_layer_material(ptr, reader);
  if (ma == nullptr || !BKE_paint_layers_reads_anchor_point(*reader)) {
    return 0;
  }
  const bUUID source = BKE_paint_layers_anchor_point_source_get(*reader);
  if (BLI_uuid_is_nil(source)) {
    return 0;
  }
  Vector<RNAAnchorPointTargetEntry> entries;
  rna_anchor_point_targets_get(*ma, *reader, entries);
  for (const int i : entries.index_range()) {
    if (BLI_uuid_equal(entries[i].marker, source)) {
      return i + 1;
    }
  }
  return 0;
}

static void rna_MaterialPaintLayer_anchor_point_target_set(PointerRNA *ptr, int value)
{
  MaterialPaintLayer *reader = static_cast<MaterialPaintLayer *>(ptr->data);
  Material *ma = rna_paint_layer_material(ptr, reader);
  if (ma == nullptr) {
    return;
  }
  if (value <= 0) {
    BKE_paint_layers_anchor_point_source_set(*ma, reader, BLI_uuid_nil());
    return;
  }
  Vector<RNAAnchorPointTargetEntry> entries;
  rna_anchor_point_targets_get(*ma, *reader, entries);
  if (value > entries.size()) {
    return;
  }
  BKE_paint_layers_anchor_point_source_set(*ma, reader, entries[value - 1].marker);
}

static int rna_MaterialPaintLayer_anchor_point_reader_count_get(PointerRNA *ptr)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  Material *ma = rna_paint_layer_material(ptr, layer);
  if (ma == nullptr || !BKE_paint_layers_is_anchor_point(*layer)) {
    return 0;
  }
  return int(paint_layer_anchor_point_readers(*ma, layer->marker).size());
}

static bool rna_MaterialPaintLayer_is_anchor_point_reader_get(PointerRNA *ptr)
{
  return BKE_paint_layers_reads_anchor_point(*static_cast<const MaterialPaintLayer *>(ptr->data));
}

/** How many channels the row takes part in; the generator's node-cost measure for the row. */
static int rna_MaterialPaintLayer_channel_weight_get(PointerRNA *ptr)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  const Material *ma = rna_paint_layer_material(ptr, layer);
  if (ma == nullptr) {
    return 0;
  }
  return BKE_paint_layers_layer_channel_weight(*ma, *layer);
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

/**
 * The object's evaluated copy carries the records the shader reads, so SHADING alone would not
 * refresh it. Once the row's Attribute nodes exist, hiding or showing an object only changes the
 * value they read: the material and its tree are touched only when \a topology_changed (the row's
 * mask flag flipped).
 */
static void rna_MaterialPaintLayer_object_mask_tag(Material &ma,
                                                   Object &ob,
                                                   const bool topology_changed)
{
  DEG_id_tag_update(&ob.id, ID_RECALC_SYNC_TO_EVAL | ID_RECALC_SHADING);
  WM_main_add_notifier(NC_OBJECT | ND_DRAW, &ob);
  /* The mask panel's checkboxes. */
  WM_main_add_notifier(NC_SPACE | ND_SPACE_PROPERTIES, nullptr);
  if (topology_changed) {
    BKE_paint_layers_tag_edited(ma);
    DEG_id_tag_update(&ma.id, ID_RECALC_SHADING);
    WM_main_add_notifier(NC_MATERIAL | ND_SHADING_DRAW, &ma);
  }
}

/* Read-only and never generates the owner uid: a getter must not change data, so a material that
 * has no uid yet reports a key of zeros until the first `*_set`. */
static void rna_MaterialPaintLayer_object_mask_key_get(PointerRNA *ptr, char *value)
{
  const Material *ma = id_cast<const Material *>(ptr->owner_id);
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  const auto key = BKE_paint_layer_object_mask_key(ma->paint_layers_owner_uid, layer->marker);
  memcpy(value, key.data(), key.size());
}

static int rna_MaterialPaintLayer_object_mask_key_length(PointerRNA * /*ptr*/)
{
  return bke::PAINT_LAYER_OBJECT_MASK_KEY_LEN;
}

static bool rna_MaterialPaintLayer_object_mask_visible_get(PointerRNA ptr, Object *object)
{
  MaterialPaintLayer *layer = nullptr;
  if (const Material *ma = rna_MaterialPaintLayer_owner(ptr, &layer)) {
    return BKE_paint_layer_object_mask_object_visible_get(*object, *ma, *layer);
  }
  return true;
}

static int rna_MaterialPaintLayer_object_mask_type_get(PointerRNA *ptr)
{
  return static_cast<const MaterialPaintLayer *>(ptr->data)->object_mask_type;
}

/**
 * The type decides nodes of the generated graph, so it may only be written on a stack row. A
 * correction or a mask item has no object mask: the kernel refuses it and clears any value that
 * reached DNA by another path, and the RNA setter reports the refusal instead of writing it.
 */
static void rna_MaterialPaintLayer_object_mask_type_set(PointerRNA *ptr, int value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  Material *ma = rna_MaterialPaintLayer_owner(*ptr, &layer);
  if (ma == nullptr || layer == nullptr) {
    return;
  }
  if (BKE_paint_layers_object_mask_type_set(*ma, *layer, int8_t(value))) {
    DEG_id_tag_update(&ma->id, ID_RECALC_SHADING);
    WM_main_add_notifier(NC_MATERIAL | ND_SHADING_DRAW, &ma->id);
  }
}

/** Whether \a object and \a ma can take an object-mask edit; reports the reason when not. */
static bool rna_MaterialPaintLayer_object_mask_editable(ReportList *reports,
                                                        const Object &object,
                                                        const Material &ma)
{
  if (!ID_IS_EDITABLE(&object.id) || ID_IS_OVERRIDE_LIBRARY(&object.id)) {
    BKE_report(reports, RPT_ERROR, "Object is not editable");
    return false;
  }
  if (!ID_IS_EDITABLE(&ma.id) || ID_IS_OVERRIDE_LIBRARY(&ma.id)) {
    BKE_report(reports, RPT_ERROR, "Material is not editable");
    return false;
  }
  return true;
}

static void rna_MaterialPaintLayer_object_mask_visible_set(PointerRNA ptr,
                                                           Main *bmain,
                                                           ReportList *reports,
                                                           Object *object,
                                                           const bool visible)
{
  MaterialPaintLayer *layer = nullptr;
  Material *ma = rna_MaterialPaintLayer_owner(ptr, &layer);
  if (ma == nullptr || layer == nullptr || object == nullptr ||
      !rna_MaterialPaintLayer_object_mask_editable(reports, *object, *ma))
  {
    return;
  }
  const int8_t old_mask_flag = layer->object_mask_flag;
  if (BKE_paint_layer_object_mask_visible_set(*bmain, *object, *ma, *layer, visible)) {
    rna_MaterialPaintLayer_object_mask_tag(*ma, *object, layer->object_mask_flag != old_mask_flag);
  }
}

static bool rna_MaterialPaintLayer_object_mask_tile_get(PointerRNA ptr, Object *object, int tile)
{
  MaterialPaintLayer *layer = nullptr;
  if (const Material *ma = rna_MaterialPaintLayer_owner(ptr, &layer)) {
    return BKE_paint_layer_object_mask_tile_visible_get(*object, *ma, *layer, tile);
  }
  return true;
}

static void rna_MaterialPaintLayer_object_mask_tile_set(PointerRNA ptr,
                                                        Main *bmain,
                                                        ReportList *reports,
                                                        Object *object,
                                                        const int tile,
                                                        const bool visible)
{
  MaterialPaintLayer *layer = nullptr;
  Material *ma = rna_MaterialPaintLayer_owner(ptr, &layer);
  if (ma == nullptr || layer == nullptr || object == nullptr) {
    return;
  }
  if (tile < bke::PAINT_LAYER_OBJECT_MASK_TILE_FIRST ||
      tile >= bke::PAINT_LAYER_OBJECT_MASK_TILE_FIRST + bke::PAINT_LAYER_OBJECT_MASK_TILE_NUM)
  {
    BKE_reportf(reports,
                RPT_ERROR,
                "Tile must be in %d..%d",
                bke::PAINT_LAYER_OBJECT_MASK_TILE_FIRST,
                bke::PAINT_LAYER_OBJECT_MASK_TILE_LAST);
    return;
  }
  if (!rna_MaterialPaintLayer_object_mask_editable(reports, *object, *ma)) {
    return;
  }
  const int8_t old_mask_flag = layer->object_mask_flag;
  if (BKE_paint_layer_object_mask_tile_set(*bmain, *object, *ma, *layer, tile, visible)) {
    rna_MaterialPaintLayer_object_mask_tag(*ma, *object, layer->object_mask_flag != old_mask_flag);
  }
}

static int rna_MaterialPaintLayer_bake_mode_get(PointerRNA *ptr)
{
  return BKE_paint_layers_bake_mode_get(*static_cast<const MaterialPaintLayer *>(ptr->data));
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
  return BKE_paint_layers_bake_size_get(*static_cast<const MaterialPaintLayer *>(ptr->data));
}

static void rna_MaterialPaintLayer_bake_size_set(PointerRNA *ptr, int value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  Material *ma = rna_MaterialPaintLayer_owner(*ptr, &layer);
  if (ma != nullptr && BKE_paint_layers_bake_size_set(*ma, *layer, value)) {
    WM_main_add_notifier(NC_MATERIAL | ND_SHADING, &ma->id);
  }
}

/* The menu form of `bake_size`: a preset size, or zero for "follow the content". A size set from
 * Python that is not a preset reads as the nearest one; the items are
 * #rna_enum_material_paint_layer_bake_size_preset_items. */
static int rna_MaterialPaintLayer_bake_size_preset_get(PointerRNA *ptr)
{
  int size = rna_MaterialPaintLayer_bake_size_get(ptr);
  if (size <= 0) {
    /* A Material row is rendered at a size of its own from the first render on: it reads the size
     * the material's layers are created with until it is given one. */
    MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
    const Material *ma = rna_MaterialPaintLayer_owner(*ptr, &layer);
    if (ma == nullptr || layer->source != MA_PAINT_LAYER_SOURCE_MATERIAL) {
      return 0;
    }
    size = BKE_paint_layers_material_row_size(*ma, *layer);
  }
  int best = PAINT_NEW_CHANNEL_IMAGE_SIZE_256;
  for (const int preset : {PAINT_NEW_CHANNEL_IMAGE_SIZE_256,
                           PAINT_NEW_CHANNEL_IMAGE_SIZE_512,
                           PAINT_NEW_CHANNEL_IMAGE_SIZE_1K,
                           PAINT_NEW_CHANNEL_IMAGE_SIZE_2K,
                           PAINT_NEW_CHANNEL_IMAGE_SIZE_4K})
  {
    if (std::abs(preset - size) < std::abs(best - size)) {
      best = preset;
    }
  }
  return best;
}

static void rna_MaterialPaintLayer_bake_size_preset_set(PointerRNA *ptr, int value)
{
  rna_MaterialPaintLayer_bake_size_set(ptr, value);
}

static bool rna_MaterialPaintLayer_bake_is_valid_get(PointerRNA *ptr)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  const Material *ma = rna_MaterialPaintLayer_owner(*ptr, &layer);
  return ma != nullptr && BKE_paint_layers_bake_is_valid(*ma, *layer);
}

static void rna_MaterialPaintLayer_bake_request(PointerRNA ptr)
{
  MaterialPaintLayer *layer = nullptr;
  if (Material *ma = rna_MaterialPaintLayer_owner(ptr, &layer)) {
    BKE_paint_layers_bake_request(*layer);
    if (layer->source == MA_PAINT_LAYER_SOURCE_MATERIAL) {
      /* An explicit request also lifts the hold a failed render put on the row. */
      BKE_paint_layers_material_bake_retry(*ma, *layer);
    }
    ma->paint_layers_flag |= MA_PAINT_LAYERS_BAKE_STALE;
    WM_main_add_notifier(NC_MATERIAL | ND_SHADING, &ma->id);
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

static void rna_MaterialPaintLayer_object_mask_tiles(PointerRNA ptr,
                                                     Object *object,
                                                     const bool refresh,
                                                     int **r_tiles,
                                                     int *r_tiles_num)
{
  *r_tiles = nullptr;
  *r_tiles_num = 0;
  MaterialPaintLayer *layer = nullptr;
  if (const Material *ma = rna_MaterialPaintLayer_owner(ptr, &layer)) {
    const Vector<int> tiles = BKE_paint_layer_object_mask_object_tiles_cached(
        *object, *ma, refresh);
    if (refresh) {
      WM_main_add_notifier(NC_SPACE | ND_SPACE_PROPERTIES, nullptr);
    }
    if (!tiles.is_empty()) {
      *r_tiles_num = int(tiles.size());
      *r_tiles = MEM_new_array_uninitialized<int>(tiles.size(), __func__);
      memcpy(*r_tiles, tiles.data(), sizeof(int) * tiles.size());
    }
  }
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

/* -------------------------------------------------------------------- */
/** \name Kind parameters
 *
 * A kind's parameters are the float properties of the row's system properties, each carrying its
 * range, default and label as UI data. The collection lists them in schema order and the struct
 * reads and writes them through that data, so no kind is named here.
 * \{ */

/** Kind parameters are the row's own scalars; the binding-marker group a smart-asset import
 * stores beside them is not one, and a group has no value a parameter could draw. */
static bool rna_MaterialPaintLayerKindParam_group_skip(CollectionPropertyIterator * /*iter*/,
                                                       void *data)
{
  return static_cast<const IDProperty *>(data)->type == IDP_GROUP;
}

static void rna_MaterialPaintLayer_kind_params_begin(CollectionPropertyIterator *iter,
                                                     PointerRNA *ptr)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  const IDProperty *group = layer->system_properties;
  if (group == nullptr || group->type != IDP_GROUP) {
    rna_iterator_listbase_begin(iter, ptr, nullptr, nullptr);
    return;
  }
  rna_iterator_listbase_begin(
      iter, ptr, &layer->system_properties->data.group, rna_MaterialPaintLayerKindParam_group_skip);
}

static void rna_MaterialPaintLayerKindParam_name_get(PointerRNA *ptr, char *value)
{
  const IDProperty *prop = static_cast<const IDProperty *>(ptr->data);
  strcpy(value, prop->name);
}

static int rna_MaterialPaintLayerKindParam_name_length(PointerRNA *ptr)
{
  const IDProperty *prop = static_cast<const IDProperty *>(ptr->data);
  return int(strlen(prop->name));
}

/** The UI data of a float property, or null when it has none. */
static const IDPropertyUIDataFloat *rna_kind_param_ui_data(const IDProperty *prop)
{
  if (prop->type != IDP_FLOAT || prop->ui_data == nullptr) {
    return nullptr;
  }
  return reinterpret_cast<const IDPropertyUIDataFloat *>(prop->ui_data);
}

static void rna_MaterialPaintLayerKindParam_label_get(PointerRNA *ptr, char *value)
{
  const IDProperty *prop = static_cast<const IDProperty *>(ptr->data);
  const IDPropertyUIDataFloat *ui_data = rna_kind_param_ui_data(prop);
  const char *label = (ui_data != nullptr && ui_data->base.description != nullptr) ?
                          ui_data->base.description :
                          prop->name;
  strcpy(value, label);
}

static int rna_MaterialPaintLayerKindParam_label_length(PointerRNA *ptr)
{
  const IDProperty *prop = static_cast<const IDProperty *>(ptr->data);
  const IDPropertyUIDataFloat *ui_data = rna_kind_param_ui_data(prop);
  const char *label = (ui_data != nullptr && ui_data->base.description != nullptr) ?
                          ui_data->base.description :
                          prop->name;
  return int(strlen(label));
}

static float rna_MaterialPaintLayerKindParam_value_get(PointerRNA *ptr)
{
  const IDProperty *prop = static_cast<const IDProperty *>(ptr->data);
  return (prop->type == IDP_FLOAT) ? IDP_float_get(prop) : 0.0f;
}

static void rna_MaterialPaintLayerKindParam_value_set(PointerRNA *ptr, const float value)
{
  IDProperty *prop = static_cast<IDProperty *>(ptr->data);
  if (prop->type != IDP_FLOAT) {
    return;
  }
  const IDPropertyUIDataFloat *ui_data = rna_kind_param_ui_data(prop);
  const float low = (ui_data != nullptr) ? float(ui_data->min) : -FLT_MAX;
  const float high = (ui_data != nullptr) ? float(ui_data->max) : FLT_MAX;
  IDP_float_set(prop, std::clamp(value, low, high));
  /* The parameter is an input of the row's group: a value edit, never a different graph. */
  if (Material *ma = reinterpret_cast<Material *>(ptr->owner_id)) {
    BKE_paint_layers_tag_value_edited(*ma);
  }
}

static void rna_MaterialPaintLayerKindParam_value_range(PointerRNA *ptr,
                                                        float *min,
                                                        float *max,
                                                        float *softmin,
                                                        float *softmax)
{
  const IDProperty *prop = static_cast<const IDProperty *>(ptr->data);
  const IDPropertyUIDataFloat *ui_data = rna_kind_param_ui_data(prop);
  if (ui_data == nullptr) {
    return;
  }
  *min = *softmin = float(ui_data->min);
  *max = *softmax = float(ui_data->max);
}

/**
 * The path of a kind parameter: its row's path and the parameter's name. The row is the one whose
 * system properties hold the property, found by address, as the property carries no back pointer.
 */
static std::optional<std::string> rna_MaterialPaintLayerKindParam_path(const PointerRNA *ptr)
{
  const Material *ma = reinterpret_cast<const Material *>(ptr->owner_id);
  const IDProperty *prop = static_cast<const IDProperty *>(ptr->data);
  const MaterialPaintLayer *owner = nullptr;
  BKE_paint_layers_foreach(
      *ma, [&](const MaterialPaintLayer &layer, const MaterialPaintLayer * /*parent*/) -> bool {
        const auto holds = [&](const MaterialPaintLayer &row) {
          return row.system_properties != nullptr && row.system_properties->type == IDP_GROUP &&
                 BLI_findindex(&row.system_properties->data.group, prop) != -1;
        };
        if (holds(layer)) {
          owner = &layer;
        }
        for (const MaterialPaintLayer &effect : layer.effects) {
          if (holds(effect)) {
            owner = &effect;
          }
        }
        /* A filter in the mask (Levels, Color ID) keeps its parameters on the mask item. */
        for (const MaterialPaintLayer &item : layer.mask_stack) {
          if (holds(item)) {
            owner = &item;
          }
        }
        return owner == nullptr;
      });
  std::string row_path;
  if (owner == nullptr || !rna_paint_layer_path_find(ma->paint_layers, owner, "paint_layers", row_path))
  {
    return std::nullopt;
  }
  return fmt::format("{}.kind_params[\"{}\"]", row_path, prop->name);
}

/**
 * Whether a smart-asset import marked this parameter as one of the asset's exposed ones. The
 * collection item is created from its row, so the row is the closest ancestor of the pointer; a
 * pointer built any other way reports false, which only costs the panel one hidden parameter.
 */
static bool rna_MaterialPaintLayerKindParam_exposed_get(PointerRNA *ptr)
{
  const IDProperty *prop = static_cast<const IDProperty *>(ptr->data);
  if (prop == nullptr || ptr->ancestors.is_empty()) {
    return false;
  }
  const AncestorPointerRNA &parent = ptr->ancestors.last();
  if (parent.type != RNA_MaterialPaintLayer || parent.data == nullptr) {
    return false;
  }
  return BKE_paint_layers_item_param_is_exposed(
      *static_cast<const MaterialPaintLayer *>(parent.data), prop->name);
}

/**
 * Whether the smart asset this material is offers the parameter to the materials that import it:
 * the asset's exposed-parameters list names it. The import reads the same list, so this is the
 * one property the asset's own panel writes; on an instance (no list on the material) it reads
 * false and stays that way.
 */
static bool rna_MaterialPaintLayerKindParam_asset_exposed_get(PointerRNA *ptr)
{
  const IDProperty *prop = static_cast<const IDProperty *>(ptr->data);
  if (prop == nullptr || ptr->owner_id == nullptr || GS(ptr->owner_id->name) != ID_MA ||
      ptr->ancestors.is_empty())
  {
    return false;
  }
  const AncestorPointerRNA &parent = ptr->ancestors.last();
  if (parent.type != RNA_MaterialPaintLayer || parent.data == nullptr) {
    return false;
  }
  return BKE_paint_layers_asset_param_is_exposed(
      *id_cast<const Material *>(ptr->owner_id),
      *static_cast<const MaterialPaintLayer *>(parent.data),
      prop->name);
}

static void rna_MaterialPaintLayerKindParam_asset_exposed_set(PointerRNA *ptr, const bool value)
{
  IDProperty *prop = static_cast<IDProperty *>(ptr->data);
  if (prop == nullptr || ptr->owner_id == nullptr || GS(ptr->owner_id->name) != ID_MA ||
      ptr->ancestors.is_empty())
  {
    return;
  }
  const AncestorPointerRNA &parent = ptr->ancestors.last();
  if (parent.type != RNA_MaterialPaintLayer || parent.data == nullptr) {
    return;
  }
  BKE_paint_layers_asset_param_expose_set(*id_cast<Material *>(ptr->owner_id),
                                          *static_cast<MaterialPaintLayer *>(parent.data),
                                          prop->name,
                                          value);
}

/**
 * Whether the smart asset offered this parameter to the instance that imported it: the import laid
 * the mark on this copy, active or switched off. Read-only -- whether the asset offers anything is
 * the asset's decision (#asset_exposed), the instance only switches what it was given. False on the
 * asset's own rows, which carry no import marks.
 */
static bool rna_MaterialPaintLayerKindParam_asset_offered_get(PointerRNA *ptr)
{
  const IDProperty *prop = static_cast<const IDProperty *>(ptr->data);
  if (prop == nullptr || ptr->ancestors.is_empty()) {
    return false;
  }
  const AncestorPointerRNA &parent = ptr->ancestors.last();
  if (parent.type != RNA_MaterialPaintLayer || parent.data == nullptr) {
    return false;
  }
  return BKE_paint_layers_item_param_is_offered(
      *static_cast<const MaterialPaintLayer *>(parent.data), prop->name);
}

/** The read side of the instance switch: a parameter that is not offered reads as switched off. */
static bool rna_MaterialPaintLayerKindParam_asset_enabled_get(PointerRNA *ptr)
{
  const IDProperty *prop = static_cast<const IDProperty *>(ptr->data);
  if (prop == nullptr || ptr->ancestors.is_empty()) {
    return false;
  }
  const AncestorPointerRNA &parent = ptr->ancestors.last();
  if (parent.type != RNA_MaterialPaintLayer || parent.data == nullptr) {
    return false;
  }
  return BKE_paint_layers_item_param_is_exposed(
      *static_cast<const MaterialPaintLayer *>(parent.data), prop->name);
}

/**
 * The write side of the instance switch: on or off for this instance alone. A parameter the asset
 * never offered has no mark to flip, and the core keeps it that way -- no mark is conjured.
 */
static void rna_MaterialPaintLayerKindParam_asset_enabled_set(PointerRNA *ptr, const bool value)
{
  IDProperty *prop = static_cast<IDProperty *>(ptr->data);
  if (prop == nullptr || ptr->ancestors.is_empty()) {
    return;
  }
  const AncestorPointerRNA &parent = ptr->ancestors.last();
  if (parent.type != RNA_MaterialPaintLayer || parent.data == nullptr) {
    return;
  }
  BKE_paint_layers_item_param_enabled_set(
      *static_cast<MaterialPaintLayer *>(parent.data), prop->name, value);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Smart-asset instances
 * \{ */

/** The instance \a row belongs to, or -1 for none (a copy of a row is bound to nothing). */
static int rna_MaterialPaintLayer_smart_asset_instance_id_get(PointerRNA *ptr)
{
  if (ptr->owner_id == nullptr || GS(ptr->owner_id->name) != ID_MA) {
    return -1;
  }
  Material &ma = *id_cast<Material *>(ptr->owner_id);
  const MaterialPaintLayer *row = static_cast<const MaterialPaintLayer *>(ptr->data);
  const int row_instance = BKE_paint_layers_asset_instance_of_row(ma, *row);
  if (row_instance >= 0) {
    return row_instance;
  }
  /* A stack row answers as before; what is not a row of an instance's `rows` may still be a mask
   * item a Smart Mask drop laid down -- the records name those in `mask_items`, and a correction
   * or a folder child matches nothing there. */
  return BKE_paint_layers_asset_instance_of_mask_item(ma, row->marker);
}

static void rna_MaterialPaintLayer_smart_asset_instance_label_get(PointerRNA *ptr, char *value)
{
  value[0] = '\0';
  const int instance_id = rna_MaterialPaintLayer_smart_asset_instance_id_get(ptr);
  if (instance_id < 0 || ptr->owner_id == nullptr || GS(ptr->owner_id->name) != ID_MA) {
    return;
  }
  const std::string label = BKE_paint_layers_asset_instance_label(
      *id_cast<Material *>(ptr->owner_id), instance_id);
  strcpy(value, label.c_str());
}

static int rna_MaterialPaintLayer_smart_asset_instance_label_length(PointerRNA *ptr)
{
  char value[MAX_NAME + 16];
  rna_MaterialPaintLayer_smart_asset_instance_label_get(ptr, value);
  return int(strlen(value));
}

/** One row of the computed `Material.smart_asset_instances` collection, a snapshot of one drop of
 * a Smart asset into the material; the array is built on iteration and owned by the iterator. */
struct MaterialSmartAssetInstanceItem {
  int id;
  int number;
  int type;
  bool is_mask;
  char asset_name[MAX_NAME];
  char label[MAX_NAME + 16];
  /* The Smart Mask target row's marker, empty for any other instance. */
  char mask_target[MAX_NAME];
};

static void rna_Material_smart_asset_instances_begin(CollectionPropertyIterator *iter,
                                                     PointerRNA *ptr)
{
  Material *ma = (ptr->owner_id != nullptr && GS(ptr->owner_id->name) == ID_MA) ?
                     id_cast<Material *>(ptr->owner_id) :
                     nullptr;
  MaterialSmartAssetInstanceItem *array = nullptr;
  int64_t count = 0;
  if (ma != nullptr) {
    Vector<PaintLayerAssetInstanceInfo> instances;
    BKE_paint_layers_asset_instances_foreach(*ma, [&](const PaintLayerAssetInstanceInfo &info) {
      instances.append(info);
      return true;
    });
    count = instances.size();
    if (count > 0) {
      array = MEM_new_array_uninitialized<MaterialSmartAssetInstanceItem>(count, __func__);
      int64_t i = 0;
      for (const PaintLayerAssetInstanceInfo &info : instances) {
        MaterialSmartAssetInstanceItem &item = array[i++];
        item.id = info.id;
        item.number = info.number;
        item.type = int(info.type);
        item.is_mask = info.is_mask;
        STRNCPY(item.asset_name, info.name.c_str());
        STRNCPY(item.label, info.label().c_str());
        STRNCPY(item.mask_target, info.mask_target.c_str());
      }
    }
  }
  rna_iterator_array_begin(
      iter, ptr, array, sizeof(MaterialSmartAssetInstanceItem), count, array != nullptr, nullptr);
}

static const MaterialSmartAssetInstanceItem *rna_MaterialSmartAssetInstance(
    const PointerRNA *ptr)
{
  return static_cast<const MaterialSmartAssetInstanceItem *>(ptr->data);
}

static int rna_MaterialSmartAssetInstance_id_get(PointerRNA *ptr)
{
  return rna_MaterialSmartAssetInstance(ptr)->id;
}

static void rna_MaterialSmartAssetInstance_label_get(PointerRNA *ptr, char *value)
{
  /* `value` is a plain pointer here, so #STRNCPY (which needs an array) cannot deduce its size;
   * the item's own buffer is the size that holds the string. */
  const MaterialSmartAssetInstanceItem *item = rna_MaterialSmartAssetInstance(ptr);
  BLI_strncpy(value, item->label, sizeof(item->label));
}

static int rna_MaterialSmartAssetInstance_label_length(PointerRNA *ptr)
{
  return int(strlen(rna_MaterialSmartAssetInstance(ptr)->label));
}

static void rna_MaterialSmartAssetInstance_asset_name_get(PointerRNA *ptr, char *value)
{
  const MaterialSmartAssetInstanceItem *item = rna_MaterialSmartAssetInstance(ptr);
  BLI_strncpy(value, item->asset_name, sizeof(item->asset_name));
}

static int rna_MaterialSmartAssetInstance_asset_name_length(PointerRNA *ptr)
{
  return int(strlen(rna_MaterialSmartAssetInstance(ptr)->asset_name));
}

static int rna_MaterialSmartAssetInstance_type_get(PointerRNA *ptr)
{
  return rna_MaterialSmartAssetInstance(ptr)->type;
}

static bool rna_MaterialSmartAssetInstance_is_mask_get(PointerRNA *ptr)
{
  return rna_MaterialSmartAssetInstance(ptr)->is_mask;
}

static void rna_MaterialSmartAssetInstance_mask_target_get(PointerRNA *ptr, char *value)
{
  const MaterialSmartAssetInstanceItem *item = rna_MaterialSmartAssetInstance(ptr);
  BLI_strncpy(value, item->mask_target, sizeof(item->mask_target));
}

static int rna_MaterialSmartAssetInstance_mask_target_length(PointerRNA *ptr)
{
  return int(strlen(rna_MaterialSmartAssetInstance(ptr)->mask_target));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Color ID colours and map-coverage filters
 * \{ */

static void rna_MaterialPaintLayer_id_colors_begin(CollectionPropertyIterator *iter,
                                                   PointerRNA *ptr)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  rna_iterator_listbase_begin(iter, ptr, &layer->id_colors, nullptr);
}

static int rna_MaterialPaintLayer_filter_type_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return (layer->source == MA_PAINT_LAYER_SOURCE_FILTER) ? int(layer->kind_subtype) : 0;
}

static int rna_MaterialPaintLayer_output_map_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *item = static_cast<const MaterialPaintLayer *>(ptr->data);
  if (BKE_paint_layers_mask_item_is_gradient(*item)) {
    return 2;
  }
  return BKE_paint_layers_mask_item_is_inverted(*item) ? 1 : 0;
}

static void rna_MaterialPaintLayer_output_map_set(PointerRNA *ptr, const int value)
{
  MaterialPaintLayer *item = static_cast<MaterialPaintLayer *>(ptr->data);
  Material *ma = rna_paint_layer_material(ptr, item);
  if (ma == nullptr || !BKE_paint_layers_is_map_coverage(*item)) {
    return;
  }
  BKE_paint_layers_mask_item_set_gradient(*ma, item, value == 2);
  if (value != 2) {
    BKE_paint_layers_channel_set_invert(
        *ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, value == 1);
  }
}

static int rna_MaterialPaintLayer_color_id_mode_get(PointerRNA *ptr)
{
  return BKE_paint_layers_mask_item_is_color_id_advanced(
             *static_cast<const MaterialPaintLayer *>(ptr->data)) ?
             1 :
             0;
}

static void rna_MaterialPaintLayer_color_id_mode_set(PointerRNA *ptr, const int value)
{
  MaterialPaintLayer *item = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, item)) {
    BKE_paint_layers_mask_item_set_color_id_advanced(*ma, item, value == 1);
  }
}

static bool rna_MaterialPaintLayer_output_alpha_get(PointerRNA *ptr)
{
  return BKE_paint_layers_mask_item_is_alpha_background(
      *static_cast<const MaterialPaintLayer *>(ptr->data));
}

static void rna_MaterialPaintLayer_output_alpha_set(PointerRNA *ptr, const bool value)
{
  MaterialPaintLayer *item = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, item)) {
    BKE_paint_layers_mask_item_set_alpha_background(*ma, item, value);
  }
}

static bool rna_MaterialPaintLayer_is_map_coverage_get(PointerRNA *ptr)
{
  return BKE_paint_layers_is_map_coverage(*static_cast<const MaterialPaintLayer *>(ptr->data));
}

static bool rna_MaterialPaintLayer_map_paintable_get(PointerRNA *ptr)
{
  return BKE_paint_layers_map_coverage_paintable(
      *static_cast<const MaterialPaintLayer *>(ptr->data));
}

/** The Color ID item that holds the colour \a ptr stands for, or null for a stale pointer. */
static MaterialPaintLayer *rna_MaterialPaintLayerIdColor_item(PointerRNA *ptr, Material **r_ma)
{
  Material *ma = reinterpret_cast<Material *>(ptr->owner_id);
  *r_ma = ma;
  if (ma == nullptr) {
    return nullptr;
  }
  return BKE_paint_layers_id_color_owner(
      *ma, *static_cast<const MaterialPaintLayerIdColor *>(ptr->data));
}

static void rna_MaterialPaintLayerIdColor_color_get(PointerRNA *ptr, float *values)
{
  const MaterialPaintLayerIdColor *color = static_cast<const MaterialPaintLayerIdColor *>(ptr->data);
  copy_v3_v3(values, color->color);
}

static void rna_MaterialPaintLayerIdColor_color_set(PointerRNA *ptr, const float *values)
{
  Material *ma = nullptr;
  MaterialPaintLayer *item = rna_MaterialPaintLayerIdColor_item(ptr, &ma);
  if (item != nullptr) {
    BKE_paint_layers_id_color_set_color(
        *ma, item, static_cast<MaterialPaintLayerIdColor *>(ptr->data), values);
  }
}

static bool rna_MaterialPaintLayerIdColor_enabled_get(PointerRNA *ptr)
{
  const MaterialPaintLayerIdColor *color = static_cast<const MaterialPaintLayerIdColor *>(ptr->data);
  return (color->flag & MA_PAINT_LAYER_ID_COLOR_ENABLED) != 0;
}

static void rna_MaterialPaintLayerIdColor_enabled_set(PointerRNA *ptr, const bool value)
{
  Material *ma = nullptr;
  MaterialPaintLayer *item = rna_MaterialPaintLayerIdColor_item(ptr, &ma);
  if (item != nullptr) {
    BKE_paint_layers_id_color_set_enabled(
        *ma, item, static_cast<MaterialPaintLayerIdColor *>(ptr->data), value);
  }
}

static float rna_MaterialPaintLayerIdColor_tolerance_get(PointerRNA *ptr)
{
  return static_cast<const MaterialPaintLayerIdColor *>(ptr->data)->tolerance;
}

static void rna_MaterialPaintLayerIdColor_tolerance_set(PointerRNA *ptr, const float value)
{
  Material *ma = nullptr;
  MaterialPaintLayer *item = rna_MaterialPaintLayerIdColor_item(ptr, &ma);
  if (item != nullptr) {
    BKE_paint_layers_id_color_set_tolerance(
        *ma, item, static_cast<MaterialPaintLayerIdColor *>(ptr->data), value);
  }
}

static bool rna_MaterialPaintLayerIdColor_show_advanced_get(PointerRNA *ptr)
{
  const MaterialPaintLayerIdColor *color = static_cast<const MaterialPaintLayerIdColor *>(ptr->data);
  return (color->flag & MA_PAINT_LAYER_ID_COLOR_EXPANDED) != 0;
}

static void rna_MaterialPaintLayerIdColor_show_advanced_set(PointerRNA *ptr, const bool value)
{
  MaterialPaintLayerIdColor *color = static_cast<MaterialPaintLayerIdColor *>(ptr->data);
  SET_FLAG_FROM_TEST(color->flag, value, MA_PAINT_LAYER_ID_COLOR_EXPANDED);
}

static std::optional<std::string> rna_MaterialPaintLayerIdColor_path(const PointerRNA *ptr)
{
  Material *ma = reinterpret_cast<Material *>(ptr->owner_id);
  const MaterialPaintLayerIdColor *color = static_cast<const MaterialPaintLayerIdColor *>(ptr->data);
  if (ma == nullptr) {
    return std::nullopt;
  }
  const MaterialPaintLayer *item = BKE_paint_layers_id_color_owner(*ma, *color);
  std::string item_path;
  if (item == nullptr ||
      !rna_paint_layer_path_find(ma->paint_layers, item, "paint_layers", item_path))
  {
    return std::nullopt;
  }
  return fmt::format("{}.id_colors[{}]", item_path, BLI_findindex(&item->id_colors, color));
}

static MaterialPaintLayer *rna_MaterialPaintLayer_mask_filter_add(PointerRNA ptr,
                                                                  ReportList *reports,
                                                                  const int filter_type)
{
  MaterialPaintLayer *layer = nullptr;
  Material *ma = rna_MaterialPaintLayer_owner(ptr, &layer);
  if (ma == nullptr) {
    BKE_report(reports, RPT_ERROR, "Paint layer is not part of this material");
    return nullptr;
  }
  MaterialPaintLayer *item = BKE_paint_layers_mask_filter_add(
      *ma, layer, eMaterialPaintLayerFilter(filter_type));
  if (item == nullptr) {
    BKE_report(reports,
               RPT_ERROR,
               "Cannot add a mask filter: the layer needs a mask first, and the filter must be known");
    return nullptr;
  }
  /* The picker and the capture act on the active mask item, so the one just added is the one the
   * user goes on to set up. */
  BKE_paint_layers_active_set(*ma, item->marker);
  WM_main_add_notifier(NC_MATERIAL | ND_SHADING, &ma->id);
  return item;
}

static MaterialPaintLayerIdColor *rna_MaterialPaintLayer_id_color_add(PointerRNA ptr,
                                                                      ReportList *reports,
                                                                      const float color[3])
{
  MaterialPaintLayer *item = nullptr;
  Material *ma = rna_MaterialPaintLayer_owner(ptr, &item);
  MaterialPaintLayerIdColor *record = (ma != nullptr) ?
                                          BKE_paint_layers_id_color_add(*ma, item, color) :
                                          nullptr;
  if (record == nullptr) {
    BKE_report(reports, RPT_ERROR, "Only a Color ID mask item holds colours");
    return nullptr;
  }
  WM_main_add_notifier(NC_MATERIAL | ND_SHADING, &ma->id);
  return record;
}

static void rna_MaterialPaintLayer_id_color_remove(PointerRNA ptr,
                                                   ReportList *reports,
                                                   const int index)
{
  MaterialPaintLayer *item = nullptr;
  Material *ma = rna_MaterialPaintLayer_owner(ptr, &item);
  MaterialPaintLayerIdColor *color = (item != nullptr) ?
                                         static_cast<MaterialPaintLayerIdColor *>(
                                             BLI_findlink(&item->id_colors, index)) :
                                         nullptr;
  if (color == nullptr || !BKE_paint_layers_id_color_remove(*ma, item, color)) {
    BKE_report(reports, RPT_ERROR, "No such colour on this mask item");
    return;
  }
  WM_main_add_notifier(NC_MATERIAL | ND_SHADING, &ma->id);
}

/** \} */

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

static void rna_MaterialPaintLayerChannel_image_update(Main *bmain,
                                                       Scene *scene,
                                                       PointerRNA *ptr)
{
  /* A Color ID item compares the map's texels against stored colours, so the map is read as data:
   * the colour space is part of "the ID Map" being right. Dropping an image, opening one and
   * picking one all come through the image property. This is the update callback, as only it gets
   * the #Main the colour space signal needs. */
  MaterialPaintLayer *layer = nullptr;
  if (rna_paint_layer_sub_owner(ptr, &layer) != nullptr && layer != nullptr &&
      BKE_paint_layers_is_map_coverage(*layer) && !BKE_paint_layers_map_coverage_paintable(*layer))
  {
    const MaterialPaintLayerChannel *record = static_cast<const MaterialPaintLayerChannel *>(
        ptr->data);
    if (record->image != nullptr) {
      BKE_paint_layers_map_coverage_image_use_data(*bmain, *record->image);
    }
  }
  rna_Material_update(bmain, scene, ptr);
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
  /* The record stores the map's domain; the UI edits the authored relief (identity for every
   * channel but Height). */
  return BKE_paint_material_channel_value_decode(
      eMaterialPaintChannel(rna_MaterialPaintLayerChannel_channel_get(ptr)), record->value[0]);
}

/** The channel's own authored range (0..1 for the factor channels, -1..1 relief for Height), the
 * same one the brush channel's value uses. */
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
    rgba[0] = BKE_paint_material_channel_value_encode(
        channel, std::clamp(value, info.value_min, info.value_max));
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

static int rna_MaterialPaintLayerChannel_normal_space_get(PointerRNA *ptr)
{
  const MaterialPaintLayerChannel *record = static_cast<const MaterialPaintLayerChannel *>(
      ptr->data);
  return (record->flag & MA_PAINT_LAYER_CHANNEL_NORMAL_DIRECTX) != 0 ? 1 : 0;
}

static void rna_MaterialPaintLayerChannel_normal_space_set(PointerRNA *ptr, int value)
{
  MaterialPaintLayer *layer = nullptr;
  if (Material *ma = rna_paint_layer_sub_owner(ptr, &layer)) {
    BKE_paint_layers_normal_set_directx(*ma, layer, value == 1);
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

static int rna_MaterialPaintLayerChannelSettings_blend_get(PointerRNA *ptr)
{
  const MaterialPaintLayerChannelSettings *settings =
      static_cast<const MaterialPaintLayerChannelSettings *>(ptr->data);
  return settings->blend;
}

static void rna_MaterialPaintLayerChannelSettings_blend_set(PointerRNA *ptr, int value)
{
  Material *ma = reinterpret_cast<Material *>(ptr->owner_id);
  if (ma == nullptr) {
    return;
  }
  const MaterialPaintLayerChannelSettings *settings =
      static_cast<const MaterialPaintLayerChannelSettings *>(ptr->data);
  if (MaterialPaintLayer *row = rna_paint_layer_by_settings(*ma, settings)) {
    BKE_paint_layers_channel_blend_set(*ma, *row, int(settings - row->channel_settings), value);
  }
}

/**
 * The Normal channel offers two blends of its own: Mix (the Normal Combine group, stored as the
 * inherit value) and Replace. Every other channel offers the full per-channel list.
 */
static const EnumPropertyItem *rna_MaterialPaintLayerChannelSettings_blend_itemf(
    bContext * /*C*/, PointerRNA *ptr, PropertyRNA * /*prop*/, bool * /*r_free*/)
{
  const MaterialPaintLayerChannelSettings *settings =
      static_cast<const MaterialPaintLayerChannelSettings *>(ptr->data);
  Material *ma = reinterpret_cast<Material *>(ptr->owner_id);
  const MaterialPaintLayer *row = (ma != nullptr) ?
                                      rna_paint_layer_by_settings(*ma, settings) :
                                      nullptr;
  if (row == nullptr || int(settings - row->channel_settings) != PAINT_MATERIAL_CHANNEL_NORMAL) {
    return rna_enum_material_paint_layer_channel_blend_items;
  }
  static const EnumPropertyItem normal_blend_items[] = {
      /* Same identifier as the other channels' inherit item, so a script reads one name for "no
       * override" on every channel; only the label differs, as the combine is the Normal Mix. */
      {-1, "INHERIT", 0, "Mix", "Combine the normals (Whiteout)"},
      {MA_PAINT_LAYER_BLEND_NORMAL_REPLACE,
       "REPLACE",
       0,
       "Replace",
       "The upper normal covers the lower one"},
      {0, nullptr, 0, nullptr, nullptr},
  };
  return normal_blend_items;
}

static int rna_MaterialPaintLayerChannelSettings_height_blend_get(PointerRNA *ptr)
{
  const MaterialPaintLayerChannelSettings *settings =
      static_cast<const MaterialPaintLayerChannelSettings *>(ptr->data);
  return settings->height_blend;
}

static void rna_MaterialPaintLayerChannelSettings_height_blend_set(PointerRNA *ptr, int value)
{
  Material *ma = id_cast<Material *>(ptr->owner_id);
  if (ma == nullptr) {
    return;
  }
  const MaterialPaintLayerChannelSettings *settings =
      static_cast<const MaterialPaintLayerChannelSettings *>(ptr->data);
  if (MaterialPaintLayer *row = rna_paint_layer_by_settings(*ma, settings)) {
    BKE_paint_layers_channel_height_blend_set(
        *ma, *row, int(settings - row->channel_settings), value);
  }
}

static float rna_MaterialPaintLayerChannelSettings_height_scale_get(PointerRNA *ptr)
{
  const MaterialPaintLayerChannelSettings *settings =
      static_cast<const MaterialPaintLayerChannelSettings *>(ptr->data);
  return 1.0f + settings->height_scale_delta;
}

static void rna_MaterialPaintLayerChannelSettings_height_scale_set(PointerRNA *ptr, float value)
{
  Material *ma = id_cast<Material *>(ptr->owner_id);
  if (ma == nullptr) {
    return;
  }
  const MaterialPaintLayerChannelSettings *settings =
      static_cast<const MaterialPaintLayerChannelSettings *>(ptr->data);
  if (MaterialPaintLayer *row = rna_paint_layer_by_settings(*ma, settings)) {
    BKE_paint_layers_channel_height_scale_set(
        *ma, *row, int(settings - row->channel_settings), value);
  }
}

static bool rna_MaterialPaintLayerChannelSettings_height_write_get(PointerRNA *ptr)
{
  const MaterialPaintLayerChannelSettings *settings =
      static_cast<const MaterialPaintLayerChannelSettings *>(ptr->data);
  return (settings->height_flag & MA_PAINT_LAYER_CHANNEL_HEIGHT_WRITE_SKIP) == 0;
}

static void rna_MaterialPaintLayerChannelSettings_height_write_set(PointerRNA *ptr, bool value)
{
  Material *ma = id_cast<Material *>(ptr->owner_id);
  if (ma == nullptr) {
    return;
  }
  const MaterialPaintLayerChannelSettings *settings =
      static_cast<const MaterialPaintLayerChannelSettings *>(ptr->data);
  if (MaterialPaintLayer *row = rna_paint_layer_by_settings(*ma, settings)) {
    BKE_paint_layers_channel_height_write_set(*ma, *row, value);
  }
}

/** Path of one (row, channel) entry below its material, so a property of it can be addressed. */
static std::optional<std::string> rna_MaterialPaintLayerChannelSettings_path(const PointerRNA *ptr)
{
  const Material *ma = reinterpret_cast<const Material *>(ptr->owner_id);
  const MaterialPaintLayerChannelSettings *settings =
      static_cast<const MaterialPaintLayerChannelSettings *>(ptr->data);
  if (ma == nullptr) {
    return std::nullopt;
  }
  MaterialPaintLayer *row = rna_paint_layer_by_settings(const_cast<Material &>(*ma), settings);
  if (row == nullptr) {
    return std::nullopt;
  }
  std::string row_path;
  if (!rna_paint_layer_path_find(ma->paint_layers, row, "paint_layers", row_path)) {
    return std::nullopt;
  }
  return fmt::format(
      "{}.channel_settings[{}]", row_path, int(settings - row->channel_settings));
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
  const float t = BKE_paint_material_t_from_value(
      info.value_min,
      info.value_max,
      BKE_paint_material_channel_value_decode(channel, record->value[0]));
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
    rgba[0] = BKE_paint_material_channel_value_encode(
        channel, BKE_paint_material_value_from_t(info.value_min, info.value_max, t));
    BKE_paint_layers_channel_set_value(*ma, layer, channel, rgba);
  }
}

/* -------------------------------------------------------------------- */
/** \name Gradient generator
 * \{ */

static bool rna_MaterialPaintLayer_gradient_supported_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return layer != nullptr && BKE_paint_layers_gradient_supported(*layer);
}

static int rna_MaterialPaintLayer_gradient_type_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return (layer != nullptr && layer->gradient != nullptr) ? layer->gradient->type : 0;
}

static void rna_MaterialPaintLayer_gradient_type_set(PointerRNA *ptr, int value)
{
  Material *ma = reinterpret_cast<Material *>(ptr->owner_id);
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (ma != nullptr && layer != nullptr) {
    BKE_paint_layers_gradient_set_type(
        *ma, *layer, eMaterialPaintLayerGradientType(value));
  }
}

static int rna_MaterialPaintLayer_gradient_distribution_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return (layer != nullptr && layer->gradient != nullptr) ? layer->gradient->distribution :
                                                            MA_PAINT_LAYER_GRADIENT_DISTRIBUTION_MIDPOINT;
}

static void rna_MaterialPaintLayer_gradient_distribution_set(PointerRNA *ptr, int value)
{
  Material *ma = reinterpret_cast<Material *>(ptr->owner_id);
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (ma != nullptr && layer != nullptr) {
    BKE_paint_layers_gradient_set_distribution(
        *ma, *layer, eMaterialPaintLayerGradientDistribution(value));
  }
}

static int rna_MaterialPaintLayer_gradient_gizmo_channel_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return (layer != nullptr && layer->gradient != nullptr) ?
             layer->gradient->gizmo_channel :
             PAINT_MATERIAL_CHANNEL_BASE_COLOR;
}

static void rna_MaterialPaintLayer_gradient_gizmo_channel_set(PointerRNA *ptr, int value)
{
  Material *ma = id_cast<Material *>(ptr->owner_id);
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (ma != nullptr && layer != nullptr) {
    BKE_paint_layers_gradient_set_gizmo_channel(
        *ma, *layer, eMaterialPaintChannel(value));
  }
}

static int rna_MaterialPaintLayer_gradient_gizmo_display_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return (layer != nullptr && layer->gradient != nullptr) ?
             layer->gradient->gizmo_display :
             MA_PAINT_LAYER_GRADIENT_GIZMO_PAINT_ONLY;
}

static void rna_MaterialPaintLayer_gradient_gizmo_display_set(PointerRNA *ptr, int value)
{
  Material *ma = reinterpret_cast<Material *>(ptr->owner_id);
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (ma != nullptr && layer != nullptr) {
    BKE_paint_layers_gradient_set_gizmo_display(
        *ma, *layer, eMaterialPaintLayerGradientGizmoDisplay(value));
  }
}

static int rna_MaterialPaintLayer_gradient_repeat_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return (layer != nullptr && layer->gradient != nullptr) ? layer->gradient->repeat : 0;
}

static void rna_MaterialPaintLayer_gradient_repeat_set(PointerRNA *ptr, int value)
{
  Material *ma = reinterpret_cast<Material *>(ptr->owner_id);
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (ma != nullptr && layer != nullptr) {
    BKE_paint_layers_gradient_set_repeat(
        *ma, *layer, eMaterialPaintLayerGradientRepeat(value));
  }
}

static float rna_MaterialPaintLayer_gradient_midpoint_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return (layer != nullptr && layer->gradient != nullptr) ? layer->gradient->midpoint : 0.5f;
}

static void rna_MaterialPaintLayer_gradient_midpoint_set(PointerRNA *ptr, float value)
{
  Material *ma = reinterpret_cast<Material *>(ptr->owner_id);
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (ma != nullptr && layer != nullptr) {
    BKE_paint_layers_gradient_set_midpoint(*ma, *layer, value);
  }
}

static bool rna_MaterialPaintLayer_gradient_gizmo_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return layer != nullptr && layer->gradient != nullptr && layer->gradient->gizmo_shown != 0;
}

static void rna_MaterialPaintLayer_gradient_gizmo_set(PointerRNA *ptr, bool value)
{
  Material *ma = reinterpret_cast<Material *>(ptr->owner_id);
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (ma != nullptr && layer != nullptr) {
    BKE_paint_layers_gradient_set_gizmo_shown(*ma, *layer, value);
  }
}

static int rna_MaterialPaintLayer_gradient_space_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return (layer != nullptr && layer->gradient != nullptr) ?
             layer->gradient->space :
             MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT;
}

static void rna_MaterialPaintLayer_gradient_space_set(PointerRNA *ptr, int value)
{
  Material *ma = reinterpret_cast<Material *>(ptr->owner_id);
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (ma != nullptr && layer != nullptr) {
    BKE_paint_layers_gradient_set_space(
        *ma, *layer, eMaterialPaintLayerGradientSpace(value));
  }
}

static void rna_MaterialPaintLayer_gradient_start_get(PointerRNA *ptr, float *values)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  if (layer != nullptr && layer->gradient != nullptr) {
    copy_v3_v3(values, layer->gradient->start);
  }
  else {
    values[0] = 0.0f;
    values[1] = 0.5f;
    values[2] = 0.0f;
  }
}

static void rna_MaterialPaintLayer_gradient_end_get(PointerRNA *ptr, float *values)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  if (layer != nullptr && layer->gradient != nullptr) {
    copy_v3_v3(values, layer->gradient->end);
  }
  else {
    values[0] = 1.0f;
    values[1] = 0.5f;
    values[2] = 0.0f;
  }
}

static void rna_MaterialPaintLayer_gradient_start_set(PointerRNA *ptr, const float *values)
{
  Material *ma = reinterpret_cast<Material *>(ptr->owner_id);
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (ma != nullptr && layer != nullptr) {
    BKE_paint_layers_gradient_set_start(*ma, *layer, values);
  }
}

static void rna_MaterialPaintLayer_gradient_end_set(PointerRNA *ptr, const float *values)
{
  Material *ma = reinterpret_cast<Material *>(ptr->owner_id);
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (ma != nullptr && layer != nullptr) {
    BKE_paint_layers_gradient_set_end(*ma, *layer, values);
  }
}

/**
 * The ramp is a node's storage in the generated graph, so a stop edit made through the template's
 * shared update callback has to rebuild the material, not only redraw it. The ColorRamp's own stop
 * callbacks do this too; the template replaces the button's callback with the ramp property's, so
 * this is the one that runs for a click on the active stop.
 */
static void rna_MaterialPaintLayer_gradient_ramp_update(Main * /*bmain*/,
                                                        Scene * /*scene*/,
                                                        PointerRNA *ptr)
{
  Material *ma = id_cast<Material *>(ptr->owner_id);
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (ma == nullptr || layer == nullptr) {
    return;
  }
  if (layer->gradient != nullptr) {
    BKE_paint_layers_gradient_ramp_edited(*ma, &layer->gradient->ramp);
  }
  DEG_id_tag_update(&ma->id, 0);
  WM_main_add_notifier(NC_MATERIAL | ND_SHADING_DRAW, ma);
}

static void rna_MaterialPaintLayerChannel_gradient_ramp_update(Main * /*bmain*/,
                                                               Scene * /*scene*/,
                                                               PointerRNA *ptr)
{
  Material *ma = id_cast<Material *>(ptr->owner_id);
  const MaterialPaintLayerChannel *record = static_cast<const MaterialPaintLayerChannel *>(
      ptr->data);
  if (ma == nullptr || record == nullptr || record->gradient_ramp == nullptr) {
    return;
  }
  BKE_paint_layers_gradient_ramp_edited(*ma, record->gradient_ramp);
  DEG_id_tag_update(&ma->id, 0);
  WM_main_add_notifier(NC_MATERIAL | ND_SHADING_DRAW, ma);
}

static PointerRNA rna_MaterialPaintLayer_gradient_ramp_get(PointerRNA *ptr)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (layer == nullptr || layer->gradient == nullptr) {
    return PointerRNA_NULL;
  }
  return RNA_pointer_create_with_parent(*ptr, RNA_ColorRamp, &layer->gradient->ramp);
}

static PointerRNA rna_MaterialPaintLayerChannel_gradient_ramp_get(PointerRNA *ptr)
{
  MaterialPaintLayerChannel *record = static_cast<MaterialPaintLayerChannel *>(ptr->data);
  /* A getter only reads: a record written before value ramps got its ramp when the file was read,
   * and a channel added later gets it on creation. */
  if (record == nullptr || record->gradient_ramp == nullptr) {
    return PointerRNA_NULL;
  }
  return RNA_pointer_create_with_parent(*ptr, RNA_ColorRamp, record->gradient_ramp);
}

static float rna_MaterialPaintLayerChannel_gradient_value_get(PointerRNA *ptr, const bool end)
{
  const MaterialPaintLayerChannel *record = static_cast<const MaterialPaintLayerChannel *>(
      ptr->data);
  /* A value ramp is the channel's source now; the two ends are its outer stops. */
  if (record->gradient_ramp != nullptr && record->gradient_ramp->tot > 0) {
    return BKE_paint_layers_gradient_channel_ramp_end_get(
        *record->gradient_ramp, eMaterialPaintChannel(record->channel), end);
  }
  return record->value[end ? 1 : 0];
}

static void rna_MaterialPaintLayerChannel_gradient_value_set(PointerRNA *ptr,
                                                            const bool end,
                                                            const float value)
{
  MaterialPaintLayer *layer = nullptr;
  if (Material *ma = rna_paint_layer_sub_owner(ptr, &layer)) {
    const MaterialPaintLayerChannel *record = static_cast<const MaterialPaintLayerChannel *>(
        ptr->data);
    const eMaterialPaintChannel channel = eMaterialPaintChannel(record->channel);
    BKE_paint_layers_gradient_set_channel_scalars(
        *ma, *layer, channel, end ? record->value[0] : value, end ? value : record->value[1]);
  }
}

static float rna_MaterialPaintLayerChannel_gradient_value_start_get(PointerRNA *ptr)
{
  return rna_MaterialPaintLayerChannel_gradient_value_get(ptr, false);
}

static void rna_MaterialPaintLayerChannel_gradient_value_start_set(PointerRNA *ptr, float value)
{
  rna_MaterialPaintLayerChannel_gradient_value_set(ptr, false, value);
}

static float rna_MaterialPaintLayerChannel_gradient_value_end_get(PointerRNA *ptr)
{
  return rna_MaterialPaintLayerChannel_gradient_value_get(ptr, true);
}

static void rna_MaterialPaintLayerChannel_gradient_value_end_set(PointerRNA *ptr, float value)
{
  rna_MaterialPaintLayerChannel_gradient_value_set(ptr, true, value);
}

/** \} */

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
    {MA_PAINT_LAYER_SOURCE_MATERIAL, "MATERIAL", 0, "Material", "What another material shows"},
    {MA_PAINT_LAYER_SOURCE_STACK, "STACK", 0, "Stack", "A nested stack of layers (a folder)"},
    {MA_PAINT_LAYER_SOURCE_FILTER, "FILTER", 0, "Filter", "A change to the colour below it"},
    {MA_PAINT_LAYER_SOURCE_ANCHOR_POINT,
     "ANCHOR_POINT",
     0,
     "Anchor Point",
     "A slice published by an Anchor Point of the same material"},
    {MA_PAINT_LAYER_SOURCE_GENERATOR,
     "GENERATOR",
     0,
     "Generator",
     "A procedural colour and coverage built from shading coordinates"},
    {MA_PAINT_LAYER_SOURCE_CURVE_PATCH,
     "CURVE_PATCH",
     0,
     "Curve Patch",
     "Maps stamped from 3D control curves through a private brush snapshot"},
    {0, nullptr, 0, nullptr, nullptr},
};

/* Values match #eMaterialPaintLayerGradientSpace. */
static const EnumPropertyItem rna_enum_material_paint_layer_gradient_space_items[] = {
    {MA_PAINT_LAYER_GRADIENT_SPACE_UV,
     "UV",
     0,
     "UV",
     "The active UV map; Z is ignored"},
    {MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT,
     "OBJECT",
     0,
     "Object",
     "Object-space coordinates, in object units"},
    {MA_PAINT_LAYER_GRADIENT_SPACE_GENERATED,
     "GENERATED",
     0,
     "Generated",
     "Generated coordinates, normalized to the object bounding box"},
    {0, nullptr, 0, nullptr, nullptr},
};

/* Values match #eMaterialPaintLayerGradientType. */
static const EnumPropertyItem rna_enum_material_paint_layer_gradient_type_items[] = {
    {MA_PAINT_LAYER_GRADIENT_LINEAR, "LINEAR", 0, "Linear", "A ramp along the line"},
    {MA_PAINT_LAYER_GRADIENT_RADIAL,
     "RADIAL",
     0,
     "Radial",
     "A ramp around the start point, reaching the end point"},
    {0, nullptr, 0, nullptr, nullptr},
};

/* Values match #eMaterialPaintLayerGradientRepeat. */
static const EnumPropertyItem rna_enum_material_paint_layer_gradient_repeat_items[] = {
    {MA_PAINT_LAYER_GRADIENT_REPEAT_NONE,
     "NONE",
     0,
     "None",
     "Clamp outside the line: the nearest end fills the surface"},
    {MA_PAINT_LAYER_GRADIENT_REPEAT_REPEAT, "REPEAT", 0, "Repeat", "Repeat the ramp"},
    {MA_PAINT_LAYER_GRADIENT_REPEAT_REFLECT,
     "REFLECT",
     0,
     "Reflect",
     "Mirror the ramp at every whole step"},
    {0, nullptr, 0, nullptr, nullptr},
};

/* Values match #eMaterialPaintLayerGradientDistribution. */
static const EnumPropertyItem rna_enum_material_paint_layer_gradient_distribution_items[] = {
    {MA_PAINT_LAYER_GRADIENT_DISTRIBUTION_MIDPOINT,
     "MIDPOINT",
     0,
     "Midpoint",
     "Bend the ramp with the midpoint: the easy control for two colors"},
    {MA_PAINT_LAYER_GRADIENT_DISTRIBUTION_RAMP,
     "RAMP",
     0,
     "Ramp",
     "Place the colors with the stops of the ramp: the control for several colors"},
    {0, nullptr, 0, nullptr, nullptr},
};

/* Values match #eMaterialPaintLayerGradientGizmoDisplay. */
static const EnumPropertyItem rna_enum_material_paint_layer_gradient_gizmo_display_items[] = {
    {MA_PAINT_LAYER_GRADIENT_GIZMO_PAINT_ONLY,
     "PAINT_ONLY",
     0,
     "Paint Only",
     "Show the handles only in the paint modes"},
    {MA_PAINT_LAYER_GRADIENT_GIZMO_ALWAYS,
     "ALWAYS",
     0,
     "Always",
     "Show the handles whenever the layer is the active one"},
    {0, nullptr, 0, nullptr, nullptr},
};

/* Values match #eMaterialPaintChannel: the channels whose ramp the gizmo can show. Normal has no
 * generator channel node, so it is absent. */
static const EnumPropertyItem rna_enum_material_paint_layer_gradient_gizmo_channel_items[] = {
    {PAINT_MATERIAL_CHANNEL_BASE_COLOR, "BASE_COLOR", 0, "Base Color", "Show the colour ramp"},
    {PAINT_MATERIAL_CHANNEL_METALLIC, "METALLIC", 0, "Metal", "Show the value ramp"},
    {PAINT_MATERIAL_CHANNEL_ROUGHNESS, "ROUGHNESS", 0, "Rough", "Show the value ramp"},
    {PAINT_MATERIAL_CHANNEL_ALPHA, "ALPHA", 0, "Alpha", "Show the value ramp"},
    {PAINT_MATERIAL_CHANNEL_AO, "AO", 0, "AO", "Show the value ramp"},
    {PAINT_MATERIAL_CHANNEL_EMISSION, "EMISSION", 0, "Emit", "Show the colour ramp"},
    {PAINT_MATERIAL_CHANNEL_SPECULAR, "SPECULAR", 0, "Spec", "Show the value ramp"},
    {0, nullptr, 0, nullptr, nullptr},
};

/* Values are #eMaterialPaintLayerFilter; 0 is "not a filter" and is only ever read. */
static const EnumPropertyItem rna_enum_material_paint_layer_filter_items[] = {
    {0, "NONE", 0, "None", "Not a filter"},
    {MA_PAINT_LAYER_FILTER_INVERT, "INVERT", 0, "Invert", "Invert the colour or coverage below"},
    {MA_PAINT_LAYER_FILTER_LEVELS, "LEVELS", 0, "Levels", "Remap the levels of what is below"},
    {MA_PAINT_LAYER_FILTER_HSL, "HSL", 0, "Hue Saturation Value", "Shift hue, saturation, value"},
    {MA_PAINT_LAYER_FILTER_COLOR_ID,
     "COLOR_ID",
     0,
     "Color ID",
     "Keep the coverage where an ID Map matches the listed colours"},
    {MA_PAINT_LAYER_FILTER_UV_ISLAND,
     "UV_ISLAND",
     0,
     "UV Island",
     "Keep the coverage on the UV islands captured from the Image Editor selection"},
    {0, nullptr, 0, nullptr, nullptr},
};

/* The Output Map of a map-coverage mask item. White and Black are the two states of the item's
 * invert flag; Gradient scales its match by its standard value. */
static const EnumPropertyItem rna_enum_material_paint_layer_output_map_items[] = {
    {0, "WHITE", 0, "White", "The filter keeps what it matches"},
    {1, "BLACK", 0, "Black", "The filter hides what it matches"},
    {2,
     "GRADIENT",
     0,
     "Gradient",
     "Scale the match by the item's standard gradient value"},
    {0, nullptr, 0, nullptr, nullptr},
};

/* How a Color ID item reads its tolerance: one shared pair, or one tolerance per colour. */
static const EnumPropertyItem rna_enum_material_paint_layer_color_id_mode_items[] = {
    {0, "SIMPLE", 0, "Simple", "Use the shared tolerance and softness"},
    {1, "ADVANCED", 0, "Advanced", "Give every colour its own tolerance, with a hard edge"},
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

static void rna_def_material_paint_layer_kind_param(BlenderRNA *brna)
{
  StructRNA *srna = RNA_def_struct(brna, "MaterialPaintLayerKindParam", nullptr);
  RNA_def_struct_path_func(srna, "rna_MaterialPaintLayerKindParam_path");
  RNA_def_struct_ui_text(
      srna, "Paint Layer Parameter", "One float parameter of a paint layer's kind, e.g. a filter's");

  PropertyRNA *prop = RNA_def_property(srna, "name", PROP_STRING, PROP_NONE);
  RNA_def_property_string_funcs(prop,
                                "rna_MaterialPaintLayerKindParam_name_get",
                                "rna_MaterialPaintLayerKindParam_name_length",
                                nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Name", "Identifier of the parameter within its kind");
  RNA_def_struct_name_property(srna, prop);

  prop = RNA_def_property(srna, "label", PROP_STRING, PROP_NONE);
  RNA_def_property_string_funcs(prop,
                                "rna_MaterialPaintLayerKindParam_label_get",
                                "rna_MaterialPaintLayerKindParam_label_length",
                                nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Label", "Name the parameter is shown with");

  prop = RNA_def_property(srna, "value", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_funcs(prop,
                               "rna_MaterialPaintLayerKindParam_value_get",
                               "rna_MaterialPaintLayerKindParam_value_set",
                               "rna_MaterialPaintLayerKindParam_value_range");
  RNA_def_property_ui_text(prop, "Value", "Value of the parameter, within its range");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "exposed", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop, "rna_MaterialPaintLayerKindParam_exposed_get", nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop,
                           "Exposed",
                           "Whether a smart-asset import marked this parameter as one the asset "
                           "exposes and this instance keeps active: a parameter the instance "
                           "switched off reads false, the mark stays");

  /* The write side of the same list the import reads: the asset's own panel toggles it, and the
   * next import of the asset marks and binds what it names. Separate from the read-only
   * #exposed above, which is the label an import already laid on this copy. */
  prop = RNA_def_property(srna, "asset_exposed", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop,
                                 "rna_MaterialPaintLayerKindParam_asset_exposed_get",
                                 "rna_MaterialPaintLayerKindParam_asset_exposed_set");
  RNA_def_property_ui_text(
      prop,
      "Expose",
      "Whether this smart asset offers the parameter to the materials that import it: an import "
      "marks and binds the exposed ones, the rest arrive as plain locals");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  /* The import marks on an instance's copy, read-only: what the asset offers is the asset's own
   * list (#asset_exposed above), the instance only works with what it was given. */
  prop = RNA_def_property(srna, "asset_offered", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop, "rna_MaterialPaintLayerKindParam_asset_offered_get", nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop,
                           "Offered",
                           "Whether the smart asset offered this parameter to the instance that "
                           "imported it; the instance's Smart Asset Parameters panel lists these");

  /* The instance's own switch over a parameter the asset offered: off, the panel greys the value
   * out and a reload takes the asset's own value instead of the instance's. The asset and the
   * other instances are not touched. A parameter that is not offered has no mark and stays off. */
  prop = RNA_def_property(srna, "asset_enabled", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop,
                                 "rna_MaterialPaintLayerKindParam_asset_enabled_get",
                                 "rna_MaterialPaintLayerKindParam_asset_enabled_set");
  RNA_def_property_ui_text(
      prop,
      "Enabled",
      "Whether this instance follows the parameter the asset offered: off, the parameter stays "
      "offered but this instance ignores it");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");
}

static void rna_def_material_smart_asset_instance(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "MaterialSmartAssetInstance", nullptr);
  RNA_def_struct_ui_text(srna,
                         "Smart Asset Instance",
                         "One drop of a Smart asset imported into a material, with its own rows "
                         "and exposed parameters");

  prop = RNA_def_property(srna, "id", PROP_INT, PROP_UNSIGNED);
  RNA_def_property_int_funcs(prop, "rna_MaterialSmartAssetInstance_id_get", nullptr, nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop,
                           "Id",
                           "Identity of the instance within its material; the Reload and Make "
                           "Unique operators take it");

  prop = RNA_def_property(srna, "label", PROP_STRING, PROP_NONE);
  RNA_def_property_string_funcs(prop,
                                "rna_MaterialSmartAssetInstance_label_get",
                                "rna_MaterialSmartAssetInstance_label_length",
                                nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop, "Label", "The instance's user-facing name: the asset's name and its drop number");

  prop = RNA_def_property(srna, "asset_name", PROP_STRING, PROP_NONE);
  RNA_def_property_string_funcs(prop,
                                "rna_MaterialSmartAssetInstance_asset_name_get",
                                "rna_MaterialSmartAssetInstance_asset_name_length",
                                nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop, "Asset Name", "Name of the Smart asset material the rows came from");

  /* Values must match #PaintLayerAssetType. */
  static const EnumPropertyItem rna_enum_material_smart_asset_type_items[] = {
      {1, "MATERIAL", 0, "Smart Material", ""},
      {2, "LAYER", 0, "Smart Layer", ""},
      {3, "MASK", 0, "Smart Mask", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };
  prop = RNA_def_property(srna, "type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_funcs(prop, "rna_MaterialSmartAssetInstance_type_get", nullptr, nullptr);
  RNA_def_property_enum_items(prop, rna_enum_material_smart_asset_type_items);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Type", "What kind of Smart asset the drop came from");

  /* A Smart Mask drop lays no rows: its parameters live on the mask items the drop landed on the
   * target row with, and the panel tells the instances apart by this flag. */
  prop = RNA_def_property(srna, "is_mask", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop, "rna_MaterialSmartAssetInstance_is_mask_get", nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop,
      "Is Mask",
      "Whether this instance is a Smart Mask drop: it owns mask items on one target row instead "
      "of stack rows");

  prop = RNA_def_property(srna, "mask_target", PROP_STRING, PROP_NONE);
  RNA_def_property_string_funcs(prop,
                                "rna_MaterialSmartAssetInstance_mask_target_get",
                                "rna_MaterialSmartAssetInstance_mask_target_length",
                                nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop,
      "Mask Target",
      "Marker of the row the Smart Mask landed on, or empty for any other instance");
}

static void rna_def_material_paint_layer_id_color(BlenderRNA *brna)
{
  StructRNA *srna = RNA_def_struct(brna, "MaterialPaintLayerIdColor", nullptr);
  RNA_def_struct_path_func(srna, "rna_MaterialPaintLayerIdColor_path");
  RNA_def_struct_ui_text(
      srna, "Color ID Color", "One colour of a Color ID mask item, matched on its ID Map");

  PropertyRNA *prop = RNA_def_property(srna, "color", PROP_FLOAT, PROP_COLOR);
  RNA_def_property_array(prop, 3);
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_float_funcs(prop,
                               "rna_MaterialPaintLayerIdColor_color_get",
                               "rna_MaterialPaintLayerIdColor_color_set",
                               nullptr);
  RNA_def_property_ui_text(
      prop, "Color", "The matched colour, as the ID Map's texels read in the shader");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "enabled", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop,
                                 "rna_MaterialPaintLayerIdColor_enabled_get",
                                 "rna_MaterialPaintLayerIdColor_enabled_set");
  RNA_def_property_ui_text(prop, "Enabled", "Whether the colour takes part in the match");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "tolerance", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_float_funcs(prop,
                               "rna_MaterialPaintLayerIdColor_tolerance_get",
                               "rna_MaterialPaintLayerIdColor_tolerance_set",
                               nullptr);
  RNA_def_property_ui_text(
      prop, "Tolerance", "The colour's own tolerance in Advanced mode; a hard edge past it");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "show_advanced", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop,
                                 "rna_MaterialPaintLayerIdColor_show_advanced_get",
                                 "rna_MaterialPaintLayerIdColor_show_advanced_set");
  RNA_def_property_ui_text(prop, "Show Advanced", "Unfold the colour's own controls");
}

/* Values match #eMaterialPaintLayerBakeMode. */
static const EnumPropertyItem rna_enum_material_paint_layer_bake_mode_items[] = {
    {MA_PAINT_LAYER_BAKE_AUTO,
     "AUTO",
     0,
     "Auto",
     "Bake the heavy inactive folders; the row being edited is evaluated live"},
    {MA_PAINT_LAYER_BAKE_ALWAYS,
     "ALWAYS",
     0,
     "Always",
     "Always bake, whatever the row weighs, once it is not being edited"},
    {MA_PAINT_LAYER_BAKE_NEVER, "NEVER", 0, "Never", "Never bake; evaluate the row live"},
    {0, nullptr, 0, nullptr, nullptr},
};

/* The menu form of `bake_size`: a preset size, or zero for "follow the content". */
static const EnumPropertyItem rna_enum_material_paint_layer_bake_size_preset_items[] = {
    {0, "AUTO", 0, "Auto", "Follow the content of the row"},
    {PAINT_NEW_CHANNEL_IMAGE_SIZE_256, "SIZE_256", 0, "256 (256 x 256)", "256 x 256"},
    {PAINT_NEW_CHANNEL_IMAGE_SIZE_512, "SIZE_512", 0, "512 (512 x 512)", "512 x 512"},
    {PAINT_NEW_CHANNEL_IMAGE_SIZE_1K, "SIZE_1K", 0, "1K (1024 x 1024)", "1024 x 1024"},
    {PAINT_NEW_CHANNEL_IMAGE_SIZE_2K, "SIZE_2K", 0, "2K (2048 x 2048)", "2048 x 2048"},
    {PAINT_NEW_CHANNEL_IMAGE_SIZE_4K, "SIZE_4K", 0, "4K (4096 x 4096)", "4096 x 4096"},
    {0, nullptr, 0, nullptr, nullptr},
};

static void rna_def_material_paint_layer_curve_patch(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "MaterialPaintLayerCurvePatch", nullptr);
  RNA_def_struct_sdna(srna, "MaterialPaintLayerCurvePatch");
  RNA_def_struct_ui_text(srna,
                         "Paint Layer Curve Patch",
                         "The owned payload of a Curve Patch layer: its curve container and brush "
                         "snapshot");

  /* The container is a real ID (bpy.data.paint_vectors); the row owns it one to one. */
  prop = RNA_def_property(srna, "vectors", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "PaintVector");
  RNA_def_property_pointer_funcs(
      prop, "rna_MaterialPaintLayerCurvePatch_vectors_get", nullptr, nullptr, nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop, "Vectors", "The curve container the Curve Patch layer paints from");

  /* The snapshot is owned by the payload; assigning a brush goes through
   * #PAINT_OT_curve_patch_layer_brush_assign, which duplicates it. */
  prop = RNA_def_property(srna, "brush", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "Brush");
  RNA_def_property_pointer_funcs(prop,
                                 "rna_MaterialPaintLayerCurvePatch_brush_get",
                                 "rna_MaterialPaintLayerCurvePatch_brush_set",
                                 nullptr,
                                 "rna_MaterialPaintLayerCurvePatch_brush_poll");
  RNA_def_property_flag(prop, PROP_EDITABLE | PROP_NEVER_UNLINK | PROP_NEVER_NULL);
  RNA_def_property_ui_text(prop,
                           "Brush",
                           "The private brush snapshot this Curve Patch paints with; drop or pick "
                           "a brush to replace it with a duplicate of that brush");
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

  /* Read-only, and derived from the instance records: a row carries no instance id of its own, so
   * a duplicate (a fresh marker) reads as bound to nothing without any cleanup. */
  prop = RNA_def_property(srna, "smart_asset_instance_id", PROP_INT, PROP_NONE);
  RNA_def_property_int_funcs(
      prop, "rna_MaterialPaintLayer_smart_asset_instance_id_get", nullptr, nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop,
                           "Smart Asset Instance Id",
                           "Id of the imported Smart asset this row came from -- a Smart Mask "
                           "instance answers for the mask items it laid down -- or -1 when the "
                           "row is not of an instance");

  prop = RNA_def_property(srna, "smart_asset_instance_label", PROP_STRING, PROP_NONE);
  RNA_def_property_string_funcs(prop,
                                "rna_MaterialPaintLayer_smart_asset_instance_label_get",
                                "rna_MaterialPaintLayer_smart_asset_instance_label_length",
                                nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop,
      "Smart Asset Instance Label",
      "Label of the imported Smart asset this row came from (\"<asset> #<number>\"), or empty");

  /* The payload of a Curve Patch row; null for every other kind. */
  prop = RNA_def_property(srna, "curve_patch", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "MaterialPaintLayerCurvePatch");
  RNA_def_property_pointer_sdna(prop, nullptr, "curve_patch");
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Curve Patch", "The curve container and brush snapshot the row owns");

  /* The channel of its source a mask correction that reads a material takes its grey from. Only
   * the channels a grey can be taken from are offered. */
  static const EnumPropertyItem mask_source_channel_items[] = {
      {PAINT_MATERIAL_CHANNEL_BASE_COLOR, "BASE_COLOR", 0, "Base Color", ""},
      {PAINT_MATERIAL_CHANNEL_METALLIC, "METALLIC", 0, "Metallic", ""},
      {PAINT_MATERIAL_CHANNEL_ROUGHNESS, "ROUGHNESS", 0, "Roughness", ""},
      {PAINT_MATERIAL_CHANNEL_SPECULAR, "SPECULAR", 0, "Specular", ""},
      {PAINT_MATERIAL_CHANNEL_HEIGHT, "HEIGHT", 0, "Height", ""},
      {PAINT_MATERIAL_CHANNEL_AO, "AO", 0, "Ambient Occlusion", ""},
      {PAINT_MATERIAL_CHANNEL_EMISSION, "EMISSION", 0, "Emission", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };
  prop = RNA_def_property(srna, "mask_source_channel", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, mask_source_channel_items);
  RNA_def_property_enum_funcs(prop,
                              "rna_MaterialPaintLayer_mask_source_channel_get",
                              "rna_MaterialPaintLayer_mask_source_channel_set",
                              nullptr);
  RNA_def_property_ui_text(
      prop,
      "Source Channel",
      "Channel of the source material the grey of this mask correction is taken from");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  /* Read-only: set when "Use Row Result" fills the row's maps, so the row can be updated from the
   * same source. Empty while the maps were not made that way. */
  prop = RNA_def_property(srna, "row_result_source", PROP_STRING, PROP_NONE);
  RNA_def_property_string_funcs(prop,
                                "rna_MaterialPaintLayer_row_result_source_get",
                                "rna_MaterialPaintLayer_row_result_source_length",
                                nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop, "Row Result Source", "Marker of the row the maps of this row were last made from");

  /* The Anchor Point an Anchor Point Reader lays. Empty for every other kind. */
  prop = RNA_def_property(srna, "anchor_point_source", PROP_STRING, PROP_NONE);
  RNA_def_property_string_funcs(prop,
                                "rna_MaterialPaintLayer_anchor_point_source_get",
                                "rna_MaterialPaintLayer_anchor_point_source_length",
                                "rna_MaterialPaintLayer_anchor_point_source_set");
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop, "Anchor Point", "Marker of the Anchor Point whose slice this reader lays");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  /* The reader's choice as a filtered enum: only markers of this material that build before it
   * and resolve cleanly. The UUID string above stays for scripts. */
  prop = RNA_def_property(srna, "anchor_point_target", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_material_paint_layer_anchor_point_target_items);
  RNA_def_property_enum_funcs(prop,
                              "rna_MaterialPaintLayer_anchor_point_target_get",
                              "rna_MaterialPaintLayer_anchor_point_target_set",
                              "rna_MaterialPaintLayer_anchor_point_target_itemf");
  RNA_def_property_ui_text(prop, "Anchor Point", "Anchor Point whose slice this reader lays");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "anchor_point_reader_count", PROP_INT, PROP_NONE);
  RNA_def_property_int_funcs(
      prop, "rna_MaterialPaintLayer_anchor_point_reader_count_get", nullptr, nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Readers", "How many rows read this Anchor Point");

  prop = RNA_def_property(srna, "is_anchor_point_reader", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(
      prop, "rna_MaterialPaintLayer_is_anchor_point_reader_get", nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Is Anchor Point Reader", "Whether this row reads a slice");

  /* Read-only: how many channels the row takes part in, the generated node-cost measure the UI
   * reads to warn about a row heavy enough to bake in AUTO mode. */
  prop = RNA_def_property(srna, "channel_weight", PROP_INT, PROP_NONE);
  RNA_def_property_int_funcs(prop, "rna_MaterialPaintLayer_channel_weight_get", nullptr, nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop, "Channel Weight", "How many channels this row takes part in, so how many it wires");

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

  /* Read-only: the row is a Gradient generator, so the Gradient panel below applies. */
  prop = RNA_def_property(srna, "gradient_supported", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop, "rna_MaterialPaintLayer_gradient_supported_get", nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop, "Gradient Supported", "Whether the row is a procedural Gradient generator");

  /* The viewport gizmo: a UI flag on the payload, so the button can sit next to the line
   * fields it edits. */
  prop = RNA_def_property(srna, "gradient_gizmo", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop,
                                 "rna_MaterialPaintLayer_gradient_gizmo_get",
                                 "rna_MaterialPaintLayer_gradient_gizmo_set");
  RNA_def_property_ui_text(
      prop,
      "Gradient Gizmo",
      "Show the gradient's line as draggable handles in the 3D Viewport and the Image Editor");
  /* A viewport-only flag: the material's shading is untouched, so only a redraw is needed. */
  RNA_def_property_update_notifier(prop, NC_MATERIAL | ND_SHADING);

  prop = RNA_def_property(srna, "gradient_gizmo_display", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_material_paint_layer_gradient_gizmo_display_items);
  RNA_def_property_enum_funcs(prop,
                              "rna_MaterialPaintLayer_gradient_gizmo_display_get",
                              "rna_MaterialPaintLayer_gradient_gizmo_display_set",
                              nullptr);
  RNA_def_property_ui_text(
      prop, "Gizmo Display", "When the handles of the gradient's line are shown");
  RNA_def_property_update_notifier(prop, NC_MATERIAL | ND_SHADING);

  prop = RNA_def_property(srna, "gradient_gizmo_channel", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_material_paint_layer_gradient_gizmo_channel_items);
  RNA_def_property_enum_funcs(prop,
                              "rna_MaterialPaintLayer_gradient_gizmo_channel_get",
                              "rna_MaterialPaintLayer_gradient_gizmo_channel_set",
                              nullptr);
  RNA_def_property_ui_text(
      prop,
      "Gizmo Channel",
      "Channel whose ramp the Gradient handles show and move");
  RNA_def_property_update_notifier(prop, NC_MATERIAL | ND_SHADING);

  prop = RNA_def_property(srna, "gradient_distribution", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_material_paint_layer_gradient_distribution_items);
  RNA_def_property_enum_funcs(prop,
                              "rna_MaterialPaintLayer_gradient_distribution_get",
                              "rna_MaterialPaintLayer_gradient_distribution_set",
                              nullptr);
  RNA_def_property_ui_text(
      prop,
      "Gradient Distribution",
      "What spreads the colors along the line: the midpoint, or the stops of the ramp");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "gradient_space", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_material_paint_layer_gradient_space_items);
  RNA_def_property_enum_funcs(prop,
                              "rna_MaterialPaintLayer_gradient_space_get",
                              "rna_MaterialPaintLayer_gradient_space_set",
                              nullptr);
  RNA_def_property_ui_text(
      prop, "Gradient Space", "Space the gradient's line is defined in");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "gradient_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_material_paint_layer_gradient_type_items);
  RNA_def_property_enum_funcs(prop,
                              "rna_MaterialPaintLayer_gradient_type_get",
                              "rna_MaterialPaintLayer_gradient_type_set",
                              nullptr);
  RNA_def_property_ui_text(prop, "Gradient Type", "Shape of the gradient's iso-lines");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "gradient_repeat", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_material_paint_layer_gradient_repeat_items);
  RNA_def_property_enum_funcs(prop,
                              "rna_MaterialPaintLayer_gradient_repeat_get",
                              "rna_MaterialPaintLayer_gradient_repeat_set",
                              nullptr);
  RNA_def_property_ui_text(
      prop, "Gradient Repeat", "How the UV parameter outside 0..1 is folded into the ramp");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "gradient_midpoint", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_funcs(prop,
                               "rna_MaterialPaintLayer_gradient_midpoint_get",
                               "rna_MaterialPaintLayer_gradient_midpoint_set",
                               nullptr);
  RNA_def_property_range(prop, math::gradient_midpoint_min, math::gradient_midpoint_max);
  RNA_def_property_ui_text(prop, "Gradient Midpoint", "Where the ramp's middle lands along the line");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "gradient_start", PROP_FLOAT, PROP_XYZ);
  RNA_def_property_array(prop, 3);
  RNA_def_property_float_funcs(prop,
                               "rna_MaterialPaintLayer_gradient_start_get",
                               "rna_MaterialPaintLayer_gradient_start_set",
                               nullptr);
  RNA_def_property_ui_text(
      prop,
      "Gradient Start",
      "Start of the line, or centre of a radial gradient, in gradient space");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "gradient_end", PROP_FLOAT, PROP_XYZ);
  RNA_def_property_array(prop, 3);
  RNA_def_property_float_funcs(prop,
                               "rna_MaterialPaintLayer_gradient_end_get",
                               "rna_MaterialPaintLayer_gradient_end_set",
                               nullptr);
  RNA_def_property_ui_text(
      prop,
      "Gradient End",
      "End of the line, or radius point of a radial gradient, in gradient space");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "gradient_ramp", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "ColorRamp");
  RNA_def_property_pointer_funcs(
      prop, "rna_MaterialPaintLayer_gradient_ramp_get", nullptr, nullptr, nullptr);
  RNA_def_property_ui_text(
      prop, "Gradient Ramp", "Colours a Gradient's colour channels read along the line");
  RNA_def_property_update(
      prop, NC_MATERIAL | ND_SHADING, "rna_MaterialPaintLayer_gradient_ramp_update");

  prop = RNA_def_property(srna, "material", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, nullptr, "material");
  RNA_def_property_struct_type(prop, "Material");
  RNA_def_property_pointer_funcs(prop,
                                 nullptr,
                                 "rna_MaterialPaintLayer_material_set",
                                 nullptr,
                                 "rna_MaterialPaintLayer_material_poll");
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop, "Material", "The material a Material layer reads; none for every other source");
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

  prop = RNA_def_property(srna, "locked", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(
      prop, "rna_MaterialPaintLayer_locked_get", "rna_MaterialPaintLayer_locked_set");
  RNA_def_property_ui_text(
      prop, "Locked", "Protect the layer from painting; its pixels and settings stay as they are");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING_DRAW, "rna_MaterialPaintLayer_locked_update");

  prop = RNA_def_property(srna, "color_tag", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_material_paint_layer_color_items);
  RNA_def_property_enum_funcs(prop,
                              "rna_MaterialPaintLayer_color_tag_get",
                              "rna_MaterialPaintLayer_color_tag_set",
                              nullptr);
  RNA_def_property_ui_text(prop,
                           "Color Tag",
                           "Display color tag of a layer group; interpreted by the UI only");
  /* Notifier only. Deliberately *not* #rna_Material_update, which tags #ID_RECALC_SHADING: a color
   * tag changes nothing evaluated, and recompiling the stack for it would be the same mistake the
   * sibling #SculptLayerGroup.color_tag documents against. */
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, nullptr);

  /* Read-only: the one predicate the UI asks. The panel shows the Mapping block only while this
   * is true, instead of inspecting channel records itself. Named after
   * #BKE_paint_layers_mapping_available, whose answer it is. */
  prop = RNA_def_property(srna, "mapping_available", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop, "rna_MaterialPaintLayer_mapping_available_get", nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop, "Mapping Available", "Whether the row keeps a UV mapping the UI can edit");

  /* Read-only: whether the mapping is applied right now (enabled and supported). */
  prop = RNA_def_property(srna, "mapping_applies", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop, "rna_MaterialPaintLayer_mapping_applies_get", nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop, "Mapping Applies", "Whether the enabled mapping actually reaches what the row shows");

  prop = RNA_def_property(srna, "mapping_enabled", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop,
                                 "rna_MaterialPaintLayer_mapping_enabled_get",
                                 "rna_MaterialPaintLayer_mapping_enabled_set");
  RNA_def_property_editable_func(prop, "rna_MaterialPaintLayer_mapping_enabled_editable");
  RNA_def_property_ui_text(prop, "Mapping Enabled", "Remap the row's maps in UV space");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "mapping_scale_lock", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop,
                                 "rna_MaterialPaintLayer_mapping_scale_lock_get",
                                 "rna_MaterialPaintLayer_mapping_scale_lock_set");
  RNA_def_property_ui_text(prop, "Lock Scale", "Edit both scale axes together");
  RNA_def_property_ui_icon(prop, ICON_UNLOCKED, 1);
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  /* Stored from the start; not editable while UV is the only space. */
  prop = RNA_def_property(srna, "mapping_space", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_material_paint_layer_mapping_space_items);
  RNA_def_property_enum_funcs(prop, "rna_MaterialPaintLayer_mapping_space_get", nullptr, nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Mapping Space", "Projection the row's mapping works in");

  prop = RNA_def_property(srna, "mapping_offset", PROP_FLOAT, PROP_NONE);
  RNA_def_property_array(prop, 2);
  RNA_def_property_float_funcs(prop,
                               "rna_MaterialPaintLayer_mapping_offset_get",
                               "rna_MaterialPaintLayer_mapping_offset_set",
                               nullptr);
  RNA_def_property_ui_text(prop, "Mapping Offset", "Translation of the row's maps in UV space");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "mapping_scale", PROP_FLOAT, PROP_NONE);
  RNA_def_property_array(prop, 2);
  RNA_def_property_float_funcs(prop,
                               "rna_MaterialPaintLayer_mapping_scale_get",
                               "rna_MaterialPaintLayer_mapping_scale_set",
                               nullptr);
  /* A negative axis mirrors the map; the shared helper keeps the sign and only lifts a magnitude
   * below #PAINT_LAYER_MAPPING_SCALE_MIN (a zero reads back as one). */
  RNA_def_property_range(prop, -FLT_MAX, FLT_MAX);
  {
    /* UI default 1,1; DNA zeroes still read back as one through the shared helper. */
    static const float mapping_scale_default[2] = {1.0f, 1.0f};
    RNA_def_property_float_array_default(prop, mapping_scale_default);
  }
  RNA_def_property_ui_text(prop, "Mapping Scale", "Scale of the row's maps in UV space");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  /* Rotation changes where the row's maps are read; a Normal map's vectors follow it. */
  prop = RNA_def_property(srna, "mapping_rotation", PROP_FLOAT, PROP_ANGLE);
  RNA_def_property_float_funcs(prop,
                               "rna_MaterialPaintLayer_mapping_rotation_get",
                               "rna_MaterialPaintLayer_mapping_rotation_set",
                               nullptr);
  RNA_def_property_ui_text(prop,
                           "Mapping Rotation",
                           "Rotation of the row's maps around Z in UV space; a Normal map's "
                           "vectors follow it");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  /* Read-only: where the tile's middle sits on the canvas. The Mapping panel's fields read it to
   * keep the tile in place while the scale and the rotation change; the offset is the one stored
   * value that moves the tile. */
  prop = RNA_def_property(srna, "mapping_tile_center", PROP_FLOAT, PROP_NONE);
  RNA_def_property_array(prop, 2);
  RNA_def_property_float_funcs(
      prop, "rna_MaterialPaintLayer_mapping_tile_center_get", nullptr, nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop,
                           "Tile Center",
                           "Position of the tile's middle on the canvas, which the scale and the "
                           "rotation edits of the UI keep still");

  /* The panel's edits: they change the scale, rotation or lock and follow the offset so the tile
   * keeps its place. The raw properties above stay the data's own meaning for scripts and
   * drivers. */
  prop = RNA_def_property(srna, "tile_scale", PROP_FLOAT, PROP_NONE);
  RNA_def_property_array(prop, 2);
  RNA_def_property_float_funcs(prop,
                               "rna_MaterialPaintLayer_mapping_tile_scale_get",
                               "rna_MaterialPaintLayer_mapping_tile_scale_set",
                               nullptr);
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_range(prop, -FLT_MAX, FLT_MAX);
  {
    static const float mapping_scale_default[2] = {1.0f, 1.0f};
    RNA_def_property_float_array_default(prop, mapping_scale_default);
  }
  RNA_def_property_ui_text(
      prop, "Scale", "Scale of the row's maps, keeping the tile's centre on the canvas");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "tile_rotation", PROP_FLOAT, PROP_ANGLE);
  RNA_def_property_float_funcs(prop,
                               "rna_MaterialPaintLayer_mapping_tile_rotation_get",
                               "rna_MaterialPaintLayer_mapping_tile_rotation_set",
                               nullptr);
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop, "Rotation", "Rotate the row's maps around the tile's centre on the canvas");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "tile_scale_lock", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop,
                                 "rna_MaterialPaintLayer_mapping_tile_scale_lock_get",
                                 "rna_MaterialPaintLayer_mapping_tile_scale_lock_set");
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop, "Lock Scale", "Edit both scale axes together, keeping the tile in place");
  RNA_def_property_ui_icon(prop, ICON_UNLOCKED, 1);
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "mapping_wrap", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_material_paint_layer_mapping_wrap_items);
  RNA_def_property_enum_funcs(prop,
                              "rna_MaterialPaintLayer_mapping_wrap_get",
                              "rna_MaterialPaintLayer_mapping_wrap_set",
                              nullptr);
  RNA_def_property_ui_text(prop,
                           "UV Wrap",
                           "What the row's maps do outside the unit UV square after the mapping");
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

  prop = RNA_def_property(srna, "kind_params", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_struct_type(prop, "MaterialPaintLayerKindParam");
  RNA_def_property_collection_funcs(prop,
                                    "rna_MaterialPaintLayer_kind_params_begin",
                                    "rna_iterator_listbase_next",
                                    "rna_iterator_listbase_end",
                                    "rna_iterator_listbase_get",
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop, "Kind Parameters", "The parameters of the row's kind, such as a filter's, in order");

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

  prop = RNA_def_property(srna, "filter_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_material_paint_layer_filter_items);
  RNA_def_property_enum_funcs(prop, "rna_MaterialPaintLayer_filter_type_get", nullptr, nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Filter Type", "Which filter a filter row is; None for any other row");

  prop = RNA_def_property(srna, "output_map", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_material_paint_layer_output_map_items);
  RNA_def_property_enum_funcs(prop,
                              "rna_MaterialPaintLayer_output_map_get",
                              "rna_MaterialPaintLayer_output_map_set",
                              nullptr);
  RNA_def_property_ui_text(prop,
                           "Output Map",
                           "What a map-coverage mask item gives back: White keeps what it matches, "
                           "Black hides it, Gradient scales the match by the standard value");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "output_alpha", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop,
                                 "rna_MaterialPaintLayer_output_alpha_get",
                                 "rna_MaterialPaintLayer_output_alpha_set");
  RNA_def_property_ui_text(prop,
                           "Alpha Background",
                           "Carry the output coverage in alpha, leaving the background transparent");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "color_id_mode", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_material_paint_layer_color_id_mode_items);
  RNA_def_property_enum_funcs(prop,
                              "rna_MaterialPaintLayer_color_id_mode_get",
                              "rna_MaterialPaintLayer_color_id_mode_set",
                              nullptr);
  RNA_def_property_ui_text(prop,
                           "Color ID Mode",
                           "Simple uses one tolerance and softness; Advanced gives every colour "
                           "its own tolerance");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "is_map_coverage", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop, "rna_MaterialPaintLayer_is_map_coverage_get", nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop,
                           "Is Map Coverage",
                           "Whether this mask item restricts the coverage by a map (Color ID or UV "
                           "Island)");

  prop = RNA_def_property(srna, "map_paintable", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop, "rna_MaterialPaintLayer_map_paintable_get", nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop, "Map Paintable", "Whether the item's map may be painted; a Color ID's ID Map is not");

  prop = RNA_def_property(srna, "id_colors", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_struct_type(prop, "MaterialPaintLayerIdColor");
  RNA_def_property_collection_funcs(prop,
                                    "rna_MaterialPaintLayer_id_colors_begin",
                                    "rna_iterator_listbase_next",
                                    "rna_iterator_listbase_end",
                                    "rna_iterator_listbase_get",
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Colors", "The colours a Color ID mask item matches, newest first");

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

  func = RNA_def_function(srna, "mask_filter_add", "rna_MaterialPaintLayer_mask_filter_add");
  RNA_def_function_ui_description(
      func, "Add a filter to the layer's mask, over the items already there");
  RNA_def_function_flag(func, FUNC_SELF_AS_RNA | FUNC_USE_REPORTS);
  RNA_def_enum(func,
               "filter_type",
               rna_enum_material_paint_layer_filter_items,
               MA_PAINT_LAYER_FILTER_COLOR_ID,
               "Filter Type",
               "Which filter to add");
  parm = RNA_def_pointer(func, "item", "MaterialPaintLayer", "", "The new mask item");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "id_color_add", "rna_MaterialPaintLayer_id_color_add");
  RNA_def_function_ui_description(func, "Add a colour on top of a Color ID mask item's list");
  RNA_def_function_flag(func, FUNC_SELF_AS_RNA | FUNC_USE_REPORTS);
  parm = RNA_def_float_color(
      func, "color", 3, nullptr, 0.0f, 1.0f, "Color", "The colour to match", 0.0f, 1.0f);
  RNA_def_parameter_flags(parm, PROP_THICK_WRAP, PARM_REQUIRED);
  parm = RNA_def_pointer(func, "id_color", "MaterialPaintLayerIdColor", "", "The new colour");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "id_color_remove", "rna_MaterialPaintLayer_id_color_remove");
  RNA_def_function_ui_description(func, "Remove the colour at \"index\" from a Color ID mask item");
  RNA_def_function_flag(func, FUNC_SELF_AS_RNA | FUNC_USE_REPORTS);
  parm = RNA_def_int(func, "index", 0, 0, INT_MAX, "Index", "Position in the list", 0, INT_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);

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
  RNA_def_property_range(prop, 0, 4096);
  RNA_def_property_ui_text(
      prop, "Bake Size", "Square side the row's baked maps are created at; zero follows the content");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "bake_size_preset", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_material_paint_layer_bake_size_preset_items);
  RNA_def_property_enum_funcs(prop,
                              "rna_MaterialPaintLayer_bake_size_preset_get",
                              "rna_MaterialPaintLayer_bake_size_preset_set",
                              nullptr);
  RNA_def_property_ui_text(
      prop, "Bake Size", "Square side the row's baked maps are created at, as a preset");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "bake_is_valid", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop, "rna_MaterialPaintLayer_bake_is_valid_get", nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop, "Bake Is Valid", "Whether the stored bake still matches the row's description");

  func = RNA_def_function(srna, "bake_request", "rna_MaterialPaintLayer_bake_request");
  RNA_def_function_ui_description(func,
                                  "Mark the row's bake stale so it is rendered again (by the planner, "
                                  "or by the editor for a Material row)");
  RNA_def_function_flag(func, FUNC_SELF_AS_RNA);

  func = RNA_def_function(srna, "bake_clear", "rna_MaterialPaintLayer_bake_clear");
  RNA_def_function_ui_description(func, "Drop the row's baked cache");
  RNA_def_function_flag(func, FUNC_SELF_AS_RNA);

  static const EnumPropertyItem object_mask_type_items[] = {
      {MA_PAINT_LAYER_OBJECT_MASK_MESH, "MESH", 0, "Mesh", "Choose the objects the layer shows on"},
      {MA_PAINT_LAYER_OBJECT_MASK_UV_TILES,
       "UV_TILES",
       0,
       "UV Tiles",
       "Choose the UDIM tiles of each object the layer shows on"},
      {0, nullptr, 0, nullptr, nullptr},
  };
  prop = RNA_def_property(srna, "object_mask_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_funcs(prop,
                              "rna_MaterialPaintLayer_object_mask_type_get",
                              "rna_MaterialPaintLayer_object_mask_type_set",
                              nullptr);
  RNA_def_property_enum_items(prop, object_mask_type_items);
  RNA_def_property_ui_text(prop, "Mask Type", "Limit the layer to some objects or UDIM tiles");
  /* Topology: the generator adds or drops the Attribute nodes. */
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  /* The per-object choices are stored on the objects, keyed by this string. */
  prop = RNA_def_property(srna, "object_mask_key", PROP_STRING, PROP_NONE);
  RNA_def_property_string_funcs(prop,
                                "rna_MaterialPaintLayer_object_mask_key_get",
                                "rna_MaterialPaintLayer_object_mask_key_length",
                                nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop, "Object Mask Key", "Key of this row's records in Object.paint_layer_masks");

  func = RNA_def_function(
      srna, "object_mask_visible_get", "rna_MaterialPaintLayer_object_mask_visible_get");
  RNA_def_function_ui_description(func, "Whether the layer shows on the object (Mesh mode)");
  RNA_def_function_flag(func, FUNC_SELF_AS_RNA);
  parm = RNA_def_pointer(func, "object", "Object", "", "The object to query");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_boolean(func, "visible", true, "", "Whether the layer shows");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(
      srna, "object_mask_visible_set", "rna_MaterialPaintLayer_object_mask_visible_set");
  RNA_def_function_ui_description(func, "Show or hide the layer on the object (Mesh mode)");
  RNA_def_function_flag(func, FUNC_SELF_AS_RNA | FUNC_USE_MAIN | FUNC_USE_REPORTS);
  parm = RNA_def_pointer(func, "object", "Object", "", "The object to change");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_boolean(func, "visible", true, "", "Whether the layer shows");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);

  func = RNA_def_function(
      srna, "object_mask_tile_get", "rna_MaterialPaintLayer_object_mask_tile_get");
  RNA_def_function_ui_description(
      func, "Whether the layer shows on a UDIM tile of the object (UV Tiles mode)");
  RNA_def_function_flag(func, FUNC_SELF_AS_RNA);
  parm = RNA_def_pointer(func, "object", "Object", "", "The object to query");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  /* Wide range: an out-of-range tile reads as visible, so the UI can query unmaskable tiles. */
  parm = RNA_def_int(func,
                     "tile",
                     bke::PAINT_LAYER_OBJECT_MASK_TILE_FIRST,
                     0,
                     INT_MAX,
                     "Tile",
                     "UDIM tile number",
                     bke::PAINT_LAYER_OBJECT_MASK_TILE_FIRST,
                     bke::PAINT_LAYER_OBJECT_MASK_TILE_LAST);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_boolean(func, "visible", true, "", "Whether the layer shows on the tile");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(
      srna, "object_mask_tile_set", "rna_MaterialPaintLayer_object_mask_tile_set");
  RNA_def_function_ui_description(
      func, "Show or hide the layer on a UDIM tile of the object (UV Tiles mode)");
  RNA_def_function_flag(func, FUNC_SELF_AS_RNA | FUNC_USE_MAIN | FUNC_USE_REPORTS);
  parm = RNA_def_pointer(func, "object", "Object", "", "The object to change");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_int(func,
                     "tile",
                     bke::PAINT_LAYER_OBJECT_MASK_TILE_FIRST,
                     bke::PAINT_LAYER_OBJECT_MASK_TILE_FIRST,
                     bke::PAINT_LAYER_OBJECT_MASK_TILE_LAST,
                     "Tile",
                     "UDIM tile number",
                     bke::PAINT_LAYER_OBJECT_MASK_TILE_FIRST,
                     bke::PAINT_LAYER_OBJECT_MASK_TILE_LAST);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_boolean(func, "visible", true, "", "Whether the layer shows on the tile");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);

  func = RNA_def_function(srna, "object_mask_tiles", "rna_MaterialPaintLayer_object_mask_tiles");
  RNA_def_function_ui_description(
      func,
      "The UDIM tiles the object's faces using this material occupy (cached until refreshed)");
  RNA_def_function_flag(func, FUNC_SELF_AS_RNA);
  parm = RNA_def_pointer(func, "object", "Object", "", "The object to query");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  RNA_def_boolean(func, "refresh", false, "Refresh", "Rescan the UVs instead of using the cache");
  parm = RNA_def_int_array(
      func, "tiles", 1, nullptr, 1001, 2000, "Tiles", "Occupied UDIM tile numbers", 1001, 2000);
  RNA_def_parameter_flags(parm, PROP_DYNAMIC, PARM_OUTPUT);

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
  RNA_def_struct_path_func(srna, "rna_MaterialPaintLayerChannelSettings_path");
  RNA_def_struct_ui_text(srna,
                         "Paint Layer Channel Settings",
                         "The blend and opacity a paint layer row is laid with in one paint "
                         "channel");

  PropertyRNA *prop = RNA_def_property(srna, "channel", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_funcs(
      prop, "rna_MaterialPaintLayerChannelSettings_channel_get", nullptr, nullptr);
  RNA_def_property_enum_items(prop, rna_enum_material_paint_channel_items);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Channel", "The paint channel these settings are for");

  prop = RNA_def_property(srna, "blend_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_funcs(prop,
                              "rna_MaterialPaintLayerChannelSettings_blend_get",
                              "rna_MaterialPaintLayerChannelSettings_blend_set",
                              "rna_MaterialPaintLayerChannelSettings_blend_itemf");
  RNA_def_property_enum_items(prop, rna_enum_material_paint_layer_channel_blend_items);
  /* Inherit, so "Reset to Default" clears the override instead of storing an explicit Mix (which
   * on the Normal channel is not even an offered item). */
  RNA_def_property_enum_default(prop, -1);
  RNA_def_property_ui_text(prop,
                           "Blend Mode",
                           "The channel's own blend, or Inherit to use the row's; the Normal "
                           "channel offers Mix (combine) or Replace");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "opacity", PROP_FLOAT, PROP_PERCENTAGE);
  RNA_def_property_float_funcs(prop,
                               "rna_MaterialPaintLayerChannelSettings_opacity_get",
                               "rna_MaterialPaintLayerChannelSettings_opacity_set",
                               nullptr);
  RNA_def_property_range(prop, 0.0f, 100.0f);
  RNA_def_property_ui_text(
      prop, "Opacity", "Opacity of the row in this channel, applied over the row's own");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "height_blend_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_funcs(
      prop,
      "rna_MaterialPaintLayerChannelSettings_height_blend_get",
      "rna_MaterialPaintLayerChannelSettings_height_blend_set",
      nullptr);
  RNA_def_property_enum_items(prop, rna_enum_material_paint_layer_height_blend_items);
  RNA_def_property_enum_default(prop, MA_PAINT_LAYER_HEIGHT_BLEND_INHERIT);
  RNA_def_property_ui_text(
      prop,
      "Height Blend Mode",
      "The Height record's own blend over the height below it, relative to the neutral level; "
      "Inherit follows the row's blend (Mix, Add and Subtract keep their meaning, any other blend "
      "reads as Mix). The Height channel alone honours it");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "height_scale", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_funcs(
      prop,
      "rna_MaterialPaintLayerChannelSettings_height_scale_get",
      "rna_MaterialPaintLayerChannelSettings_height_scale_set",
      nullptr);
  RNA_def_property_range(prop, PAINT_HEIGHT_SCALE_MIN, PAINT_HEIGHT_SCALE_MAX);
  RNA_def_property_ui_text(
      prop,
      "Height Scale",
      "The scale of the record's distance from the neutral level; one is neutral and negative "
      "scales are not offered (Subtract inverts). The Height channel alone honours it");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "height_write", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(
      prop,
      "rna_MaterialPaintLayerChannelSettings_height_write_get",
      "rna_MaterialPaintLayerChannelSettings_height_write_set");
  RNA_def_property_ui_text(
      prop,
      "Write Height",
      "Whether the row's Height record reaches the relief; off keeps the record and its map, and "
      "leaves the row's other channels untouched");
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
  /* The setter does its own id_us_plus()/id_us_min() (see #BKE_paint_layers_channel_set_image), so
   * the flag has to name that explicitly or #RNA_property_pointer_set's user-count consistency
   * check misreads it -- and a caller balancing a #BKE_image_load_exists loan on the flag (the
   * drop target) leaks a user per assignment without it. */
  RNA_def_property_flag(prop, PROP_EDITABLE | PROP_ID_REFCOUNT);
  RNA_def_property_ui_text(prop, "Image", "The channel's map, or none for a constant");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_MaterialPaintLayerChannel_image_update");

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

  static const EnumPropertyItem normal_space_items[] = {
      {0, "OPENGL", 0, "OpenGL", "Tangent-space normal map with green pointing up (+Y)"},
      {1,
       "DIRECTX",
       0,
       "DirectX",
       "Tangent-space normal map with green pointing down (-Y); the generated graph flips the "
       "green channel"},
      {0, nullptr, 0, nullptr, nullptr},
  };
  prop = RNA_def_property(srna, "normal_space", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, normal_space_items);
  RNA_def_property_enum_funcs(prop,
                              "rna_MaterialPaintLayerChannel_normal_space_get",
                              "rna_MaterialPaintLayerChannel_normal_space_set",
                              nullptr);
  RNA_def_property_ui_text(
      prop, "Tangent Space", "Convention of the Normal map: OpenGL or DirectX tangent space");
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

  /* A scalar channel of a Gradient generator ramps between these two values. */
  prop = RNA_def_property(srna, "gradient_value_start", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_funcs(prop,
                               "rna_MaterialPaintLayerChannel_gradient_value_start_get",
                               "rna_MaterialPaintLayerChannel_gradient_value_start_set",
                               "rna_MaterialPaintLayerChannel_value_scalar_range");
  RNA_def_property_ui_text(prop, "Gradient Start", "The scalar channel's value at the line start");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  /* A scalar channel of a Gradient generator reads this value ramp (grey stops) instead of the
   * two ends above. Colour channels read the row's own `gradient_ramp`. */
  prop = RNA_def_property(srna, "gradient_ramp", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "ColorRamp");
  RNA_def_property_pointer_funcs(
      prop, "rna_MaterialPaintLayerChannel_gradient_ramp_get", nullptr, nullptr, nullptr);
  RNA_def_property_ui_text(
      prop,
      "Value Ramp",
      "The scalar channel's value ramp: grey stops along the gradient, edited as values");
  RNA_def_property_update(
      prop,
      NC_MATERIAL | ND_SHADING,
      "rna_MaterialPaintLayerChannel_gradient_ramp_update");

  prop = RNA_def_property(srna, "gradient_value_end", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_funcs(prop,
                               "rna_MaterialPaintLayerChannel_gradient_value_end_get",
                               "rna_MaterialPaintLayerChannel_gradient_value_end_set",
                               "rna_MaterialPaintLayerChannel_value_scalar_range");
  RNA_def_property_ui_text(prop, "Gradient End", "The scalar channel's value at the line end");
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
  RNA_def_pointer(func,
                  "material",
                  "Material",
                  "Material",
                  "The material a layer of the Material source reads");
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
  RNA_def_function_flag(func, FUNC_USE_MAIN | FUNC_USE_REPORTS);
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
  RNA_def_function_ui_description(func,
                                  "Composite one channel of the stack into an image on the CPU");
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

  prop = RNA_def_property(srna, "paint_layer_add_material", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "Material");
  RNA_def_property_pointer_funcs(prop,
                                 "rna_Material_paint_layer_add_material_get",
                                 "rna_Material_paint_layer_add_material_set",
                                 nullptr,
                                 "rna_Material_paint_layer_add_material_poll");
  RNA_def_property_flag(prop, PROP_EDITABLE | PROP_HIDDEN);
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_override_flag(prop, PROPOVERRIDE_NO_COMPARISON);
  RNA_def_property_ui_text(prop,
                           "Add Material Layer",
                           "Write-only: assigning a material adds a layer that reads it above the "
                           "active layer; reads as none");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_paint_layer_add_material_update");

  /* The same write-only assignment for a Material correction on the active layer: the ID Browser
   * popover of the Add asks for the material through it. */
  prop = RNA_def_property(srna, "paint_layer_add_material_correction", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "Material");
  RNA_def_property_pointer_funcs(prop,
                                 "rna_Material_paint_layer_add_material_get",
                                 "rna_Material_paint_layer_add_material_set",
                                 nullptr,
                                 "rna_Material_paint_layer_add_material_poll");
  RNA_def_property_flag(prop, PROP_EDITABLE | PROP_HIDDEN);
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_override_flag(prop, PROPOVERRIDE_NO_COMPARISON);
  RNA_def_property_ui_text(prop,
                           "Add Material Correction",
                           "Write-only: assigning a material adds a correction that lays it over "
                           "the active layer; reads as none");
  RNA_def_property_update(
      prop, NC_MATERIAL | ND_SHADING, "rna_Material_paint_layer_add_material_correction_update");

  /* And for a mask correction: the grey of the picked material's Base Color over the active
   * layer's mask. */
  prop = RNA_def_property(srna, "paint_layer_add_material_mask_correction", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "Material");
  RNA_def_property_pointer_funcs(prop,
                                 "rna_Material_paint_layer_add_material_get",
                                 "rna_Material_paint_layer_add_material_set",
                                 nullptr,
                                 "rna_Material_paint_layer_add_material_poll");
  RNA_def_property_flag(prop, PROP_EDITABLE | PROP_HIDDEN);
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_override_flag(prop, PROPOVERRIDE_NO_COMPARISON);
  RNA_def_property_ui_text(prop,
                           "Add Mask Material Correction",
                           "Write-only: assigning a material adds a mask correction that lays the "
                           "grey of its Base Color over the active layer's mask; reads as none");
  RNA_def_property_update(prop,
                          NC_MATERIAL | ND_SHADING,
                          "rna_Material_paint_layer_add_material_mask_correction_update");

  /* And for a mask backed by an image: the ID Browser of the Add Mask menu picks it through this
   * write-only property, and its update makes the mask. */
  prop = RNA_def_property(srna, "paint_layer_add_image_mask", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "Image");
  RNA_def_property_pointer_funcs(prop,
                                 "rna_Material_paint_layer_add_image_mask_get",
                                 "rna_Material_paint_layer_add_image_mask_set",
                                 nullptr,
                                 "rna_Material_paint_layer_add_image_mask_poll");
  RNA_def_property_flag(prop, PROP_EDITABLE | PROP_HIDDEN);
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_override_flag(prop, PROPOVERRIDE_NO_COMPARISON);
  RNA_def_property_ui_text(prop,
                           "Add Image Mask",
                           "Write-only: assigning an image gives the active layer a mask that reads "
                           "it, and makes the mask the paint target; reads as none");
  RNA_def_property_update(
      prop, NC_MATERIAL | ND_SHADING, "rna_Material_paint_layer_add_image_mask_update");

  prop = RNA_def_property(srna, "paint_layers_is_stale", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop, "rna_Material_paint_layers_is_stale_get", nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop,
      "Paint Layers Bake Is Stale",
      "The material's baked paint layer result (image maps, composites, exported passes) does not "
      "match its current layer stack yet: an edit is waiting out its short pause, or a render is "
      "queued or running. Wait for it to become False, or use MATERIAL_OT_paint_layers_bake_now, "
      "before reading a baked result");

  prop = RNA_def_property(srna, "smart_asset_instance", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop, "rna_Material_smart_asset_instance_get", nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop,
                           "Smart Asset Instance",
                           "Whether this material holds rows imported from a Smart asset, with the "
                           "parameters the asset exposes marked on them");

  prop = RNA_def_property(srna, "smart_asset_instances", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_struct_type(prop, "MaterialSmartAssetInstance");
  RNA_def_property_collection_funcs(prop,
                                    "rna_Material_smart_asset_instances_begin",
                                    "rna_iterator_array_next",
                                    "rna_iterator_array_end",
                                    "rna_iterator_array_get",
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop,
      "Smart Asset Instances",
      "The Smart assets dropped into this material, oldest first; each holds its own rows, which "
      "Reload and Make Unique act on separately");

  prop = RNA_def_property(srna, "paint_layers_image_size", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "paint_layers_image_size");
  RNA_def_property_enum_items(prop, rna_enum_material_paint_layers_image_size_items);
  RNA_def_property_enum_default(prop, PAINT_NEW_CHANNEL_IMAGE_SIZE_4K);
  RNA_def_property_ui_text(prop,
                           "Image Size",
                           "Width and height of the maps created for this material's layers, "
                           "masks and fills; existing maps keep their size");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, nullptr);

  prop = RNA_def_property(srna, "paint_layers_composite_device", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "paint_layers_composite_device");
  RNA_def_property_enum_items(prop, rna_enum_material_paint_layers_composite_device_items);
  RNA_def_property_ui_text(prop,
                           "Image Editor Composite",
                           "What shades the Combined and channel previews in the Image Editor");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, nullptr);

  prop = RNA_def_property(srna, "paint_layers_preview_sync", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_negative_sdna(
      prop, nullptr, "paint_layers_preview_flag", MA_PAINT_PREVIEW_DELAYED);
  RNA_def_property_ui_text(
      prop,
      "Sync to Image Editor",
      "While a layer that reads another material is edited, render its maps for the Image "
      "Editor's composite right after each edit. Off: wait for the chosen delay after the last "
      "edit. Leaving the layer renders the maps at their own size either way");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "paint_layers_preview_delay", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "paint_layers_preview_delay");
  RNA_def_property_enum_items(prop, rna_enum_material_paint_layers_preview_delay_items);
  RNA_def_property_ui_text(
      prop, "Preview Delay", "How long after the last edit the Image Editor's maps are rendered");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "paint_layers_preview_size", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "paint_layers_preview_size");
  RNA_def_property_enum_items(prop, rna_enum_material_paint_layers_preview_size_items);
  RNA_def_property_enum_default(prop, MA_PAINT_PREVIEW_SIZE_2048);
  RNA_def_property_ui_text(prop,
                           "Preview Size",
                           "The side the maps are rendered at while a layer is edited. Never "
                           "larger than the layer's own size or the size layers are created with");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

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
      "and the build default. Base Color may be left out; an empty set is ignored");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_paint_layers_channels_update");

  prop = RNA_def_property(srna, "paint_layers_view_pass", PROP_ENUM, PROP_NONE);
  /* Dynamic items: the placeholder marks the property for the item callback below. */
  RNA_def_property_enum_items(prop, rna_enum_dummy_NULL_items);
  RNA_def_property_enum_funcs(prop,
                              "rna_Material_paint_layers_view_pass_get",
                              "rna_Material_paint_layers_view_pass_set",
                              "rna_Material_paint_layers_view_pass_itemf");
  RNA_def_property_ui_text(
      prop,
      "View Pass",
      "The paint pass the 3D Viewport shows this material through while painting, in Material "
      "Preview shading; Combined shows the shaded material. The pass is stored in the material, so "
      "final renders show it too until Combined is selected");
  RNA_def_property_update(
      prop, NC_MATERIAL | ND_SHADING, "rna_Material_paint_layers_view_pass_update");

  prop = RNA_def_property(srna, "paint_layers_bump_strength", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_funcs(prop,
                               "rna_Material_paint_layers_bump_strength_get",
                               "rna_Material_paint_layers_bump_strength_set",
                               nullptr);
  RNA_def_property_range(prop, 0.0f, PAINT_LAYERS_RESPONSE_STRENGTH_MAX);
  RNA_def_property_ui_range(prop, 0.0f, 20.0f, 1.0, 3);
  RNA_def_property_ui_text(
      prop,
      "Bump Strength",
      "The strength of the relief the material's paint Height channel gives; reaches the stack's "
      "own Bump node without regenerating the stack");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "paint_layers_bump_distance", PROP_FLOAT, PROP_DISTANCE);
  RNA_def_property_float_funcs(prop,
                               "rna_Material_paint_layers_bump_distance_get",
                               "rna_Material_paint_layers_bump_distance_set",
                               nullptr);
  RNA_def_property_range(prop, 0.0f, PAINT_LAYERS_RESPONSE_DISTANCE_MAX);
  RNA_def_property_ui_range(prop, 0.0f, 10.0f, 1.0, 3);
  RNA_def_property_ui_text(
      prop,
      "Bump Distance",
      "The distance, in metres, the material's paint Height channel spans; the stack's own Bump "
      "node takes it without regenerating the stack");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "paint_layers_normal_strength", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_funcs(prop,
                               "rna_Material_paint_layers_normal_strength_get",
                               "rna_Material_paint_layers_normal_strength_set",
                               nullptr);
  RNA_def_property_range(prop, 0.0f, PAINT_LAYERS_RESPONSE_STRENGTH_MAX);
  RNA_def_property_ui_range(prop, 0.0f, 4.0f, 1.0, 3);
  RNA_def_property_ui_text(
      prop,
      "Normal Strength",
      "The strength of the normal maps the material's paint Normal channel lays; reaches the "
      "stack's own Normal Map node without regenerating the stack");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  /* Read-only: the nodes come and go with the regeneration, never with an edit here; the response
   * panel polls them to show only the sliders the owned nodes answer. */
  prop = RNA_def_property(srna, "paint_layers_has_bump", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop, "rna_Material_paint_layers_has_bump_get", nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop,
      "Has Bump",
      "The material's own tree carries the Bump node the paint layer stack owns; it appears "
      "when a Height channel is wired into the stack");

  prop = RNA_def_property(srna, "paint_layers_has_normal_map", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop, "rna_Material_paint_layers_has_normal_map_get", nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop,
      "Has Normal Map",
      "The material's own tree carries the Normal Map node the paint layer stack owns; it appears "
      "when a Normal channel is wired into the stack");

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
  rna_def_material_smart_asset_instance(brna);
  rna_def_material_paint_layer_kind_param(brna);
  rna_def_material_paint_layer_id_color(brna);
  rna_def_material_paint_layer_curve_patch(brna);
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
