/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 *
 * The list of stack kinds the Outliner knows about.
 *
 * Built-in and ordered by hand rather than registered dynamically: the sources are C++ in this
 * module, there is no run-time registration to serve, and a fixed list keeps the DNA enum and the
 * RNA items honest. A Python-side registry, if it is ever wanted, plugs in here without any of the
 * code above #StackSource having to learn about it.
 */

#include "BLI_vector.hh"

#include "DNA_space_types.h"

#include "UI_resources.hh"

#include "outliner_stack_source.hh"

namespace blender::ed::outliner {

namespace {

Span<const StackSource *> stack_sources_ensure()
{
  /* Static storage: the sources are stateless descriptions, and nothing outlives them that could
   * still ask for one during static destruction at exit. */
  static Vector<std::unique_ptr<StackSource>> owned = []() {
    Vector<std::unique_ptr<StackSource>> sources;
    sources.append(stack_source_shape_keys_create());
    return sources;
  }();
  static Vector<const StackSource *> pointers = [&]() {
    Vector<const StackSource *> result;
    for (const std::unique_ptr<StackSource> &source : owned) {
      result.append(source.get());
    }
    return result;
  }();
  return pointers;
}

}  // namespace

Span<const StackSource *> stack_sources_get()
{
  return stack_sources_ensure();
}

int StackSource::focus_button_icon() const
{
  return ICON_RENDERLAYERS;
}

int StackSource::empty_preview_icon() const
{
  return ICON_TEXTURE_DATA;
}

const StackSource *stack_source_get(const eSpaceOutliner_StackSource type)
{
  const Span<const StackSource *> sources = stack_sources_ensure();
  for (const StackSource *source : sources) {
    if (source->type() == type) {
      return source;
    }
  }
  /* A file can name a source this build does not have, the same way it can name a display mode it
   * does not have. Falling back beats returning null to a caller that has a tree to draw. */
  return sources.first();
}

const StackSource *stack_source_for_space(const SpaceOutliner &space_outliner)
{
  return stack_source_get(space_outliner.stack_source);
}

}  // namespace blender::ed::outliner
