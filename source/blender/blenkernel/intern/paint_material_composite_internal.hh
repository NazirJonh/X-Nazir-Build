/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

namespace blender {

/**
 * Interface-socket identifiers of the Normal Combine group, fixed by the creation order in
 * #BKE_paint_material_normal_combine_group_ensure. A group instance inherits these `Socket_N`
 * identifiers verbatim, while its socket *names* ("A", "Result", ...) are display strings and are
 * translated when the "New Data" translation preference is on. #node_find_socket matches
 * identifiers, so a reader of a combine-group instance must ask for these, not for the names.
 */
constexpr const char *NORMAL_COMBINE_ID_A = "Socket_0";
constexpr const char *NORMAL_COMBINE_ID_B = "Socket_1";
constexpr const char *NORMAL_COMBINE_ID_FACTOR = "Socket_2";
constexpr const char *NORMAL_COMBINE_ID_RESULT = "Socket_3";

}  // namespace blender
