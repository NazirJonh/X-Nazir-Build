/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bpygpu
 */

#pragma once

#include <Python.h>

#include "BLI_compiler_attrs.h"

namespace blender {

namespace gpu {
class Batch;
}

#define USE_GPU_PY_REFERENCES

extern PyTypeObject BPyGPUBatch_Type;

#define BPyGPUBatch_Check(v) (Py_TYPE(v) == &BPyGPUBatch_Type)

struct BPyGPUBatch {
  PyObject_HEAD
  /* If true, Python owns the batch and will discard it on dealloc.
   * If false, the batch is managed externally (e.g., by mesh cache). */
  bool owns_batch;
  blender::gpu::Batch *batch;
#ifdef USE_GPU_PY_REFERENCES
  /* Just to keep a user to prevent freeing buffers we're using. */
  PyObject *references;
#endif
};

/**
 * Create a Python GPUBatch object that OWNS the batch.
 * The batch will be discarded when the Python object is deallocated.
 */
[[nodiscard]] PyObject *BPyGPUBatch_CreatePyObject(blender::gpu::Batch *batch) ATTR_NONNULL(1);

/**
 * Create a Python GPUBatch wrapper that does NOT own the batch.
 * The batch will NOT be discarded when the Python object is deallocated.
 * Use this for batches managed by external caches (e.g., mesh batch cache).
 */
[[nodiscard]] PyObject *BPyGPUBatch_CreatePyObject_Wrap(blender::gpu::Batch *batch) ATTR_NONNULL(1);

}  // namespace blender
