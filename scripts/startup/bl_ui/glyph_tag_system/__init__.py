# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Category Tabs / Glyph / Tag system package.

Incrementally extracted from ``space_userpref.py``. Provides the pure (bpy-free,
state-free) layers: data constants (:mod:`.defaults`), pure conversions
(:mod:`.conversions`), debug/dedup logging (:mod:`.log`) and JSON schema
migrations (:mod:`.migrations`). Stateful/behavioural modules will follow.
"""
