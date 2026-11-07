/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * The interface-socket identifiers of the node groups the paint material composite owns, fixed by
 * the creation order in each group's `*_ensure` function. A group instance inherits these
 * `Socket_N` identifiers verbatim, while its socket *names* ("A", "Result", ...) are display
 * strings and are translated when the "New Data" translation preference is on. #node_find_socket
 * matches identifiers, so every reader of a group instance -- the build, the sync, the CPU
 * composite -- must ask for these, never for the names.
 *
 * The light header the group's readers share: the kind schemas of the generators and the build
 * include it beside the composite's own translation units, so no reader of the identifiers has to
 * pull the composite's internal declarations (`paint_material_composite_internal.hh`).
 */

namespace blender {

/**
 * Interface-socket identifiers of the Normal Combine group, fixed by the creation order in
 * #BKE_paint_material_normal_combine_group_ensure.
 */
constexpr const char *NORMAL_COMBINE_ID_A = "Socket_0";
constexpr const char *NORMAL_COMBINE_ID_B = "Socket_1";
constexpr const char *NORMAL_COMBINE_ID_FACTOR = "Socket_2";
constexpr const char *NORMAL_COMBINE_ID_RESULT = "Socket_3";

/**
 * Interface-socket identifiers of the shared Blend group, fixed by the creation order in
 * #BKE_paint_material_blend_group_ensure: Factor, A, B, Mode and the Result output.
 */
constexpr const char *BLEND_GROUP_ID_FACTOR = "Socket_0";
constexpr const char *BLEND_GROUP_ID_A = "Socket_1";
constexpr const char *BLEND_GROUP_ID_B = "Socket_2";
constexpr const char *BLEND_GROUP_ID_MODE = "Socket_3";
constexpr const char *BLEND_GROUP_ID_RESULT = "Socket_4";

/**
 * Interface-socket identifiers of the shared Gradient Factor group, fixed by the creation order in
 * #BKE_paint_material_gradient_group_ensure: Coordinate (the first socket keeps the legacy `UV`
 * identifier), Start, End, Midpoint, Type, Repeat and the Factor output.
 */
constexpr const char *GRADIENT_GROUP_ID_COORDINATE = "Socket_0";
constexpr const char *GRADIENT_GROUP_ID_START = "Socket_1";
constexpr const char *GRADIENT_GROUP_ID_END = "Socket_2";
constexpr const char *GRADIENT_GROUP_ID_MIDPOINT = "Socket_3";
constexpr const char *GRADIENT_GROUP_ID_TYPE = "Socket_4";
constexpr const char *GRADIENT_GROUP_ID_REPEAT = "Socket_5";
constexpr const char *GRADIENT_GROUP_ID_FACTOR = "Socket_6";

/**
 * Interface-socket identifiers of the shared Scratches group, fixed by the creation order in
 * #BKE_paint_material_scratches_group_ensure. The five the Voronoi node reads are its own inputs,
 * passed straight through; the last three are the line's width, its fading and the stretch that
 * turns a cell edge into a scratch.
 */
constexpr const char *SCRATCHES_GROUP_ID_COORDINATE = "Socket_0";
constexpr const char *SCRATCHES_GROUP_ID_SCALE = "Socket_1";
constexpr const char *SCRATCHES_GROUP_ID_DETAIL = "Socket_2";
constexpr const char *SCRATCHES_GROUP_ID_ROUGHNESS = "Socket_3";
constexpr const char *SCRATCHES_GROUP_ID_LACUNARITY = "Socket_4";
constexpr const char *SCRATCHES_GROUP_ID_RANDOMNESS = "Socket_5";
constexpr const char *SCRATCHES_GROUP_ID_WIDTH = "Socket_6";
constexpr const char *SCRATCHES_GROUP_ID_SOFTEN = "Socket_7";
constexpr const char *SCRATCHES_GROUP_ID_STRETCH = "Socket_8";
constexpr const char *SCRATCHES_GROUP_ID_FACTOR = "Socket_9";

/**
 * Interface-socket identifiers of the shared Weave group, fixed by the creation order in
 * #BKE_paint_material_weave_group_ensure. The pattern -- plain or twill -- is a value here, not a
 * choice of nodes: the group switches between the two by a Mix node, so an edit of it is a value
 * the row's own sync writes.
 */
constexpr const char *WEAVE_GROUP_ID_COORDINATE = "Socket_0";
constexpr const char *WEAVE_GROUP_ID_SCALE = "Socket_1";
constexpr const char *WEAVE_GROUP_ID_PATTERN = "Socket_2";
constexpr const char *WEAVE_GROUP_ID_THREAD_WIDTH = "Socket_3";
constexpr const char *WEAVE_GROUP_ID_GAP = "Socket_4";
constexpr const char *WEAVE_GROUP_ID_SOFTEN = "Socket_5";
constexpr const char *WEAVE_GROUP_ID_JITTER = "Socket_6";
constexpr const char *WEAVE_GROUP_ID_FACTOR = "Socket_7";

/**
 * Interface-socket identifiers of the shared Grunge group, fixed by the creation order in
 * #BKE_paint_material_grunge_group_ensure. The first four are the base noise's own inputs; the
 * variation pair drives the second noise, whose scale is the product of the two, and the last two
 * are the threshold the patches break against.
 */
constexpr const char *GRUNGE_GROUP_ID_COORDINATE = "Socket_0";
constexpr const char *GRUNGE_GROUP_ID_SCALE = "Socket_1";
constexpr const char *GRUNGE_GROUP_ID_DETAIL = "Socket_2";
constexpr const char *GRUNGE_GROUP_ID_ROUGHNESS = "Socket_3";
constexpr const char *GRUNGE_GROUP_ID_LACUNARITY = "Socket_4";
constexpr const char *GRUNGE_GROUP_ID_DISTORTION = "Socket_5";
constexpr const char *GRUNGE_GROUP_ID_VARIATION_STRENGTH = "Socket_6";
constexpr const char *GRUNGE_GROUP_ID_VARIATION_SCALE = "Socket_7";
constexpr const char *GRUNGE_GROUP_ID_THRESHOLD = "Socket_8";
constexpr const char *GRUNGE_GROUP_ID_SOFTEN = "Socket_9";
constexpr const char *GRUNGE_GROUP_ID_FACTOR = "Socket_10";

}  // namespace blender
