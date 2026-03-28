/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Experimental build information API.
 * Provides centralized access to custom build metadata.
 */

#pragma once

#include "BLI_sys_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Structure containing experimental build information.
 * This is the single source of truth for build metadata.
 */
typedef struct ExperimentalBuildInfo {
  /** Display name of the build */
  const char *build_name;
  /** Author of the build */
  const char *author;
  /** Brief description - part 1 */
  const char *description1;
  /** Brief description - part 2 */
  const char *description2;
  /** Warning message for users - part 1 */
  const char *warning_message1;
  /** Warning message for users - part 2 */
  const char *warning_message2;
  /** Build date string */
  const char *build_date;
  /** Additional notes or credits */
  const char *credits;
} ExperimentalBuildInfo;

/**
 * Get the experimental build information.
 * Returns a pointer to a static structure containing build metadata.
 *
 * \return Pointer to the ExperimentalBuildInfo structure.
 */
const ExperimentalBuildInfo *BKE_experimental_build_info_get(void);

/**
 * Check if this is an experimental/custom build.
 * Always returns true for custom builds.
 *
 * \return True if this is an experimental build.
 */
bool BKE_experimental_build_is_custom(void);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

namespace blender {

/**
 * C++ wrapper for the experimental build info.
 * Provides convenient access to build metadata.
 */
inline const ExperimentalBuildInfo &experimental_build_info()
{
  return *BKE_experimental_build_info_get();
}

}  // namespace blender

#endif
