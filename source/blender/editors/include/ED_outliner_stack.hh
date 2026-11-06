/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 *
 * Access the Stack Layers display mode from outside its module: for RNA, and for the editors that
 * have to follow the stack the user is working on.
 *
 * Kept apart from #ED_outliner.hh so the module's main public header stays about the editor API:
 * this is the narrow door the Outliner's own RNA properties walk through, nothing more.
 */

#pragma once

#include <string>

#include "BLI_string_ref.hh"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "DNA_material_types.h"
#include "DNA_space_enums.h"

#include "RNA_types.hh"

struct bContext;
struct EnumPropertyItem;
struct FunctionRNA;
struct ID;
struct Image;
struct Main;
struct Material;
struct MaterialPaintLayer;
struct PointerRNA;
struct ScrArea;
struct SpaceOutliner;
struct StackDecoratorInfo;
struct StackRowBuilderData;
struct StackSourceInfo;
struct StructRNA;

namespace blender {
struct Scene;
struct ViewLayer;
namespace ed::outliner {
class StackRowDecoratorPython;
class StackSourcePython;
}  // namespace ed::outliner
}  // namespace blender

namespace blender::ed::outliner {

/**
 * Show the tool header only in Stack Layers.
 *
 * The tool header carries the Stack Layers controls and nothing else, so an empty strip under the
 * header in every other display mode is wasted space the user did not ask for. Every path that
 * changes the display mode outside the RNA property's own update -- an operator that sets the mode
 * directly, a mode restored from a file, a duplicated area -- goes through this, so they all agree
 * with a mode set from the UI. The re-layout it triggers is deferred to the next window draw (see
 * the definition), which is why it needs no `bContext`.
 */
void outliner_tool_header_visibility_sync(bContext * /*C*/,
                                          ScrArea *area,
                                          const SpaceOutliner &space_outliner);

/**
 * The name of the data-block the displayed Stack Layers stack belongs to, as the source last
 * resolved it.
 *
 * Kept on the space's runtime rather than resolved on the fly, because an RNA getter has no
 * context to resolve a focus with. Empty until the stack has been drawn once.
 */
const char *outliner_stack_focus_name_get(const SpaceOutliner &space_outliner);

/** Index of the focused stack's source-defined sub-selection, such as a material slot. */
int outliner_stack_focus_sub_index_get(const SpaceOutliner &space_outliner);
/**
 * Select one of the focused object's source-defined sub-selections and make it the active stack.
 *
 * Used by the Stack Layers header's RNA enum so its standard Ctrl-Wheel cycling follows the same
 * path as an explicit stack selection.
 */
void outliner_stack_focus_sub_index_set(SpaceOutliner &space_outliner, int sub_index);
bool outliner_stack_focus_sub_index_apply(bContext &C, SpaceOutliner &space_outliner);
/** Source-defined choices for #outliner_stack_focus_sub_index_get. */
const EnumPropertyItem *outliner_stack_focus_sub_index_itemf(bContext *C,
                                                             SpaceOutliner &space_outliner,
                                                             bool *r_free);

/**
 * The idname of the stack source the space shows: a registered source answers the name it
 * registered under, a built-in its own ("paint_material"). Read-only: the picker writes
 * #SpaceOutliner::stack_source, this only reads back what that resolved to.
 */
StringRefNull outliner_stack_source_idname_get(const SpaceOutliner &space_outliner);
/**
 * Whether the active source wants the header's sub-selection picker (the built-in stacks'
 * material chooser): a registered source answers through its #use_sub_selection class attribute,
 * the built-ins keep theirs.
 */
bool outliner_stack_source_use_sub_selection(const SpaceOutliner &space_outliner);

/**
 * Follow the active object with the display again, and put it on the object's stack now.
 *
 * Answers a display the user left by hand and the space's own switch, so the option that turns the
 * follow on reads as what the Outliner does rather than as a promise for the next object change.
 * Returns true when the display mode changed.
 */
bool outliner_stack_view_follow_resume(bContext &C, SpaceOutliner &space_outliner);
/**
 * The display mode was chosen by hand, so the follow no longer owns it: a View Layer it opened is
 * the user's from now on, and it will not bring the Stack Layers mode back on the next change.
 */
void outliner_stack_follow_release_mode(SpaceOutliner &space_outliner);
/** As the display's own normalization of the focus, for the RNA update of Pin Stack. */
void outliner_stack_focus_normalize(bContext &C, SpaceOutliner &space_outliner);

/**
 * Tell the Stack Layers displays that \a owner gained the row identified by \a row_id outside their
 * own operators -- the Add popover's Material layer is created by an RNA property update, not by
 * the stack's Add operator.
 *
 * Every Outliner currently showing that owner re-reads its rows and reads the named one as the
 * active, selected row, which is what #outliner_stack_row_add leaves behind for a row it made.
 */
void outliner_stack_row_added_sync(bContext &C, ID &owner, const bUUID &row_id);

/**
 * Update this Outliner's Stack Layers state after a paint-stack row became active outside its own
 * operators -- a viewport pick of a Curve Patch effect's curve, or another editor's activation.
 *
 * The data-level activation belongs to the paint stack itself (see
 * #blender::ed::sculpt_paint::paint_stack_row_activate); this only makes the Outliner follow it:
 * the material's active row becomes the selected one and an attached section is revealed. Driven
 * by the `NC_MATERIAL | ND_SHADING` notifier the activation sends, not by a call from the editor
 * that changed the row.
 */
void outliner_stack_paint_row_ui_sync(Main &bmain,
                                      Scene &scene,
                                      ViewLayer &view_layer,
                                      SpaceOutliner &space_outliner,
                                      Material &material);

/**
 * Make this Outliner's Stack Layers selection and active row name the owner's active row again.
 *
 * The remembered selection and active row are per space, and a screen keeps them from when it was
 * last used: after a workspace switch they name whichever row was active then, not the one the
 * data has been telling every other editor about since. This reads the active row from the source
 * -- the material's active paint layer, the object's active shape key -- and leaves it as the one
 * selected and active row, the same thing #outliner_stack_paint_row_ui_sync does when a row is
 * activated by a notifier this space can name.
 */
void outliner_stack_row_ui_resync(Main &bmain,
                                  Scene &scene,
                                  ViewLayer &view_layer,
                                  SpaceOutliner &space_outliner);

/**
 * Give \a layer a mask that reads \a image: the white base mask an Add White Mask makes, and a
 * Fill correction over it whose Base Color record carries the image. The paint half is
 * #blender::ed::sculpt_paint::paint_layers_mask_add_image's; this adds the Outliner sync, so the
 * Outliners showing the material read the new correction as the active row.
 *
 * Shared so a mask made from the Stack Layers Add and one made by the ID Browser assignment are
 * the same mask. The undo step is the caller's: an operator pushes one, the RNA assignment pushes
 * its own.
 *
 * \return the new correction, or null when \a layer is not a row of \a material or has a mask.
 */
MaterialPaintLayer *outliner_stack_row_mask_add_image(bContext &C,
                                                     Material &material,
                                                     MaterialPaintLayer &layer,
                                                     Image &image);

/**
 * \name Python-defined stack sources and row decorators
 *
 * The registration half of the `bpy.types.OutlinerStackSource` and
 * `bpy.types.OutlinerStackRowDecorator` RNA types: what their RNA-side register functions call,
 * and the narrow vocabulary the module answers. The Python methods themselves are dispatched by
 * RNA (the way a Python panel's #draw is); this is only the door in and out of the module.
 *
 * \{ */

/**
 * The optional functions of the #OutlinerStackSource RNA type, in the base type's definition
 * order: the one list the #StackSourceFunctionSet members, the registration's binding table
 * (#stack_source_func_bindings in rna_space_outliner_stack.cc) and the function count are written
 * from, so a new verb is one line here plus one `RNA_def_function` block in the same position --
 * the registration's have-function flags arrive in this order, and `rna_def_outliner_stack_source`
 * asserts its count against #STACK_SOURCE_FUNCTION_NUM. #has_stack_key is the optional cache key
 * the source defines for its #has_stack answers (an integer-valued float), mixed into
 * #object_stack_key.
 */
#define STACK_SOURCE_FUNCTION_LIST(X) \
  X(has_stack) \
  X(has_stack_key) \
  X(owner_get) \
  X(state_hash) \
  X(rows_build) \
  X(row_activate) \
  X(row_is_active) \
  X(row_is_selected) \
  X(row_set_selected) \
  X(can_reorder) \
  X(row_add) \
  X(row_action) \
  X(row_set_enabled) \
  X(row_set_locked) \
  X(row_set_color_tag) \
  X(row_duplicate) \
  X(row_rename) \
  X(row_remove) \
  X(row_move) \
  X(notifier_invalidates) \
  X(undo_reset)

#define STACK_SOURCE_FUNCTION_COUNT_ADD(name) +1
/** How many verbs #STACK_SOURCE_FUNCTION_LIST names. */
constexpr int STACK_SOURCE_FUNCTION_NUM =
    0 STACK_SOURCE_FUNCTION_LIST(STACK_SOURCE_FUNCTION_COUNT_ADD);
#undef STACK_SOURCE_FUNCTION_COUNT_ADD

/**
 * The RNA functions of the #OutlinerStackSource base type, resolved once by the registration and
 * handed over: an entry is null when the registered subclass did not answer that verb, which is
 * how the module knows to refuse it.
 */
struct StackSourceFunctionSet {
#define STACK_SOURCE_FUNCTION_FIELD(name) FunctionRNA *name = nullptr;
  STACK_SOURCE_FUNCTION_LIST(STACK_SOURCE_FUNCTION_FIELD)
#undef STACK_SOURCE_FUNCTION_FIELD
};

/** Create the module's half of a registered stack source; ownership passes to the module on
 * #outliner_stack_source_python_register. */
StackSourcePython *outliner_stack_source_python_new(const StackSourceInfo &info);
/** The idname the module owns for \a source: the string an RNA struct created for it has to point
 * at, since #RNA_def_struct_ptr keeps the pointer instead of copying it. */
const char *outliner_stack_source_python_idname(const StackSourcePython &source);
/** The srna the source \a idname's registration created for its subclass, or null when none is
 * registered under the name. */
StructRNA *outliner_stack_source_python_srna(StringRefNull idname);
/** The subclass srna the source whose description the given info is was registered with, or
 * null when no registered source owns it (the RNA refinement falls back to the base type). */
StructRNA *outliner_stack_source_python_refine(const StackSourceInfo *info);
/** The same, for the decorator the given info names. */
StructRNA *outliner_stack_decorator_python_refine(const StackDecoratorInfo *info);
/** Bind \a source to its registered subclass, store it in \a py_srna's blender-type slot as its
 * base #StackSource (the same type the unregister reads back), list it, and invalidate every
 * Outliner's cached rows so the next build sees it. Takes ownership of \a source. */
void outliner_stack_source_python_register(Main &bmain,
                                           StackSourcePython *source,
                                           void *py_class,
                                           StructCallbackFunc py_call,
                                           StructFreeFunc py_free,
                                           StructRNA *py_srna,
                                           const StackSourceFunctionSet &funcs);
/** Everything an unregister does: drop the source, release its Python class, free its srna.
 * False when \a srna does not name a registered source. */
bool outliner_stack_source_python_unregister(Main &bmain, StructRNA *srna);

/** Create the module's half of a registered row decorator; ownership passes to the module on
 * #outliner_stack_decorator_register. */
StackRowDecoratorPython *outliner_stack_decorator_new(const StackDecoratorInfo &info);
/** The same, for a decorator. */
const char *outliner_stack_decorator_idname(const StackRowDecoratorPython &decorator);
/** Bind \a decorator to its registered subclass, list it, and invalidate every Outliner's cached
 * rows so the next build runs it. Takes ownership of \a decorator. */
void outliner_stack_decorator_register(Main &bmain,
                                       StackRowDecoratorPython *decorator,
                                       void *py_class,
                                       StructCallbackFunc py_call,
                                       StructFreeFunc py_free,
                                       StructRNA *py_srna,
                                       FunctionRNA *decorate_row);
/** Everything an unregister does, for the decorator \a srna names. */
bool outliner_stack_decorator_unregister(Main &bmain, StructRNA *srna);
/** The same, addressed by the idname the decorator registered under; false when none. */
bool outliner_stack_decorator_unregister_by_name(Main &bmain, StringRefNull idname);

/** Invalidate the cached Stack Layers rows of every Outliner in \a bmain. */
void outliner_stack_rows_invalidate_all(Main &bmain);

/** The widest column a Python source may ask for (#StackSourceInfo's width fields), in UI units:
 * past this one column would crowd out the row's name. The registration's RNA range and the
 * column layout's own clamp have to agree on it, so both read it here. */
constexpr float STACK_SOURCE_COLUMN_WIDTH_MAX = 16.0f;
/** The most extra icon columns a Python source may reserve (#StackSourceInfo::icon_columns). */
constexpr int STACK_SOURCE_ICON_COLUMNS_MAX = 8;

/** Every source known, built-ins first, then the registered ones in registration order. The
 * source picker's enum reads this; \a is_custom marks the entries whose DNA value is
 * #SO_STACK_SRC_CUSTOM plus an idname rather than a built-in enum value. */
struct StackSourceListItem {
  std::string idname;
  std::string ui_name;
  bool is_custom = false;
  /** The DNA enum the source answers as: a built-in's value in the source picker is this, read
   * from the source rather than assumed from its place in the listing. */
  eSpaceOutliner_StackSource type = SO_STACK_SRC_NONE;
  /** For a registered source: the session value scripts and the space property write to select
   * it. Assigned once at registration, so it stays with the source however the registry changes
   * around it; meaningless for a built-in, whose value is #type. */
  int session_value = 0;
};
void outliner_stack_sources_list(blender::Vector<StackSourceListItem> &r_items);

/** The RNA type the row builder functions run on; set once by the first registration. */
void outliner_stack_row_builder_srna_set(StructRNA *srna);
StructRNA *outliner_stack_row_builder_srna();

/** \} */


}  // namespace blender::ed::outliner

namespace blender {

/**
 * \name Row builder RNA callbacks
 *
 * The C half of the `OutlinerStackRowBuilder` RNA type: what a Python #rows_build (or
 * #decorate_row) call drives. Named by the RNA definition, resolved at link time; the parameter
 * order follows the RNA function definitions in `rna_space_outliner_stack.cc`. The generated code
 * declares and calls these unqualified from this namespace, the one every RNA callback lives in,
 * so they stand here rather than inside the module's own namespace.
 *
 * \{ */
/**
 * One #outliner_stack_row_builder_add_row call's arguments: what a Python `add_row` names.
 *
 * The RNA dispatch hands the builder's entry point its parameters positionally (the generated
 * bridge cannot build a struct), so the RNA side fills this and the builder reads it: the
 * parameter list stops growing with every row attribute a source gains, and only the RNA
 * definition and its bridge ever spell it out.
 */
struct StackRowAddArgs {
  const char *name = nullptr;
  const char *icon_name = nullptr;
  int glyph = 0;
  int depth = 0;
  /** Ordinal of the enclosing group row, or -1 at the top level. */
  int parent_ordinal = -1;
  bool can_hold_children = false;
  bool has_children = false;
  bool enabled = true;
  bool locked = false;
  bool aux_enabled = true;
  int color_tag = 0;
  bool compact = false;
  const char *stable_key = nullptr;
  const char *unsupported_reason = nullptr;
  PointerRNA *value = nullptr;
  const char *value_prop = nullptr;
  PointerRNA *mode = nullptr;
  const char *mode_prop = nullptr;
  bool value_inherited = false;
  bool mode_inherited = false;
};

/** Append a row; returns its ordinal, or -1 when the stack is full or \a builder is in decorator
 * mode. \a StackRowAddArgs::stable_key names the row's identity: rows built from the same key
 * keep their collapsed and selected state across rebuilds, and only a keyed row can be renamed in
 * place. */
int outliner_stack_row_builder_add_row(StackRowBuilderData *builder,
                                       const StackRowAddArgs &args);

/** The RNA dispatch's positional entry point for `add_row`: one parameter per RNA parameter,
 * packed into #StackRowAddArgs for the builder. Defined in the runtime half of
 * `rna_space_outliner_stack.cc` (the half the generated bridge includes, not the one the
 * `makesrna` host tool links); declared here so the generated bridge, which runs in this
 * namespace, sees it. */
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
                                       bool mode_inherited);
/** Append a button to the row \a row_ordinal names (the row a decorator is editing in decorator
 * mode). Either \a operator_idname or \a menu_idname has to be set. The draw hands the button's
 * operator the row's "ordinal" and, when it has an identity, its "marker", read off the row the
 * button hangs on. */
bool outliner_stack_row_builder_button_add(StackRowBuilderData *builder,
                                           int row_ordinal,
                                           const char *text,
                                           const char *tooltip,
                                           const char *icon_name,
                                           int glyph,
                                           const char *operator_idname,
                                           const char *menu_idname,
                                           bool pressed,
                                           bool enabled,
                                           bool before_value,
                                           bool hover_only,
                                           const char *int_prop,
                                           int int_value);

/** Append a preview slot to the row \a row_ordinal names. A slot with \a operator_idname
 * invokes that operator on click, carrying the row's "ordinal" and, when it has an identity, its
 * "marker"; one without it only draws the icon, unless it also names a section. */
bool outliner_stack_row_builder_preview_slot_add(StackRowBuilderData *builder,
                                                 int row_ordinal,
                                                 const char *icon_name,
                                                 int glyph,
                                                 const char *label,
                                                 const char *tooltip,
                                                 const char *operator_idname,
                                                 const char *section_id,
                                                 bool enabled,
                                                 bool keeps_row_icon);

/** \} */

}  // namespace blender
