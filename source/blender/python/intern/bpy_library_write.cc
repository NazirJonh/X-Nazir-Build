/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup pythonintern
 *
 * Python API for writing a set of data-blocks into a file.
 * Useful for writing out asset-libraries, defines: `bpy.data.libraries.write(...)`.
 */

#include <Python.h>
#include <cstddef>

#include "BLI_path_utils.hh"
#include "BLI_string.h"

#include "BKE_blendfile.hh"
#include "BKE_global.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_report.hh"
#include "BKE_sculpt_layers.hh"

#include "BLO_writefile.hh"

#include "RNA_types.hh"

#include "bpy_capi_utils.hh"
#include "bpy_library.hh" /* Declaration for #BPY_library_load_method_def */
#include "bpy_rna.hh"

#include "../generic/py_capi_utils.hh"
#include "../generic/python_compat.hh" /* IWYU pragma: keep. */

namespace blender {

using namespace blender::bke::blendfile;

PyDoc_STRVAR(
    /* Wrap. */
    bpy_lib_write_doc,
    ".. method:: write(filepath, datablocks, *, "
    "path_remap='NONE', fake_user=False, compress=False)\n"
    "\n"
    "   Write data-blocks into a blend file.\n"
    "\n"
    "   .. note::\n"
    "\n"
    "      Indirectly referenced data-blocks will be expanded and written too.\n"
    "\n"
    "   :param filepath: The path to write the blend-file.\n"
    "   :type filepath: str | bytes\n"
    "   :param datablocks: set of data-blocks.\n"
    "   :type datablocks: set[:class:`bpy.types.ID`]\n"
    "   :param path_remap: Optionally remap paths when writing the file:\n"
    "\n"
    "      - ``NONE`` No path manipulation (default).\n"
    "      - ``RELATIVE`` Remap paths that are already relative to the new location.\n"
    "      - ``RELATIVE_ALL`` Remap all paths to be relative to the new location.\n"
    "      - ``ABSOLUTE`` Make all paths absolute on writing.\n"
    "\n"
    "   :type path_remap: str\n"
    "   :param fake_user: When True, data-blocks will be written with fake-user flag enabled.\n"
    "   :type fake_user: bool\n"
    "   :param compress: When True, write a compressed blend file.\n"
    "   :type compress: bool\n");
static PyObject *bpy_lib_write(BPy_PropertyRNA *self, PyObject *args, PyObject *kw)
{
  /* args */
  PyC_UnicodeAsBytesAndSize_Data filepath_data = {nullptr};
  char filepath_abs[FILE_MAX];
  PyObject *datablocks = nullptr;

  const PyC_StringEnumItems path_remap_items[] = {
      {BLO_WRITE_PATH_REMAP_NONE, "NONE"},
      {BLO_WRITE_PATH_REMAP_RELATIVE, "RELATIVE"},
      {BLO_WRITE_PATH_REMAP_RELATIVE_ALL, "RELATIVE_ALL"},
      {BLO_WRITE_PATH_REMAP_ABSOLUTE, "ABSOLUTE"},
      {0, nullptr},
  };
  PyC_StringEnum path_remap = {path_remap_items, BLO_WRITE_PATH_REMAP_NONE};

  bool use_fake_user = false, use_compress = false;

  static const char *_keywords[] = {
      "filepath",
      "datablocks",
      "path_remap",
      "fake_user",
      "compress",
      nullptr,
  };
  static _PyArg_Parser _parser = {
      "O&" /* `filepath` */
      "O!" /* `datablocks` */
      "|$" /* Optional keyword only arguments. */
      "O&" /* `path_remap` */
      "O&" /* `fake_user` */
      "O&" /* `compress` */
      ":write",
      _keywords,
      nullptr,
  };
  if (!_PyArg_ParseTupleAndKeywordsFast(args,
                                        kw,
                                        &_parser,
                                        PyC_ParseUnicodeAsBytesAndSize,
                                        &filepath_data,
                                        &PySet_Type,
                                        &datablocks,
                                        PyC_ParseStringEnum,
                                        &path_remap,
                                        PyC_ParseBool,
                                        &use_fake_user,
                                        PyC_ParseBool,
                                        &use_compress))
  {
    return nullptr;
  }

  Main *bmain_src = static_cast<Main *>(self->ptr->data); /* Typically #G_MAIN */
  int write_flags = 0;

  if (use_compress) {
    write_flags |= G_FILE_COMPRESS;
  }

  STRNCPY(filepath_abs, filepath_data.value);
  Py_XDECREF(filepath_data.value_coerce);

  const bool was_relative = BLI_path_abs(filepath_abs, BKE_main_blendfile_path_from_global());
  if (!was_relative) {
    /* If this was a blendfile-relative path, it's been made absolute now. However, BLI_path_abs()
     * doesn't turn normal relative paths into absolute ones, so that still has to happen. */
    BLI_path_abs_from_cwd(filepath_abs, sizeof(filepath_abs));
  }

  /* Park any open sculpt layer weight-mask editing session for the duration, exactly as the regular
   * save paths do (see #wm_file_write). A session keeps the layer's weights in the mesh's own
   * `.sculpt_mask` with the user's mask set aside, and #PartialWriteContext::id_add below copies the
   * mesh verbatim — so without this the written file would carry a sculpt layer's weight map
   * masquerading as the user's sculpt mask, silently. Scoped over #id_add as well as the write: the
   * copies are taken there, not at write time. */
  const bke::sculpt_layers::MaskEditSuspendGuard mask_edit_guard{*bmain_src};

  PartialWriteContext partial_write_ctx{*bmain_src};
  const PartialWriteContext::IDAddOptions add_options{
      (PartialWriteContext::IDAddOperations::ADD_DEPENDENCIES |
       PartialWriteContext::IDAddOperations(
           use_fake_user ? PartialWriteContext::IDAddOperations::SET_FAKE_USER : 0))};

  if (PySet_GET_SIZE(datablocks) > 0) {
    PyObject *it = PyObject_GetIter(datablocks);
    PyObject *key;
    while ((key = PyIter_Next(it))) {
      /* Borrow from the set. */
      Py_DECREF(key);
      ID *id;
      if (!pyrna_id_FromPyObject(key, &id)) {
        PyErr_Format(PyExc_TypeError, "Expected an ID type, not %.200s", Py_TYPE(key)->tp_name);
        break;
      }
      partial_write_ctx.id_add(id, add_options, nullptr);
    }
    Py_DECREF(it);
    if (key) {
      return nullptr;
    }
  }

  BLI_assert(partial_write_ctx.is_valid());

  /* write blend */
  ReportList reports;

  BKE_reports_init(&reports, RPT_STORE | RPT_PRINT_HANDLED_BY_OWNER);

  /* Recorded before the write so the wording matches the other writers, but only *surfaced* after
   * it: a refusal is never a reason to skip the write (see #MaskEditSuspendGuard). */
  const bool mask_edit_refused = mask_edit_guard.suspend_refused();
  if (mask_edit_refused) {
    BKE_report(&reports,
               RPT_WARNING,
               "A sculpt layer mask session could not be suspended: the file was saved with the "
               "layer's mask weights in place of the sculpt mask. Close the mask session and "
               "repaint the sculpt mask");
  }

  bool success = partial_write_ctx.write(
      filepath_abs, write_flags, path_remap.value_found, reports);

  PyObject *py_return_value;
  if (success) {
    BKE_reports_print(&reports, RPT_ERROR_ALL);
    py_return_value = Py_None;
    Py_INCREF(py_return_value);
  }
  else {
    if (BPy_reports_to_error(&reports, PyExc_IOError, false) == 0) {
      PyErr_SetString(PyExc_IOError, "Unknown error writing library data");
    }
    py_return_value = nullptr;
  }

  /* The #ReportList above never reaches Python on its own: the success path prints
   * #RPT_ERROR_ALL only, which excludes warnings, and the failure path turns errors into the
   * exception. A Python warning is the only channel a caller can see or filter, so raise one
   * too. It may be configured as an error, in which case the (already written) file stands but
   * the call reports failure. */
  if (mask_edit_refused && py_return_value != nullptr) {
    if (PyErr_WarnEx(PyExc_RuntimeWarning,
                     "A sculpt layer mask session could not be suspended: the file was saved with "
                     "the layer's mask weights in place of the sculpt mask. Close the mask session "
                     "and repaint the sculpt mask",
                     1) == -1)
    {
      Py_DECREF(py_return_value);
      py_return_value = nullptr;
    }
  }

  BKE_reports_free(&reports);

  return py_return_value;
}

#ifdef __GNUC__
#  ifdef __clang__
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wcast-function-type"
#  else
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wcast-function-type"
#  endif
#endif

PyMethodDef BPY_library_write_method_def = {
    "write",
    reinterpret_cast<PyCFunction>(bpy_lib_write),
    METH_VARARGS | METH_KEYWORDS,
    bpy_lib_write_doc,
};

#ifdef __GNUC__
#  ifdef __clang__
#    pragma clang diagnostic pop
#  else
#    pragma GCC diagnostic pop
#  endif
#endif

}  // namespace blender
