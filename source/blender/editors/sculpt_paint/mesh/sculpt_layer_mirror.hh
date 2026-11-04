/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Internal interface of #SCULPT_OT_layer_mirror_copy (see `sculpt_layer_mirror.cc`); the mapping
 * core it runs is `sculpt_layer_mirror_map.hh`.
 */

#pragma once

struct wmKeyConfig;

namespace blender::ed::sculpt_paint::layers {

/** Modal keymap of #SCULPT_OT_layer_mirror_copy. Bindings live in `blender_default.py`. */
void mirror_copy_modal_keymap(wmKeyConfig *keyconf);

}  // namespace blender::ed::sculpt_paint::layers
