/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * Temporary diagnostics of Paint Layers. Set to 1 to print; the whole header and every `PL_DEBUG`
 * use are removed in the final cleanup. Lives in the public blenkernel headers because the editors
 * (bake, Outliner) print too and cannot see `blenkernel/intern`.
 */

#include <cstdio>

#define PAINT_LAYERS_DEBUG_LOG 1

#if PAINT_LAYERS_DEBUG_LOG
#  define PL_DEBUG_PRINTF(...) printf(__VA_ARGS__)
#  define PL_DEBUG_ENABLED true

/**
 * Names the caller of `BKE_paint_layers_source_material_tree_hash` in the hash trace. Thread local
 * because the hash is asked from job threads as well as the main one.
 */
inline thread_local const char *g_source_hash_caller = nullptr;

struct SourceHashCallerScope {
  const char *previous;
  explicit SourceHashCallerScope(const char *name) : previous(g_source_hash_caller)
  {
    g_source_hash_caller = name;
  }
  ~SourceHashCallerScope()
  {
    g_source_hash_caller = previous;
  }
};
/** Declares a scope-long caller tag; put it before the statement that asks for the hash. */
#  define PL_HASH_CALLER(name) const SourceHashCallerScope pl_hash_caller_scope_(name)
#else
#  define PL_HASH_CALLER(name) ((void)0)
#  define PL_DEBUG_PRINTF(...) ((void)0)
#  define PL_DEBUG_ENABLED false
#endif
