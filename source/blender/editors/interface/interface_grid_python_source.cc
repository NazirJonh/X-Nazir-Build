/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include "interface_grid_view_sources.hh"

#include "BKE_context.hh"
#include "BKE_screen.hh"

#include "BLI_string.h"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "UI_grid_view.hh"
#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"

#include "WM_api.hh"
#include "WM_types.hh"

namespace blender::ui {

namespace {

struct PyGridItemDesc {
  std::string identifier;
  std::string label;
  int icon = ICON_NONE;
  int badge_icon = ICON_NONE;
};

/** Defined below; calls the Python-registered #UIGrid.draw_context_menu() override, if any. */
static void pygrid_draw_context_menu(const bContext &C,
                                     uiGridType *grid_type,
                                     StringRef identifier,
                                     Layout &layout);

class PyGridItem : public PreviewGridItem {
  uiGridType *grid_type_;
  std::string activate_operator_;

 public:
  PyGridItem(StringRef identifier, StringRef label, const int icon, uiGridType *grid_type)
      : PreviewGridItem(identifier, label, icon),
        grid_type_(grid_type),
        activate_operator_(grid_type->activate_operator)
  {
  }

  void on_activate(bContext &C) override
  {
    if (activate_operator_.empty()) {
      return;
    }
    wmOperatorType *ot = WM_operatortype_find(activate_operator_.c_str(), true);
    if (!ot) {
      return;
    }
    PointerRNA *op_props = MEM_new<PointerRNA>(__func__, WM_operator_properties_create_ptr(ot));
    RNA_string_set(op_props, "identifier", identifier_.c_str());
    WM_operator_name_call_ptr(&C, ot, wm::OpCallContext::InvokeRegionWin, op_props, nullptr);
    WM_operator_properties_free(op_props);
    MEM_delete(op_props);
  }

  void build_context_menu(bContext &C, Layout &column) const override
  {
    pygrid_draw_context_menu(C, grid_type_, identifier_, column);
  }
};

/**
 * \a r_grid_inst is owned by the caller (not a shared static): #uiGrid only stores the
 * #uiGridType pointer used by #rna_UIGrid_refine() to resolve the registered Python subclass, so
 * a shared instance would let a reentrant call (e.g. a `get_item()` callback that itself
 * triggers drawing of another #UIGrid) corrupt an in-flight call's type identity.
 */
static PointerRNA uigrid_python_pointer(const bContext &C, uiGridType *grid_type, uiGrid &r_grid_inst)
{
  r_grid_inst.type = grid_type;
  return RNA_pointer_create_discrete(&CTX_wm_screen(&C)->id, grid_type->rna_ext.srna, &r_grid_inst);
}

static bool pygrid_get_item_desc(const bContext &C,
                                 uiGridType *grid_type,
                                 const PointerRNA &dataptr,
                                 StringRef propname,
                                 const int index,
                                 PyGridItemDesc &r_desc)
{
  uiGrid grid_inst;
  PointerRNA grid_ptr = uigrid_python_pointer(C, grid_type, grid_inst);
  FunctionRNA *func = RNA_struct_find_function(grid_ptr.type, "get_item");
  if (!func) {
    return false;
  }

  ParameterList list;
  RNA_parameter_list_create(&list, &grid_ptr, func);

  const bContext *context = &C;
  RNA_parameter_set_lookup(&list, "context", &context);
  PointerRNA dataptr_mut = dataptr;
  RNA_parameter_set_lookup(&list, "data", &dataptr_mut);
  const std::string propname_str(propname);
  const char *propname_c = propname_str.c_str();
  RNA_parameter_set_lookup(&list, "propname", &propname_c);
  RNA_parameter_set_lookup(&list, "index", &index);

  /* Outputs need no pre-initialization: #RNA_parameter_list_create zero-fills the buffer, so the
   * PROP_THICK_WRAP string slots already read as "" and the int slots as #ICON_NONE (0) even if
   * the Python call raises before writing them. */
  grid_type->rna_ext.call(const_cast<bContext *>(&C), &grid_ptr, func, &list);

  void *ret;

  /* Read straight out of the ParameterList-owned THICK_WRAP buffers into #std::string (copying
   * before #RNA_parameter_list_free frees them). No intermediate fixed-size stack buffer, so no
   * second length to keep in sync with the RNA definition's #RNA_DYN_DESCR_MAX. */
  RNA_parameter_get(&list, RNA_function_find_parameter(nullptr, func, "identifier"), &ret);
  const char *identifier = ret ? static_cast<const char *>(ret) : "";

  if (!identifier[0]) {
    RNA_parameter_list_free(&list);
    return false;
  }
  r_desc.identifier = identifier;

  RNA_parameter_get(&list, RNA_function_find_parameter(nullptr, func, "label"), &ret);
  r_desc.label = ret ? static_cast<const char *>(ret) : "";

  RNA_parameter_get(&list, RNA_function_find_parameter(nullptr, func, "icon"), &ret);
  r_desc.icon = ret ? *static_cast<int *>(ret) : ICON_NONE;

  RNA_parameter_get(&list, RNA_function_find_parameter(nullptr, func, "badge_icon"), &ret);
  r_desc.badge_icon = ret ? *static_cast<int *>(ret) : ICON_NONE;

  RNA_parameter_list_free(&list);
  return true;
}

static void pygrid_draw_context_menu(const bContext &C,
                                     uiGridType *grid_type,
                                     StringRef identifier,
                                     Layout &layout)
{
  uiGrid grid_inst;
  PointerRNA grid_ptr = uigrid_python_pointer(C, grid_type, grid_inst);
  FunctionRNA *func = RNA_struct_find_function(grid_ptr.type, "draw_context_menu");
  if (!func) {
    return;
  }

  ParameterList list;
  RNA_parameter_list_create(&list, &grid_ptr, func);

  const bContext *context = &C;
  RNA_parameter_set_lookup(&list, "context", &context);
  const std::string identifier_str(identifier);
  const char *identifier_c = identifier_str.c_str();
  RNA_parameter_set_lookup(&list, "identifier", &identifier_c);
  Layout *layout_ptr = &layout;
  RNA_parameter_set_lookup(&list, "layout", &layout_ptr);

  grid_type->rna_ext.call(const_cast<bContext *>(&C), &grid_ptr, func, &list);

  RNA_parameter_list_free(&list);
}

}  // namespace

PyCallbackGridDataSource::PyCallbackGridDataSource(uiGridType *grid_type,
                                                   PointerRNA dataptr,
                                                   const StringRef propname)
    : grid_type_(grid_type), dataptr_(dataptr), propname_(propname)
{
}

int PyCallbackGridDataSource::item_count(const bContext &C) const
{
  uiGrid grid_inst;
  PointerRNA grid_ptr = uigrid_python_pointer(C, grid_type_, grid_inst);
  FunctionRNA *func = RNA_struct_find_function(grid_ptr.type, "get_item_count");
  if (!func) {
    return 0;
  }

  ParameterList list;
  RNA_parameter_list_create(&list, &grid_ptr, func);

  const bContext *context = &C;
  RNA_parameter_set_lookup(&list, "context", &context);
  PointerRNA dataptr = dataptr_;
  RNA_parameter_set_lookup(&list, "data", &dataptr);
  const char *propname = propname_.c_str();
  RNA_parameter_set_lookup(&list, "propname", &propname);

  grid_type_->rna_ext.call(const_cast<bContext *>(&C), &grid_ptr, func, &list);

  void *ret;
  RNA_parameter_get(&list, RNA_function_find_parameter(nullptr, func, "result"), &ret);
  const int count = ret ? *static_cast<int *>(ret) : 0;
  RNA_parameter_list_free(&list);
  return count;
}

void PyCallbackGridDataSource::build_window(const bContext &C,
                                            AbstractGridView &view,
                                            const IndexRange window)
{
  for (const int index : window) {
    PyGridItemDesc desc;
    if (!pygrid_get_item_desc(C, grid_type_, dataptr_, propname_, index, desc)) {
      continue;
    }
    view.add_item<PyGridItem>(desc.identifier, desc.label, desc.icon, grid_type_);
  }
}

}  // namespace blender::ui
