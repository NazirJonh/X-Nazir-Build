/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup pythonintern
 *
 * This file extends RNA types from `bpy.types` with C/Python API methods and attributes.
 *
 * We should avoid adding code here, and prefer:
 * - `source/blender/makesrna/intern/rna_context.cc` using the RNA C API.
 * - `scripts/modules/_bpy_types.py` when additions can be written in Python.
 *
 * Otherwise functions can be added here as a last resort.
 */

#include <Python.h>
#include <descrobject.h>

#include "BLI_utildefines.h"

#include "bpy_library.hh"
#include "bpy_rna.hh"
#include "bpy_rna_callback.hh"
#include "bpy_rna_context.hh"
#include "bpy_rna_data.hh"
#include "bpy_rna_id_collection.hh"
#include "bpy_rna_text.hh"
#include "bpy_rna_types_capi.hh"
#include "bpy_rna_ui.hh"
#include "bpy_rna_wm.hh"

#include "bpy_rna_operator.hh"

#include "../generic/py_capi_utils.hh"
#include "../gpu/gpu_py_batch.hh"
#include "bpy_capi_utils.hh"

#include "RNA_prototypes.hh"

#include "MEM_guardedalloc.h"

#include "WM_api.hh"

#include "DNA_mesh_types.h"
#include "draw_cache_impl.hh"

#include "BKE_context.hh"
#include "DEG_depsgraph_query.hh"


namespace blender {

/* -------------------------------------------------------------------- */
/** \name Blend Data
 * \{ */

static PyMethodDef pyrna_blenddata_methods[] = {
    {nullptr, nullptr, 0, nullptr}, /* #BPY_rna_id_collection_user_map_method_def */
    {nullptr, nullptr, 0, nullptr}, /* #BPY_rna_id_collection_file_path_map_method_def */
    {nullptr, nullptr, 0, nullptr}, /* #BPY_rna_id_collection_file_path_foreach_method_def */
    {nullptr, nullptr, 0, nullptr}, /* #BPY_rna_id_collection_batch_remove_method_def */
    {nullptr, nullptr, 0, nullptr}, /* #BPY_rna_id_collection_orphans_purge_method_def */
    {nullptr, nullptr, 0, nullptr}, /* #BPY_rna_data_context_method_def */
    {nullptr, nullptr, 0, nullptr},
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Blend Data Libraries
 * \{ */

static PyMethodDef pyrna_blenddatalibraries_methods[] = {
    {nullptr, nullptr, 0, nullptr}, /* #BPY_library_load_method_def */
    {nullptr, nullptr, 0, nullptr}, /* #BPY_library_write_method_def */
    {nullptr, nullptr, 0, nullptr},
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name UI Layout
 * \{ */

static PyMethodDef pyrna_uilayout_methods[] = {
    {nullptr, nullptr, 0, nullptr}, /* #BPY_rna_uilayout_introspect_method_def */
    {nullptr, nullptr, 0, nullptr},
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operator
 * \{ */

static PyMethodDef pyrna_operator_methods[] = {
    {nullptr, nullptr, 0, nullptr}, /* #BPY_rna_operator_poll_message_set */
    {nullptr, nullptr, 0, nullptr},
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Text Editor
 * \{ */

static PyMethodDef pyrna_text_methods[] = {
    {nullptr, nullptr, 0, nullptr}, /* #BPY_rna_region_as_string_method_def */
    {nullptr, nullptr, 0, nullptr}, /* #BPY_rna_region_from_string_method_def */
    {nullptr, nullptr, 0, nullptr},
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Window Manager Type
 * \{ */

static PyMethodDef pyrna_windowmanager_methods[] = {
    {nullptr, nullptr, 0, nullptr}, /* #BPY_rna_windowmanager_draw_cursor_add_method_def */
    {nullptr, nullptr, 0, nullptr}, /* #BPY_rna_windowmanager_draw_cursor_remove_method_def */
    {nullptr, nullptr, 0, nullptr},
};

static PyGetSetDef pyrna_windowmanager_getset[] = {
    {nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr}, /* #BPY_rna_windowmanager_clipboard_getset_def */
    {nullptr, nullptr, nullptr, nullptr, nullptr}, /* Sentinel */
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Window Type
 * \{ */

static PyMethodDef pyrna_window_methods[] = {
    {nullptr, nullptr, 0, nullptr}, /* #BPY_rna_window_screenshot_method_def */
    {nullptr, nullptr, 0, nullptr},
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Context Type
 * \{ */

static PyMethodDef pyrna_context_methods[] = {
    {nullptr, nullptr, 0, nullptr}, /* #BPY_rna_context_temp_override_method_def */
    {nullptr, nullptr, 0, nullptr},
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Space Type
 * \{ */

PyDoc_STRVAR(
    /* Wrap. */
    pyrna_draw_handler_add_doc,
    ".. classmethod:: draw_handler_add(callback, args, region_type, draw_type)\n"
    "\n"
    "   Add a new draw handler to this space type.\n"
    "   It will be called every time the specified region in the space type will be drawn.\n"
    "   Note: All arguments are positional only for now.\n"
    "\n"
    "   :param callback:\n"
    "      A function that will be called when the region is drawn.\n"
    "      It gets the specified arguments as input, it's return value is ignored.\n"
    "   :type callback: Callable[..., Any]\n"
    "   :param args: Arguments that will be passed to the callback.\n"
    "   :type args: tuple[Any, ...]\n"
    "   :param region_type: The region type the callback draws in; usually ``WINDOW``. "
    "(:class:`bpy.types.Region.type`)\n"
    "   :type region_type: str\n"
    "   :param draw_type: Usually ``POST_PIXEL`` for 2D drawing and ``POST_VIEW`` for 3D drawing. "
    "In some cases ``PRE_VIEW`` can be used. ``BACKDROP`` can be used for backdrops in the node "
    "editor.\n"
    "   :type draw_type: str\n"
    "   :return: Handler that can be removed later on.\n"
    "   :rtype: object\n");
PyDoc_STRVAR(
    /* Wrap. */
    pyrna_draw_handler_remove_doc,
    ".. classmethod:: draw_handler_remove(handler, region_type)\n"
    "\n"
    "   Remove a draw handler that was added previously.\n"
    "\n"
    "   :param handler: The draw handler that should be removed.\n"
    "   :type handler: object\n"
    "   :param region_type: Region type the callback was added to.\n"
    "   :type region_type: str\n");

static PyMethodDef pyrna_space_methods[] = {
    {"draw_handler_add",
     static_cast<PyCFunction>(pyrna_callback_classmethod_add),
     METH_VARARGS | METH_CLASS,
     pyrna_draw_handler_add_doc},
    {"draw_handler_remove",
     static_cast<PyCFunction>(pyrna_callback_classmethod_remove),
     METH_VARARGS | METH_CLASS,
     pyrna_draw_handler_remove_doc},
    {nullptr, nullptr, 0, nullptr},
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mesh Type - Sculpt Custom Overlay API
 * \{ */

PyDoc_STRVAR(
    /* Wrap. */
    pyrna_mesh_get_sculpt_custom_triangles_batch_doc,
    ".. method:: get_sculpt_custom_triangles_batch()\n"
    "\n"
    "   Get GPU batch for custom overlay triangles in Sculpt mode.\n"
    "   The returned batch is managed by the mesh cache and should not be discarded manually.\n"
    "   Used by Python addons to create custom overlays in Sculpt mode.\n"
    "\n"
    "   :return: GPU Batch object or None if not available.\n"
    "   :rtype: :class:`gpu.types.GPUBatch` or None\n");

static PyObject *pyrna_mesh_get_sculpt_custom_triangles_batch(PyObject *self)
{
  BPy_StructRNA *pyrna = reinterpret_cast<BPy_StructRNA *>(self);
  if (!pyrna->ptr.has_value() || !pyrna->ptr->data) {
    Py_RETURN_NONE;
  }
  Mesh *mesh = static_cast<Mesh *>(pyrna->ptr->data);
  
  /* Try to get active object from context to use as key for Map */
  bContext *C = BPY_context_get();
  Object *ob = nullptr;
  if (C) {
    ob = CTX_data_active_object(C);
    if (ob && ob->type == OB_MESH && ob->data == &mesh->id) {
      /* Active object's mesh matches - use it */
    }
    else {
      ob = nullptr; /* Don't use if mesh doesn't match */
    }
  }
  
  // #region agent log
  {
    static unsigned long long counter = 0;
    FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
    if (f) {
      fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"bpy_rna_types_capi.cc:294\",\"message\":\"PYTHON_API: get_sculpt_custom_triangles_batch called\",\"data\":{\"mesh_id\":\"%p\",\"object_id\":\"%p\"},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"J\"}\n",
              counter++, mesh, ob);
      fclose(f);
    }
  }
  // #endregion
  
  blender::gpu::Batch *batch = blender::draw::DRW_mesh_batch_cache_get_sculpt_custom_triangles(
      *mesh, ob);
  
  // #region agent log
  {
    static unsigned long long counter = 0;
    FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
    if (f) {
      fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"bpy_rna_types_capi.cc:310\",\"message\":\"PYTHON_API: get_sculpt_custom_triangles_batch returning\",\"data\":{\"batch_ptr\":\"%p\"},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"J\"}\n",
              counter++, batch);
      fclose(f);
    }
  }
  // #endregion
  
  if (!batch) {
    Py_RETURN_NONE;
  }
  return BPyGPUBatch_CreatePyObject_Wrap(batch);
}

PyDoc_STRVAR(
    /* Wrap. */
    pyrna_mesh_get_sculpt_custom_edges_batch_doc,
    ".. method:: get_sculpt_custom_edges_batch()\n"
    "\n"
    "   Get GPU batch for custom overlay edges in Sculpt mode.\n"
    "   The returned batch is managed by the mesh cache and should not be discarded manually.\n"
    "   Used by Python addons to create custom overlays in Sculpt mode.\n"
    "\n"
    "   :return: GPU Batch object or None if not available.\n"
    "   :rtype: :class:`gpu.types.GPUBatch` or None\n");

static PyObject *pyrna_mesh_get_sculpt_custom_edges_batch(PyObject *self)
{
  BPy_StructRNA *pyrna = reinterpret_cast<BPy_StructRNA *>(self);
  if (!pyrna->ptr.has_value() || !pyrna->ptr->data) {
    Py_RETURN_NONE;
  }
  Mesh *mesh = static_cast<Mesh *>(pyrna->ptr->data);
  /* Try to get active object from context to use as key for Map */
  bContext *C = BPY_context_get();
  Object *ob = nullptr;
  if (C) {
    ob = CTX_data_active_object(C);
    if (ob && ob->type == OB_MESH && ob->data == &mesh->id) {
      /* Active object's mesh matches - use it */
    }
    else {
      ob = nullptr; /* Don't use if mesh doesn't match */
    }
  }
  
  blender::gpu::Batch *batch = blender::draw::DRW_mesh_batch_cache_get_sculpt_custom_edges(*mesh, ob);
  if (!batch) {
    Py_RETURN_NONE;
  }
  return BPyGPUBatch_CreatePyObject_Wrap(batch);
}

PyDoc_STRVAR(
    /* Wrap. */
    pyrna_mesh_get_sculpt_custom_vertices_batch_doc,
    ".. method:: get_sculpt_custom_vertices_batch()\n"
    "\n"
    "   Get GPU batch for custom overlay vertices in Sculpt mode.\n"
    "   The returned batch is managed by the mesh cache and should not be discarded manually.\n"
    "   Used by Python addons to create custom overlays in Sculpt mode.\n"
    "\n"
    "   :return: GPU Batch object or None if not available.\n"
    "   :rtype: :class:`gpu.types.GPUBatch` or None\n");

static PyObject *pyrna_mesh_get_sculpt_custom_vertices_batch(PyObject *self)
{
  BPy_StructRNA *pyrna = reinterpret_cast<BPy_StructRNA *>(self);
  if (!pyrna->ptr.has_value() || !pyrna->ptr->data) {
    Py_RETURN_NONE;
  }
  Mesh *mesh = static_cast<Mesh *>(pyrna->ptr->data);
  
  /* Try to get active object from context to use as key for Map */
  bContext *C = BPY_context_get();
  Object *ob = nullptr;
  if (C) {
    ob = CTX_data_active_object(C);
    if (ob && ob->type == OB_MESH && ob->data == &mesh->id) {
      /* Active object's mesh matches - use it */
    }
    else {
      ob = nullptr; /* Don't use if mesh doesn't match */
    }
  }
  
  blender::gpu::Batch *batch = blender::draw::DRW_mesh_batch_cache_get_sculpt_custom_vertices(
      *mesh, ob);
  if (!batch) {
    Py_RETURN_NONE;
  }
  return BPyGPUBatch_CreatePyObject_Wrap(batch);
}

PyDoc_STRVAR(
    /* Wrap. */
    pyrna_mesh_get_custom_overlay_batch_doc,
    ".. method:: get_custom_overlay_batch(type)\n"
    "\n"
    "   Get GPU batch for custom overlay.\n"
    "   Works in both Edit Mode (returns edit batches) and Sculpt Mode (returns custom sculpt batches).\n"
    "   :param type: Type of batch ('triangles', 'edges', 'vertices')\n"
    "   :type type: str\n"
    "   :return: GPU Batch object or None if not available.\n"
    "   :rtype: :class:`gpu.types.GPUBatch` or None\n");

static PyObject *pyrna_mesh_get_custom_overlay_batch(PyObject *self, PyObject *args)
{
  BPy_StructRNA *pyrna = reinterpret_cast<BPy_StructRNA *>(self);
  if (!pyrna->ptr.has_value() || !pyrna->ptr->data) {
    Py_RETURN_NONE;
  }
  Mesh *mesh = static_cast<Mesh *>(pyrna->ptr->data);
  
  const char *type_str;
  if (!PyArg_ParseTuple(args, "s", &type_str)) {
    return NULL;
  }
  
  /* Try to get active object from context to use as key for Map */
  bContext *C = BPY_context_get();
  Object *ob = nullptr;
  if (C) {
    ob = CTX_data_active_object(C);
    if (ob && ob->type == OB_MESH && ob->data == &mesh->id) {
      /* Active object's mesh matches - use it */
    }
    else {
      ob = nullptr; /* Don't use if mesh doesn't match */
    }
  }
  
  blender::draw::CustomOverlayType type;
  if (STREQ(type_str, "triangles")) {
    type = blender::draw::CustomOverlayType::TypeTriangles;
  }
  else if (STREQ(type_str, "edges")) {
    type = blender::draw::CustomOverlayType::TypeEdges;
  }
  else if (STREQ(type_str, "vertices")) {
    type = blender::draw::CustomOverlayType::TypeVertices;
  }
  else {
    PyErr_SetString(PyExc_ValueError, "Invalid type: must be 'triangles', 'edges', or 'vertices'");
    return NULL;
  }
  
  blender::gpu::Batch *batch = blender::draw::DRW_mesh_batch_cache_get_custom_overlay(
      *mesh, ob, type);
  
  if (!batch) {
    Py_RETURN_NONE;
  }
  return BPyGPUBatch_CreatePyObject_Wrap(batch);
}

PyDoc_STRVAR(
    /* Wrap. */
    pyrna_mesh_get_custom_overlay_mode_doc,
    ".. method:: get_custom_overlay_mode()\n"
    "\n"
    "   Get current custom overlay mode ('edit' or 'sculpt' or None).\n"
    "   This depends on the active object's mode.\n"
    "   :return: Mode string or None.\n"
    "   :rtype: str or None\n");

static PyObject *pyrna_mesh_get_custom_overlay_mode(PyObject *self)
{
  BPy_StructRNA *pyrna = reinterpret_cast<BPy_StructRNA *>(self);
  if (!pyrna->ptr.has_value() || !pyrna->ptr->data) {
     Py_RETURN_NONE;
  }
  Mesh *mesh = static_cast<Mesh *>(pyrna->ptr->data);

  /* Need Object to determine mode. Use active object if matches mesh. */
  bContext *C = BPY_context_get();
  Object *ob = nullptr;
  if (C) {
    ob = CTX_data_active_object(C);
    if (ob && ob->type == OB_MESH && ob->data == &mesh->id) {
       /* Match */
    } else {
       ob = nullptr;
    }
  }
  
  blender::draw::CustomOverlayMode mode = blender::draw::get_custom_overlay_mode(ob);
  
  switch (mode) {
    case blender::draw::CustomOverlayMode::ModeEdit:
      return PyUnicode_FromString("edit");
    case blender::draw::CustomOverlayMode::ModeSculpt:
      return PyUnicode_FromString("sculpt");
    default:
      Py_RETURN_NONE;
  }
}

static PyMethodDef pyrna_mesh_methods[] = {
    {"get_sculpt_custom_triangles_batch",
     (PyCFunction)pyrna_mesh_get_sculpt_custom_triangles_batch,
     METH_NOARGS,
     pyrna_mesh_get_sculpt_custom_triangles_batch_doc},
    {"get_sculpt_custom_edges_batch",
     (PyCFunction)pyrna_mesh_get_sculpt_custom_edges_batch,
     METH_NOARGS,
     pyrna_mesh_get_sculpt_custom_edges_batch_doc},
    {"get_sculpt_custom_vertices_batch",
     (PyCFunction)pyrna_mesh_get_sculpt_custom_vertices_batch,
     METH_NOARGS,
     pyrna_mesh_get_sculpt_custom_vertices_batch_doc},
    {"get_custom_overlay_batch",
     (PyCFunction)pyrna_mesh_get_custom_overlay_batch,
     METH_VARARGS,
     pyrna_mesh_get_custom_overlay_batch_doc},
    {"get_custom_overlay_mode",
     (PyCFunction)pyrna_mesh_get_custom_overlay_mode,
     METH_NOARGS,
     pyrna_mesh_get_custom_overlay_mode_doc},
    {nullptr, nullptr, 0, nullptr},
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public API
 * \{ */

void BPY_rna_types_extend_capi(PyObject *bpy_types)
{
  /* BlendData */
  ARRAY_SET_ITEMS(pyrna_blenddata_methods,
                  BPY_rna_id_collection_user_map_method_def,
                  BPY_rna_id_collection_file_path_map_method_def,
                  BPY_rna_id_collection_file_path_foreach_method_def,
                  BPY_rna_id_collection_batch_remove_method_def,
                  BPY_rna_id_collection_orphans_purge_method_def,
                  BPY_rna_data_context_method_def);
  BLI_STATIC_ASSERT(ARRAY_SIZE(pyrna_blenddata_methods) == 7, "Unexpected number of methods")
  pyrna_struct_type_extend_capi(RNA_BlendData, pyrna_blenddata_methods, nullptr);

  /* BlendDataLibraries */
  ARRAY_SET_ITEMS(
      pyrna_blenddatalibraries_methods, BPY_library_load_method_def, BPY_library_write_method_def);
  BLI_STATIC_ASSERT(ARRAY_SIZE(pyrna_blenddatalibraries_methods) == 3,
                    "Unexpected number of methods")
  pyrna_struct_type_extend_capi(RNA_BlendDataLibraries, pyrna_blenddatalibraries_methods, nullptr);

  /* ui::Layout */
  ARRAY_SET_ITEMS(pyrna_uilayout_methods, BPY_rna_uilayout_introspect_method_def);
  BLI_STATIC_ASSERT(ARRAY_SIZE(pyrna_uilayout_methods) == 2, "Unexpected number of methods")
  pyrna_struct_type_extend_capi(RNA_UILayout, pyrna_uilayout_methods, nullptr);

  /* Space */
  pyrna_struct_type_extend_capi(RNA_Space, pyrna_space_methods, nullptr);

  /* Text Editor */
  ARRAY_SET_ITEMS(pyrna_text_methods,
                  BPY_rna_region_as_string_method_def,
                  BPY_rna_region_from_string_method_def);
  BLI_STATIC_ASSERT(ARRAY_SIZE(pyrna_text_methods) == 3, "Unexpected number of methods")
  pyrna_struct_type_extend_capi(RNA_Text, pyrna_text_methods, nullptr);

  /* wmOperator */
  ARRAY_SET_ITEMS(pyrna_operator_methods, BPY_rna_operator_poll_message_set_method_def);
  BLI_STATIC_ASSERT(ARRAY_SIZE(pyrna_operator_methods) == 2, "Unexpected number of methods")
  pyrna_struct_type_extend_capi(RNA_Operator, pyrna_operator_methods, nullptr);

  /* WindowManager */
  ARRAY_SET_ITEMS(pyrna_windowmanager_methods,
                  BPY_rna_windowmanager_draw_cursor_add_method_def,
                  BPY_rna_windowmanager_draw_cursor_remove_method_def);
  BLI_STATIC_ASSERT(ARRAY_SIZE(pyrna_windowmanager_methods) == 3, "Unexpected number of methods")
  ARRAY_SET_ITEMS(pyrna_windowmanager_getset, BPY_rna_windowmanager_clipboard_getset_def);
  pyrna_struct_type_extend_capi(
      RNA_WindowManager, pyrna_windowmanager_methods, pyrna_windowmanager_getset);

  /* Window */
  ARRAY_SET_ITEMS(pyrna_window_methods, BPY_rna_window_screenshot_method_def);
  BLI_STATIC_ASSERT(ARRAY_SIZE(pyrna_window_methods) == 2, "Unexpected number of methods")
  pyrna_struct_type_extend_capi(RNA_Window, pyrna_window_methods, nullptr);

  /* Context */
  bpy_rna_context_types_init(bpy_types);

  ARRAY_SET_ITEMS(pyrna_context_methods, BPY_rna_context_temp_override_method_def);
  pyrna_struct_type_extend_capi(RNA_Context, pyrna_context_methods, nullptr);

  /* Mesh - Sculpt Custom Overlay API */
  pyrna_struct_type_extend_capi(RNA_Mesh, pyrna_mesh_methods, nullptr);
}

/** \} */

}  // namespace blender
