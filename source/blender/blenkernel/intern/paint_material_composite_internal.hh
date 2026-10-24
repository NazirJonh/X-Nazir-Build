/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

namespace blender {

struct ID;
struct Main;
struct bNode;
struct bNodeTree;

/**
 * The marker shared by the node groups this module owns (Normal Combine, Blend, Gradient): one
 * IDProperty key, told apart by \a marker_value. Found by marker, so a rename never loses a group.
 */
bool paint_material_group_has_marker(const bNode &node, const char *marker_value);

/** The group of \a bmain carrying \a marker_value, or null. */
bNodeTree *paint_material_group_find(Main &bmain, const char *marker_value);

/** Stamp \a group with \a marker_value. */
void paint_material_group_marker_set(bNodeTree &group, const char *marker_value);

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

/**
 * Interface-socket identifiers of the shared Blend group, fixed by the creation order in
 * #BKE_paint_material_blend_group_ensure: Factor, A, B, Mode and the Result output. The same
 * identifier-versus-name rule as above applies: the generator, the sync and the CPU reader all ask
 * for these, never for the translatable display names.
 */
constexpr const char *BLEND_GROUP_ID_FACTOR = "Socket_0";
constexpr const char *BLEND_GROUP_ID_A = "Socket_1";
constexpr const char *BLEND_GROUP_ID_B = "Socket_2";
constexpr const char *BLEND_GROUP_ID_MODE = "Socket_3";
constexpr const char *BLEND_GROUP_ID_RESULT = "Socket_4";

/**
 * Interface-socket identifiers of the shared Gradient Factor group, fixed by the creation order in
 * #BKE_paint_material_gradient_group_ensure: Coordinate (the first socket keeps the legacy `UV`
 * identifier), Start, End, Midpoint, Type, Repeat and the Factor output. The same
 * identifier-versus-name rule as above applies.
 */
constexpr const char *GRADIENT_GROUP_ID_COORDINATE = "Socket_0";
constexpr const char *GRADIENT_GROUP_ID_START = "Socket_1";
constexpr const char *GRADIENT_GROUP_ID_END = "Socket_2";
constexpr const char *GRADIENT_GROUP_ID_MIDPOINT = "Socket_3";
constexpr const char *GRADIENT_GROUP_ID_TYPE = "Socket_4";
constexpr const char *GRADIENT_GROUP_ID_REPEAT = "Socket_5";
constexpr const char *GRADIENT_GROUP_ID_FACTOR = "Socket_6";

}  // namespace blender
