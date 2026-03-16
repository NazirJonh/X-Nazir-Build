/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Experimental build information implementation.
 */

#include "BKE_experimental_build.hh"

#include "BLI_time.h"

namespace blender {

/**
 * Static build information.
 * This is the single source of truth for custom build metadata.
 *
 * IMPORTANT: Modify these values to customize your build.
 */
static const ExperimentalBuildInfo g_experimental_build_info = {
    /* build_name */ "Experimental Build",
    /* author */ "Nazir Galimov",
    /* description */ "This is a custom Blender build with experimental features.",
    /* warning_message */ "Use at your own risk. Not officially supported.",
    /* build_date */ __DATE__ " " __TIME__,
    /* credits */ "Built with love and caffeine.",
};

}  // namespace blender

/* C API implementation */

const ExperimentalBuildInfo *BKE_experimental_build_info_get()
{
  return &blender::g_experimental_build_info;
}

bool BKE_experimental_build_is_custom()
{
  return true;
}
