/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup RNA
 *
 * RNA of the Paint Vector ID. Minimal surface: `bpy.data.paint_vectors` with its item
 * collection and the basic item properties; the geometry accessors are added as the editor layer
 * needs them.
 */

#include <cstdlib>

#include "DNA_paint_vector_types.h"

#include "RNA_define.hh"

#include "rna_internal.hh"

#include "WM_types.hh"

#ifdef RNA_RUNTIME

#  include "BKE_paint_vector.hh"

namespace blender {

static void rna_PaintVector_items_begin(CollectionPropertyIterator *iter, PointerRNA *ptr)
{
  PaintVector *pv = static_cast<PaintVector *>(ptr->data);
  rna_iterator_array_begin(
      iter, ptr, pv->items, sizeof(PaintVectorItem), pv->items_num, false, nullptr);
}

static int rna_PaintVector_items_length(PointerRNA *ptr)
{
  PaintVector *pv = static_cast<PaintVector *>(ptr->data);
  return pv->items_num;
}

/** The per-curve look belongs to Curve Patch curves; a Shape item has no use for it. */
static int rna_PaintVectorItem_curve_look_editable(const PointerRNA *ptr, const char ** /*r_info*/)
{
  const PaintVectorItem *item = static_cast<const PaintVectorItem *>(ptr->data);
  return item->type == PAINT_VECTOR_ITEM_CURVE_PATCH ? PROP_EDITABLE : 0;
}

static void rna_PaintVectorItem_curve_look_update(Main * /*bmain*/,
                                                  Scene * /*scene*/,
                                                  PointerRNA *ptr)
{
  /* The row's bake is invalidated through the document revision. */
  if (PaintVector *pv = id_cast<PaintVector *>(ptr->owner_id)) {
    BKE_paint_vector_tag_changed(*pv);
  }
}

}  // namespace blender

#else

namespace blender {

static const EnumPropertyItem rna_enum_paint_vector_item_type_items[] = {
    {PAINT_VECTOR_ITEM_SHAPE, "SHAPE", 0, "Shape", "A shape drawn by the shape tools"},
    {PAINT_VECTOR_ITEM_CURVE_PATCH,
     "CURVE_PATCH",
     0,
     "Curve Patch",
     "A Curve Patch control curve"},
    {0, nullptr, 0, nullptr, nullptr},
};

static void rna_def_paint_vector_item(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "PaintVectorItem", nullptr);
  RNA_def_struct_ui_text(srna, "Paint Vector Item", "One element of a Paint Vector document");

  prop = RNA_def_property(srna, "type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "type");
  RNA_def_property_enum_items(prop, rna_enum_paint_vector_item_type_items);
  RNA_def_property_ui_text(prop, "Type", "Kind of vector element");
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);

  prop = RNA_def_property(srna, "origin", PROP_FLOAT, PROP_XYZ);
  RNA_def_property_float_sdna(prop, nullptr, "origin");
  RNA_def_property_array(prop, 2);
  RNA_def_property_ui_text(prop, "Origin", "Pivot of the item, in its shape space");

  prop = RNA_def_property(srna, "curve_name", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, nullptr, "curve_name");
  RNA_def_property_ui_text(
      prop, "Curve Name", "List name of a Curve Patch curve, shown in the Active Tool panel");

  prop = RNA_def_property(srna, "hide", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "curve_patch_params.hidden", 1);
  RNA_def_property_ui_text(prop, "Hide", "Skip the curve when stamping; it stays editable");
  RNA_def_property_editable_func(prop, "rna_PaintVectorItem_curve_look_editable");
  RNA_def_property_update(prop, NC_SPACE | ND_SPACE_VIEW3D, "rna_PaintVectorItem_curve_look_update");

  prop = RNA_def_property(srna, "opacity", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "curve_patch_params.opacity");
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_text(prop, "Opacity", "Multiplier on the stamp strength of the curve");
  RNA_def_property_editable_func(prop, "rna_PaintVectorItem_curve_look_editable");
  RNA_def_property_update(prop, NC_SPACE | ND_SPACE_VIEW3D, "rna_PaintVectorItem_curve_look_update");

  /* No unit: the radius is in object space, and a distance unit would scale it with the object. */
  prop = RNA_def_property(srna, "width", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, nullptr, "curve_patch_params.radius");
  RNA_def_property_range(prop, 0.0001f, 1000.0f);
  RNA_def_property_ui_range(prop, 0.001f, 10.0f, 0.01f, 3);
  RNA_def_property_ui_text(
      prop,
      "Width",
      "Radius of the curve's ribbon, in object space. Taken from the brush Size when the curve is "
      "made, then its own");
  RNA_def_property_editable_func(prop, "rna_PaintVectorItem_curve_look_editable");
  RNA_def_property_update(prop, NC_SPACE | ND_SPACE_VIEW3D, "rna_PaintVectorItem_curve_look_update");
}

static void rna_def_paint_vector(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "PaintVector", "ID");
  RNA_def_struct_ui_text(srna, "Paint Vector", "A non-destructive vector document");
  RNA_def_struct_ui_icon(srna, ICON_CURVE_BEZCURVE);

  /* Not "items": that identifier is reserved by Python (dict.items), and makesrna rejects it.
   * The DNA field stays `PaintVector::items`; the iterators below read it directly. */
  prop = RNA_def_property(srna, "elements", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_struct_type(prop, "PaintVectorItem");
  RNA_def_property_collection_funcs(prop,
                                    "rna_PaintVector_items_begin",
                                    "rna_iterator_array_next",
                                    "rna_iterator_array_end",
                                    "rna_iterator_array_get",
                                    "rna_PaintVector_items_length",
                                    nullptr,
                                    nullptr,
                                    nullptr);
  RNA_def_property_ui_text(prop, "Elements", "Elements of the vector document");
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);

  prop = RNA_def_property(srna, "active_item", PROP_INT, PROP_NONE);
  RNA_def_property_int_sdna(prop, nullptr, "active_item");
  RNA_def_property_ui_text(prop, "Active Item", "Index of the item the editor acts on");
  RNA_def_property_update(prop, NC_SPACE | ND_SPACE_VIEW3D, nullptr);

  prop = RNA_def_property(srna, "show_all_curves", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", PAINT_VECTOR_SHOW_ALL_CURVES);
  RNA_def_property_ui_text(
      prop,
      "Show All Curves",
      "Show and pick the curves of the effect that are not selected, instead of only the active "
      "one");
  RNA_def_property_update(prop, NC_SPACE | ND_SPACE_VIEW3D, nullptr);
}

void RNA_def_paint_vector(BlenderRNA *brna)
{
  rna_def_paint_vector_item(brna);
  rna_def_paint_vector(brna);
}

}  // namespace blender

#endif
