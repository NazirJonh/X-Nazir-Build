# SPDX-FileCopyrightText: 2026 XNazir
#
# SPDX-License-Identifier: GPL-2.0-or-later

from . import (
    operators,
    preferences,
)

_modules = (operators, preferences)


def register():
    for module in _modules:
        module.register()


def unregister():
    for module in reversed(_modules):
        module.unregister()
