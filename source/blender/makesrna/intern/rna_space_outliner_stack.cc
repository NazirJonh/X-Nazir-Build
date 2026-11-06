/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup rna
 *
 * The Outliner's Stack Layers RNA types a Python stack source registers through:
 * #OutlinerStackSource and #OutlinerStackRowBuilder, plus the registration half (refinement, the
 * optional-function table, register/unregister). The SpaceOutliner stack properties and their
 * callbacks stay in rna_space.cc, next to the space's other properties: an RNA property's
 * callbacks have to compile in the translation unit the generated data for that property lives in.
 */

#include <cfloat>
#include <cstring>
#include <iterator>

#include "BLI_index_range.hh"
#include "BLI_string.h"

#include "BLT_translation.hh"

#include "BKE_report.hh"

#include "DNA_space_enums.h"
#include "DNA_space_types.h"

#include "ED_outliner_stack.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "rna_internal.hh"
#include "rna_internal_types.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#ifdef RNA_RUNTIME

namespace blender {

/* A registered instance reads as its subclass: the refinement asks the module which srna the
 * description the pointer wraps was registered for. Unregistered data (the registration's own
 * dummy, a freed source) reads as the base type. */
static StructRNA *rna_OutlinerStackSource_refine(PointerRNA *ptr)
{
  StructRNA *srna = blender::ed::outliner::outliner_stack_source_python_refine(
      static_cast<const StackSourceInfo *>(ptr->data));
  return (srna != nullptr) ? srna : RNA_OutlinerStackSource;
}

/* How many optional functions the #OutlinerStackSource RNA type declares, in definition order:
 * the registration walks them once and hands both the set and the have-function flags to the
 * module. */
constexpr int STACK_SOURCE_FUNC_NUM = 7;

/* One optional function of the base type: its RNA name (what validation and lookup key on) paired
 * with the #StackSourceFunctionSet member it resolves into. The table is in the base type's
 * definition order -- the same order the validate callback filled #have_function in -- so the
 * table entry at position i lines up with #have_function[i], and the loop binds each by name and
 * member rather than by hand-numbered assignments. */
struct StackSourceFuncBinding {
  const char *name;
  FunctionRNA *blender::ed::outliner::StackSourceFunctionSet::*member;
};
constexpr StackSourceFuncBinding stack_source_func_bindings[STACK_SOURCE_FUNC_NUM] = {
    {"has_stack", &blender::ed::outliner::StackSourceFunctionSet::has_stack},
    {"has_stack_key", &blender::ed::outliner::StackSourceFunctionSet::has_stack_key},
    {"owner_get", &blender::ed::outliner::StackSourceFunctionSet::owner_get},
    {"state_hash", &blender::ed::outliner::StackSourceFunctionSet::state_hash},
    {"rows_build", &blender::ed::outliner::StackSourceFunctionSet::rows_build},
    {"row_activate", &blender::ed::outliner::StackSourceFunctionSet::row_activate},
    {"row_is_active", &blender::ed::outliner::StackSourceFunctionSet::row_is_active},
};
static_assert(std::size(stack_source_func_bindings) == STACK_SOURCE_FUNC_NUM,
              "the binding table must name every optional function the registration reads");

/* Defined below; the re-registration dedup inside the register function calls it. */
static bool rna_OutlinerStackSource_unregister(Main *bmain, StructRNA *type);

static StructRNA *rna_OutlinerStackSource_register(Main *bmain,
                                                   ReportList *reports,
                                                   void *data,
                                                   const char * /*identifier*/,
                                                   StructValidateFunc validate,
                                                   StructCallbackFunc call,
                                                   StructFreeFunc free)
{
  const char *error_prefix = RPT_("Registering outliner stack source:");
  StackSourceInfo dummy_info = {};
  /* C++ defaults that differ from a zeroed field, set before the class's attributes are read in:
   * an attribute the class left out is not written by the validation, so without this it would
   * read as zero where the C++ half answers with its own default (a zero #icon_columns would
   * drop the built-in visibility column, whose layout default is one). Keep in step with
   * #StackColumnLayout, and later additions with the C++ base's non-zero defaults. */
  dummy_info.icon_columns = 1;
  bool have_function[STACK_SOURCE_FUNC_NUM];

  /* The row builder pointers handed to Python have to be typed by its RNA type; the first
   * registration teaches the module what that type is. */
  if (blender::ed::outliner::outliner_stack_row_builder_srna() == nullptr) {
    blender::ed::outliner::outliner_stack_row_builder_srna_set(RNA_OutlinerStackRowBuilder);
  }

  PointerRNA dummy_ptr = RNA_pointer_create_discrete(nullptr, RNA_OutlinerStackSource, &dummy_info);
  if (validate(&dummy_ptr, data, have_function) != 0) {
    return nullptr;
  }

  if (dummy_info.idname[0] == '\0') {
    BKE_reportf(reports, RPT_ERROR, "%s 'bl_idname' must be set", error_prefix);
    return nullptr;
  }

  /* A re-registration of the same idname replaces the previous one, the way a re-registered
   * Python class does; the name check below must see the slot free first. */
  if (StructRNA *previous_srna = blender::ed::outliner::outliner_stack_source_python_srna(
          dummy_info.idname))
  {
    rna_OutlinerStackSource_unregister(bmain, previous_srna);
  }

  if (!RNA_struct_available_or_report(reports, dummy_info.idname)) {
    return nullptr;
  }

  /* Resolve the icon name the class gave. An unknown name reads as no icon rather than refusing
   * the registration: the source is still fully usable. */
  if (dummy_info.icon_name[0] != '\0') {
    int icon = 0;
    if (RNA_enum_value_from_id(rna_enum_icon_items, dummy_info.icon_name, &icon)) {
      dummy_info.icon = icon;
    }
    else {
      dummy_info.icon = 0;
      BKE_reportf(reports,
                  RPT_WARNING,
                  "%s '%s': unknown icon '%s'",
                  error_prefix,
                  dummy_info.idname,
                  dummy_info.icon_name);
    }
  }

  /* The functions the subclass answers, in the base type's definition order -- the same order
   * the validation walked the have-function flags in. A function the subclass left out reads as
   * absent, so the source refuses that verb instead of erroring on every ask. */
  blender::ed::outliner::StackSourceFunctionSet funcs;
  for (const int i : IndexRange(STACK_SOURCE_FUNC_NUM)) {
    if (!have_function[i]) {
      continue;
    }
    funcs.*(stack_source_func_bindings[i].member) =
        RNA_struct_find_function(RNA_OutlinerStackSource, stack_source_func_bindings[i].name);
  }

  /* #rows_build is the one function a source cannot do without: a source that describes no rows
   * is not a source. */
  if (funcs.rows_build == nullptr) {
    BKE_reportf(reports,
                RPT_ERROR,
                "%s '%s': a 'rows_build' method is required",
                error_prefix,
                dummy_info.idname);
    return nullptr;
  }

  blender::ed::outliner::StackSourcePython *source =
      blender::ed::outliner::outliner_stack_source_python_new(dummy_info);

  StructRNA *srna = RNA_def_struct_ptr(
      &RNA_blender_rna_get(), dummy_info.idname, RNA_OutlinerStackSource);
  RNA_def_struct_translation_context(srna, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  RNA_def_struct_flag(srna, STRUCT_NO_IDPROPERTIES);

  /* The registration also stores \a source in the srna's blender-type slot, as its base
   * #StackSource: the module owns both sides of that round-trip, and a void* only round-trips to
   * the type it was stored as. */
  blender::ed::outliner::outliner_stack_source_python_register(
      *bmain, source, data, call, free, srna, funcs);

  WM_main_add_notifier(NC_WINDOW, nullptr);
  return srna;
}

static bool rna_OutlinerStackSource_unregister(Main *bmain, StructRNA *type)
{
  return blender::ed::outliner::outliner_stack_source_python_unregister(*bmain, type);
}


/** \} */

}  // namespace blender
#else

namespace blender {

/** The functions of #OutlinerStackSource, in the order the registration's have-function table
 * reads them. Every one is optional except #rows_build; the C half of each lives in the
 * Outliner's stack module, dispatched through the registered subclass's RNA extension. */
static void rna_def_outliner_stack_source(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;
  PropertyRNA *parm;
  FunctionRNA *func;

  srna = RNA_def_struct(brna, "OutlinerStackSource", nullptr);
  RNA_def_struct_ui_text(srna,
                         "Outliner Stack Source",
                         "A kind of layer stack the Outliner's Stack Layers display mode shows: "
                         "register a subclass to add one");
  RNA_def_struct_sdna(srna, "StackSourceInfo");
  RNA_def_struct_refine_func(srna, "rna_OutlinerStackSource_refine");
  RNA_def_struct_flag(srna, STRUCT_PUBLIC_NAMESPACE_INHERIT);
  RNA_def_struct_register_funcs(
      srna, "rna_OutlinerStackSource_register", "rna_OutlinerStackSource_unregister", nullptr);
  RNA_def_struct_translation_context(srna, BLT_I18NCONTEXT_DEFAULT_BPYRNA);

  /* The static description: read from the class once, at registration. */
  prop = RNA_def_property(srna, "bl_idname", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, nullptr, "idname");
  RNA_def_property_flag(prop, PROP_REGISTER);
  RNA_def_property_ui_text(
      prop, "ID Name", "Unique name the source registers under; scripts and files name it by it");

  prop = RNA_def_property(srna, "ui_name", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, nullptr, "ui_name");
  RNA_def_property_flag(prop, PROP_REGISTER_OPTIONAL);
  RNA_def_property_ui_text(prop, "Name", "The name the user reads in the source picker");

  prop = RNA_def_property(srna, "icon", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, nullptr, "icon_name");
  RNA_def_property_flag(prop, PROP_REGISTER_OPTIONAL);
  RNA_def_property_ui_text(prop, "Icon", "Icon shown next to the name, as its identifier");

  prop = RNA_def_property(srna, "value_width", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, nullptr, "value_width");
  RNA_def_property_flag(prop, PROP_REGISTER_OPTIONAL);
  /* A class attribute lands here without passing a Python-side check, so the range is the first
   * guard; #StackSourcePython::column_layout re-checks on every read. */
  RNA_def_property_range(prop, 0.0f, blender::ed::outliner::STACK_SOURCE_COLUMN_WIDTH_MAX);
  RNA_def_property_ui_range(prop, 0.0f, blender::ed::outliner::STACK_SOURCE_COLUMN_WIDTH_MAX, 1, 2);
  RNA_def_property_ui_text(prop,
                           "Value Column Width",
                           "Width of the column that modulates each row, in UI units; zero hides "
                           "it");

  prop = RNA_def_property(srna, "mode_width", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, nullptr, "mode_width");
  RNA_def_property_flag(prop, PROP_REGISTER_OPTIONAL);
  RNA_def_property_range(prop, 0.0f, blender::ed::outliner::STACK_SOURCE_COLUMN_WIDTH_MAX);
  RNA_def_property_ui_range(prop, 0.0f, blender::ed::outliner::STACK_SOURCE_COLUMN_WIDTH_MAX, 1, 2);
  RNA_def_property_ui_text(prop,
                           "Mode Column Width",
                           "Width of the column that says how each row combines, in UI units; "
                           "zero hides it");

  prop = RNA_def_property(srna, "action_width", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, nullptr, "action_width");
  RNA_def_property_flag(prop, PROP_REGISTER_OPTIONAL);
  RNA_def_property_range(prop, 0.0f, blender::ed::outliner::STACK_SOURCE_COLUMN_WIDTH_MAX);
  RNA_def_property_ui_range(prop, 0.0f, blender::ed::outliner::STACK_SOURCE_COLUMN_WIDTH_MAX, 1, 2);
  RNA_def_property_ui_text(
      prop, "Action Column Width", "Width of the per-row action column, in UI units");

  prop = RNA_def_property(srna, "icon_columns", PROP_INT, PROP_NONE);
  RNA_def_property_int_sdna(prop, nullptr, "icon_columns");
  RNA_def_property_flag(prop, PROP_REGISTER_OPTIONAL);
  RNA_def_property_range(prop, 0, blender::ed::outliner::STACK_SOURCE_ICON_COLUMNS_MAX);
  RNA_def_property_ui_text(
      prop, "Icon Columns", "Extra icon columns the rows reserve past the built-in toggles");

  prop = RNA_def_property(srna, "supports_row_lock", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "supports_row_lock", 0);
  RNA_def_property_flag(prop, PROP_REGISTER_OPTIONAL);
  RNA_def_property_ui_text(
      prop, "Supports Row Lock", "Whether the rows carry a painting lock the Options can offer");

  /* The dynamic answers: each one is a Python method of the registered subclass. */
  func = RNA_def_function(srna, "has_stack", nullptr);
  RNA_def_function_ui_description(func, "Whether the object has a stack of this kind to show");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_pointer(func, "object", "Object", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
  RNA_def_function_return(func, RNA_def_boolean(func, "has_stack", false, "", ""));

  func = RNA_def_function(srna, "has_stack_key", nullptr);
  RNA_def_function_ui_description(
      func,
      "A value that changes whenever has_stack would answer differently, so the Outliner can cache "
      "the answer: for a data-tree-backed source the tree's structural revision. Return an "
      "integer-valued float below 2^24 (an RNA float's exact integer range); leave the method out "
      "to key the cache on the object's identity alone");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_pointer(func, "object", "Object", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
  RNA_def_function_return(func, RNA_def_float(func, "key", 0.0f, 0.0f, FLT_MAX, "", "", 0.0f, FLT_MAX));

  func = RNA_def_function(srna, "owner_get", nullptr);
  RNA_def_function_ui_description(
      func, "The data-block whose stack the focus names, or None when there is nothing to show");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_int(func,
                     "object_uid",
                     0,
                     0,
                     INT32_MAX,
                     "Object UID",
                     "Session UID of the object the focus names, or 0 for the active object",
                     0,
                     INT32_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_int(func,
                     "sub_index",
                     -1,
                     -1,
                     INT32_MAX,
                     "Sub-Selection",
                     "Source-defined sub-selection within the object, -1 for the default",
                     -1,
                     INT32_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  RNA_def_function_return(func, RNA_def_pointer(func, "owner", "ID", "", ""));

  func = RNA_def_function(srna, "state_hash", nullptr);
  RNA_def_function_ui_description(
      func,
      "A value that changes whenever rows_build would produce different rows; return an "
      "integer-valued float below 2^24 (an RNA float's exact integer range). Without it the rows "
      "are re-read on every rebuild");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_pointer(func, "owner", "ID", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
  RNA_def_function_return(func, RNA_def_float(func, "hash", -1.0f, -1.0f, FLT_MAX, "", "", 0.0f, FLT_MAX));

  func = RNA_def_function(srna, "rows_build", nullptr);
  RNA_def_function_ui_description(
      func, "Describe the rows of the owner's stack, bottom to top, into the row builder");
  RNA_def_function_flag(func, FUNC_REGISTER);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_pointer(func, "owner", "ID", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
  parm = RNA_def_pointer(func, "rows", "OutlinerStackRowBuilder", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);

  func = RNA_def_function(srna, "row_activate", nullptr);
  RNA_def_function_ui_description(func, "Make the row the thing the rest of Blender acts on");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL | FUNC_ALLOW_WRITE);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_pointer(func, "owner", "ID", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
  parm = RNA_def_int(func, "ordinal", 0, 0, INT16_MAX, "Ordinal", "", 0, INT16_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  RNA_def_function_return(func, RNA_def_boolean(func, "activated", false, "", ""));

  func = RNA_def_function(srna, "row_is_active", nullptr);
  RNA_def_function_ui_description(func, "Whether the row is the stack's active one");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_pointer(func, "owner", "ID", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
  parm = RNA_def_int(func, "ordinal", 0, 0, INT16_MAX, "Ordinal", "", 0, INT16_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  RNA_def_function_return(func, RNA_def_boolean(func, "active", false, "", ""));
}

/** The row builder: the handle a #rows_build or #decorate_row call writes rows through. Created
 * by the Outliner for the duration of the call; never constructed from Python. */
static void rna_def_outliner_stack_row_builder(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *parm;
  FunctionRNA *func;

  srna = RNA_def_struct(brna, "OutlinerStackRowBuilder", nullptr);
  RNA_def_struct_ui_text(srna,
                         "Outliner Stack Row Builder",
                         "Describes the rows of a stack while a source's rows_build runs");
  RNA_def_struct_sdna(srna, "StackRowBuilderData");

  func = RNA_def_function(srna, "add_row", "outliner_stack_row_builder_add_row");
  RNA_def_function_ui_description(
      func,
      "Append a row and return its ordinal. Rows list top first, so a row's children come "
      "before it in the vector: build the tree bottom-up, and name the parent by the ordinal "
      "add_row returned for it");
  parm = RNA_def_string(func, "name", nullptr, MAX_NAME, "Name", "The row's name");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_string(func, "icon", nullptr, 64, "Icon", "Icon identifier, or empty for none");
  /* Optional: the C half defaults it. */
  parm = RNA_def_int(func, "glyph", 0, 0, INT32_MAX, "Glyph", "Material Symbols codepoint, or 0", 0, INT32_MAX);
  parm = RNA_def_int(func, "depth", 0, 0, INT16_MAX, "Depth", "How deeply the row is nested", 0, INT16_MAX);
  parm = RNA_def_int(func,
                     "parent_ordinal",
                     -1,
                     -1,
                     INT16_MAX,
                     "Parent",
                     "Ordinal of the enclosing group row, -1 for a top-level row",
                     -1,
                     INT16_MAX);
  parm = RNA_def_boolean(func, "can_hold_children", false, "", "The row is a group other rows nest in");
  parm = RNA_def_boolean(func, "has_children", false, "", "The group currently holds at least one row");
  parm = RNA_def_boolean(func, "enabled", true, "", "The row is switched on");
  parm = RNA_def_boolean(func, "locked", false, "", "The row is protected from editing");
  parm = RNA_def_boolean(func, "aux_enabled", true, "", "The row's auxiliary content is switched on");
  parm = RNA_def_int(func,
                     "color_tag",
                     0,
                     0,
                     8,
                     "Color Tag",
                     "Collection-palette color for a group, 0 for none",
                     0,
                     8);
  parm = RNA_def_boolean(func, "compact", false, "", "Keep the row at the single-unit height");
  /* Optional: the C half defaults it. */
  parm = RNA_def_string(
      func, "unsupported_reason", nullptr, MAX_NAME, "Unsupported Reason", "Why the row has no controls");
  /* Optional: the C half defaults it. */
  parm = RNA_def_pointer(func, "value", "AnyType", "", "Data the value column edits");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_RNAPTR);
  parm = RNA_def_string(func, "value_prop", nullptr, 64, "Value Property", "RNA property of the value column");
  /* Optional: the C half defaults it. */
  parm = RNA_def_pointer(func, "mode", "AnyType", "", "Data the mode column edits");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_RNAPTR);
  parm = RNA_def_string(func, "mode_prop", nullptr, 64, "Mode Property", "RNA property of the mode column");
  /* Optional: the C half defaults it. */
  parm = RNA_def_boolean(func, "value_inherited", false, "", "The value column shows an inherited default");
  parm = RNA_def_boolean(func, "mode_inherited", false, "", "The mode column shows an inherited default");
  RNA_def_function_return(
      func, RNA_def_int(func, "ordinal", -1, -1, INT16_MAX, "Ordinal", "", -1, INT16_MAX));
}

void RNA_def_space_outliner_stack(BlenderRNA *brna)
{
  rna_def_outliner_stack_source(brna);
  rna_def_outliner_stack_row_builder(brna);
}

}  // namespace blender

#endif /* RNA_RUNTIME */
