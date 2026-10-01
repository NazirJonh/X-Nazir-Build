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
}

void RNA_def_paint_vector(BlenderRNA *brna)
{
  rna_def_paint_vector_item(brna);
  rna_def_paint_vector(brna);
}

}  // namespace blender

#endif
