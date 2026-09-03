/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <string>

#include "BLI_string_ref.hh"

namespace blender {

struct bContext;

namespace ui {
struct Block;
struct Button;
struct Layout;
struct TooltipData;

/* names */
#define UI_MAX_DRAW_STR 550
#define UI_MAX_NAME_STR 256
#define UI_MAX_SHORTCUT_STR 64

#define RNA_NO_INDEX -1

/* Menu Callbacks */

using MenuCreateFunc = void (*)(bContext *C, Layout *layout, void *arg1);
using MenuHandleFunc = void (*)(bContext *C, void *arg, int event);

/**
 * Used for cycling menu values without opening the menu (Ctrl-Wheel).
 * \param direction: forward or backwards [1 / -1].
 * \param but: the button being cycled. Gives access to its context store and RNA source, which a
 * plain `Button.poin` argument cannot provide (needed by selectors whose item list depends on the
 * layout context, see #grid_library_selector_menu_step).
 * \return the button's new value. Applied to the RNA property for enum buttons, so a stepper on
 * such a button must return the new enum value; steppers on non-RNA menu buttons apply the change
 * themselves and the return value is unused.
 */
using MenuStepFunc = int (*)(bContext *C, int direction, Button *but);

using CopyArgFunc = void *(*)(const void *arg);
using FreeArgFunc = void (*)(void *arg);

/** Must return an allocated string. */
using ButtonToolTipFunc = std::string (*)(bContext *C, void *argN, StringRef tip);

/**
 * \param data: The tooltip data to be filled.
 * \param but: The exact button the tooltip is shown for. This is needed when the tooltip function
 *   is shared across multiple buttons but there still needs to be some customization per button.
 *   Mostly useful when using #uiLayoutSetTooltipCustomFunc.
 */
using ButtonToolTipCustomFunc = void (*)(bContext &C, TooltipData &data, Button *but, void *argN);

}  // namespace ui

namespace ocio {
class Display;
}  // namespace ocio
using ColorManagedDisplay = ocio::Display;

}  // namespace blender
