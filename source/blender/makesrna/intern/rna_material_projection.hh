/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup RNA
 *
 * The projection RNA of a Paint Layers row: the mapping space that switches it on and the frame
 * (center, size, normal+roll, axis, clip) with its controller object.
 *
 * Included by `rna_material.cc` rather than compiled on its own: makesrna emits the callbacks of a
 * property into the generated file of the file that defines it, so the runtime functions below
 * have to be part of that translation unit.
 */

#pragma once

#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_gradient.hh"

#include <cfloat>

#include "BLI_math_constants.h"
#include "BLI_math_vector.h"

#include "BLT_translation.hh"

#include "DNA_material_types.h"
#include "DNA_object_types.h"

#include "DEG_depsgraph.hh"

#include "ED_paint.hh"

#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "rna_internal.hh"

namespace blender {

/* The projection space of a row's maps. UV reads the active UVs; the others project in object or
 * controller space and each has its own frame. */
static const EnumPropertyItem rna_enum_material_paint_layer_mapping_space_items[] = {
    {MA_PAINT_LAYER_MAPPING_SPACE_UV, "UV", 0, "UV", "Project with the stack UV map"},
    {MA_PAINT_LAYER_MAPPING_SPACE_PLANAR,
     "PLANAR",
     0,
     "Planar",
     "Project along the frame's local axis onto its plane"},
    {MA_PAINT_LAYER_MAPPING_SPACE_BOX,
     "BOX",
     0,
     "Box",
     "Project the map onto the six sides of the frame, one map copy per face"},
    {MA_PAINT_LAYER_MAPPING_SPACE_TRIPLANAR,
     "TRIPLANAR",
     0,
     "Triplanar",
     "Blend three projections by the surface normal"},
    {MA_PAINT_LAYER_MAPPING_SPACE_CUBE,
     "CUBE",
     0,
     "Cube",
     "Project the map onto a six-sided cube-cross atlas in one texture"},
    {MA_PAINT_LAYER_MAPPING_SPACE_CYLINDER,
     "CYLINDER",
     0,
     "Cylinder",
     "Wrap the map around the frame's axis cylinder"},
    {MA_PAINT_LAYER_MAPPING_SPACE_SPHERE,
     "SPHERE",
     0,
     "Sphere",
     "Wrap the map around the frame's spherical surface"},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem rna_enum_material_paint_layer_projection_axis_items[] = {
    {MA_PAINT_LAYER_PROJECTION_AXIS_X, "X", 0, "X", "Project along the frame's local X axis"},
    {MA_PAINT_LAYER_PROJECTION_AXIS_Y, "Y", 0, "Y", "Project along the frame's local Y axis"},
    {MA_PAINT_LAYER_PROJECTION_AXIS_Z, "Z", 0, "Z", "Project along the frame's local Z axis"},
    {0, nullptr, 0, nullptr, nullptr},
};

/* Indexed like the atlas table of #BKE_paint_layers_projection_cube_layout_info. */
static const EnumPropertyItem rna_enum_material_paint_layer_projection_cube_layout_items[] = {
    {MA_PAINT_LAYER_PROJECTION_CUBE_CROSS_4X3,
     "CROSS_4X3",
     0,
     "Cross 4x3",
     "Horizontal cross: +Y above, -Y below, -X +Z +X -Z across the middle"},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem rna_enum_material_paint_layer_projection_preset_items[] = {
    {PAINT_LAYER_PROJ_PRESET_DECAL, "DECAL", 0, "Decal", "Clip, depth, aspect, facing fade"},
    {PAINT_LAYER_PROJ_PRESET_FLAT, "FLAT", 0, "Flat", "No clip, no depth, no facing"},
    {PAINT_LAYER_PROJ_PRESET_CUSTOM, "CUSTOM", 0, "Custom", "Flags do not match any preset"},
    {0, nullptr, 0, nullptr, nullptr},
};

#ifdef RNA_RUNTIME

static int rna_MaterialPaintLayer_mapping_space_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  const int16_t space = layer->mapping.space;
  return (space >= MA_PAINT_LAYER_MAPPING_SPACE_UV && space < MA_PAINT_LAYER_MAPPING_SPACE_NUM) ?
             int(space) :
             int(MA_PAINT_LAYER_MAPPING_SPACE_UV);
}

static bool rna_MaterialPaintLayer_projection_uses_axis_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return BKE_paint_layers_projection_mode_info(layer->mapping.space).uses_axis;
}

static bool rna_MaterialPaintLayer_projection_is_volume_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return BKE_paint_layers_projection_mode_info(layer->mapping.space).is_volume;
}

static bool rna_MaterialPaintLayer_projection_uses_blend_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return BKE_paint_layers_projection_mode_info(layer->mapping.space).uses_blend;
}

static bool rna_MaterialPaintLayer_projection_uses_cube_layout_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return BKE_paint_layers_projection_mode_info(layer->mapping.space).uses_cube_layout;
}

static int rna_MaterialPaintLayer_projection_cube_layout_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return int(BKE_paint_layers_projection_cube_layout_get(*layer));
}

static void rna_MaterialPaintLayer_projection_cube_layout_set(PointerRNA *ptr, int value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    BKE_paint_layers_projection_set_cube_layout(*ma, layer, int8_t(value));
  }
}

static void rna_MaterialPaintLayer_mapping_space_set(PointerRNA *ptr, int value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    BKE_paint_layers_projection_set_space(*ma, layer, int16_t(value));
    /* The controller Empty shows the frame kind of the space it controls. The switch above
     * releases the controller when the row goes back to UV, so a surviving one is still in
     * use and can be read. */
    if (Object *controller = layer->projection_object) {
      if (ED_paint_layer_projection_controller_display_type_update(*controller, *layer)) {
        /* The display type is object data: the evaluated copy and the viewport only show the new
         * arrow after the object is tagged and the draw notifier reaches the window. */
        DEG_id_tag_update(&controller->id, ID_RECALC_TRANSFORM);
        WM_main_add_notifier(NC_OBJECT | ND_DRAW, controller);
      }
    }
  }
}

/* Defined below in rna_material.cc: the space switch keeps the material tag the plain update
 * brings, on top of its own tool follow. */
static void rna_Material_update(Main *bmain, Scene *scene, PointerRNA *ptr);

static void rna_MaterialPaintLayer_mapping_space_update(bContext *C, PointerRNA *ptr)
{
  rna_Material_update(nullptr, nullptr, ptr);
  /* A shown row switched into a volume space is the moment the Projection Frame tool comes up
   * with it; every other space restores the tool it replaced, per the shared helper. */
  Material *ma = id_cast<Material *>(ptr->owner_id);
  if (ma != nullptr) {
    ed::sculpt_paint::projection::ED_paint_projection_frame_tool_activate_if_needed(
        C, *ma, static_cast<const MaterialPaintLayer *>(ptr->data));
  }
}

/**
 * A row with a map may switch into any projection; once projecting it may always switch back to
 * UV, even after the map it projected is gone.
 */
static int rna_MaterialPaintLayer_mapping_space_editable(const PointerRNA *ptr,
                                                         const char **r_info)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  PointerRNA ptr_mutable = *ptr;
  Material *ma = rna_paint_layer_material(&ptr_mutable, layer);
  if (ma == nullptr) {
    return 0;
  }
  if (layer->mapping.space != MA_PAINT_LAYER_MAPPING_SPACE_UV ||
      BKE_paint_layers_projection_supported(*ma, *layer))
  {
    return PROP_EDITABLE;
  }
  if (r_info != nullptr) {
    if (BKE_paint_layers_gradient_supported(*layer)) {
      if (layer->role == MA_PAINT_LAYER_ROLE_MASK_ITEM) {
        *r_info = N_("A gradient mask item reads the projection frame of its row");
      }
      else if (layer->role == MA_PAINT_LAYER_ROLE_LAYER) {
        *r_info = N_(
            "A Gradient projects in UV space only: switch the gradient's space to UV first");
      }
      else {
        *r_info = N_("A Gradient correction keeps its own coordinates");
      }
    }
    else {
      *r_info = N_("Row reads no repeatable map; assign an image to a Fill first");
    }
  }
  return 0;
}

/* The stored contrast is an offset from 1 (0 is identity), so a file written before the option
 * reads as it always did; the property shows the actual multiplier. */
static float rna_MaterialPaintLayer_mask_contrast_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return BKE_paint_layers_mask_contrast_get(*layer);
}

static void rna_MaterialPaintLayer_mask_contrast_set(PointerRNA *ptr, float value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    BKE_paint_layers_mask_contrast_set(*ma, layer, value);
  }
}

static bool rna_MaterialPaintLayer_projection_supported_get(PointerRNA *ptr)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  Material *ma = rna_paint_layer_material(ptr, layer);
  return ma != nullptr && BKE_paint_layers_projection_supported(*ma, *layer);
}

static bool rna_MaterialPaintLayer_projection_applies_get(PointerRNA *ptr)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  Material *ma = rna_paint_layer_material(ptr, layer);
  return ma != nullptr && BKE_paint_layers_projection_applies(*ma, *layer);
}

static int rna_MaterialPaintLayer_projection_editable(const PointerRNA *ptr,
                                                      const char ** /*r_info*/)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return (layer->projection != nullptr &&
          layer->mapping.space != MA_PAINT_LAYER_MAPPING_SPACE_UV) ?
             PROP_EDITABLE :
             0;
}

static void rna_MaterialPaintLayer_projection_center_get(PointerRNA *ptr, float *value)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  if (layer->projection != nullptr) {
    copy_v3_v3(value, layer->projection->center);
  }
  else {
    zero_v3(value);
  }
}

static void rna_MaterialPaintLayer_projection_center_set(PointerRNA *ptr, const float *value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    BKE_paint_layers_projection_set_center(*ma, layer, value);
  }
}

static void rna_MaterialPaintLayer_projection_size_get(PointerRNA *ptr, float *value)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  for (int i = 0; i < 3; i++) {
    value[i] = (layer->projection != nullptr) ?
                   BKE_paint_layers_projection_size_normalize(layer->projection->size[i]) :
                   1.0f;
  }
}

static void rna_MaterialPaintLayer_projection_size_set(PointerRNA *ptr, const float *value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    BKE_paint_layers_projection_set_size(*ma, layer, value);
  }
}

static void rna_MaterialPaintLayer_projection_rotation_get(PointerRNA *ptr, float *value)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  if (layer->projection != nullptr) {
    float normal[3];
    BKE_paint_layers_projection_normal_get(*layer, normal);
    BKE_paint_layers_projection_frame_euler(normal, layer->projection->roll, value);
  }
  else {
    zero_v3(value);
  }
}

static void rna_MaterialPaintLayer_projection_rotation_set(PointerRNA *ptr, const float *value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    BKE_paint_layers_projection_set_rotation(*ma, layer, value);
  }
}

static float rna_MaterialPaintLayer_projection_roll_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return (layer->projection != nullptr) ? layer->projection->roll : 0.0f;
}

static void rna_MaterialPaintLayer_projection_roll_set(PointerRNA *ptr, float value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    BKE_paint_layers_projection_set_roll(*ma, layer, value);
  }
}

static float rna_MaterialPaintLayer_projection_blend_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return (layer->projection != nullptr) ? layer->projection->blend : 0.0f;
}

static void rna_MaterialPaintLayer_projection_blend_set(PointerRNA *ptr, float value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    BKE_paint_layers_projection_set_blend(*ma, layer, value);
  }
}

static float rna_MaterialPaintLayer_projection_depth_size_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  if (layer->projection != nullptr) {
    return BKE_paint_layers_projection_size_normalize(layer->projection->size[2]);
  }
  return 1.0f;
}

static void rna_MaterialPaintLayer_projection_depth_size_set(PointerRNA *ptr, float value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    if (layer->projection != nullptr) {
      float size[3];
      copy_v3_v3(size, layer->projection->size);
      size[2] = value;
      BKE_paint_layers_projection_set_size(*ma, layer, size);
    }
  }
}

static int rna_MaterialPaintLayer_projection_preset_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return int(BKE_paint_layers_projection_preset_get(*layer));
}

static void rna_MaterialPaintLayer_projection_preset_set(PointerRNA *ptr, int value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    BKE_paint_layers_projection_apply_preset(*ma, layer, ePaintLayerProjectionPreset(value));
  }
}

static bool rna_MaterialPaintLayer_projection_clip_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return BKE_paint_layers_projection_clip_get(*layer);
}

static void rna_MaterialPaintLayer_projection_clip_set(PointerRNA *ptr, bool value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    BKE_paint_layers_projection_set_clip(*ma, layer, value);
  }
}

static bool rna_MaterialPaintLayer_projection_depth_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return BKE_paint_layers_projection_depth_get(*layer);
}

static void rna_MaterialPaintLayer_projection_depth_set(PointerRNA *ptr, bool value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    BKE_paint_layers_projection_set_depth(*ma, layer, value);
  }
}

static bool rna_MaterialPaintLayer_projection_facing_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return BKE_paint_layers_projection_facing_get(*layer);
}

static void rna_MaterialPaintLayer_projection_facing_set(PointerRNA *ptr, bool value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    BKE_paint_layers_projection_set_facing(*ma, layer, value);
  }
}

static bool rna_MaterialPaintLayer_projection_facing_invert_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return BKE_paint_layers_projection_facing_invert_get(*layer);
}

static void rna_MaterialPaintLayer_projection_facing_invert_set(PointerRNA *ptr, bool value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    BKE_paint_layers_projection_set_facing_invert(*ma, layer, value);
  }
}

static bool rna_MaterialPaintLayer_projection_fit_aspect_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return BKE_paint_layers_projection_fit_aspect_get(*layer);
}

static void rna_MaterialPaintLayer_projection_fit_aspect_set(PointerRNA *ptr, bool value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    BKE_paint_layers_projection_set_fit_aspect(*ma, layer, value);
  }
}

static bool rna_MaterialPaintLayer_projection_snap_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return BKE_paint_layers_projection_snap_get(*layer);
}

static void rna_MaterialPaintLayer_projection_snap_set(PointerRNA *ptr, bool value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    if (BKE_paint_layers_projection_set_snap(*ma, layer, value)) {
      /* The constraint lives on the controller object, so the editor owns it; the flag alone is the
       * BKE half. A frame with no controller has nothing to snap until one is assigned. */
      if (layer->projection_object != nullptr) {
        ED_paint_layer_projection_controller_snap_update(*layer->projection_object, *layer);
      }
    }
  }
}

static bool rna_MaterialPaintLayer_projection_size_from_controller_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return BKE_paint_layers_projection_size_from_controller_get(*layer);
}

static void rna_MaterialPaintLayer_projection_size_from_controller_set(PointerRNA *ptr,
                                                                      bool value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    BKE_paint_layers_projection_set_size_from_controller(*ma, layer, value);
  }
}

static float rna_MaterialPaintLayer_projection_angle_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  /* The payload keeps degrees; PROP_ANGLE reads and writes radians. */
  return DEG2RADF(BKE_paint_layers_projection_angle_get(*layer));
}

static void rna_MaterialPaintLayer_projection_angle_set(PointerRNA *ptr, float value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    BKE_paint_layers_projection_set_angle(*ma, layer, RAD2DEGF(value));
  }
}

static int rna_MaterialPaintLayer_projection_axis_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  return BKE_paint_layers_projection_axis_get(*layer);
}

static void rna_MaterialPaintLayer_projection_axis_set(PointerRNA *ptr, int value)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    BKE_paint_layers_projection_set_axis(*ma, layer, int8_t(value));
  }
}

static PointerRNA rna_MaterialPaintLayer_projection_object_get(PointerRNA *ptr)
{
  const MaterialPaintLayer *layer = static_cast<const MaterialPaintLayer *>(ptr->data);
  if (layer->projection_object == nullptr) {
    return PointerRNA_NULL;
  }
  return RNA_id_pointer_create(&layer->projection_object->id);
}

static void rna_MaterialPaintLayer_projection_object_set(PointerRNA *ptr,
                                                         PointerRNA value,
                                                         ReportList * /*reports*/)
{
  MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(ptr->data);
  if (Material *ma = rna_paint_layer_material(ptr, layer)) {
    if (BKE_paint_layers_projection_set_object(*ma, layer, static_cast<Object *>(value.data)) &&
        layer->projection_object != nullptr)
    {
      /* A new controller starts with no constraint: give it the one the flag asks for, as the Add
       * operator would. */
      ED_paint_layer_projection_controller_snap_update(*layer->projection_object, *layer);
    }
  }
}

#else

void rna_def_material_paint_layer_projection(StructRNA *srna)
{
  PropertyRNA *prop;

  /* The space the row reads its maps in: UV, or a projection whose frame is edited below. */
  prop = RNA_def_property(srna, "mapping_space", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_material_paint_layer_mapping_space_items);
  RNA_def_property_enum_funcs(prop,
                              "rna_MaterialPaintLayer_mapping_space_get",
                              "rna_MaterialPaintLayer_mapping_space_set",
                              nullptr);
  RNA_def_property_editable_func(prop, "rna_MaterialPaintLayer_mapping_space_editable");
  RNA_def_property_ui_text(
      prop, "Mapping Space", "Projection the row's map lookup works in");
  /* The update callback switches tools, it needs the context. */
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_update(
      prop, NC_MATERIAL | ND_SHADING, "rna_MaterialPaintLayer_mapping_space_update");

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

  /* Mirror: fold the maps around mirror_center. Read-only predicate first, then the axes and the
   * center. The fold reads the map through |uv - center| + center per folded axis, so what either
   * side shows, both sides show, and painting one side updates the other in real time through the
   * generated graph and the GPU composite. */
  prop = RNA_def_property(srna, "mapping_mirror_applies", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(
      prop, "rna_MaterialPaintLayer_mapping_mirror_applies_get", nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop, "Mirror Applies", "Whether the enabled Mirror fold reaches what the row shows");

  prop = RNA_def_property(srna, "mapping_mirror_enabled", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop,
                                 "rna_MaterialPaintLayer_mapping_mirror_enabled_get",
                                 "rna_MaterialPaintLayer_mapping_mirror_enabled_set");
  RNA_def_property_editable_func(prop, "rna_MaterialPaintLayer_mapping_mirror_editable");
  RNA_def_property_ui_text(
      prop, "Mirror", "Switch the Mirror fold on or off; its axes and center are kept");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "mapping_mirror_u", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop,
                                 "rna_MaterialPaintLayer_mapping_mirror_u_get",
                                 "rna_MaterialPaintLayer_mapping_mirror_u_set");
  RNA_def_property_editable_func(prop, "rna_MaterialPaintLayer_mapping_mirror_editable");
  RNA_def_property_ui_text(prop,
                           "Mirror U",
                           "Fold the row's maps along U: both sides of the center show the same");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "mapping_mirror_v", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop,
                                 "rna_MaterialPaintLayer_mapping_mirror_v_get",
                                 "rna_MaterialPaintLayer_mapping_mirror_v_set");
  RNA_def_property_editable_func(prop, "rna_MaterialPaintLayer_mapping_mirror_editable");
  RNA_def_property_ui_text(prop,
                           "Mirror V",
                           "Fold the row's maps along V: both sides of the center show the same");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "mapping_mirror_center", PROP_FLOAT, PROP_NONE);
  RNA_def_property_array(prop, 2);
  RNA_def_property_float_funcs(prop,
                               "rna_MaterialPaintLayer_mapping_mirror_center_get",
                               "rna_MaterialPaintLayer_mapping_mirror_center_set",
                               nullptr);
  RNA_def_property_editable_func(prop,
                                 "rna_MaterialPaintLayer_mapping_mirror_center_editable");
  {
    /* The center defaults to the tile's middle (0.5, 0.5), where the fold is immediately visible. */
    static const float mapping_mirror_center_default[2] = {0.5f, 0.5f};
    RNA_def_property_float_array_default(prop, mapping_mirror_center_default);
  }
  RNA_def_property_ui_text(
      prop,
      "Mirror Center",
      "The UV point the Mirror fold reflects around on every folded axis");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  /* The pivot of a Transform Mapping effect: scale and rotation turn about it, and moving it
   * leaves what the effect shows where it is. Stored in the mirror center, which that kind does
   * not use. */
  prop = RNA_def_property(srna, "mapping_pivot", PROP_FLOAT, PROP_NONE);
  RNA_def_property_array(prop, 2);
  RNA_def_property_float_funcs(prop,
                               "rna_MaterialPaintLayer_mapping_pivot_get",
                               "rna_MaterialPaintLayer_mapping_pivot_set",
                               nullptr);
  RNA_def_property_editable_func(prop, "rna_MaterialPaintLayer_mapping_pivot_editable");
  RNA_def_property_ui_text(prop,
                           "Pivot",
                           "The UV point the Transform Mapping scales and rotates about");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  /* Read-only: whether the row holds a map a projection could read at all. */
  prop = RNA_def_property(srna, "projection_supported", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop, "rna_MaterialPaintLayer_projection_supported_get", nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop, "Projection Supported", "Whether the row reads a map a projection can move");

  /* Read-only: whether a projection is active right now. */
  prop = RNA_def_property(srna, "projection_applies", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop, "rna_MaterialPaintLayer_projection_applies_get", nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop, "Projection Applies", "Whether the row's maps are read through the projection");

  prop = RNA_def_property(srna, "projection_center", PROP_FLOAT, PROP_TRANSLATION);
  RNA_def_property_array(prop, 3);
  RNA_def_property_float_funcs(prop,
                               "rna_MaterialPaintLayer_projection_center_get",
                               "rna_MaterialPaintLayer_projection_center_set",
                               nullptr);
  RNA_def_property_editable_func(prop, "rna_MaterialPaintLayer_projection_editable");
  RNA_def_property_ui_text(prop, "Projection Center", "Frame centre in the controller's space");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "projection_size", PROP_FLOAT, PROP_XYZ);
  RNA_def_property_array(prop, 3);
  RNA_def_property_float_funcs(prop,
                               "rna_MaterialPaintLayer_projection_size_get",
                               "rna_MaterialPaintLayer_projection_size_set",
                               nullptr);
  RNA_def_property_editable_func(prop, "rna_MaterialPaintLayer_projection_editable");
  RNA_def_property_range(prop, -FLT_MAX, FLT_MAX);
  {
    static const float projection_size_default[3] = {1.0f, 1.0f, 1.0f};
    RNA_def_property_float_array_default(prop, projection_size_default);
  }
  RNA_def_property_ui_text(prop,
                           "Projection Size",
                           "Full extents of the projection frame in the controller's space, so "
                           "the controller's own scale multiplies them");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "projection_rotation", PROP_FLOAT, PROP_EULER);
  RNA_def_property_array(prop, 3);
  RNA_def_property_float_funcs(prop,
                               "rna_MaterialPaintLayer_projection_rotation_get",
                               "rna_MaterialPaintLayer_projection_rotation_set",
                               nullptr);
  RNA_def_property_editable_func(prop, "rna_MaterialPaintLayer_projection_editable");
  RNA_def_property_ui_text(prop,
                           "Projection Rotation",
                           "Frame rotation, XYZ Euler, in the controller's space (script wrapper; "
                           "use projection_roll for the interactive UI)");
  /* The frame is stored as normal+roll: an Euler written here is decomposed and reads back as an
   * equivalent triple, not the same angles, so an F-Curve on its channels would not interpolate
   * what its keys say. */
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "projection_roll", PROP_FLOAT, PROP_ANGLE);
  RNA_def_property_float_funcs(prop,
                               "rna_MaterialPaintLayer_projection_roll_get",
                               "rna_MaterialPaintLayer_projection_roll_set",
                               nullptr);
  RNA_def_property_editable_func(prop, "rna_MaterialPaintLayer_projection_editable");
  RNA_def_property_range(prop, -float(M_PI), float(M_PI));
  RNA_def_property_ui_text(prop, "Roll", "Rotation of the frame around the projection normal");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "projection_blend", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_funcs(prop,
                               "rna_MaterialPaintLayer_projection_blend_get",
                               "rna_MaterialPaintLayer_projection_blend_set",
                               nullptr);
  RNA_def_property_editable_func(prop, "rna_MaterialPaintLayer_projection_editable");
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_text(prop,
                           "Normal Blend",
                           "Triplanar face blend: 0 hard faces, 1 the softest normal blend (Box "
                           "keeps hard faces)");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "projection_cube_layout", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_material_paint_layer_projection_cube_layout_items);
  RNA_def_property_enum_funcs(prop,
                              "rna_MaterialPaintLayer_projection_cube_layout_get",
                              "rna_MaterialPaintLayer_projection_cube_layout_set",
                              nullptr);
  RNA_def_property_editable_func(prop, "rna_MaterialPaintLayer_projection_editable");
  RNA_def_property_ui_text(prop, "Cube Layout", "How the six faces sit in the Cube atlas");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "projection_depth_size", PROP_FLOAT, PROP_DISTANCE);
  RNA_def_property_float_funcs(prop,
                               "rna_MaterialPaintLayer_projection_depth_size_get",
                               "rna_MaterialPaintLayer_projection_depth_size_set",
                               nullptr);
  RNA_def_property_editable_func(prop, "rna_MaterialPaintLayer_projection_editable");
  RNA_def_property_range(prop, PAINT_LAYER_PROJECTION_SIZE_MIN, FLT_MAX);
  RNA_def_property_ui_text(
      prop,
      "Depth",
      "Extent of the projection slab along the frame Z axis (the projection direction)");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "projection_preset", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_material_paint_layer_projection_preset_items);
  RNA_def_property_enum_funcs(prop,
                              "rna_MaterialPaintLayer_projection_preset_get",
                              "rna_MaterialPaintLayer_projection_preset_set",
                              nullptr);
  RNA_def_property_editable_func(prop, "rna_MaterialPaintLayer_projection_editable");
  RNA_def_property_ui_text(prop, "Preset", "Decal or Flat preset for the projection flags");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "projection_clip", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop,
                                 "rna_MaterialPaintLayer_projection_clip_get",
                                 "rna_MaterialPaintLayer_projection_clip_set");
  RNA_def_property_editable_func(prop, "rna_MaterialPaintLayer_projection_editable");
  RNA_def_property_ui_text(prop,
                           "Clip to Frame",
                           "Hide the projection outside its frame instead of repeating it");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "projection_depth", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop,
                                 "rna_MaterialPaintLayer_projection_depth_get",
                                 "rna_MaterialPaintLayer_projection_depth_set");
  RNA_def_property_editable_func(prop, "rna_MaterialPaintLayer_projection_editable");
  RNA_def_property_ui_text(
      prop,
      "Limit Depth",
      "Limit the projection to a slab along its axis, of the frame's size on it; off lets it cut "
      "through the object no matter how deep");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "projection_facing", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop,
                                 "rna_MaterialPaintLayer_projection_facing_get",
                                 "rna_MaterialPaintLayer_projection_facing_set");
  RNA_def_property_editable_func(prop, "rna_MaterialPaintLayer_projection_editable");
  RNA_def_property_ui_text(
      prop,
      "Facing Fade",
      "Fade the projection out on faces that look away from the frame's +Z axis, so it does not "
      "show through on the far side or stretch over faces parallel to it");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "projection_facing_invert", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop,
                                 "rna_MaterialPaintLayer_projection_facing_invert_get",
                                 "rna_MaterialPaintLayer_projection_facing_invert_set");
  RNA_def_property_editable_func(prop, "rna_MaterialPaintLayer_projection_editable");
  RNA_def_property_ui_text(
      prop,
      "Flip Facing",
      "Keeps the faces that look away from the frame's +Z axis instead of the ones that look "
      "toward it");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "projection_snap", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop,
                                 "rna_MaterialPaintLayer_projection_snap_get",
                                 "rna_MaterialPaintLayer_projection_snap_set");
  RNA_def_property_editable_func(prop, "rna_MaterialPaintLayer_projection_editable");
  RNA_def_property_ui_text(
      prop,
      "Snap to Surface",
      "Keep the controller on the painted object's surface with a Shrinkwrap constraint, so the "
      "frame follows the surface it lands on");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "projection_angle", PROP_FLOAT, PROP_ANGLE);
  RNA_def_property_float_funcs(prop,
                               "rna_MaterialPaintLayer_projection_angle_get",
                               "rna_MaterialPaintLayer_projection_angle_set",
                               nullptr);
  RNA_def_property_range(prop, DEG2RADF(1.0f), DEG2RADF(180.0f));
  RNA_def_property_ui_text(
      prop,
      "Facing Angle",
      "Angle from the axis where the facing fade reaches zero; the fade stays full 10 degrees "
      "below it");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "projection_fit_aspect", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop,
                                 "rna_MaterialPaintLayer_projection_fit_aspect_get",
                                 "rna_MaterialPaintLayer_projection_fit_aspect_set");
  RNA_def_property_editable_func(prop, "rna_MaterialPaintLayer_projection_editable");
  RNA_def_property_ui_text(
      prop,
      "Fit Image Aspect",
      "Scale the frame's footprint to the row's image aspect, so a non-square image is not "
      "stretched over the surface");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "projection_size_from_controller", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop,
                                 "rna_MaterialPaintLayer_projection_size_from_controller_get",
                                 "rna_MaterialPaintLayer_projection_size_from_controller_set");
  RNA_def_property_editable_func(prop, "rna_MaterialPaintLayer_projection_editable");
  RNA_def_property_ui_text(
      prop,
      "Size from Empty",
      "Read the frame's extents as one and take its scale from the controller's own transform, so "
      "the Empty is the one place the frame is sized; the inline size is kept but ignored");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "projection_axis", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_material_paint_layer_projection_axis_items);
  RNA_def_property_enum_funcs(prop,
                              "rna_MaterialPaintLayer_projection_axis_get",
                              nullptr,
                              nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop, "Projection Axis", "Fixed to Z; the frame normal sets the projection direction");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");

  prop = RNA_def_property(srna, "projection_uses_axis", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_boolean_funcs(prop, "rna_MaterialPaintLayer_projection_uses_axis_get", nullptr);
  RNA_def_property_ui_text(
      prop, "Uses Axis", "The row's projection space runs along a local axis of the frame");

  prop = RNA_def_property(srna, "projection_is_volume", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_boolean_funcs(prop, "rna_MaterialPaintLayer_projection_is_volume_get", nullptr);
  RNA_def_property_ui_text(
      prop, "Is Volume", "The row's projection space fills the frame's whole extents");

  prop = RNA_def_property(srna, "projection_uses_blend", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_boolean_funcs(prop, "rna_MaterialPaintLayer_projection_uses_blend_get", nullptr);
  RNA_def_property_ui_text(
      prop, "Uses Blend", "The row's projection space blends its faces by the surface normal");

  prop = RNA_def_property(srna, "projection_uses_cube_layout", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_boolean_funcs(
      prop, "rna_MaterialPaintLayer_projection_uses_cube_layout_get", nullptr);
  RNA_def_property_ui_text(
      prop, "Uses Cube Layout", "The row's projection space reads a cube atlas");

  /* The optional controller object: its own transform places, turns and scales the frame, and the
   * inline values above are applied inside its space. */
  prop = RNA_def_property(srna, "projection_object", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "Object");
  RNA_def_property_pointer_funcs(prop,
                                 "rna_MaterialPaintLayer_projection_object_get",
                                 "rna_MaterialPaintLayer_projection_object_set",
                                 nullptr,
                                 nullptr);
  RNA_def_property_editable_func(prop, "rna_MaterialPaintLayer_projection_editable");
  /* The setter takes and releases a user itself (BKE_paint_layers_projection_set_object); the flag
   * tells RNA that is the contract, and a mismatch would fail the user-count asserts. */
  RNA_def_property_flag(prop, PROP_EDITABLE | PROP_ID_REFCOUNT);
  RNA_def_property_ui_text(
      prop,
      "Projection Object",
      "Object whose space the frame lives in, usually an Empty; empty uses the painted object. "
      "The controller's own transform places, turns and scales the whole frame, so its scale "
      "multiplies the inline size");
  RNA_def_property_update(prop, NC_MATERIAL | ND_SHADING, "rna_Material_update");
}

#endif

}  // namespace blender


