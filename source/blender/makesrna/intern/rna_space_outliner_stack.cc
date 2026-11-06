/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup rna
 *
 * The Outliner's Stack Layers RNA types a Python stack source registers through:
 * #OutlinerStackSource, #OutlinerStackRowBuilder and #OutlinerStackRowDecorator, plus the
 * registration half (refinement, the optional-function table, register/unregister). The
 * SpaceOutliner stack properties and their callbacks stay in rna_space.cc, next to the space's
 * other properties: an RNA property's callbacks have to compile in the translation unit the
 * generated data for that property lives in.
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

static StructRNA *rna_OutlinerStackRowDecorator_refine(PointerRNA *ptr)
{
  StructRNA *srna = blender::ed::outliner::outliner_stack_decorator_python_refine(
      static_cast<const StackDecoratorInfo *>(ptr->data));
  return (srna != nullptr) ? srna : RNA_OutlinerStackRowDecorator;
}

/* The optional functions the #OutlinerStackSource RNA type declares, in definition order, are the
 * #STACK_SOURCE_FUNCTION_LIST in ED_outliner_stack.hh: the registration walks them once and hands
 * both the set and the have-function flags to the module. */
constexpr int STACK_SOURCE_FUNC_NUM = blender::ed::outliner::STACK_SOURCE_FUNCTION_NUM;

/* One optional function of the base type: its RNA name (what validation and lookup key on) paired
 * with the #StackSourceFunctionSet member it resolves into. The table is in the base type's
 * definition order -- the same order the validate callback filled #have_function in -- so the
 * table entry at position i lines up with #have_function[i], and the loop binds each by name and
 * member rather than by hand-numbered assignments. */
struct StackSourceFuncBinding {
  const char *name;
  FunctionRNA *blender::ed::outliner::StackSourceFunctionSet::*member;
};
constexpr StackSourceFuncBinding stack_source_func_bindings[] = {
#define STACK_SOURCE_FUNC_BINDING(name) \
  {STRINGIFY(name), &blender::ed::outliner::StackSourceFunctionSet::name},
    STACK_SOURCE_FUNCTION_LIST(STACK_SOURCE_FUNC_BINDING)
#undef STACK_SOURCE_FUNC_BINDING
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
  /* #StackSource::use_sub_selection answers true by default (the built-ins' material picker). */
  dummy_info.use_sub_selection = 1;
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

  if (blender::ed::outliner::outliner_stack_source_idname_reserved(dummy_info.idname)) {
    BKE_reportf(reports,
                RPT_ERROR,
                "%s '%s' is the name of a built-in stack source",
                error_prefix,
                dummy_info.idname);
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

  /* Resolve the Focus Stack button's icon the same way: an unknown name reads as no override,
   * and the source's default answers. */
  dummy_info.focus_button_icon_id = 0;
  if (dummy_info.focus_button_icon[0] != '\0') {
    int icon = 0;
    if (RNA_enum_value_from_id(rna_enum_icon_items, dummy_info.focus_button_icon, &icon)) {
      dummy_info.focus_button_icon_id = icon;
    }
    else {
      BKE_reportf(reports,
                  RPT_WARNING,
                  "%s '%s': unknown focus button icon '%s'",
                  error_prefix,
                  dummy_info.idname,
                  dummy_info.focus_button_icon);
    }
  }

  /* Resolve the Focus Stack mode the class named the same way the icon is, and store the value so
   * the C side reads it back without re-parsing the string. An empty or unknown name leaves the -1
   * sentinel (no mode), so focusing keeps the object's current mode. */
  dummy_info.focus_object_mode_id = -1;
  if (dummy_info.focus_object_mode[0] != '\0') {
    int mode = 0;
    if (RNA_enum_value_from_id(rna_enum_object_mode_items,
                               dummy_info.focus_object_mode,
                               &mode))
    {
      dummy_info.focus_object_mode_id = mode;
    }
    else {
      BKE_reportf(reports,
                  RPT_WARNING,
                  "%s '%s': unknown focus object mode '%s'",
                  error_prefix,
                  dummy_info.idname,
                  dummy_info.focus_object_mode);
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
    /* The class defined the method, so the base type must hold the function its table entry
     * names: a null here means the list and the rna_def definitions drifted apart (a renamed or
     * missing definition), which would otherwise read as a silently refused verb. */
    BLI_assert(funcs.*(stack_source_func_bindings[i].member) != nullptr);
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

  /* The srna keeps the identifier pointer rather than a copy, so it names the module's own copy of
   * the idname (alive until unregister frees both), never the stack-local #dummy_info: that
   * dangled once this function returned and left a garbage struct name in `bpy.types`. */
  StructRNA *srna = RNA_def_struct_ptr(
      &RNA_blender_rna_get(),
      blender::ed::outliner::outliner_stack_source_python_idname(*source),
      RNA_OutlinerStackSource);
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

constexpr int STACK_DECORATOR_FUNC_NUM = 1;

static StructRNA *rna_OutlinerStackRowDecorator_register(Main *bmain,
                                                         ReportList *reports,
                                                         void *data,
                                                         const char * /*identifier*/,
                                                         StructValidateFunc validate,
                                                         StructCallbackFunc call,
                                                         StructFreeFunc free)
{
  const char *error_prefix = RPT_("Registering outliner stack row decorator:");
  StackDecoratorInfo dummy_info = {};
  bool have_function[STACK_DECORATOR_FUNC_NUM];

  if (blender::ed::outliner::outliner_stack_row_builder_srna() == nullptr) {
    blender::ed::outliner::outliner_stack_row_builder_srna_set(RNA_OutlinerStackRowBuilder);
  }

  PointerRNA dummy_ptr = RNA_pointer_create_discrete(nullptr,
                                                     RNA_OutlinerStackRowDecorator,
                                                     &dummy_info);
  if (validate(&dummy_ptr, data, have_function) != 0) {
    return nullptr;
  }

  if (dummy_info.idname[0] == '\0') {
    BKE_reportf(reports, RPT_ERROR, "%s 'bl_idname' must be set", error_prefix);
    return nullptr;
  }
  if (dummy_info.source[0] == '\0') {
    STRNCPY(dummy_info.source, "ANY");
  }

  /* A re-registration of the same idname replaces the previous one; the name check below must
   * see the slot free first. */
  blender::ed::outliner::outliner_stack_decorator_unregister_by_name(*bmain, dummy_info.idname);

  if (!RNA_struct_available_or_report(reports, dummy_info.idname)) {
    return nullptr;
  }

  blender::ed::outliner::StackRowDecoratorPython *decorator =
      blender::ed::outliner::outliner_stack_decorator_new(dummy_info);

  /* Same as for the source: the srna keeps the pointer, so it names the module's copy. */
  StructRNA *srna = RNA_def_struct_ptr(
      &RNA_blender_rna_get(),
      blender::ed::outliner::outliner_stack_decorator_idname(*decorator),
      RNA_OutlinerStackRowDecorator);
  RNA_def_struct_translation_context(srna, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  RNA_def_struct_flag(srna, STRUCT_NO_IDPROPERTIES);
  /* The slot carries the module's own half as a #StackRowDecoratorPython -- the exact type the
   * unregister path reads back through #RNA_struct_blender_type_get. The decorator has no base
   * class, so there is no offset to keep honest; the point of naming the type on both sides is
   * that a void* only round-trips to the type it was stored as. */
  RNA_struct_blender_type_set(srna, decorator);

  blender::ed::outliner::outliner_stack_decorator_register(
      *bmain,
      decorator,
      data,
      call,
      free,
      srna,
      RNA_struct_find_function(RNA_OutlinerStackRowDecorator, "decorate_row"));

  WM_main_add_notifier(NC_WINDOW, nullptr);
  return srna;
}

static bool rna_OutlinerStackRowDecorator_unregister(Main *bmain, StructRNA *type)
{
  return blender::ed::outliner::outliner_stack_decorator_unregister(*bmain, type);
}

/* The RNA dispatch hands `add_row` its parameters over positionally, one per RNA parameter; the
 * builder reads them as one #StackRowAddArgs (see the declaration in ED_outliner_stack.hh). This
 * packing and the RNA definition below are the only places the list is spelled out, so a new row
 * attribute touches those two and nothing past them.
 *
 * Lives in the runtime half on purpose: the definition half is also compiled into the `makesrna`
 * host tool, which does not link the editors, so a call into the Outliner module from there is an
 * unresolved external. The generated bridge includes this half only. */
int outliner_stack_row_builder_add_row(StackRowBuilderData *builder,
                                       const char *name,
                                       const char *icon_name,
                                       int glyph,
                                       int depth,
                                       int parent_ordinal,
                                       bool can_hold_children,
                                       bool has_children,
                                       bool enabled,
                                       bool locked,
                                       bool aux_enabled,
                                       int color_tag,
                                       bool compact,
                                       const char *stable_key,
                                       const char *unsupported_reason,
                                       PointerRNA *value,
                                       const char *value_prop,
                                       PointerRNA *mode,
                                       const char *mode_prop,
                                       bool value_inherited,
                                       bool mode_inherited)
{
  StackRowAddArgs args;
  args.name = name;
  args.icon_name = icon_name;
  args.glyph = glyph;
  args.depth = depth;
  args.parent_ordinal = parent_ordinal;
  args.can_hold_children = can_hold_children;
  args.has_children = has_children;
  args.enabled = enabled;
  args.locked = locked;
  args.aux_enabled = aux_enabled;
  args.color_tag = color_tag;
  args.compact = compact;
  args.stable_key = stable_key;
  args.unsupported_reason = unsupported_reason;
  args.value = value;
  args.value_prop = value_prop;
  args.mode = mode;
  args.mode_prop = mode_prop;
  args.value_inherited = value_inherited;
  args.mode_inherited = mode_inherited;
  return outliner_stack_row_builder_add_row(builder, args);
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

  prop = RNA_def_property(srna, "focus_button_icon", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, nullptr, "focus_button_icon");
  RNA_def_property_flag(prop, PROP_REGISTER_OPTIONAL);
  RNA_def_property_ui_text(prop,
                           "Focus Button Icon",
                           "Icon of the Focus Stack button on the object rows of the View Layer "
                           "mode, as its identifier; empty keeps the source's default");

  prop = RNA_def_property(srna, "focus_object_mode", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, nullptr, "focus_object_mode");
  RNA_def_property_flag(prop, PROP_REGISTER_OPTIONAL);
  RNA_def_property_ui_text(prop,
                           "Focus Object Mode",
                           "Object mode a Focus Stack click with 'enter edit mode' switches to, "
                           "as an identifier like 'SCULPT'; empty keeps the object's mode");

  prop = RNA_def_property(srna, "use_sub_selection", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "use_sub_selection", 0);
  RNA_def_property_flag(prop, PROP_REGISTER_OPTIONAL);
  RNA_def_property_ui_text(
      prop,
      "Use Sub-Selection",
      "Whether the header offers the source's sub-selection picker (the material chooser of the "
      "built-in stacks); a source whose stack always belongs to the object answers false");

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

  func = RNA_def_function(srna, "row_is_selected", nullptr);
  RNA_def_function_ui_description(
      func,
      "Whether the row is selected, for a source whose selection lives in its own data; leave the "
      "method out to leave selection to the Outliner");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_pointer(func, "owner", "ID", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
  parm = RNA_def_int(func, "ordinal", 0, 0, INT16_MAX, "Ordinal", "", 0, INT16_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  RNA_def_function_return(func, RNA_def_boolean(func, "selected", false, "", ""));

  func = RNA_def_function(srna, "row_set_selected", nullptr);
  RNA_def_function_ui_description(
      func, "Write one row's selection into the source's data after the Outliner changed it");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL | FUNC_ALLOW_WRITE);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_pointer(func, "owner", "ID", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
  parm = RNA_def_int(func, "ordinal", 0, 0, INT16_MAX, "Ordinal", "", 0, INT16_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_boolean(func, "select", false, "", "The row is selected");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  RNA_def_function_return(func, RNA_def_boolean(func, "changed", false, "", ""));

  func = RNA_def_function(srna, "can_reorder", nullptr);
  RNA_def_function_ui_description(
      func, "Whether the rows of the owner can change places at all");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL);
  parm = RNA_def_pointer(func, "owner", "ID", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
  RNA_def_function_return(func, RNA_def_boolean(func, "reorderable", false, "", ""));

  func = RNA_def_function(srna, "row_add", nullptr);
  RNA_def_function_ui_description(
      func, "Create a row of the given kind relative to the row the ordinal names");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL | FUNC_ALLOW_WRITE);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_pointer(func, "owner", "ID", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
  parm = RNA_def_int(func, "kind", 0, 0, INT32_MAX, "Kind", "Index into the source's kinds", 0, INT32_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_int(func, "ordinal", -1, -1, INT16_MAX, "Ordinal", "Row to place relative to", -1, INT16_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_int(func,
                     "place",
                     0,
                     0,
                     2,
                     "Place",
                     "0 above the anchor, 1 below it, 2 inside it",
                     0,
                     2);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  RNA_def_function_return(func, RNA_def_int(func, "new_ordinal", -1, -1, INT16_MAX, "", "", -1, INT16_MAX));

  func = RNA_def_function(srna, "row_action", nullptr);
  RNA_def_function_ui_description(func, "The row's action button was pressed");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL | FUNC_ALLOW_WRITE);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_pointer(func, "owner", "ID", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
  parm = RNA_def_int(func, "ordinal", 0, 0, INT16_MAX, "Ordinal", "", 0, INT16_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  RNA_def_function_return(func, RNA_def_boolean(func, "done", false, "", ""));

  func = RNA_def_function(srna, "row_set_enabled", nullptr);
  RNA_def_function_ui_description(func, "Turn the row on or off");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL | FUNC_ALLOW_WRITE);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_pointer(func, "owner", "ID", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
  parm = RNA_def_int(func, "ordinal", 0, 0, INT16_MAX, "Ordinal", "", 0, INT16_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_boolean(func, "enable", true, "", "");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  RNA_def_function_return(func, RNA_def_boolean(func, "done", false, "", ""));

  func = RNA_def_function(srna, "row_set_locked", nullptr);
  RNA_def_function_ui_description(func, "Protect the row, or lift that protection");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL | FUNC_ALLOW_WRITE);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_pointer(func, "owner", "ID", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
  parm = RNA_def_int(func, "ordinal", 0, 0, INT16_MAX, "Ordinal", "", 0, INT16_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_boolean(func, "locked", true, "", "");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  RNA_def_function_return(func, RNA_def_boolean(func, "done", false, "", ""));

  func = RNA_def_function(srna, "row_set_color_tag", nullptr);
  RNA_def_function_ui_description(func, "Set the color tag of a group row");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL | FUNC_ALLOW_WRITE);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_pointer(func, "owner", "ID", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
  parm = RNA_def_int(func, "ordinal", 0, 0, INT16_MAX, "Ordinal", "", 0, INT16_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_int(func,
                     "color_tag",
                     0,
                     0,
                     8,
                     "Color Tag",
                     "Palette index, zero for no tag",
                     0,
                     8);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  RNA_def_function_return(func, RNA_def_boolean(func, "done", false, "", ""));

  func = RNA_def_function(srna, "row_duplicate", nullptr);
  RNA_def_function_ui_description(func, "Copy the row, putting the copy directly above it");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL | FUNC_ALLOW_WRITE);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_pointer(func, "owner", "ID", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
  parm = RNA_def_int(func, "ordinal", 0, 0, INT16_MAX, "Ordinal", "", 0, INT16_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  RNA_def_function_return(func, RNA_def_int(func, "new_ordinal", -1, -1, INT16_MAX, "", "", -1, INT16_MAX));

  func = RNA_def_function(srna, "row_rename", nullptr);
  RNA_def_function_ui_description(func, "Rename the row");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL | FUNC_ALLOW_WRITE);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_pointer(func, "owner", "ID", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
  parm = RNA_def_int(func, "ordinal", 0, 0, INT16_MAX, "Ordinal", "", 0, INT16_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_string(func, "name", nullptr, MAX_NAME, "Name", "");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  RNA_def_function_return(func, RNA_def_boolean(func, "done", false, "", ""));

  func = RNA_def_function(srna, "row_remove", nullptr);
  RNA_def_function_ui_description(func, "Take the row out of the stack");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL | FUNC_ALLOW_WRITE);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_pointer(func, "owner", "ID", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
  parm = RNA_def_int(func, "ordinal", 0, 0, INT16_MAX, "Ordinal", "", 0, INT16_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  RNA_def_function_return(func, RNA_def_boolean(func, "done", false, "", ""));

  func = RNA_def_function(srna, "row_move", nullptr);
  RNA_def_function_ui_description(
      func,
      "Move the row next to the anchor row: above it, below it, or inside it when it holds rows");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL | FUNC_ALLOW_WRITE);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_pointer(func, "owner", "ID", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
  parm = RNA_def_int(func, "from_ordinal", 0, 0, INT16_MAX, "From", "", 0, INT16_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_int(func, "anchor_ordinal", -1, -1, INT16_MAX, "Anchor", "", -1, INT16_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_int(func, "place", 0, 0, 2, "Place", "0 above, 1 below, 2 inside", 0, 2);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  RNA_def_function_return(func, RNA_def_int(func, "new_ordinal", -1, -1, INT16_MAX, "", "", -1, INT16_MAX));

  func = RNA_def_function(srna, "notifier_invalidates", nullptr);
  RNA_def_function_ui_description(
      func,
      "Whether a notifier of the given category is reason enough to re-read the rows; without it "
      "a broad default answers for the owner's data");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_int(func, "category", 0, INT32_MIN, INT32_MAX, "Category", "", INT32_MIN, INT32_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_int(func, "data", 0, INT32_MIN, INT32_MAX, "Data", "", INT32_MIN, INT32_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_int(func, "action", 0, INT32_MIN, INT32_MAX, "Action", "", INT32_MIN, INT32_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  RNA_def_function_return(func, RNA_def_boolean(func, "invalidates", false, "", ""));

  func = RNA_def_function(srna, "undo_reset", nullptr);
  RNA_def_function_ui_description(
      func, "Drop runtime state an undo step invalidated, the way a paint target does");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);

  /* The definitions above are the other half of the #STACK_SOURCE_FUNCTION_LIST: one block per
   * name, in the same order, so the registration's have-function flags line up with the binding
   * table. A function added to one side only reads as absent at the other, so the count mismatch
   * is caught here at the definition, before any source can register. */
  BLI_assert(srna->functions.size() == blender::ed::outliner::STACK_SOURCE_FUNCTION_NUM);
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
                         "Describes the rows of a stack while a source's rows_build, or a "
                         "decorator's decorate_row, runs");
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
  parm = RNA_def_string(func,
                        "stable_key",
                        nullptr,
                        MAX_NAME,
                        "Stable Key",
                        "The row's identity: rows built from the same key keep their state "
                        "across rebuilds, and only a keyed row can be renamed in place");
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

  func = RNA_def_function(srna, "add_button", "outliner_stack_row_builder_button_add");
  RNA_def_function_ui_description(
      func,
      "Append an icon button to the row: it invokes an operator, or opens a menu. The operator "
      "properties always carry the row's ordinal and, when it has one, its marker");
  parm = RNA_def_int(func,
                     "row_ordinal",
                     -1,
                     -1,
                     INT16_MAX,
                     "Row",
                     "Ordinal of the row to add to, -1 for the row just added",
                     -1,
                     INT16_MAX);
  /* Optional: the C half defaults it. */
  parm = RNA_def_string(func, "text", nullptr, MAX_NAME, "Text", "Label for a menu-style button");
  /* Optional: the C half defaults it. */
  parm = RNA_def_string(func, "tooltip", nullptr, MAX_NAME, "Tooltip", "");
  /* Optional: the C half defaults it. */
  parm = RNA_def_string(func, "icon", nullptr, 64, "Icon", "Icon identifier, or empty for none");
  /* Optional: the C half defaults it. */
  parm = RNA_def_int(func, "glyph", 0, 0, INT32_MAX, "Glyph", "Material Symbols codepoint, or 0", 0, INT32_MAX);
  parm = RNA_def_string(func, "operator", nullptr, 64, "Operator", "Operator to invoke, or empty");
  /* Optional: the C half defaults it. */
  parm = RNA_def_string(func, "menu", nullptr, 64, "Menu", "Menu to open, or empty");
  /* Optional: the C half defaults it. */
  parm = RNA_def_boolean(func, "pressed", false, "", "Draw the button pressed");
  parm = RNA_def_boolean(func, "enabled", true, "", "Draw the button enabled");
  parm = RNA_def_boolean(func,
                         "before_value",
                         false,
                         "",
                         "Draw the button in a column before the value column");
  parm = RNA_def_boolean(func,
                         "hover_only",
                         false,
                         "",
                         "Draw the button only while the pointer is over the row");
  parm = RNA_def_string(func,
                        "int_prop",
                        nullptr,
                        64,
                        "Integer Property",
                        "Name of one extra integer operator property to set, or empty");
  parm = RNA_def_int(func,
                     "int_value",
                     0,
                     INT_MIN,
                     INT_MAX,
                     "Integer Value",
                     "Value of the extra integer operator property",
                     INT_MIN,
                     INT_MAX);
  RNA_def_function_return(func, RNA_def_boolean(func, "added", false, "", ""));

  func = RNA_def_function(srna, "add_preview_slot", "outliner_stack_row_builder_preview_slot_add");
  RNA_def_function_ui_description(
      func,
      "Append a preview slot to the row. A slot naming an operator invokes it on click, carrying "
      "the row's ordinal and, when it has one, its marker");
  parm = RNA_def_int(func,
                     "row_ordinal",
                     -1,
                     -1,
                     INT16_MAX,
                     "Row",
                     "Ordinal of the row to add to, -1 for the row just added",
                     -1,
                     INT16_MAX);
  /* Optional: the C half defaults it. */
  parm = RNA_def_string(func, "icon", nullptr, 64, "Icon", "Icon identifier, or empty for none");
  parm = RNA_def_int(func, "glyph", 0, 0, INT32_MAX, "Glyph", "Material Symbols codepoint, or 0", 0, INT32_MAX);
  parm = RNA_def_string(func, "label", nullptr, MAX_NAME, "Label", "Label shown in the tooltip");
  /* Optional: the C half defaults it. */
  parm = RNA_def_string(func, "tooltip", nullptr, MAX_NAME, "Tooltip", "");
  /* Optional: the C half defaults it. */
  parm = RNA_def_string(func,
                        "operator",
                        nullptr,
                        64,
                        "Operator",
                        "Operator to invoke on click, or empty");
  /* Optional: the C half defaults it. */
  parm = RNA_def_string(func,
                        "section_id",
                        nullptr,
                        64,
                        "Section",
                        "Identifier the slot is framed by while it is the one showing, or empty");
  parm = RNA_def_boolean(func, "enabled", true, "", "Accept clicks on the slot");
  parm = RNA_def_boolean(func,
                         "keeps_row_icon",
                         false,
                         "",
                         "Draw the row's own icon ahead of the slot");
  RNA_def_function_return(func, RNA_def_boolean(func, "added", false, "", ""));
}

/** The row decorator: a registered subclass that adds buttons to the rows of the source its
 * `source` names, whichever module registered that source. */
static void rna_def_outliner_stack_row_decorator(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;
  PropertyRNA *parm;
  FunctionRNA *func;

  srna = RNA_def_struct(brna, "OutlinerStackRowDecorator", nullptr);
  RNA_def_struct_ui_text(srna,
                         "Outliner Stack Row Decorator",
                         "Adds buttons to the rows of a stack source: register a subclass to "
                         "decorate another add-on's rows");
  RNA_def_struct_sdna(srna, "StackDecoratorInfo");
  RNA_def_struct_refine_func(srna, "rna_OutlinerStackRowDecorator_refine");
  RNA_def_struct_flag(srna, STRUCT_PUBLIC_NAMESPACE_INHERIT);
  RNA_def_struct_register_funcs(srna,
                                "rna_OutlinerStackRowDecorator_register",
                                "rna_OutlinerStackRowDecorator_unregister",
                                nullptr);
  RNA_def_struct_translation_context(srna, BLT_I18NCONTEXT_DEFAULT_BPYRNA);

  prop = RNA_def_property(srna, "bl_idname", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, nullptr, "idname");
  RNA_def_property_flag(prop, PROP_REGISTER);
  RNA_def_property_ui_text(prop, "ID Name", "Unique name the decorator registers under");

  prop = RNA_def_property(srna, "source", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, nullptr, "source");
  RNA_def_property_flag(prop, PROP_REGISTER_OPTIONAL);
  RNA_def_property_ui_text(prop,
                           "Source",
                           "Idname of the stack source whose rows this decorates, or 'ANY' for "
                           "every source");

  func = RNA_def_function(srna, "decorate_row", nullptr);
  RNA_def_function_ui_description(func, "Add buttons to the row, through the row builder");
  RNA_def_function_flag(func, FUNC_REGISTER);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_pointer(func, "owner", "ID", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
  parm = RNA_def_pointer(func, "row", "OutlinerStackRowBuilder", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
}

void RNA_def_space_outliner_stack(BlenderRNA *brna)
{
  rna_def_outliner_stack_source(brna);
  rna_def_outliner_stack_row_builder(brna);
  rna_def_outliner_stack_row_decorator(brna);
}

}  // namespace blender

#endif /* RNA_RUNTIME */
