/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The console diagnostics of the Transform Selection surface decal (Image Editor and 3D Viewport).
 * One switch turns all of them off.
 */

#pragma once

#include <cstdio>

/** Set to 0 to compile every decal diagnostic out. */
#define IMAGE_SELECT_DECAL_LOG_ENABLED 1

/** Message prefixes, concatenated to the format string by the caller. */
#define IMAGE_SELECT_DECAL_TAG_EDITOR "[image-editor-decal] "
#define IMAGE_SELECT_DECAL_TAG_3D "[transform-decal] "

#if IMAGE_SELECT_DECAL_LOG_ENABLED
/** `IMAGE_SELECT_DECAL_LOG(IMAGE_SELECT_DECAL_TAG_EDITOR "text %d\n", value)`. */
#  define IMAGE_SELECT_DECAL_LOG(...) printf(__VA_ARGS__)
#else
#  define IMAGE_SELECT_DECAL_LOG(...) ((void)0)
#endif
