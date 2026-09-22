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

#define PAINT_LAYERS_DEBUG_LOG 0

#if PAINT_LAYERS_DEBUG_LOG
#  define PL_DEBUG_PRINTF(...) printf(__VA_ARGS__)
#  define PL_DEBUG_ENABLED true
#else
#  define PL_DEBUG_PRINTF(...) ((void)0)
#  define PL_DEBUG_ENABLED false
#endif
