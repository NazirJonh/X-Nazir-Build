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
    /* description1 */ "This is a custom build",
    /* description2 */ "with a prototype of an advanced tab management system.",
    /* warning_message1 */ "Use at your own risk.",
    /* warning_message2 */ "Do not use it to create important files for production.",
    /* build_date */ __DATE__ " " __TIME__,
    /* credits */ "To leave feedback, use the >>>Send FEEDBACK<<< button.",
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
