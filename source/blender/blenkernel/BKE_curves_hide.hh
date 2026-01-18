/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <optional>

#include "BLI_index_range.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "BKE_attribute.hh"

struct Object;

namespace blender {

struct Curves;

namespace bke {

class CurvesGeometry;

}

namespace ed::sculpt_paint {

enum class VisAction {
  Hide = 0,
  Show = 1,
};

namespace curves_hide {

bke::SpanAttributeWriter<bool> hide_point_ensure(Curves &curves_id);
bke::SpanAttributeWriter<bool> hide_curve_ensure(Curves &curves_id);

void hide_attributes_remove(Curves &curves_id);

bool point_is_hidden(const bke::CurvesGeometry &curves, int point_index);
bool curve_is_hidden(const bke::CurvesGeometry &curves, int curve_index);
bool has_hidden_elements(const bke::CurvesGeometry &curves);

void hide_points(Object &object, const IndexMask &point_mask, VisAction action);

void hide_curves(Object &object, const IndexMask &curve_mask, VisAction action);

void show_all(Object &object);

void invert_hide(Object &object, const IndexMask &mask, bke::AttrDomain domain);

void sync_hide_from_points_to_curves(Curves &curves_id);
void sync_hide_from_curves_to_points(Curves &curves_id);

}  // namespace blender::ed::sculpt_paint::curves_hide

}  // namespace blender::ed::sculpt_paint

}  // namespace blender 
